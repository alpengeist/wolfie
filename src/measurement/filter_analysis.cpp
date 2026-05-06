#include "measurement/filter_analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>
#include <vector>

#include "measurement/dsp_utils.h"
#include "measurement/stereo_diagnostics.h"

namespace wolfie::measurement {

namespace {

constexpr double kDegreesToRadians = std::numbers::pi_v<double> / 180.0;

struct ComplexSpectrumPair {
    std::vector<double> frequencyAxisHz;
    std::vector<std::complex<double>> left;
    std::vector<std::complex<double>> right;

    [[nodiscard]] bool valid() const {
        return !frequencyAxisHz.empty() &&
               left.size() == frequencyAxisHz.size() &&
               right.size() == frequencyAxisHz.size();
    }
};

struct TimeDomainPair {
    std::vector<double> timeSeconds;
    std::vector<double> left;
    std::vector<double> right;

    [[nodiscard]] bool valid() const {
        return !timeSeconds.empty() &&
               left.size() == timeSeconds.size() &&
               right.size() == timeSeconds.size();
    }
};

const MeasurementValueSet* findValidValueSet(const MeasurementResult& result, const std::string& key) {
    const MeasurementValueSet* valueSet = result.findValueSet(key);
    return valueSet != nullptr && valueSet->valid() ? valueSet : nullptr;
}

double dbToAmplitude(double valueDb) {
    return std::pow(10.0, valueDb / 20.0);
}

double interpolateLinear(const std::vector<double>& xValues,
                         const std::vector<double>& yValues,
                         double x) {
    if (xValues.empty() || xValues.size() != yValues.size()) {
        return 0.0;
    }
    if (x <= xValues.front()) {
        return yValues.front();
    }
    if (x >= xValues.back()) {
        return yValues.back();
    }

    const auto upper = std::lower_bound(xValues.begin(), xValues.end(), x);
    if (upper == xValues.begin()) {
        return yValues.front();
    }
    if (upper == xValues.end()) {
        return yValues.back();
    }

    const size_t upperIndex = static_cast<size_t>(upper - xValues.begin());
    const size_t lowerIndex = upperIndex - 1;
    const double span = xValues[upperIndex] - xValues[lowerIndex];
    if (std::abs(span) < 1.0e-12) {
        return yValues[lowerIndex];
    }
    const double blend = (x - xValues[lowerIndex]) / span;
    return yValues[lowerIndex] + ((yValues[upperIndex] - yValues[lowerIndex]) * blend);
}

ComplexSpectrumPair loadSpectrumPair(const MeasurementResult& result, std::string_view window) {
    const std::string prefix = "measurement." + std::string(window);
    const MeasurementValueSet* magnitude = findValidValueSet(result, prefix + "_magnitude_spectrum");
    const MeasurementValueSet* phase = findValidValueSet(result, prefix + "_phase_spectrum");
    if (magnitude == nullptr || phase == nullptr) {
        magnitude = findValidValueSet(result, prefix + "_magnitude_response");
        phase = findValidValueSet(result, prefix + "_phase_response");
    }
    if (magnitude == nullptr || phase == nullptr ||
        magnitude->xValues.size() != phase->xValues.size() ||
        magnitude->leftValues.size() != phase->leftValues.size() ||
        magnitude->rightValues.size() != phase->rightValues.size()) {
        return {};
    }

    ComplexSpectrumPair pair;
    pair.frequencyAxisHz = magnitude->xValues;
    pair.left.reserve(magnitude->xValues.size());
    pair.right.reserve(magnitude->xValues.size());
    for (size_t index = 0; index < magnitude->xValues.size(); ++index) {
        pair.left.push_back(std::polar(dbToAmplitude(magnitude->leftValues[index]),
                                       phase->leftValues[index] * kDegreesToRadians));
        pair.right.push_back(std::polar(dbToAmplitude(magnitude->rightValues[index]),
                                        phase->rightValues[index] * kDegreesToRadians));
    }
    return pair;
}

TimeDomainPair loadImpulsePair(const MeasurementResult& result, std::string_view window) {
    const MeasurementValueSet* impulse =
        findValidValueSet(result, "measurement." + std::string(window) + "_impulse_response");
    if (impulse == nullptr) {
        return {};
    }

    TimeDomainPair pair;
    pair.timeSeconds = impulse->xValues;
    pair.left = impulse->leftValues;
    pair.right = impulse->rightValues;
    return pair;
}

std::vector<std::complex<double>> buildSpectrum(const std::vector<double>& samples, size_t fftSize) {
    std::vector<std::complex<double>> spectrum(fftSize, {0.0, 0.0});
    for (size_t index = 0; index < samples.size() && index < spectrum.size(); ++index) {
        spectrum[index] = {samples[index], 0.0};
    }
    fft(spectrum, false);
    return spectrum;
}

std::vector<std::complex<double>> buildFilterResponse(const std::vector<double>& taps,
                                                      int sampleRate,
                                                      size_t fftSize,
                                                      const std::vector<double>& targetFrequencyAxisHz) {
    if (taps.empty() || targetFrequencyAxisHz.empty()) {
        return {};
    }

    const std::vector<std::complex<double>> spectrum = buildSpectrum(taps, fftSize);
    const std::vector<double> sourceFrequencyAxisHz = buildLinearFrequencyAxis(sampleRate, fftSize);
    const size_t positiveCount = std::min(sourceFrequencyAxisHz.size(), (fftSize / 2) + 1);
    std::vector<double> amplitudes;
    std::vector<double> phasesRadians;
    amplitudes.reserve(positiveCount);
    phasesRadians.reserve(positiveCount);
    for (size_t index = 0; index < positiveCount; ++index) {
        amplitudes.push_back(std::abs(spectrum[index]));
        phasesRadians.push_back(std::arg(spectrum[index]));
    }
    phasesRadians = unwrapPhaseRadians(phasesRadians);

    std::vector<std::complex<double>> response;
    response.reserve(targetFrequencyAxisHz.size());
    const std::vector<double> interpolationAxis(sourceFrequencyAxisHz.begin(),
                                                sourceFrequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(positiveCount));
    for (const double frequencyHz : targetFrequencyAxisHz) {
        const double amplitude = interpolateLinear(interpolationAxis, amplitudes, frequencyHz);
        const double phaseRadians = interpolateLinear(interpolationAxis, phasesRadians, frequencyHz);
        response.push_back(std::polar(std::max(amplitude, 1.0e-12), phaseRadians));
    }
    return response;
}

ComplexSpectrumPair applyFilterToSpectrum(const ComplexSpectrumPair& input,
                                          const FilterDesignResult& filterResult) {
    if (!input.valid() ||
        filterResult.left.filterTaps.empty() ||
        filterResult.right.filterTaps.empty()) {
        return {};
    }

    const int sampleRate = std::max(filterResult.sampleRate, 1);
    const size_t fftSize = filterResult.fftSize > 0
                               ? static_cast<size_t>(filterResult.fftSize)
                               : nextPowerOfTwo(std::max({filterResult.left.filterTaps.size(),
                                                          filterResult.right.filterTaps.size(),
                                                          size_t{4096}}));
    const std::vector<std::complex<double>> leftFilter =
        buildFilterResponse(filterResult.left.filterTaps, sampleRate, fftSize, input.frequencyAxisHz);
    const std::vector<std::complex<double>> rightFilter =
        buildFilterResponse(filterResult.right.filterTaps, sampleRate, fftSize, input.frequencyAxisHz);
    if (leftFilter.size() != input.frequencyAxisHz.size() ||
        rightFilter.size() != input.frequencyAxisHz.size()) {
        return {};
    }

    ComplexSpectrumPair output;
    output.frequencyAxisHz = input.frequencyAxisHz;
    output.left.reserve(input.frequencyAxisHz.size());
    output.right.reserve(input.frequencyAxisHz.size());
    for (size_t index = 0; index < input.frequencyAxisHz.size(); ++index) {
        output.left.push_back(input.left[index] * leftFilter[index]);
        output.right.push_back(input.right[index] * rightFilter[index]);
    }
    return output;
}

std::vector<double> convolveRealSignals(const std::vector<double>& signal,
                                        const std::vector<double>& taps) {
    if (signal.empty() || taps.empty()) {
        return {};
    }

    const size_t outputSize = signal.size() + taps.size() - 1;
    const size_t fftSize = nextPowerOfTwo(outputSize);
    std::vector<std::complex<double>> signalSpectrum(fftSize, {0.0, 0.0});
    std::vector<std::complex<double>> tapSpectrum(fftSize, {0.0, 0.0});
    for (size_t index = 0; index < signal.size(); ++index) {
        signalSpectrum[index] = {signal[index], 0.0};
    }
    for (size_t index = 0; index < taps.size(); ++index) {
        tapSpectrum[index] = {taps[index], 0.0};
    }
    fft(signalSpectrum, false);
    fft(tapSpectrum, false);
    for (size_t index = 0; index < fftSize; ++index) {
        signalSpectrum[index] *= tapSpectrum[index];
    }
    fft(signalSpectrum, true);

    std::vector<double> output(outputSize, 0.0);
    for (size_t index = 0; index < outputSize; ++index) {
        output[index] = signalSpectrum[index].real();
    }
    return output;
}

TimeDomainPair applyFilterToImpulse(const TimeDomainPair& input,
                                    const FilterDesignResult& filterResult,
                                    int sampleRate) {
    if (!input.valid() ||
        filterResult.left.filterTaps.empty() ||
        filterResult.right.filterTaps.empty()) {
        return {};
    }

    TimeDomainPair output;
    output.left = convolveRealSignals(input.left, filterResult.left.filterTaps);
    output.right = convolveRealSignals(input.right, filterResult.right.filterTaps);
    if (output.left.empty() || output.right.empty() || output.left.size() != output.right.size()) {
        return {};
    }

    const double sampleStepSeconds =
        input.timeSeconds.size() >= 2
            ? (input.timeSeconds[1] - input.timeSeconds[0])
            : (1.0 / static_cast<double>(std::max(sampleRate, 1)));
    const double startTimeSeconds = input.timeSeconds.empty() ? 0.0 : input.timeSeconds.front();
    output.timeSeconds.reserve(output.left.size());
    for (size_t index = 0; index < output.left.size(); ++index) {
        output.timeSeconds.push_back(startTimeSeconds + (static_cast<double>(index) * sampleStepSeconds));
    }
    return output;
}

double computeDelayMismatchMs(const TimeDomainPair& directImpulse, int sampleRate) {
    if (!directImpulse.valid() || sampleRate <= 0) {
        return 0.0;
    }

    const int maxLagSamples = std::max(1, sampleRate / 500);
    double bestMagnitude = -1.0;
    int bestLagSamples = 0;
    for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag) {
        double numerator = 0.0;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        for (size_t index = 0; index < directImpulse.left.size(); ++index) {
            const int shiftedIndex = static_cast<int>(index) + lag;
            if (shiftedIndex < 0 || shiftedIndex >= static_cast<int>(directImpulse.right.size())) {
                continue;
            }
            const double leftValue = directImpulse.left[index];
            const double rightValue = directImpulse.right[static_cast<size_t>(shiftedIndex)];
            numerator += leftValue * rightValue;
            leftEnergy += leftValue * leftValue;
            rightEnergy += rightValue * rightValue;
        }
        if (leftEnergy <= 1.0e-12 || rightEnergy <= 1.0e-12) {
            continue;
        }
        const double correlation = numerator / std::sqrt(leftEnergy * rightEnergy);
        const double magnitude = std::abs(correlation);
        if (magnitude > bestMagnitude) {
            bestMagnitude = magnitude;
            bestLagSamples = lag;
        }
    }

    return -static_cast<double>(bestLagSamples) * 1000.0 / static_cast<double>(sampleRate);
}

StereoDiagnosticsResult buildFilteredDiagnostics(const MeasurementResult& measurement,
                                                 const FilterDesignResult& filterResult,
                                                 std::string_view window,
                                                 const FilterAnalysisProgressCallback& progressCallback) {
    if (progressCallback) {
        progressCallback(window, "Preparing data");
    }

    const ComplexSpectrumPair measuredSpectrum = loadSpectrumPair(measurement, window);
    const ComplexSpectrumPair correctedSpectrum = applyFilterToSpectrum(measuredSpectrum, filterResult);
    if (!correctedSpectrum.valid()) {
        return {};
    }

    const int sampleRate = std::max(filterResult.sampleRate, std::max(measurement.analysis.sampleRate, 1));
    const TimeDomainPair measuredDirectImpulse = loadImpulsePair(measurement, "direct");
    const TimeDomainPair correctedDirectImpulse =
        applyFilterToImpulse(measuredDirectImpulse, filterResult, sampleRate);
    const TimeDomainPair measuredWindowImpulse = loadImpulsePair(measurement, window);
    const TimeDomainPair correctedWindowImpulse =
        applyFilterToImpulse(measuredWindowImpulse, filterResult, sampleRate);

    StereoDiagnosticsInput input;
    input.sampleRate = sampleRate;
    input.delayMismatchMs = computeDelayMismatchMs(correctedDirectImpulse, sampleRate);
    input.frequencyAxisHz = correctedSpectrum.frequencyAxisHz;
    input.leftSpectrum = correctedSpectrum.left;
    input.rightSpectrum = correctedSpectrum.right;
    input.directImpulseTimeSeconds = correctedDirectImpulse.timeSeconds;
    input.directImpulseLeft = correctedDirectImpulse.left;
    input.directImpulseRight = correctedDirectImpulse.right;
    input.windowImpulseTimeSeconds = correctedWindowImpulse.timeSeconds;
    input.windowImpulseLeft = correctedWindowImpulse.left;
    input.windowImpulseRight = correctedWindowImpulse.right;
    return buildStereoDiagnostics(input,
                                  window,
                                  [&](std::string_view metricLabel) {
                                      if (progressCallback) {
                                          progressCallback(window, metricLabel);
                                      }
                                  });
}

}  // namespace

FilterAnalysisResult buildFilterAnalysis(const MeasurementResult& measurement,
                                         const FilterDesignResult& filterResult,
                                         const FilterAnalysisProgressCallback& progressCallback) {
    FilterAnalysisResult result;
    if (!filterResult.valid) {
        return result;
    }

    result.room = buildFilteredDiagnostics(measurement, filterResult, "room", progressCallback);
    result.available = result.room.available;
    return result;
}

}  // namespace wolfie::measurement
