#include "measurement/filter_designer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <numbers>
#include <sstream>
#include <string_view>
#include <vector>

#include "measurement/dsp_utils.h"
#include "measurement/phase_preparation.h"
#include "measurement/target_curve_designer.h"

namespace wolfie::measurement {

namespace {

constexpr double kRadiansToDegrees = 180.0 / std::numbers::pi_v<double>;
constexpr double kDegreesToRadians = std::numbers::pi_v<double> / 180.0;
enum class NormalizedPhaseMode {
    Minimum,
    ExcessLf,
    Mixed
};

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

NormalizedPhaseMode normalizePhaseMode(std::string& phaseMode) {
    std::string lowered = phaseMode;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lowered == "excess-lf" ||
        lowered == "excess_lf" ||
        lowered == "lf-excess" ||
        lowered == "lf_excess") {
        phaseMode = "excess-lf";
        return NormalizedPhaseMode::ExcessLf;
    }
    if (lowered == "mixed" ||
        lowered == "mixed-phase" ||
        lowered == "mixed_phase") {
        phaseMode = "mixed";
        return NormalizedPhaseMode::Mixed;
    }

    phaseMode = "minimum";
    return NormalizedPhaseMode::Minimum;
}

void appendProcessLog(std::vector<std::string>& processLog, std::string message) {
    processLog.push_back(std::move(message));
}

std::string formatMilliseconds(double seconds) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    stream << (seconds * 1000.0) << " ms";
    return stream.str();
}

std::string formatKilohertz(int sampleRate) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    stream << (static_cast<double>(sampleRate) / 1000.0) << " kHz";
    return stream.str();
}

std::string describePhasePreparationSource(const PreparedPhaseData& preparedPhase) {
    if (!preparedPhase.valid) {
        return "no matched phase-preparation source";
    }

    std::ostringstream stream;
    stream << preparedPhase.sourceKey;
    if (!preparedPhase.sourceSeriesKind.empty()) {
        stream << " (" << preparedPhase.sourceSeriesKind << ")";
    }
    if (!preparedPhase.phaseValueSetKey.empty()) {
        stream << " from " << preparedPhase.phaseValueSetKey;
    }
    return stream.str();
}

double lowCorrectionWeightAt(double frequencyHz, const FilterDesignSettings& settings) {
    const double lowCorrectionHz = std::max(settings.lowCorrectionHz, 1.0);
    const double lowStartHz =
        lowCorrectionHz / std::pow(2.0, std::max(settings.lowTaperOctaves, 0.0) * 0.5);
    if (frequencyHz <= lowStartHz) {
        return 0.0;
    }
    if (frequencyHz < lowCorrectionHz) {
        return smoothRamp((frequencyHz - lowStartHz) / std::max(lowCorrectionHz - lowStartHz, 1.0e-9));
    }
    return 1.0;
}

double correctionWeightAt(double frequencyHz, const FilterDesignSettings& settings) {
    double weight = lowCorrectionWeightAt(frequencyHz, settings);

    const double lowCorrectionHz = std::max(settings.lowCorrectionHz, 1.0);
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

double applyNoBoostSoftTarget(double valueDb) {
    if (valueDb <= 0.0) {
        return valueDb;
    }
    return 0.0;
}

double applyCorrectionLimit(double valueDb, double minValueDb, double maxValueDb) {
    const double cutLimitedDb = applyAsymmetricSoftLimit(std::min(valueDb, 0.0), minValueDb, 0.0);
    if (maxValueDb <= 1.0e-9) {
        if (minValueDb >= -1.0e-9) {
            return 0.0;
        }
        if (valueDb <= 0.0) {
            return applyNoBoostSoftTarget(cutLimitedDb);
        }
        return applyNoBoostSoftTarget(valueDb);
    }

    if (valueDb <= 0.0) {
        return cutLimitedDb;
    }

    return applyAsymmetricSoftLimit(valueDb, 0.0, maxValueDb);
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
        fullCorrectionDb.push_back(applyCorrectionLimit(rawCorrectionDb, -settings.maxCutDb, settings.maxBoostDb));
    }

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
        const double rawCorrectionDb = fullCorrectionDb[index];
        const double minValueDb = -settings.maxCutDb * weight;
        const double maxValueDb = settings.maxBoostDb * weight;
        desiredCorrectionDb.push_back(applyCorrectionLimit(rawCorrectionDb, minValueDb, maxValueDb));
        trackingWeights.push_back(0.05 + (0.95 * weight));
        lowerBoundsDb.push_back(minValueDb);
        upperBoundsDb.push_back(maxValueDb);
    }

    const double regularization = 12.0 *
                                  std::max(static_cast<double>(count) / 512.0, 1.0) *
                                  smoothnessRegularizationScale(settings.smoothness);
    std::vector<double> correction = solveRegularizedCurve(desiredCorrectionDb, trackingWeights, regularization);
    for (size_t index = 0; index < correction.size() && index < lowerBoundsDb.size() && index < upperBoundsDb.size(); ++index) {
        correction[index] = applyCorrectionLimit(correction[index], lowerBoundsDb[index], upperBoundsDb[index]);
    }
    return correction;
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

std::vector<std::complex<double>> buildMinimumPhaseSpectrum(const std::vector<double>& positiveMagnitude,
                                                            int fftSize) {
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
    return minimumLogSpectrum;
}

std::vector<double> buildMinimumPhaseImpulse(const std::vector<double>& positiveMagnitude,
                                             int fftSize,
                                             int tapCount) {
    std::vector<std::complex<double>> minimumSpectrum = buildMinimumPhaseSpectrum(positiveMagnitude, fftSize);
    fft(minimumSpectrum, true);
    std::vector<double> impulse(static_cast<size_t>(std::max(tapCount, 0)), 0.0);
    for (size_t index = 0; index < impulse.size() && index < minimumSpectrum.size(); ++index) {
        impulse[index] = minimumSpectrum[index].real();
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

std::vector<double> radiansToDegrees(const std::vector<double>& radians) {
    std::vector<double> degrees;
    degrees.reserve(radians.size());
    for (const double value : radians) {
        degrees.push_back(value * kRadiansToDegrees);
    }
    return degrees;
}

std::vector<double> degreesToRadians(const std::vector<double>& degrees) {
    std::vector<double> radians;
    radians.reserve(degrees.size());
    for (const double value : degrees) {
        radians.push_back(value * kDegreesToRadians);
    }
    return radians;
}

std::vector<double> addSeries(const std::vector<double>& left, const std::vector<double>& right) {
    const size_t count = std::min(left.size(), right.size());
    std::vector<double> result;
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        result.push_back(left[index] + right[index]);
    }
    return result;
}

std::vector<double> subtractConstant(const std::vector<double>& values, double offset) {
    std::vector<double> result = values;
    for (double& value : result) {
        value -= offset;
    }
    return result;
}

std::vector<double> averageSeries(const std::vector<double>& left, const std::vector<double>& right) {
    const size_t count = std::min(left.size(), right.size());
    std::vector<double> result;
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        result.push_back((left[index] + right[index]) * 0.5);
    }
    return result;
}

double meanAbsoluteBandValue(const std::vector<double>& frequencyAxisHz,
                             const std::vector<double>& values,
                             double minFrequencyHz,
                             double maxFrequencyHz) {
    const size_t count = std::min(frequencyAxisHz.size(), values.size());
    double sum = 0.0;
    size_t used = 0;
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index]) ||
            frequencyAxisHz[index] < minFrequencyHz ||
            frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        sum += std::abs(values[index]);
        ++used;
    }
    return used == 0 ? 0.0 : sum / static_cast<double>(used);
}

double meanAbsoluteBandDelta(const std::vector<double>& frequencyAxisHz,
                             const std::vector<double>& leftValues,
                             const std::vector<double>& rightValues,
                             double minFrequencyHz,
                             double maxFrequencyHz) {
    const size_t count = std::min({frequencyAxisHz.size(), leftValues.size(), rightValues.size()});
    double sum = 0.0;
    size_t used = 0;
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(leftValues[index]) ||
            !std::isfinite(rightValues[index]) ||
            frequencyAxisHz[index] < minFrequencyHz ||
            frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        sum += std::abs(leftValues[index] - rightValues[index]);
        ++used;
    }
    return used == 0 ? 0.0 : sum / static_cast<double>(used);
}

double excessPhaseCorrectionWeightAt(double frequencyHz,
                                     const FilterDesignSettings& settings,
                                     int sampleRate) {
    const double lowWeight = lowCorrectionWeightAt(frequencyHz, settings);
    if (lowWeight <= 0.0) {
        return 0.0;
    }

    const double nyquist = static_cast<double>(std::max(sampleRate, 1)) * 0.5;
    const double fullCorrectionHz =
        clampValue(settings.mixedPhaseMaxFrequencyHz, 60.0, std::max(120.0, nyquist * 0.25));
    const double taperEndHz =
        clampValue(fullCorrectionHz * 2.0, fullCorrectionHz + 40.0, std::max(fullCorrectionHz + 40.0, nyquist * 0.4));

    if (frequencyHz <= fullCorrectionHz) {
        return lowWeight;
    }
    if (frequencyHz >= taperEndHz) {
        return 0.0;
    }
    return lowWeight * (1.0 - smoothRamp((frequencyHz - fullCorrectionHz) /
                                         std::max(taperEndHz - fullCorrectionHz, 1.0e-9)));
}

std::vector<double> buildExcessPhaseCorrectionDegrees(const std::vector<double>& displayFrequencyAxisHz,
                                                      const std::vector<double>& inputExcessPhaseRadians,
                                                      const FilterDesignSettings& settings,
                                                      int sampleRate) {
    const size_t count = std::min(displayFrequencyAxisHz.size(), inputExcessPhaseRadians.size());
    std::vector<double> inputExcessPhaseDegrees;
    inputExcessPhaseDegrees.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        inputExcessPhaseDegrees.push_back(inputExcessPhaseRadians[index] * kRadiansToDegrees);
    }

    std::vector<double> desiredCorrectionDegrees(count, 0.0);
    std::vector<double> trackingWeights(count, 0.0);
    std::vector<double> taperWeights(count, 0.0);
    for (size_t index = 0; index < count; ++index) {
        const double taperWeight =
            excessPhaseCorrectionWeightAt(displayFrequencyAxisHz[index], settings, sampleRate);
        taperWeights[index] = taperWeight;
        desiredCorrectionDegrees[index] = -inputExcessPhaseDegrees[index] * taperWeight * settings.mixedPhaseStrength;
        trackingWeights[index] = taperWeight * taperWeight;
    }

    const double regularization = 48.0 *
                                  std::max(static_cast<double>(count) / 512.0, 1.0) *
                                  smoothnessRegularizationScale(std::max(settings.smoothness, 1.0));
    std::vector<double> correctionDegrees =
        solveRegularizedCurve(desiredCorrectionDegrees, trackingWeights, regularization);
    for (size_t index = 0; index < correctionDegrees.size() && index < taperWeights.size(); ++index) {
        const double taperWeight = taperWeights[index];
        const double limitedDegrees = settings.mixedPhaseMaxCorrectionDegrees * taperWeight;
        correctionDegrees[index] =
            clampValue(correctionDegrees[index] * taperWeight, -limitedDegrees, limitedDegrees);
    }
    return correctionDegrees;
}

std::vector<double> buildPositivePhaseCorrectionRadians(int sampleRate,
                                                        int fftSize,
                                                        const std::vector<double>& displayFrequencyAxisHz,
                                                        const std::vector<double>& correctionPhaseDegrees) {
    const size_t positiveBinCount = static_cast<size_t>(std::max(fftSize / 2, 0) + 1);
    std::vector<double> phaseRadians(positiveBinCount, 0.0);
    for (size_t bin = 1; bin < positiveBinCount; ++bin) {
        const double frequencyHz = static_cast<double>(sampleRate) * static_cast<double>(bin) /
                                   static_cast<double>(std::max(fftSize, 1));
        phaseRadians[bin] =
            interpolateLogFrequency(displayFrequencyAxisHz, correctionPhaseDegrees, frequencyHz) * kDegreesToRadians;
    }
    return phaseRadians;
}

void applyPositivePhaseCorrection(std::vector<std::complex<double>>& spectrum,
                                  const std::vector<double>& positivePhaseRadians) {
    if (spectrum.empty()) {
        return;
    }

    const size_t half = spectrum.size() / 2;
    const size_t positiveCount = std::min(positivePhaseRadians.size(), half + 1);
    for (size_t index = 1; index < positiveCount; ++index) {
        if ((spectrum.size() % 2) == 0 && index == half) {
            continue;
        }
        const std::complex<double> rotation = std::polar(1.0, positivePhaseRadians[index]);
        spectrum[index] *= rotation;
        spectrum[spectrum.size() - index] *= std::conj(rotation);
    }
}

std::vector<double> buildRealImpulseFromSpectrum(const std::vector<std::complex<double>>& spectrum) {
    std::vector<std::complex<double>> timeDomain = spectrum;
    fft(timeDomain, true);
    std::vector<double> impulse(timeDomain.size(), 0.0);
    for (size_t index = 0; index < timeDomain.size(); ++index) {
        impulse[index] = timeDomain[index].real();
    }
    return impulse;
}

size_t dominantSampleIndex(const std::vector<double>& values) {
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

void applyTailFade(std::vector<double>& impulse) {
    const size_t fadeCount = std::min(impulse.size() / 8, size_t{4096});
    if (fadeCount < 64) {
        return;
    }

    const size_t fadeStart = impulse.size() - fadeCount;
    for (size_t index = fadeStart; index < impulse.size(); ++index) {
        const double t = static_cast<double>(index - fadeStart) /
                         static_cast<double>(std::max<size_t>(fadeCount - 1, 1));
        impulse[index] *= std::cos(t * std::numbers::pi * 0.5);
    }
}

void delayImpulse(std::vector<double>& impulse, size_t delaySamples) {
    if (delaySamples == 0 || impulse.empty()) {
        return;
    }
    if (delaySamples >= impulse.size()) {
        std::fill(impulse.begin(), impulse.end(), 0.0);
        return;
    }

    for (size_t index = impulse.size(); index-- > delaySamples;) {
        impulse[index] = impulse[index - delaySamples];
    }
    std::fill(impulse.begin(), impulse.begin() + static_cast<std::ptrdiff_t>(delaySamples), 0.0);
}

size_t bestCircularWindowStart(const std::vector<double>& impulse, size_t windowLength) {
    if (impulse.empty() || windowLength == 0) {
        return 0;
    }

    const size_t window = std::min(windowLength, impulse.size());
    double energy = 0.0;
    for (size_t index = 0; index < window; ++index) {
        energy += impulse[index] * impulse[index];
    }

    double bestEnergy = energy;
    size_t bestStart = 0;
    for (size_t start = 1; start < impulse.size(); ++start) {
        const size_t removedIndex = start - 1;
        const size_t addedIndex = (start + window - 1) % impulse.size();
        energy += (impulse[addedIndex] * impulse[addedIndex]) -
                  (impulse[removedIndex] * impulse[removedIndex]);
        if (energy > bestEnergy) {
            bestEnergy = energy;
            bestStart = start;
        }
    }
    return bestStart;
}

std::vector<double> extractCircularWindow(const std::vector<double>& impulse,
                                          size_t windowStart,
                                          size_t windowLength) {
    std::vector<double> window;
    if (impulse.empty() || windowLength == 0) {
        return window;
    }

    const size_t extractedLength = std::min(windowLength, impulse.size());
    window.reserve(extractedLength);
    for (size_t index = 0; index < extractedLength; ++index) {
        window.push_back(impulse[(windowStart + index) % impulse.size()]);
    }
    return window;
}

std::vector<double> buildMixedPhaseImpulse(const std::vector<double>& positiveMagnitude,
                                           const std::vector<double>& positivePhaseCorrectionRadians,
                                           int fftSize,
                                           int tapCount) {
    const size_t outputLength = static_cast<size_t>(std::max(tapCount, 0));
    if (outputLength == 0) {
        return {};
    }

    double peakPhaseCorrectionRadians = 0.0;
    for (const double value : positivePhaseCorrectionRadians) {
        peakPhaseCorrectionRadians = std::max(peakPhaseCorrectionRadians, std::abs(value));
    }
    if (peakPhaseCorrectionRadians <= 1.0e-9) {
        return buildMinimumPhaseImpulse(positiveMagnitude, fftSize, tapCount);
    }

    std::vector<std::complex<double>> combinedSpectrum =
        buildMinimumPhaseSpectrum(positiveMagnitude, fftSize);
    applyPositivePhaseCorrection(combinedSpectrum, positivePhaseCorrectionRadians);
    const std::vector<double> fullImpulse = buildRealImpulseFromSpectrum(combinedSpectrum);
    if (fullImpulse.empty()) {
        return {};
    }

    const size_t windowStart = bestCircularWindowStart(fullImpulse, outputLength);
    std::vector<double> impulse = extractCircularWindow(fullImpulse, windowStart, outputLength);
    applyTailFade(impulse);
    return impulse;
}

struct RealizedFilterAnalysis {
    std::vector<double> binFrequencyAxisHz;
    std::vector<double> binMagnitudeDb;
    std::vector<double> filterResponseDb;
    std::vector<double> filterPhaseRadians;
    std::vector<double> binPhaseRadians;
    std::vector<double> groupDelayMs;
    std::vector<double> correctedResponseDb;
};

RealizedFilterAnalysis analyzeRealizedFilter(const std::vector<double>& filterTaps,
                                             const std::vector<double>& sourceCurveDb,
                                             const std::vector<double>& displayFrequencyAxisHz,
                                             int sampleRate,
                                             int fftSize) {
    RealizedFilterAnalysis analysis;
    const std::vector<std::complex<double>> spectrum = spectrumOfRealSignal(filterTaps, fftSize);
    const std::vector<double> binFrequencyAxisHz = buildLinearFrequencyAxis(sampleRate, fftSize);
    const std::vector<double> binMagnitudeDb = buildMagnitudeDbSeries(spectrum);
    const std::vector<double> binPhaseRadians = buildPhaseSeries(spectrum);

    analysis.binFrequencyAxisHz = binFrequencyAxisHz;
    analysis.binMagnitudeDb = binMagnitudeDb;
    analysis.binPhaseRadians = binPhaseRadians;
    analysis.filterResponseDb =
        resampleLogFrequency(binFrequencyAxisHz, binMagnitudeDb, displayFrequencyAxisHz);
    analysis.filterPhaseRadians =
        resampleLogFrequency(binFrequencyAxisHz, binPhaseRadians, displayFrequencyAxisHz);
    analysis.groupDelayMs = buildGroupDelayMs(displayFrequencyAxisHz, analysis.filterPhaseRadians);
    analysis.correctedResponseDb.reserve(displayFrequencyAxisHz.size());
    for (size_t index = 0; index < displayFrequencyAxisHz.size() &&
                           index < sourceCurveDb.size() &&
                           index < analysis.filterResponseDb.size(); ++index) {
        analysis.correctedResponseDb.push_back(sourceCurveDb[index] + analysis.filterResponseDb[index]);
    }
    return analysis;
}

PreparedPhaseView buildPredictedPhaseView(const PreparedPhaseChannel& inputChannel,
                                          const std::vector<double>& addedFrequencyAxisHz,
                                          const std::vector<double>& displayFrequencyAxisHz,
                                          const ResponseSmoothingSettings& smoothingSettings,
                                          int sampleRate,
                                          int fftSize,
                                          const std::vector<double>& addedMagnitudeDb,
                                          const std::vector<double>& addedPhaseRadians,
                                          double addedBulkDelaySeconds,
                                          std::string_view sourceSuffix) {
    if (!inputChannel.valid() ||
        addedMagnitudeDb.size() != addedPhaseRadians.size()) {
        return {};
    }

    const std::vector<double> nativeAddedMagnitudeDb =
        resampleLogFrequency(addedFrequencyAxisHz,
                             addedMagnitudeDb,
                             inputChannel.nativeFrequencyAxisHz);
    const std::vector<double> nativeAddedPhaseRadians =
        resampleLogFrequency(addedFrequencyAxisHz,
                             addedPhaseRadians,
                             inputChannel.nativeFrequencyAxisHz);

    std::vector<double> predictedMagnitudeDb = inputChannel.measuredMagnitudeDb;
    std::vector<double> predictedPhaseRadians = inputChannel.delayCorrectedPhaseRadians;
    for (size_t index = 0; index < predictedMagnitudeDb.size() &&
                           index < nativeAddedMagnitudeDb.size(); ++index) {
        predictedMagnitudeDb[index] += nativeAddedMagnitudeDb[index];
    }
    for (size_t index = 0; index < predictedPhaseRadians.size() &&
                           index < nativeAddedPhaseRadians.size(); ++index) {
        predictedPhaseRadians[index] += nativeAddedPhaseRadians[index];
    }

    const PreparedPhaseChannel predicted =
        prepareMatchedPhaseChannel(inputChannel.nativeFrequencyAxisHz,
                                   predictedMagnitudeDb,
                                   predictedPhaseRadians,
                                   std::max(addedBulkDelaySeconds, 0.0),
                                   smoothingSettings,
                                   sampleRate,
                                   fftSize,
                                   inputChannel.sourceKey + std::string(sourceSuffix));
    return resamplePreparedPhaseChannel(predicted, displayFrequencyAxisHz);
}

size_t maxAbsIndex(const std::vector<double>& values) {
    return dominantSampleIndex(values);
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

std::vector<double> buildUnitImpulse(int tapCount) {
    std::vector<double> impulse(static_cast<size_t>(std::max(tapCount, 0)), 0.0);
    if (!impulse.empty()) {
        impulse.front() = 1.0;
    }
    return impulse;
}

struct DesignedChannel {
    std::vector<double> correctionDb;
    std::vector<double> filterResponseDb;
    std::vector<double> correctedResponseDb;
    std::vector<double> inputGroupDelayMs;
    std::vector<double> groupDelayMs;
    std::vector<double> inputExcessPhaseDegrees;
    std::vector<double> inputExcessPhaseContinuousDegrees;
    std::vector<double> predictedExcessPhaseDegrees;
    std::vector<double> predictedExcessPhaseContinuousDegrees;
    std::vector<double> predictedGroupDelayMs;
    std::vector<double> impulseTimeMs;
    std::vector<double> filterTaps;
    int impulsePeakIndex = 0;
    double peakAmplitude = 0.0;
};

void refreshMixedPhaseChannel(DesignedChannel& channel,
                              const std::vector<double>& sourceCurveDb,
                              const PreparedPhaseChannel* preparedInput,
                              const PreparedPhaseView* inputPhaseView,
                              const ResponseSmoothingSettings& smoothingSettings,
                              const std::vector<double>& displayFrequencyAxisHz,
                              int sampleRate,
                              int fftSize) {
    channel.impulsePeakIndex = static_cast<int>(maxAbsIndex(channel.filterTaps));
    channel.peakAmplitude = maxAbsSample(channel.filterTaps);
    channel.impulseTimeMs = buildImpulseTimeAxisMs(channel.filterTaps.size(),
                                                   static_cast<size_t>(std::max(channel.impulsePeakIndex, 0)),
                                                   sampleRate);

    const RealizedFilterAnalysis realized =
        analyzeRealizedFilter(channel.filterTaps,
                              sourceCurveDb,
                              displayFrequencyAxisHz,
                              sampleRate,
                              fftSize);
    channel.filterResponseDb = realized.filterResponseDb;
    channel.correctedResponseDb = realized.correctedResponseDb;
    channel.groupDelayMs = realized.groupDelayMs;

    if (preparedInput == nullptr || inputPhaseView == nullptr || !inputPhaseView->valid()) {
        return;
    }

    channel.inputGroupDelayMs = inputPhaseView->groupDelayMs;
    channel.inputExcessPhaseDegrees = inputPhaseView->wrappedExcessPhaseDegrees;
    channel.inputExcessPhaseContinuousDegrees = inputPhaseView->continuousExcessPhaseDegrees;

    const PreparedPhaseView predictedView =
        buildPredictedPhaseView(*preparedInput,
                                realized.binFrequencyAxisHz,
                                displayFrequencyAxisHz,
                                smoothingSettings,
                                sampleRate,
                                fftSize,
                                realized.binMagnitudeDb,
                                realized.binPhaseRadians,
                                static_cast<double>(std::max(channel.impulsePeakIndex, 0)) /
                                    static_cast<double>(std::max(sampleRate, 1)),
                                ".predicted");
    channel.predictedExcessPhaseDegrees = predictedView.wrappedExcessPhaseDegrees;
    channel.predictedExcessPhaseContinuousDegrees = predictedView.continuousExcessPhaseDegrees;
    channel.predictedGroupDelayMs = predictedView.groupDelayMs;
}

DesignedChannel designChannel(const std::vector<double>& displayFrequencyAxisHz,
                              const std::vector<double>& targetCurveDb,
                              const std::vector<double>& sourceCurveDb,
                              const PreparedPhaseChannel* preparedInput,
                              const PreparedPhaseView* inputPhaseView,
                              const ResponseSmoothingSettings& smoothingSettings,
                              const FilterDesignSettings& settings,
                              NormalizedPhaseMode phaseMode,
                              const std::vector<double>* sharedMixedCorrectionDegrees,
                              int sampleRate,
                              int fftSize) {
    DesignedChannel channel;
    const bool hasSourcePhase = preparedInput != nullptr &&
                                inputPhaseView != nullptr &&
                                inputPhaseView->valid();
    const bool useExcessPreview = phaseMode == NormalizedPhaseMode::ExcessLf;
    const bool useMixedMode = phaseMode == NormalizedPhaseMode::Mixed && hasSourcePhase;

    if (phaseMode == NormalizedPhaseMode::ExcessLf) {
        channel.correctionDb.assign(displayFrequencyAxisHz.size(), 0.0);
        channel.filterResponseDb.assign(displayFrequencyAxisHz.size(), 0.0);
        channel.correctedResponseDb = sourceCurveDb;
        channel.filterTaps = buildUnitImpulse(settings.tapCount);
        channel.impulsePeakIndex = 0;
        channel.peakAmplitude = channel.filterTaps.empty() ? 0.0 : 1.0;
        channel.impulseTimeMs = buildImpulseTimeAxisMs(channel.filterTaps.size(), 0, sampleRate);
    } else {
        channel.correctionDb = buildCorrectionCurve(displayFrequencyAxisHz, targetCurveDb, sourceCurveDb, settings);
        const std::vector<double> positiveMagnitude =
            buildPositiveMagnitudeResponse(sampleRate, fftSize, displayFrequencyAxisHz, channel.correctionDb);
        if (useMixedMode) {
            const std::vector<double> correctionPhaseDegrees =
                sharedMixedCorrectionDegrees != nullptr &&
                        sharedMixedCorrectionDegrees->size() == displayFrequencyAxisHz.size()
                    ? *sharedMixedCorrectionDegrees
                    : buildExcessPhaseCorrectionDegrees(displayFrequencyAxisHz,
                                                        inputPhaseView->excessPhaseRadians,
                                                        settings,
                                                        sampleRate);
            const std::vector<double> positivePhaseCorrectionRadians =
                buildPositivePhaseCorrectionRadians(sampleRate,
                                                    fftSize,
                                                    displayFrequencyAxisHz,
                                                    correctionPhaseDegrees);
            channel.filterTaps = buildMixedPhaseImpulse(positiveMagnitude,
                                                        positivePhaseCorrectionRadians,
                                                        fftSize,
                                                        settings.tapCount);
        } else {
            channel.filterTaps = buildMinimumPhaseImpulse(positiveMagnitude, fftSize, settings.tapCount);
        }
        channel.impulsePeakIndex = static_cast<int>(maxAbsIndex(channel.filterTaps));
        channel.peakAmplitude = maxAbsSample(channel.filterTaps);
        channel.impulseTimeMs = buildImpulseTimeAxisMs(channel.filterTaps.size(),
                                                       static_cast<size_t>(std::max(channel.impulsePeakIndex, 0)),
                                                       sampleRate);
        const RealizedFilterAnalysis realized =
            analyzeRealizedFilter(channel.filterTaps,
                                  sourceCurveDb,
                                  displayFrequencyAxisHz,
                                  sampleRate,
                                  fftSize);
        channel.filterResponseDb = realized.filterResponseDb;
        channel.correctedResponseDb = realized.correctedResponseDb;
        channel.groupDelayMs = realized.groupDelayMs;

        if (useMixedMode) {
            const PreparedPhaseView predictedView =
                buildPredictedPhaseView(*preparedInput,
                                        realized.binFrequencyAxisHz,
                                        displayFrequencyAxisHz,
                                        smoothingSettings,
                                        sampleRate,
                                        fftSize,
                                        realized.binMagnitudeDb,
                                        realized.binPhaseRadians,
                                        static_cast<double>(std::max(channel.impulsePeakIndex, 0)) /
                                            static_cast<double>(std::max(sampleRate, 1)),
                                        ".predicted");
            channel.predictedExcessPhaseDegrees = predictedView.wrappedExcessPhaseDegrees;
            channel.predictedExcessPhaseContinuousDegrees = predictedView.continuousExcessPhaseDegrees;
            channel.predictedGroupDelayMs = predictedView.groupDelayMs;
        }
    }

    if (hasSourcePhase) {
        channel.inputGroupDelayMs = inputPhaseView->groupDelayMs;
        channel.inputExcessPhaseDegrees = inputPhaseView->wrappedExcessPhaseDegrees;
        channel.inputExcessPhaseContinuousDegrees = inputPhaseView->continuousExcessPhaseDegrees;

        if (useExcessPreview) {
            const std::vector<double> correctionPhaseDegrees =
                buildExcessPhaseCorrectionDegrees(displayFrequencyAxisHz,
                                                  inputPhaseView->excessPhaseRadians,
                                                  settings,
                                                  sampleRate);
            const std::vector<double> correctionPhaseRadians = degreesToRadians(correctionPhaseDegrees);

            channel.groupDelayMs = buildGroupDelayMs(displayFrequencyAxisHz, correctionPhaseRadians);
            const PreparedPhaseView predictedView =
                buildPredictedPhaseView(*preparedInput,
                                        displayFrequencyAxisHz,
                                        displayFrequencyAxisHz,
                                        smoothingSettings,
                                        sampleRate,
                                        fftSize,
                                        channel.filterResponseDb,
                                        correctionPhaseRadians,
                                        0.0,
                                        ".preview");
            channel.predictedExcessPhaseDegrees = predictedView.wrappedExcessPhaseDegrees;
            channel.predictedExcessPhaseContinuousDegrees =
                predictedView.continuousExcessPhaseDegrees;
            channel.predictedGroupDelayMs = predictedView.groupDelayMs;
        } else {
            // Minimum-phase filters alter the minimum-phase component only, so excess phase is unchanged.
            if (!useMixedMode) {
                channel.predictedExcessPhaseDegrees = channel.inputExcessPhaseDegrees;
                channel.predictedExcessPhaseContinuousDegrees = channel.inputExcessPhaseContinuousDegrees;
                const double peakDelayMs =
                    static_cast<double>(std::max(channel.impulsePeakIndex, 0)) * 1000.0 /
                    static_cast<double>(std::max(sampleRate, 1));
                channel.predictedGroupDelayMs =
                    addSeries(inputPhaseView->groupDelayMs,
                              subtractConstant(channel.groupDelayMs, peakDelayMs));
            }
        }
    }
    return channel;
}

}  // namespace

void normalizeFilterDesignSettings(FilterDesignSettings& settings, int sampleRate) {
    const int safeSampleRate = std::max(sampleRate, 44100);
    const int nyquist = safeSampleRate / 2;
    normalizePhaseMode(settings.phaseMode);

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

    const int taps[] = {16384, 32768, 65536, 131072};
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
    settings.mixedPhaseMaxFrequencyHz =
        clampValue(settings.mixedPhaseMaxFrequencyHz, 60.0, static_cast<double>(std::max((nyquist / 2), 120)));
    settings.mixedPhaseStrength = clampValue(settings.mixedPhaseStrength, 0.0, 1.0);
    settings.mixedPhaseMaxCorrectionDegrees =
        clampValue(settings.mixedPhaseMaxCorrectionDegrees, 30.0, 720.0);
}

FilterDesignResult designFiltersForSampleRate(const SmoothedResponse& response,
                                              const MeasurementSettings& measurement,
                                              const TargetCurveSettings& targetCurve,
                                              const FilterDesignSettings& sourceSettings,
                                              int sampleRate,
                                              const MeasurementResult* sourceMeasurement) {
    FilterDesignResult result;
    appendProcessLog(result.processLog, "Starting filter design at " + formatKilohertz(sampleRate) + ".");
    if (response.frequencyAxisHz.empty() ||
        response.leftChannelDb.size() != response.frequencyAxisHz.size() ||
        response.rightChannelDb.size() != response.frequencyAxisHz.size()) {
        appendProcessLog(result.processLog, "Aborted filter design because no valid smoothed response was available.");
        return result;
    }

    MeasurementSettings exportMeasurement = measurement;
    exportMeasurement.sampleRate = sampleRate;

    const TargetCurvePlotData targetPlot = buildTargetCurvePlotData(response, exportMeasurement, targetCurve, std::nullopt);
    if (targetPlot.frequencyAxisHz.empty() || targetPlot.targetCurveDb.empty()) {
        appendProcessLog(result.processLog, "Aborted filter design because target-curve evaluation returned no data.");
        return result;
    }

    FilterDesignSettings settings = sourceSettings;
    normalizeFilterDesignSettings(settings, exportMeasurement.sampleRate);
    const NormalizedPhaseMode phaseMode = normalizePhaseMode(settings.phaseMode);
    appendProcessLog(result.processLog,
                     "Normalized filter settings: phase mode " + settings.phaseMode +
                         ", tap count " + std::to_string(settings.tapCount) + ".");

    const std::vector<double> displayFrequencyAxisHz =
        buildLogFrequencyAxis(targetPlot.minFrequencyHz, targetPlot.maxFrequencyHz, settings.displayPointCount);
    const std::vector<double> rawTargetCurveDb =
        resampleLogFrequency(targetPlot.frequencyAxisHz, targetPlot.targetCurveDb, displayFrequencyAxisHz);
    const std::vector<double> leftSourceDb =
        resampleLogFrequency(response.frequencyAxisHz, response.leftChannelDb, displayFrequencyAxisHz);
    const std::vector<double> rightSourceDb =
        resampleLogFrequency(response.frequencyAxisHz, response.rightChannelDb, displayFrequencyAxisHz);
    const std::vector<double> targetCurveDb = rawTargetCurveDb;
    const int fftSize = static_cast<int>(nextPowerOfTwo(static_cast<size_t>(settings.tapCount * 4)));
    appendProcessLog(result.processLog,
                     "Built display axis with " + std::to_string(displayFrequencyAxisHz.size()) +
                         " points and FFT size " + std::to_string(fftSize) + ".");
    const PreparedPhaseData preparedPhase =
        preparePhaseData(sourceMeasurement,
                         response.smoothingSettings,
                         exportMeasurement.sampleRate,
                         fftSize);
    if (preparedPhase.valid) {
        appendProcessLog(result.processLog,
                         "Prepared matched phase data from " + describePhasePreparationSource(preparedPhase) +
                             "; removed bulk delay " + formatMilliseconds(preparedPhase.bulkDelaySeconds) + ".");
    } else if (sourceMeasurement != nullptr) {
        appendProcessLog(result.processLog,
                         "No matched phase-preparation source was available; proceeding with magnitude-only data.");
    } else {
        appendProcessLog(result.processLog,
                         "No measurement result was supplied for phase preparation; proceeding with magnitude-only data.");
    }
    const PreparedPhaseView leftPreparedView =
        preparedPhase.valid ? resamplePreparedPhaseChannel(preparedPhase.left, displayFrequencyAxisHz)
                            : PreparedPhaseView{};
    const PreparedPhaseView rightPreparedView =
        preparedPhase.valid ? resamplePreparedPhaseChannel(preparedPhase.right, displayFrequencyAxisHz)
                            : PreparedPhaseView{};
    std::vector<double> sharedMixedCorrectionDegrees;
    if (phaseMode == NormalizedPhaseMode::Mixed &&
        leftPreparedView.valid() &&
        rightPreparedView.valid()) {
        const double leftLfMean = meanAbsoluteBandValue(displayFrequencyAxisHz,
                                                        leftPreparedView.continuousExcessPhaseDegrees,
                                                        20.0,
                                                        150.0);
        const double rightLfMean = meanAbsoluteBandValue(displayFrequencyAxisHz,
                                                         rightPreparedView.continuousExcessPhaseDegrees,
                                                         20.0,
                                                         150.0);
        const double stereoLfDelta = meanAbsoluteBandDelta(displayFrequencyAxisHz,
                                                           leftPreparedView.continuousExcessPhaseDegrees,
                                                           rightPreparedView.continuousExcessPhaseDegrees,
                                                           20.0,
                                                           150.0);
        const bool preserveStereoRelationship =
            std::min(leftLfMean, rightLfMean) >= 15.0 && stereoLfDelta >= 45.0;
        if (preserveStereoRelationship) {
            const std::vector<double> leftCorrectionDegrees =
                buildExcessPhaseCorrectionDegrees(displayFrequencyAxisHz,
                                                  leftPreparedView.excessPhaseRadians,
                                                  settings,
                                                  exportMeasurement.sampleRate);
            const std::vector<double> rightCorrectionDegrees =
                buildExcessPhaseCorrectionDegrees(displayFrequencyAxisHz,
                                                  rightPreparedView.excessPhaseRadians,
                                                  settings,
                                                  exportMeasurement.sampleRate);
            sharedMixedCorrectionDegrees = averageSeries(leftCorrectionDegrees, rightCorrectionDegrees);
            appendProcessLog(result.processLog,
                             "Built shared low-frequency mixed-phase correction to preserve stereo phase relationship.");
        } else {
            appendProcessLog(result.processLog,
                             "Built per-channel low-frequency mixed-phase correction targets from the prepared excess phase.");
        }
    } else if (phaseMode == NormalizedPhaseMode::Mixed) {
        appendProcessLog(result.processLog,
                         "Mixed mode had no valid prepared phase view; designed magnitude correction only.");
    } else if (phaseMode == NormalizedPhaseMode::ExcessLf) {
        appendProcessLog(result.processLog,
                         "Built excess-phase preview from the prepared phase data without changing magnitude correction.");
    }
    DesignedChannel left = designChannel(displayFrequencyAxisHz,
                                         targetCurveDb,
                                         leftSourceDb,
                                         preparedPhase.valid ? &preparedPhase.left : nullptr,
                                         leftPreparedView.valid() ? &leftPreparedView : nullptr,
                                         response.smoothingSettings,
                                         settings,
                                         phaseMode,
                                         sharedMixedCorrectionDegrees.empty() ? nullptr : &sharedMixedCorrectionDegrees,
                                         exportMeasurement.sampleRate,
                                         fftSize);
    appendProcessLog(result.processLog, "Designed left filter channel.");
    DesignedChannel right = designChannel(displayFrequencyAxisHz,
                                          targetCurveDb,
                                          rightSourceDb,
                                          preparedPhase.valid ? &preparedPhase.right : nullptr,
                                          rightPreparedView.valid() ? &rightPreparedView : nullptr,
                                          response.smoothingSettings,
                                          settings,
                                          phaseMode,
                                          sharedMixedCorrectionDegrees.empty() ? nullptr : &sharedMixedCorrectionDegrees,
                                          exportMeasurement.sampleRate,
                                          fftSize);
    appendProcessLog(result.processLog, "Designed right filter channel.");

    if (phaseMode == NormalizedPhaseMode::Mixed &&
        !left.filterTaps.empty() &&
        left.filterTaps.size() == right.filterTaps.size()) {
        const int sharedPeakIndex = std::max(left.impulsePeakIndex, right.impulsePeakIndex);
        if (left.impulsePeakIndex < sharedPeakIndex) {
            delayImpulse(left.filterTaps, static_cast<size_t>(sharedPeakIndex - left.impulsePeakIndex));
        }
        if (right.impulsePeakIndex < sharedPeakIndex) {
            delayImpulse(right.filterTaps, static_cast<size_t>(sharedPeakIndex - right.impulsePeakIndex));
        }
        refreshMixedPhaseChannel(left,
                                 leftSourceDb,
                                 preparedPhase.valid ? &preparedPhase.left : nullptr,
                                 leftPreparedView.valid() ? &leftPreparedView : nullptr,
                                 response.smoothingSettings,
                                 displayFrequencyAxisHz,
                                 exportMeasurement.sampleRate,
                                 fftSize);
        refreshMixedPhaseChannel(right,
                                 rightSourceDb,
                                 preparedPhase.valid ? &preparedPhase.right : nullptr,
                                 rightPreparedView.valid() ? &rightPreparedView : nullptr,
                                 response.smoothingSettings,
                                 displayFrequencyAxisHz,
                                 exportMeasurement.sampleRate,
                                 fftSize);
        appendProcessLog(result.processLog,
                         "Aligned mixed-phase stereo impulse peaks to a shared latency and refreshed predicted diagnostics.");
    }

    result.valid = true;
    result.sampleRate = exportMeasurement.sampleRate;
    result.tapCount = settings.tapCount;
    result.fftSize = fftSize;
    result.positiveBinCount = (fftSize / 2) + 1;
    result.phaseMode = settings.phaseMode;
    result.phasePreparationSourceWindow = preparedPhase.sourceWindow;
    result.phasePreparationSourceKey = preparedPhase.sourceKey;
    result.phasePreparationSeriesKind = preparedPhase.sourceSeriesKind;
    result.phasePreparationBulkDelaySeconds = preparedPhase.bulkDelaySeconds;
    result.frequencyAxisHz = displayFrequencyAxisHz;
    result.targetCurveDb = targetCurveDb;
    result.left.correctionCurveDb = left.correctionDb;
    result.left.filterResponseDb = left.filterResponseDb;
    result.left.correctedResponseDb = left.correctedResponseDb;
    result.left.inputGroupDelayMs = left.inputGroupDelayMs;
    result.left.groupDelayMs = left.groupDelayMs;
    result.left.inputExcessPhaseDegrees = left.inputExcessPhaseDegrees;
    result.left.inputExcessPhaseContinuousDegrees = left.inputExcessPhaseContinuousDegrees;
    result.left.predictedExcessPhaseDegrees = left.predictedExcessPhaseDegrees;
    result.left.predictedExcessPhaseContinuousDegrees = left.predictedExcessPhaseContinuousDegrees;
    result.left.predictedGroupDelayMs = left.predictedGroupDelayMs;
    result.left.impulseTimeMs = left.impulseTimeMs;
    result.left.filterTaps = left.filterTaps;
    result.left.impulsePeakIndex = left.impulsePeakIndex;
    result.left.peakAmplitude = left.peakAmplitude;
    result.right.correctionCurveDb = right.correctionDb;
    result.right.filterResponseDb = right.filterResponseDb;
    result.right.correctedResponseDb = right.correctedResponseDb;
    result.right.inputGroupDelayMs = right.inputGroupDelayMs;
    result.right.groupDelayMs = right.groupDelayMs;
    result.right.inputExcessPhaseDegrees = right.inputExcessPhaseDegrees;
    result.right.inputExcessPhaseContinuousDegrees = right.inputExcessPhaseContinuousDegrees;
    result.right.predictedExcessPhaseDegrees = right.predictedExcessPhaseDegrees;
    result.right.predictedExcessPhaseContinuousDegrees = right.predictedExcessPhaseContinuousDegrees;
    result.right.predictedGroupDelayMs = right.predictedGroupDelayMs;
    result.right.impulseTimeMs = right.impulseTimeMs;
    result.right.filterTaps = right.filterTaps;
    result.right.impulsePeakIndex = right.impulsePeakIndex;
    result.right.peakAmplitude = right.peakAmplitude;
    appendProcessLog(result.processLog,
                     "Completed filter design with phase source " +
                         (result.phasePreparationSourceKey.empty() ? std::string("none") : result.phasePreparationSourceKey) +
                         ".");
    return result;
}

FilterDesignResult designFilters(const SmoothedResponse& response,
                                 const MeasurementSettings& measurement,
                                 const TargetCurveSettings& targetCurve,
                                 const FilterDesignSettings& settings,
                                 const MeasurementResult* sourceMeasurement) {
    return designFiltersForSampleRate(response,
                                      measurement,
                                      targetCurve,
                                      settings,
                                      measurement.sampleRate,
                                      sourceMeasurement);
}

}  // namespace wolfie::measurement
