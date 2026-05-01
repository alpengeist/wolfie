#include "measurement/stereo_diagnostics.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include "measurement/dsp_utils.h"

namespace wolfie::measurement {

namespace {

constexpr double kRadiansToDegrees = 180.0 / std::numbers::pi_v<double>;
constexpr double kDegreesToRadians = std::numbers::pi_v<double> / 180.0;
constexpr double kMinPlotFrequencyHz = 20.0;
constexpr double kLowBandStartHz = 40.0;
constexpr double kLowBandEndHz = 120.0;
constexpr double kMidBandStartHz = 120.0;
constexpr double kMidBandEndHz = 300.0;
constexpr double kMagnitudeBandEndHz = 300.0;
constexpr double kSimilarityBandEndHz = 1000.0;
constexpr double kDelayFitMinFrequencyHz = 80.0;
constexpr double kDelayFitMaxFrequencyHz = 1500.0;
constexpr double kIaccMaxLagSeconds = 0.001;
constexpr double kIacc10EndSeconds = 0.010;
constexpr double kIacc20EndSeconds = 0.020;
constexpr double kIacc80EndSeconds = 0.080;

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double amplitudeToDb(double amplitude) {
    return 20.0 * std::log10(std::max(amplitude, 1.0e-12));
}

double dbToAmplitude(double db) {
    return std::pow(10.0, db / 20.0);
}

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

const MeasurementValueSet* findValidValueSet(const MeasurementResult& result, std::string_view key) {
    const MeasurementValueSet* valueSet = result.findValueSet(key);
    return valueSet != nullptr && valueSet->valid() ? valueSet : nullptr;
}

ComplexSpectrumPair loadSpectrumPair(const MeasurementResult& result,
                                     std::string_view window,
                                     bool referenceCompensated) {
    const std::string prefix = referenceCompensated
                                   ? "measurement.reference_compensated_" + std::string(window)
                                   : "measurement." + std::string(window);
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

TimeDomainPair loadImpulsePair(const MeasurementResult& result, std::string_view key) {
    TimeDomainPair pair;
    const MeasurementValueSet* impulse = findValidValueSet(result, key);
    if (impulse == nullptr) {
        return pair;
    }
    pair.timeSeconds = impulse->xValues;
    pair.left = impulse->leftValues;
    pair.right = impulse->rightValues;
    return pair;
}

double interpolateScalar(const std::vector<double>& xValues,
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
    if (std::abs(span) < 1.0e-9) {
        return yValues[lowerIndex];
    }

    const double blend = (x - xValues[lowerIndex]) / span;
    return yValues[lowerIndex] + ((yValues[upperIndex] - yValues[lowerIndex]) * blend);
}

std::vector<double> resampleLogFrequency(const std::vector<double>& sourceAxisHz,
                                         const std::vector<double>& sourceValues,
                                         const std::vector<double>& targetAxisHz) {
    std::vector<double> resampled;
    if (sourceAxisHz.empty() || sourceValues.size() != sourceAxisHz.size()) {
        return resampled;
    }

    resampled.reserve(targetAxisHz.size());
    for (const double frequencyHz : targetAxisHz) {
        const double clampedFrequencyHz = clampValue(frequencyHz, sourceAxisHz.front(), sourceAxisHz.back());
        resampled.push_back(interpolateScalar(sourceAxisHz, sourceValues, clampedFrequencyHz));
    }
    return resampled;
}

std::vector<double> buildPhaseDifferenceRadians(const ComplexSpectrumPair& pair) {
    std::vector<double> wrapped;
    wrapped.reserve(pair.frequencyAxisHz.size());
    for (size_t index = 0; index < pair.frequencyAxisHz.size(); ++index) {
        const std::complex<double> safeRight =
            std::abs(pair.right[index]) > 1.0e-12 ? pair.right[index] : std::complex<double>(1.0e-12, 0.0);
        wrapped.push_back(std::arg(pair.left[index] / safeRight));
    }
    return unwrapPhaseRadians(wrapped);
}

double estimateLinearPhaseSlope(const std::vector<double>& frequencyAxisHz,
                                const std::vector<double>& phaseRadians,
                                double minFrequencyHz,
                                double maxFrequencyHz) {
    if (frequencyAxisHz.size() != phaseRadians.size()) {
        return 0.0;
    }

    double sumFrequency = 0.0;
    double sumPhase = 0.0;
    size_t usedCount = 0;
    for (size_t index = 0; index < frequencyAxisHz.size(); ++index) {
        const double frequencyHz = frequencyAxisHz[index];
        const double phase = phaseRadians[index];
        if (!std::isfinite(frequencyHz) || !std::isfinite(phase) ||
            frequencyHz < minFrequencyHz || frequencyHz > maxFrequencyHz) {
            continue;
        }
        sumFrequency += frequencyHz;
        sumPhase += phase;
        ++usedCount;
    }
    if (usedCount < 2) {
        return 0.0;
    }

    const double meanFrequency = sumFrequency / static_cast<double>(usedCount);
    const double meanPhase = sumPhase / static_cast<double>(usedCount);
    double covariance = 0.0;
    double variance = 0.0;
    for (size_t index = 0; index < frequencyAxisHz.size(); ++index) {
        const double frequencyHz = frequencyAxisHz[index];
        const double phase = phaseRadians[index];
        if (!std::isfinite(frequencyHz) || !std::isfinite(phase) ||
            frequencyHz < minFrequencyHz || frequencyHz > maxFrequencyHz) {
            continue;
        }
        const double centeredFrequency = frequencyHz - meanFrequency;
        covariance += centeredFrequency * (phase - meanPhase);
        variance += centeredFrequency * centeredFrequency;
    }
    if (variance <= 1.0e-12) {
        return 0.0;
    }
    return covariance / variance;
}

std::vector<double> removeLinearPhaseSlope(const std::vector<double>& frequencyAxisHz,
                                           const std::vector<double>& phaseRadians) {
    std::vector<double> corrected = phaseRadians;
    const double slope = estimateLinearPhaseSlope(frequencyAxisHz,
                                                  phaseRadians,
                                                  kDelayFitMinFrequencyHz,
                                                  kDelayFitMaxFrequencyHz);
    for (size_t index = 0; index < corrected.size(); ++index) {
        corrected[index] -= slope * frequencyAxisHz[index];
    }
    return corrected;
}

std::vector<double> buildMagnitudeDifferenceDb(const ComplexSpectrumPair& pair) {
    std::vector<double> difference;
    difference.reserve(pair.frequencyAxisHz.size());
    for (size_t index = 0; index < pair.frequencyAxisHz.size(); ++index) {
        difference.push_back(amplitudeToDb(std::abs(pair.left[index])) - amplitudeToDb(std::abs(pair.right[index])));
    }
    return difference;
}

double computeBandRms(const std::vector<double>& frequencyAxisHz,
                      const std::vector<double>& values,
                      double minFrequencyHz,
                      double maxFrequencyHz) {
    if (frequencyAxisHz.size() != values.size()) {
        return 0.0;
    }

    double sumSquares = 0.0;
    size_t usedCount = 0;
    for (size_t index = 0; index < frequencyAxisHz.size(); ++index) {
        const double frequencyHz = frequencyAxisHz[index];
        const double value = values[index];
        if (!std::isfinite(frequencyHz) || !std::isfinite(value) ||
            frequencyHz < minFrequencyHz || frequencyHz > maxFrequencyHz) {
            continue;
        }
        sumSquares += value * value;
        ++usedCount;
    }
    if (usedCount == 0) {
        return 0.0;
    }
    return std::sqrt(sumSquares / static_cast<double>(usedCount));
}

double computePhaseSimilarity(const std::vector<double>& frequencyAxisHz,
                              const std::vector<double>& phaseRadians,
                              double minFrequencyHz,
                              double maxFrequencyHz) {
    if (frequencyAxisHz.size() != phaseRadians.size()) {
        return 0.0;
    }

    std::complex<double> phaseSum(0.0, 0.0);
    size_t usedCount = 0;
    for (size_t index = 0; index < frequencyAxisHz.size(); ++index) {
        const double frequencyHz = frequencyAxisHz[index];
        const double phase = phaseRadians[index];
        if (!std::isfinite(frequencyHz) || !std::isfinite(phase) ||
            frequencyHz < minFrequencyHz || frequencyHz > maxFrequencyHz) {
            continue;
        }
        phaseSum += std::complex<double>(std::cos(phase), std::sin(phase));
        ++usedCount;
    }
    if (usedCount == 0) {
        return 0.0;
    }
    return std::abs(phaseSum) / static_cast<double>(usedCount);
}

double computeImpulseCorrelation(const TimeDomainPair& impulse) {
    if (!impulse.valid()) {
        return 0.0;
    }

    const std::vector<double>& left = impulse.left;
    const std::vector<double>& right = impulse.right;
    double sampleStepSeconds = 0.0;
    if (impulse.timeSeconds.size() >= 2) {
        sampleStepSeconds = impulse.timeSeconds[1] - impulse.timeSeconds[0];
    }
    const int sampleRate = sampleStepSeconds > 1.0e-12
                               ? std::max(1, static_cast<int>(std::lround(1.0 / sampleStepSeconds)))
                               : 1;
    const int maxLagSamples = std::max(1, sampleRate / 500);
    double bestCorrelation = 0.0;
    double bestMagnitude = -1.0;
    for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag) {
        double numerator = 0.0;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        for (size_t index = 0; index < left.size(); ++index) {
            const int shiftedIndex = static_cast<int>(index) + lag;
            if (shiftedIndex < 0 || shiftedIndex >= static_cast<int>(right.size())) {
                continue;
            }
            const double leftValue = left[index];
            const double rightValue = right[static_cast<size_t>(shiftedIndex)];
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
            bestCorrelation = correlation;
        }
    }
    return bestCorrelation;
}

double computeIaccWindow(const TimeDomainPair& impulse,
                         int sampleRate,
                         double startTimeSeconds,
                         double endTimeSeconds) {
    if (sampleRate <= 0 || !impulse.valid()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    std::vector<double> leftWindow;
    std::vector<double> rightWindow;
    leftWindow.reserve(impulse.timeSeconds.size());
    rightWindow.reserve(impulse.timeSeconds.size());
    for (size_t index = 0; index < impulse.timeSeconds.size(); ++index) {
        const double timeSeconds = impulse.timeSeconds[index];
        if (!std::isfinite(timeSeconds) || timeSeconds < startTimeSeconds || timeSeconds >= endTimeSeconds) {
            continue;
        }
        leftWindow.push_back(impulse.left[index]);
        rightWindow.push_back(impulse.right[index]);
    }

    if (leftWindow.size() < 4) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const int maxLagSamples = std::max(1, static_cast<int>(std::lround(kIaccMaxLagSeconds * sampleRate)));
    double bestMagnitude = -1.0;
    for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag) {
        double numerator = 0.0;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        for (size_t index = 0; index < leftWindow.size(); ++index) {
            const int shiftedIndex = static_cast<int>(index) + lag;
            if (shiftedIndex < 0 || shiftedIndex >= static_cast<int>(rightWindow.size())) {
                continue;
            }
            const double leftValue = leftWindow[index];
            const double rightValue = rightWindow[static_cast<size_t>(shiftedIndex)];
            numerator += leftValue * rightValue;
            leftEnergy += leftValue * leftValue;
            rightEnergy += rightValue * rightValue;
        }
        if (leftEnergy <= 1.0e-12 || rightEnergy <= 1.0e-12) {
            continue;
        }
        const double magnitude = std::abs(numerator / std::sqrt(leftEnergy * rightEnergy));
        bestMagnitude = std::max(bestMagnitude, magnitude);
    }

    return bestMagnitude >= 0.0 ? bestMagnitude : std::numeric_limits<double>::quiet_NaN();
}

StereoDiagnosticsResult buildResultFromData(std::string_view window,
                                            const ComplexSpectrumPair& pair,
                                            double delayMismatchMs,
                                            const TimeDomainPair& directImpulse,
                                            const TimeDomainPair& windowImpulse,
                                            int sampleRate,
                                            const StereoDiagnosticsProgressCallback& progressCallback) {
    StereoDiagnosticsResult diagnostics;
    diagnostics.available = pair.valid();
    diagnostics.window = std::string(window);
    if (progressCallback) {
        progressCallback("Direct L-R Delay");
    }
    diagnostics.summary.delayMismatchMs = delayMismatchMs;
    if (progressCallback) {
        progressCallback("Direct Impulse Corr");
    }
    diagnostics.summary.directImpulseCorrelation = computeImpulseCorrelation(directImpulse);
    if (!pair.valid()) {
        return diagnostics;
    }

    const std::vector<double> phaseDifferenceRadians = buildPhaseDifferenceRadians(pair);
    const std::vector<double> correctedPhaseDifferenceRadians =
        removeLinearPhaseSlope(pair.frequencyAxisHz, phaseDifferenceRadians);
    std::vector<double> correctedPhaseDifferenceDegrees;
    correctedPhaseDifferenceDegrees.reserve(correctedPhaseDifferenceRadians.size());
    for (const double radians : correctedPhaseDifferenceRadians) {
        correctedPhaseDifferenceDegrees.push_back(radians * kRadiansToDegrees);
    }
    const std::vector<double> magnitudeDifferenceDb = buildMagnitudeDifferenceDb(pair);

    const double maxFrequencyHz = pair.frequencyAxisHz.back();
    const size_t pointCount = 512;
    diagnostics.frequencyAxisHz = buildLogFrequencyAxis(kMinPlotFrequencyHz,
                                                        std::max(kMinPlotFrequencyHz + 1.0, maxFrequencyHz),
                                                        pointCount);
    diagnostics.phaseDeltaDegrees =
        resampleLogFrequency(pair.frequencyAxisHz, correctedPhaseDifferenceDegrees, diagnostics.frequencyAxisHz);
    diagnostics.magnitudeDeltaDb =
        resampleLogFrequency(pair.frequencyAxisHz, magnitudeDifferenceDb, diagnostics.frequencyAxisHz);

    diagnostics.summary.available = true;
    if (progressCallback) {
        progressCallback("Phase RMS 40-120");
    }
    diagnostics.summary.lowBandPhaseRmsDegrees =
        computeBandRms(pair.frequencyAxisHz, correctedPhaseDifferenceDegrees, kLowBandStartHz, kLowBandEndHz);
    if (progressCallback) {
        progressCallback("Phase RMS 120-300");
    }
    diagnostics.summary.midBandPhaseRmsDegrees =
        computeBandRms(pair.frequencyAxisHz, correctedPhaseDifferenceDegrees, kMidBandStartHz, kMidBandEndHz);
    if (progressCallback) {
        progressCallback("Mag RMS 40-300");
    }
    diagnostics.summary.lowBandMagnitudeRmsDb =
        computeBandRms(pair.frequencyAxisHz, magnitudeDifferenceDb, kLowBandStartHz, kMagnitudeBandEndHz);
    if (progressCallback) {
        progressCallback("Phase Similarity");
    }
    diagnostics.summary.phaseSimilarity =
        computePhaseSimilarity(pair.frequencyAxisHz,
                               correctedPhaseDifferenceRadians,
                               kLowBandStartHz,
                               kSimilarityBandEndHz);

    if (windowImpulse.valid()) {
        const int safeSampleRate = std::max(sampleRate, 1);
        if (progressCallback) {
            progressCallback("IACC10");
        }
        diagnostics.summary.iacc10 = computeIaccWindow(windowImpulse, safeSampleRate, 0.0, kIacc10EndSeconds);
        if (progressCallback) {
            progressCallback("IACC20");
        }
        diagnostics.summary.iacc20 = computeIaccWindow(windowImpulse, safeSampleRate, 0.0, kIacc20EndSeconds);
        if (progressCallback) {
            progressCallback("IACC80");
        }
        diagnostics.summary.iacc80 = computeIaccWindow(windowImpulse, safeSampleRate, 0.0, kIacc80EndSeconds);
        if (progressCallback) {
            progressCallback("IACC Late");
        }
        diagnostics.summary.iaccLate =
            computeIaccWindow(windowImpulse,
                              safeSampleRate,
                              kIacc80EndSeconds,
                              std::numeric_limits<double>::infinity());
    } else {
        diagnostics.summary.iacc10 = std::numeric_limits<double>::quiet_NaN();
        diagnostics.summary.iacc20 = std::numeric_limits<double>::quiet_NaN();
        diagnostics.summary.iacc80 = std::numeric_limits<double>::quiet_NaN();
        diagnostics.summary.iaccLate = std::numeric_limits<double>::quiet_NaN();
    }
    return diagnostics;
}

}  // namespace

StereoDiagnosticsResult buildStereoDiagnostics(const MeasurementResult& result,
                                               std::string_view window,
                                               const StereoDiagnosticsProgressCallback& progressCallback) {
    const ComplexSpectrumPair rawPair = loadSpectrumPair(result, window, false);
    const TimeDomainPair directImpulse = loadImpulsePair(result, "measurement.direct_impulse_response");
    const TimeDomainPair windowImpulse =
        loadImpulsePair(result, "measurement." + std::string(window) + "_impulse_response");
    return buildResultFromData(window,
                               rawPair,
                               (result.analysis.left.onsetTimeSeconds - result.analysis.right.onsetTimeSeconds) * 1000.0,
                               directImpulse,
                               windowImpulse,
                               result.analysis.sampleRate,
                               progressCallback);
}

StereoDiagnosticsResult buildStereoDiagnostics(const StereoDiagnosticsInput& input,
                                               std::string_view window,
                                               const StereoDiagnosticsProgressCallback& progressCallback) {
    ComplexSpectrumPair pair;
    pair.frequencyAxisHz = input.frequencyAxisHz;
    pair.left = input.leftSpectrum;
    pair.right = input.rightSpectrum;

    TimeDomainPair directImpulse;
    directImpulse.timeSeconds = input.directImpulseTimeSeconds;
    directImpulse.left = input.directImpulseLeft;
    directImpulse.right = input.directImpulseRight;

    TimeDomainPair windowImpulse;
    windowImpulse.timeSeconds = input.windowImpulseTimeSeconds;
    windowImpulse.left = input.windowImpulseLeft;
    windowImpulse.right = input.windowImpulseRight;

    return buildResultFromData(window,
                               pair,
                               input.delayMismatchMs,
                               directImpulse,
                               windowImpulse,
                               input.sampleRate,
                               progressCallback);
}

}  // namespace wolfie::measurement
