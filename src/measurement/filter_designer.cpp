#include "measurement/filter_designer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

#include "measurement/dsp_utils.h"
#include "measurement/target_curve_designer.h"

namespace wolfie::measurement {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double interpolateLinear(double x, double x0, double y0, double x1, double y1) {
    if (std::abs(x1 - x0) < 1.0e-12) {
        return y1;
    }
    const double t = clampValue((x - x0) / (x1 - x0), 0.0, 1.0);
    return y0 + ((y1 - y0) * t);
}

double interpolateLogFrequency(const std::vector<double>& frequencyAxisHz,
                               const std::vector<double>& values,
                               double frequencyHz) {
    if (frequencyAxisHz.empty() || values.empty()) {
        return 0.0;
    }

    const size_t count = std::min(frequencyAxisHz.size(), values.size());
    if (count == 1 || frequencyHz <= frequencyAxisHz.front()) {
        return values.front();
    }
    if (frequencyHz >= frequencyAxisHz[count - 1]) {
        return values[count - 1];
    }

    const auto it = std::lower_bound(frequencyAxisHz.begin(), frequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count), frequencyHz);
    const size_t upperIndex = static_cast<size_t>(std::distance(frequencyAxisHz.begin(), it));
    if (upperIndex == 0) {
        return values.front();
    }
    const size_t lowerIndex = upperIndex - 1;
    const double x0 = std::log10(std::max(frequencyAxisHz[lowerIndex], 1.0));
    const double x1 = std::log10(std::max(frequencyAxisHz[upperIndex], 1.0));
    const double x = std::log10(std::max(frequencyHz, 1.0));
    return interpolateLinear(x, x0, values[lowerIndex], x1, values[upperIndex]);
}

std::vector<double> resampleLogFrequency(const std::vector<double>& sourceAxisHz,
                                         const std::vector<double>& sourceValues,
                                         const std::vector<double>& destinationAxisHz) {
    std::vector<double> values;
    values.reserve(destinationAxisHz.size());
    for (const double frequencyHz : destinationAxisHz) {
        values.push_back(interpolateLogFrequency(sourceAxisHz, sourceValues, frequencyHz));
    }
    return values;
}

double smoothRamp(double t) {
    const double clamped = clampValue(t, 0.0, 1.0);
    return 0.5 - (0.5 * std::cos(clamped * std::numbers::pi));
}

double correctionWeightAt(double frequencyHz, const FilterDesignSettings& settings) {
    double weight = 1.0;

    const double lowCorrectionHz = std::max(settings.lowCorrectionHz, 1.0);
    const double lowStartHz = lowCorrectionHz / std::pow(2.0, std::max(settings.lowTaperOctaves, 0.0));
    if (frequencyHz <= lowStartHz) {
        weight = 0.0;
    } else if (frequencyHz < lowCorrectionHz) {
        weight *= smoothRamp((frequencyHz - lowStartHz) / std::max(lowCorrectionHz - lowStartHz, 1.0e-9));
    }

    const double highCorrectionHz = std::max(settings.highCorrectionHz, lowCorrectionHz + 1.0);
    const double highEndHz = highCorrectionHz * std::pow(2.0, std::max(settings.highTaperOctaves, 0.0));
    if (frequencyHz >= highEndHz) {
        weight = 0.0;
    } else if (frequencyHz > highCorrectionHz) {
        weight *= 1.0 - smoothRamp((frequencyHz - highCorrectionHz) / std::max(highEndHz - highCorrectionHz, 1.0e-9));
    }

    return clampValue(weight, 0.0, 1.0);
}

double applyAsymmetricSoftLimit(double valueDb, double minValueDb, double maxValueDb) {
    if (valueDb >= 0.0) {
        if (maxValueDb <= 1.0e-9) {
            return 0.0;
        }
        return maxValueDb * std::tanh(valueDb / maxValueDb);
    }

    const double cutLimitDb = std::max(-minValueDb, 0.0);
    if (cutLimitDb <= 1.0e-9) {
        return 0.0;
    }
    return -cutLimitDb * std::tanh((-valueDb) / cutLimitDb);
}

double smoothnessRegularizationScale(double smoothness) {
    const double clamped = clampValue(smoothness, 0.1, 4.0);
    return std::pow(8.0, clamped - 1.0);
}

double dotProduct(const std::vector<double>& a, const std::vector<double>& b) {
    double sum = 0.0;
    for (size_t index = 0; index < a.size() && index < b.size(); ++index) {
        sum += a[index] * b[index];
    }
    return sum;
}

void applySecondDifferencePenalty(const std::vector<double>& input, std::vector<double>& output, double scale) {
    if (input.size() < 3 || scale <= 0.0) {
        return;
    }

    for (size_t index = 0; index + 2 < input.size(); ++index) {
        const double secondDifference = input[index] - (2.0 * input[index + 1]) + input[index + 2];
        output[index] += scale * secondDifference;
        output[index + 1] -= 2.0 * scale * secondDifference;
        output[index + 2] += scale * secondDifference;
    }
}

std::vector<double> applyRegularizedSystem(const std::vector<double>& trackingWeights,
                                           const std::vector<double>& values,
                                           double regularization) {
    std::vector<double> result(values.size(), 0.0);
    for (size_t index = 0; index < values.size() && index < trackingWeights.size(); ++index) {
        result[index] = trackingWeights[index] * values[index];
    }
    applySecondDifferencePenalty(values, result, regularization);
    return result;
}

std::vector<double> solveRegularizedCurve(const std::vector<double>& desiredCurveDb,
                                          const std::vector<double>& trackingWeights,
                                          double regularization) {
    const size_t count = std::min(desiredCurveDb.size(), trackingWeights.size());
    std::vector<double> solution(desiredCurveDb.begin(), desiredCurveDb.begin() + static_cast<std::ptrdiff_t>(count));
    if (count < 3) {
        return solution;
    }

    std::vector<double> rhs(count, 0.0);
    for (size_t index = 0; index < count; ++index) {
        rhs[index] = trackingWeights[index] * desiredCurveDb[index];
    }

    std::vector<double> applied = applyRegularizedSystem(trackingWeights, solution, regularization);
    std::vector<double> residual(count, 0.0);
    for (size_t index = 0; index < count; ++index) {
        residual[index] = rhs[index] - applied[index];
    }

    std::vector<double> direction = residual;
    double residualEnergy = dotProduct(residual, residual);
    const double rhsEnergy = std::max(dotProduct(rhs, rhs), 1.0e-12);
    if (residualEnergy <= rhsEnergy * 1.0e-12) {
        return solution;
    }

    for (int iteration = 0; iteration < 96; ++iteration) {
        const std::vector<double> systemDirection = applyRegularizedSystem(trackingWeights, direction, regularization);
        const double denominator = dotProduct(direction, systemDirection);
        if (std::abs(denominator) < 1.0e-12) {
            break;
        }

        const double alpha = residualEnergy / denominator;
        for (size_t index = 0; index < count; ++index) {
            solution[index] += alpha * direction[index];
            residual[index] -= alpha * systemDirection[index];
        }

        const double nextResidualEnergy = dotProduct(residual, residual);
        if (nextResidualEnergy <= rhsEnergy * 1.0e-10) {
            break;
        }

        const double beta = nextResidualEnergy / std::max(residualEnergy, 1.0e-12);
        for (size_t index = 0; index < count; ++index) {
            direction[index] = residual[index] + (beta * direction[index]);
        }
        residualEnergy = nextResidualEnergy;
    }

    return solution;
}

std::vector<double> buildCorrectionCurve(const std::vector<double>& frequencyAxisHz,
                                         const std::vector<double>& targetCurveDb,
                                         const std::vector<double>& sourceCurveDb,
                                         const FilterDesignSettings& settings) {
    const size_t count = std::min({frequencyAxisHz.size(), targetCurveDb.size(), sourceCurveDb.size()});
    if (count == 0) {
        return {};
    }

    std::vector<double> fullCorrectionDb;
    fullCorrectionDb.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const double rawCorrectionDb = targetCurveDb[index] - sourceCurveDb[index];
        fullCorrectionDb.push_back(applyAsymmetricSoftLimit(rawCorrectionDb, -settings.maxCutDb, settings.maxBoostDb));
    }

    const double lowCorrectionHz = std::max(settings.lowCorrectionHz, 1.0);
    const double lowEdgeCorrectionDb = interpolateLogFrequency(frequencyAxisHz, fullCorrectionDb, lowCorrectionHz);

    std::vector<double> desiredCorrectionDb;
    desiredCorrectionDb.reserve(count);
    std::vector<double> trackingWeights;
    trackingWeights.reserve(count);
    std::vector<double> lowerBoundsDb;
    lowerBoundsDb.reserve(count);
    std::vector<double> upperBoundsDb;
    upperBoundsDb.reserve(count);

    for (size_t index = 0; index < count; ++index) {
        const double frequencyHz = frequencyAxisHz[index];
        const double weight = correctionWeightAt(frequencyHz, settings);
        const double rawCorrectionDb = frequencyHz < lowCorrectionHz ? lowEdgeCorrectionDb : fullCorrectionDb[index];
        const double minValueDb = -settings.maxCutDb * weight;
        const double maxValueDb = settings.maxBoostDb * weight;
        desiredCorrectionDb.push_back(applyAsymmetricSoftLimit(rawCorrectionDb, minValueDb, maxValueDb));
        trackingWeights.push_back(0.05 + (0.95 * weight));
        lowerBoundsDb.push_back(minValueDb);
        upperBoundsDb.push_back(maxValueDb);
    }

    const double regularization = 12.0 *
                                  std::max(static_cast<double>(count) / 512.0, 1.0) *
                                  smoothnessRegularizationScale(settings.smoothness);
    std::vector<double> correction = solveRegularizedCurve(desiredCorrectionDb, trackingWeights, regularization);
    for (size_t index = 0; index < correction.size() && index < lowerBoundsDb.size() && index < upperBoundsDb.size(); ++index) {
        correction[index] = applyAsymmetricSoftLimit(correction[index], lowerBoundsDb[index], upperBoundsDb[index]);
    }
    return correction;
}

std::vector<double> alignTargetCurveLevel(const std::vector<double>& frequencyAxisHz,
                                          const std::vector<double>& targetCurveDb,
                                          const std::vector<double>& leftSourceDb,
                                          const std::vector<double>& rightSourceDb,
                                          const FilterDesignSettings& settings) {
    const size_t count = std::min({frequencyAxisHz.size(), targetCurveDb.size(), leftSourceDb.size(), rightSourceDb.size()});
    std::vector<double> alignedTargetDb(targetCurveDb.begin(), targetCurveDb.begin() + static_cast<std::ptrdiff_t>(count));
    if (count == 0) {
        return alignedTargetDb;
    }

    double weightedOffsetSum = 0.0;
    double totalWeight = 0.0;
    for (size_t index = 0; index < count; ++index) {
        const double weight = correctionWeightAt(frequencyAxisHz[index], settings);
        if (weight <= 1.0e-9) {
            continue;
        }

        const double sourceMeanDb = (leftSourceDb[index] + rightSourceDb[index]) * 0.5;
        weightedOffsetSum += (sourceMeanDb - targetCurveDb[index]) * weight;
        totalWeight += weight;
    }

    const double levelOffsetDb = totalWeight > 1.0e-9 ? weightedOffsetSum / totalWeight : 0.0;
    for (double& valueDb : alignedTargetDb) {
        valueDb += levelOffsetDb;
    }
    return alignedTargetDb;
}

std::vector<double> buildPositiveMagnitudeResponse(int sampleRate,
                                                   int fftSize,
                                                   const std::vector<double>& frequencyAxisHz,
                                                   const std::vector<double>& correctionDb) {
    const size_t positiveBinCount = static_cast<size_t>(std::max(fftSize / 2, 0) + 1);
    std::vector<double> magnitude;
    magnitude.reserve(positiveBinCount);
    for (size_t bin = 0; bin < positiveBinCount; ++bin) {
        const double frequencyHz = static_cast<double>(sampleRate) * static_cast<double>(bin) / static_cast<double>(std::max(fftSize, 1));
        const double correctionAtBinDb = frequencyHz <= 0.0 ? 0.0 : interpolateLogFrequency(frequencyAxisHz, correctionDb, frequencyHz);
        magnitude.push_back(std::pow(10.0, correctionAtBinDb / 20.0));
    }
    return magnitude;
}

std::vector<double> buildMinimumPhaseImpulse(const std::vector<double>& positiveMagnitude,
                                             int fftSize,
                                             int tapCount) {
    const size_t spectrumSize = static_cast<size_t>(std::max(fftSize, 1));
    std::vector<std::complex<double>> logSpectrum(spectrumSize, {0.0, 0.0});
    const size_t positiveCount = std::min(positiveMagnitude.size(), (spectrumSize / 2) + 1);
    for (size_t index = 0; index < positiveCount; ++index) {
        const double logMagnitude = std::log(std::max(positiveMagnitude[index], 1.0e-9));
        logSpectrum[index] = {logMagnitude, 0.0};
    }
    for (size_t index = 1; index + 1 < positiveCount; ++index) {
        logSpectrum[spectrumSize - index] = logSpectrum[index];
    }

    std::vector<std::complex<double>> cepstrum = logSpectrum;
    fft(cepstrum, true);

    std::vector<std::complex<double>> minimumCepstrum(spectrumSize, {0.0, 0.0});
    if (!cepstrum.empty()) {
        minimumCepstrum[0] = cepstrum[0];
    }
    const size_t half = spectrumSize / 2;
    for (size_t index = 1; index < half; ++index) {
        minimumCepstrum[index] = cepstrum[index] * 2.0;
    }
    if ((spectrumSize % 2) == 0 && half < cepstrum.size()) {
        minimumCepstrum[half] = cepstrum[half];
    }

    std::vector<std::complex<double>> minimumLogSpectrum = minimumCepstrum;
    fft(minimumLogSpectrum, false);
    for (auto& value : minimumLogSpectrum) {
        value = std::exp(value);
    }

    fft(minimumLogSpectrum, true);
    std::vector<double> impulse(static_cast<size_t>(std::max(tapCount, 0)), 0.0);
    for (size_t index = 0; index < impulse.size() && index < minimumLogSpectrum.size(); ++index) {
        impulse[index] = minimumLogSpectrum[index].real();
    }

    const size_t fadeCount = std::min(impulse.size() / 8, size_t{4096});
    if (fadeCount >= 64) {
        const size_t fadeStart = impulse.size() - fadeCount;
        for (size_t index = fadeStart; index < impulse.size(); ++index) {
            const double t = static_cast<double>(index - fadeStart) / static_cast<double>(std::max<size_t>(fadeCount - 1, 1));
            impulse[index] *= std::cos(t * std::numbers::pi * 0.5);
        }
    }
    return impulse;
}

std::vector<std::complex<double>> spectrumOfRealSignal(const std::vector<double>& samples, int fftSize) {
    std::vector<std::complex<double>> spectrum(static_cast<size_t>(std::max(fftSize, 1)), {0.0, 0.0});
    for (size_t index = 0; index < samples.size() && index < spectrum.size(); ++index) {
        spectrum[index] = {samples[index], 0.0};
    }
    fft(spectrum, false);
    return spectrum;
}

double magnitudeDb(const std::complex<double>& value) {
    return 20.0 * std::log10(std::max(std::abs(value), 1.0e-9));
}

std::vector<double> buildMagnitudeDbSeries(const std::vector<std::complex<double>>& spectrum) {
    const size_t positiveBinCount = (spectrum.size() / 2) + 1;
    std::vector<double> values;
    values.reserve(positiveBinCount);
    for (size_t index = 0; index < positiveBinCount; ++index) {
        values.push_back(magnitudeDb(spectrum[index]));
    }
    return values;
}

std::vector<double> buildPhaseSeries(const std::vector<std::complex<double>>& spectrum) {
    const size_t positiveBinCount = (spectrum.size() / 2) + 1;
    std::vector<double> phase;
    phase.reserve(positiveBinCount);
    for (size_t index = 0; index < positiveBinCount; ++index) {
        phase.push_back(std::arg(spectrum[index]));
    }
    return unwrapPhaseRadians(phase);
}

std::vector<double> buildGroupDelayMs(const std::vector<double>& frequencyAxisHz,
                                      const std::vector<double>& unwrappedPhaseRadians) {
    std::vector<double> groupDelayMs(frequencyAxisHz.size(), 0.0);
    if (frequencyAxisHz.size() < 3 || unwrappedPhaseRadians.size() < 3) {
        return groupDelayMs;
    }

    const size_t count = std::min(frequencyAxisHz.size(), unwrappedPhaseRadians.size());
    for (size_t index = 1; index + 1 < count; ++index) {
        const double phaseDelta = unwrappedPhaseRadians[index + 1] - unwrappedPhaseRadians[index - 1];
        const double frequencyDelta = frequencyAxisHz[index + 1] - frequencyAxisHz[index - 1];
        if (std::abs(frequencyDelta) < 1.0e-12) {
            continue;
        }
        groupDelayMs[index] = (-phaseDelta / (2.0 * std::numbers::pi * frequencyDelta)) * 1000.0;
    }
    if (count >= 2) {
        groupDelayMs.front() = groupDelayMs[1];
        groupDelayMs[count - 1] = groupDelayMs[count - 2];
    }
    return groupDelayMs;
}

size_t maxAbsIndex(const std::vector<double>& values) {
    size_t bestIndex = 0;
    double bestValue = 0.0;
    for (size_t index = 0; index < values.size(); ++index) {
        const double magnitude = std::abs(values[index]);
        if (magnitude > bestValue) {
            bestValue = magnitude;
            bestIndex = index;
        }
    }
    return bestIndex;
}

double maxAbsSample(const std::vector<double>& values) {
    double peak = 0.0;
    for (const double value : values) {
        peak = std::max(peak, std::abs(value));
    }
    return peak;
}

std::vector<double> buildImpulseTimeAxisMs(size_t sampleCount, size_t peakIndex, int sampleRate) {
    std::vector<double> axis;
    axis.reserve(sampleCount);
    const double sampleRateValue = static_cast<double>(std::max(sampleRate, 1));
    for (size_t index = 0; index < sampleCount; ++index) {
        axis.push_back((static_cast<double>(index) - static_cast<double>(peakIndex)) * 1000.0 / sampleRateValue);
    }
    return axis;
}

struct DesignedChannel {
    std::vector<double> correctionDb;
    std::vector<double> filterResponseDb;
    std::vector<double> correctedResponseDb;
    std::vector<double> groupDelayMs;
    std::vector<double> impulseTimeMs;
    std::vector<double> filterTaps;
    int impulsePeakIndex = 0;
    double peakAmplitude = 0.0;
};

DesignedChannel designChannel(const std::vector<double>& displayFrequencyAxisHz,
                              const std::vector<double>& targetCurveDb,
                              const std::vector<double>& sourceCurveDb,
                              const FilterDesignSettings& settings,
                              int sampleRate,
                              int fftSize) {
    DesignedChannel channel;
    channel.correctionDb = buildCorrectionCurve(displayFrequencyAxisHz, targetCurveDb, sourceCurveDb, settings);
    const std::vector<double> positiveMagnitude =
        buildPositiveMagnitudeResponse(sampleRate, fftSize, displayFrequencyAxisHz, channel.correctionDb);
    channel.filterTaps = buildMinimumPhaseImpulse(positiveMagnitude, fftSize, settings.tapCount);
    channel.impulsePeakIndex = static_cast<int>(maxAbsIndex(channel.filterTaps));
    channel.peakAmplitude = maxAbsSample(channel.filterTaps);
    channel.impulseTimeMs = buildImpulseTimeAxisMs(channel.filterTaps.size(),
                                                   static_cast<size_t>(std::max(channel.impulsePeakIndex, 0)),
                                                   sampleRate);

    const std::vector<std::complex<double>> spectrum = spectrumOfRealSignal(channel.filterTaps, fftSize);
    const std::vector<double> binFrequencyAxisHz = buildLinearFrequencyAxis(sampleRate, fftSize);
    const std::vector<double> binMagnitudeDb = buildMagnitudeDbSeries(spectrum);
    const std::vector<double> binPhaseRadians = buildPhaseSeries(spectrum);

    channel.filterResponseDb = resampleLogFrequency(binFrequencyAxisHz, binMagnitudeDb, displayFrequencyAxisHz);
    channel.correctedResponseDb.reserve(displayFrequencyAxisHz.size());
    for (size_t index = 0; index < displayFrequencyAxisHz.size() &&
                           index < sourceCurveDb.size() &&
                           index < channel.filterResponseDb.size(); ++index) {
        channel.correctedResponseDb.push_back(sourceCurveDb[index] + channel.filterResponseDb[index]);
    }
    const std::vector<double> displayPhaseRadians =
        resampleLogFrequency(binFrequencyAxisHz, binPhaseRadians, displayFrequencyAxisHz);
    channel.groupDelayMs = buildGroupDelayMs(displayFrequencyAxisHz, displayPhaseRadians);
    return channel;
}

}  // namespace

void normalizeFilterDesignSettings(FilterDesignSettings& settings, int sampleRate) {
    const int safeSampleRate = std::max(sampleRate, 44100);
    const int nyquist = safeSampleRate / 2;

    const bool legacyEdgeDefaults =
        std::abs(settings.lowCorrectionHz - 20.0) < 0.001 &&
        std::abs(settings.lowTaperOctaves - 1.0) < 0.001 &&
        std::abs(settings.highCorrectionHz - 20000.0) < 0.001 &&
        std::abs(settings.highTaperOctaves - 0.75) < 0.001;
    if (legacyEdgeDefaults) {
        settings.lowCorrectionHz = 30.0;
        settings.lowTaperOctaves = 2.0;
        settings.highCorrectionHz = 12000.0;
        settings.highTaperOctaves = 1.25;
    }

    const int taps[] = {16384, 32768, 65536};
    int closestTapCount = taps[0];
    int closestDistance = std::abs(settings.tapCount - taps[0]);
    for (const int tapCount : taps) {
        const int distance = std::abs(settings.tapCount - tapCount);
        if (distance < closestDistance) {
            closestDistance = distance;
            closestTapCount = tapCount;
        }
    }
    settings.tapCount = closestTapCount;
    settings.maxBoostDb = clampValue(settings.maxBoostDb, 0.0, 24.0);
    settings.maxCutDb = clampValue(settings.maxCutDb, 0.0, 36.0);
    settings.smoothness = clampValue(settings.smoothness, 0.1, 4.0);
    settings.lowCorrectionHz = clampValue(settings.lowCorrectionHz, 10.0, static_cast<double>(std::max(nyquist - 10, 20)));
    settings.lowTaperOctaves = clampValue(settings.lowTaperOctaves, 0.0, 4.0);
    settings.highCorrectionHz =
        clampValue(settings.highCorrectionHz, settings.lowCorrectionHz + 10.0, static_cast<double>(std::max(nyquist - 1, 1000)));
    settings.highTaperOctaves = clampValue(settings.highTaperOctaves, 0.0, 4.0);
    settings.displayPointCount = clampValue(settings.displayPointCount, 256, 4096);
    settings.phaseMode = "minimum";
}

FilterDesignResult designFiltersForSampleRate(const SmoothedResponse& response,
                                              const MeasurementSettings& measurement,
                                              const TargetCurveSettings& targetCurve,
                                              const FilterDesignSettings& sourceSettings,
                                              int sampleRate) {
    FilterDesignResult result;
    if (response.frequencyAxisHz.empty() ||
        response.leftChannelDb.size() != response.frequencyAxisHz.size() ||
        response.rightChannelDb.size() != response.frequencyAxisHz.size()) {
        return result;
    }

    MeasurementSettings exportMeasurement = measurement;
    exportMeasurement.sampleRate = sampleRate;

    const TargetCurvePlotData targetPlot = buildTargetCurvePlotData(response, exportMeasurement, targetCurve, std::nullopt);
    if (targetPlot.frequencyAxisHz.empty() || targetPlot.targetCurveDb.empty()) {
        return result;
    }

    FilterDesignSettings settings = sourceSettings;
    normalizeFilterDesignSettings(settings, exportMeasurement.sampleRate);

    const std::vector<double> displayFrequencyAxisHz =
        buildLogFrequencyAxis(targetPlot.minFrequencyHz, targetPlot.maxFrequencyHz, settings.displayPointCount);
    const std::vector<double> rawTargetCurveDb =
        resampleLogFrequency(targetPlot.frequencyAxisHz, targetPlot.targetCurveDb, displayFrequencyAxisHz);
    const std::vector<double> leftSourceDb =
        resampleLogFrequency(response.frequencyAxisHz, response.leftChannelDb, displayFrequencyAxisHz);
    const std::vector<double> rightSourceDb =
        resampleLogFrequency(response.frequencyAxisHz, response.rightChannelDb, displayFrequencyAxisHz);
    const std::vector<double> targetCurveDb =
        alignTargetCurveLevel(displayFrequencyAxisHz, rawTargetCurveDb, leftSourceDb, rightSourceDb, settings);

    const int fftSize = static_cast<int>(nextPowerOfTwo(static_cast<size_t>(settings.tapCount * 4)));
    const DesignedChannel left = designChannel(displayFrequencyAxisHz,
                                               targetCurveDb,
                                               leftSourceDb,
                                               settings,
                                               exportMeasurement.sampleRate,
                                               fftSize);
    const DesignedChannel right = designChannel(displayFrequencyAxisHz,
                                                targetCurveDb,
                                                rightSourceDb,
                                                settings,
                                                exportMeasurement.sampleRate,
                                                fftSize);

    result.valid = true;
    result.sampleRate = exportMeasurement.sampleRate;
    result.tapCount = settings.tapCount;
    result.fftSize = fftSize;
    result.positiveBinCount = (fftSize / 2) + 1;
    result.phaseMode = settings.phaseMode;
    result.frequencyAxisHz = displayFrequencyAxisHz;
    result.targetCurveDb = targetCurveDb;
    result.left.correctionCurveDb = left.correctionDb;
    result.left.filterResponseDb = left.filterResponseDb;
    result.left.correctedResponseDb = left.correctedResponseDb;
    result.left.groupDelayMs = left.groupDelayMs;
    result.left.impulseTimeMs = left.impulseTimeMs;
    result.left.filterTaps = left.filterTaps;
    result.left.impulsePeakIndex = left.impulsePeakIndex;
    result.left.peakAmplitude = left.peakAmplitude;
    result.right.correctionCurveDb = right.correctionDb;
    result.right.filterResponseDb = right.filterResponseDb;
    result.right.correctedResponseDb = right.correctedResponseDb;
    result.right.groupDelayMs = right.groupDelayMs;
    result.right.impulseTimeMs = right.impulseTimeMs;
    result.right.filterTaps = right.filterTaps;
    result.right.impulsePeakIndex = right.impulsePeakIndex;
    result.right.peakAmplitude = right.peakAmplitude;
    return result;
}

FilterDesignResult designFilters(const SmoothedResponse& response,
                                 const MeasurementSettings& measurement,
                                 const TargetCurveSettings& targetCurve,
                                 const FilterDesignSettings& settings) {
    return designFiltersForSampleRate(response,
                                      measurement,
                                      targetCurve,
                                      settings,
                                      measurement.sampleRate);
}

}  // namespace wolfie::measurement
