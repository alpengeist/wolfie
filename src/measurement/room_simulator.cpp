#include "measurement/room_simulator.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <ctime>
#include <iomanip>
#include <numbers>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "measurement/dsp_utils.h"
#include "measurement/sweep_generator.h"

namespace wolfie::measurement {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

constexpr double kDbFloor = -90.0;
constexpr double kPi = std::numbers::pi_v<double>;

double dbToAmplitude(double valueDb) {
    return std::pow(10.0, valueDb / 20.0);
}

double amplitudeToDb(double amplitude) {
    if (amplitude <= 1.0e-12) {
        return kDbFloor;
    }
    return clampValue(20.0 * std::log10(amplitude), kDbFloor, 24.0);
}

double rmsAmplitude(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    double energy = 0.0;
    for (const double value : values) {
        energy += value * value;
    }
    return std::sqrt(energy / static_cast<double>(values.size()));
}

size_t maxAbsIndex(const std::vector<double>& values) {
    size_t bestIndex = 0;
    double bestMagnitude = 0.0;
    for (size_t index = 0; index < values.size(); ++index) {
        const double magnitude = std::abs(values[index]);
        if (magnitude > bestMagnitude) {
            bestMagnitude = magnitude;
            bestIndex = index;
        }
    }
    return bestIndex;
}

double gaussianBellDb(double frequencyHz, double centerHz, double q, double gainDb) {
    if (frequencyHz <= 0.0 || centerHz <= 0.0 || q <= 0.0 || gainDb == 0.0) {
        return 0.0;
    }

    const double widthOctaves = clampValue(1.4 / q, 0.03, 2.5);
    const double distanceOctaves = std::log2(std::max(frequencyHz, 1.0) / std::max(centerHz, 1.0));
    const double normalized = distanceOctaves / widthOctaves;
    return gainDb * std::exp(-0.5 * normalized * normalized);
}

double lowShelfDb(double frequencyHz, double cornerHz, double gainDb) {
    if (frequencyHz <= 0.0 || cornerHz <= 0.0 || gainDb == 0.0) {
        return 0.0;
    }

    const double octaves = std::log2(std::max(frequencyHz, 1.0) / std::max(cornerHz, 1.0));
    return gainDb * 0.5 * (1.0 - std::tanh(octaves * 2.4));
}

double spectralTiltDb(double frequencyHz, double tiltDbPerOctave) {
    if (frequencyHz <= 0.0 || tiltDbPerOctave == 0.0) {
        return 0.0;
    }
    return tiltDbPerOctave * std::log2(std::max(frequencyHz, 1.0) / 1000.0);
}

std::string currentTimestampUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::vector<double> makeTimeAxisSeconds(size_t sampleCount, size_t preRollSamples, int sampleRate) {
    std::vector<double> axis;
    axis.reserve(sampleCount);
    const double rate = static_cast<double>(std::max(sampleRate, 1));
    for (size_t index = 0; index < sampleCount; ++index) {
        axis.push_back((static_cast<double>(index) - static_cast<double>(preRollSamples)) / rate);
    }
    return axis;
}

std::vector<std::complex<double>> fftOfRealSignal(const std::vector<double>& samples, size_t fftSize) {
    std::vector<std::complex<double>> spectrum(fftSize, std::complex<double>(0.0, 0.0));
    for (size_t index = 0; index < samples.size() && index < fftSize; ++index) {
        spectrum[index] = std::complex<double>(samples[index], 0.0);
    }
    fft(spectrum, false);
    return spectrum;
}

void appendIfValid(MeasurementResult& result, MeasurementValueSet valueSet) {
    if (valueSet.valid()) {
        result.valueSets.push_back(std::move(valueSet));
    }
}

MeasurementValueSet buildImpulseValueSet(const std::string& key,
                                         const std::vector<double>& timeAxisSeconds,
                                         const std::vector<double>& leftImpulse,
                                         const std::vector<double>& rightImpulse) {
    MeasurementValueSet valueSet;
    valueSet.key = key;
    valueSet.xQuantity = "time";
    valueSet.xUnit = "seconds";
    valueSet.yQuantity = "amplitude";
    valueSet.yUnit = "linear";
    valueSet.xValues = timeAxisSeconds;
    valueSet.leftValues = leftImpulse;
    valueSet.rightValues = rightImpulse;
    return valueSet;
}

MeasurementValueSet buildFullSpectrumValueSet(const std::string& key,
                                              std::string yQuantity,
                                              std::string yUnit,
                                              const std::vector<std::complex<double>>& leftSpectrum,
                                              const std::vector<std::complex<double>>& rightSpectrum,
                                              int sampleRate,
                                              bool magnitude) {
    MeasurementValueSet valueSet;
    valueSet.key = key;
    valueSet.yQuantity = std::move(yQuantity);
    valueSet.yUnit = std::move(yUnit);
    const size_t positiveBinCount = leftSpectrum.empty() ? 0 : (leftSpectrum.size() / 2) + 1;
    valueSet.xValues.reserve(positiveBinCount);
    valueSet.leftValues.reserve(positiveBinCount);
    valueSet.rightValues.reserve(positiveBinCount);
    for (size_t index = 0; index < positiveBinCount; ++index) {
        const double frequencyHz = static_cast<double>(index) * static_cast<double>(sampleRate) /
                                   static_cast<double>(std::max<size_t>(1, leftSpectrum.size()));
        valueSet.xValues.push_back(frequencyHz);
        if (magnitude) {
            valueSet.leftValues.push_back(amplitudeToDb(std::abs(leftSpectrum[index])));
            valueSet.rightValues.push_back(amplitudeToDb(std::abs(rightSpectrum[index])));
        } else {
            valueSet.leftValues.push_back(std::arg(leftSpectrum[index]) * 180.0 / kPi);
            valueSet.rightValues.push_back(std::arg(rightSpectrum[index]) * 180.0 / kPi);
        }
    }
    return valueSet;
}

std::complex<double> interpolateSpectrumAtFrequency(const std::vector<std::complex<double>>& spectrum,
                                                    int sampleRate,
                                                    double frequencyHz) {
    if (spectrum.empty()) {
        return {};
    }

    const size_t positiveBinCount = (spectrum.size() / 2) + 1;
    const double maxFrequencyHz = static_cast<double>(std::max(sampleRate, 1)) * 0.5;
    const double clampedFrequencyHz = clampValue(frequencyHz, 0.0, maxFrequencyHz);
    const double scaledBin = clampedFrequencyHz * static_cast<double>(spectrum.size()) /
                             static_cast<double>(std::max(sampleRate, 1));
    const size_t lowIndex = clampValue<size_t>(static_cast<size_t>(scaledBin), 0, positiveBinCount - 1);
    const size_t highIndex = clampValue<size_t>(lowIndex + 1, 0, positiveBinCount - 1);
    if (highIndex == lowIndex) {
        return spectrum[lowIndex];
    }

    const double blend = clampValue(scaledBin - static_cast<double>(lowIndex), 0.0, 1.0);
    return spectrum[lowIndex] + ((spectrum[highIndex] - spectrum[lowIndex]) * blend);
}

MeasurementValueSet buildMagnitudeResponseValueSet(const std::string& key,
                                                   const std::vector<double>& frequencyAxisHz,
                                                   const std::vector<std::complex<double>>& leftSpectrum,
                                                   const std::vector<std::complex<double>>& rightSpectrum,
                                                   int sampleRate) {
    MeasurementValueSet valueSet;
    valueSet.key = key;
    valueSet.xValues = frequencyAxisHz;
    valueSet.leftValues.reserve(frequencyAxisHz.size());
    valueSet.rightValues.reserve(frequencyAxisHz.size());
    for (const double frequencyHz : frequencyAxisHz) {
        valueSet.leftValues.push_back(amplitudeToDb(std::abs(interpolateSpectrumAtFrequency(leftSpectrum, sampleRate, frequencyHz))));
        valueSet.rightValues.push_back(amplitudeToDb(std::abs(interpolateSpectrumAtFrequency(rightSpectrum, sampleRate, frequencyHz))));
    }
    return valueSet;
}

MeasurementValueSet buildPhaseResponseValueSet(const std::string& key,
                                               const std::vector<double>& frequencyAxisHz,
                                               const std::vector<std::complex<double>>& leftSpectrum,
                                               const std::vector<std::complex<double>>& rightSpectrum,
                                               int sampleRate) {
    MeasurementValueSet valueSet;
    valueSet.key = key;
    valueSet.yQuantity = "phase";
    valueSet.yUnit = "degrees";
    valueSet.xValues = frequencyAxisHz;

    std::vector<double> leftWrapped;
    std::vector<double> rightWrapped;
    leftWrapped.reserve(frequencyAxisHz.size());
    rightWrapped.reserve(frequencyAxisHz.size());
    for (const double frequencyHz : frequencyAxisHz) {
        leftWrapped.push_back(std::arg(interpolateSpectrumAtFrequency(leftSpectrum, sampleRate, frequencyHz)));
        rightWrapped.push_back(std::arg(interpolateSpectrumAtFrequency(rightSpectrum, sampleRate, frequencyHz)));
    }

    const std::vector<double> leftUnwrapped = unwrapPhaseRadians(leftWrapped);
    const std::vector<double> rightUnwrapped = unwrapPhaseRadians(rightWrapped);
    valueSet.leftValues.reserve(frequencyAxisHz.size());
    valueSet.rightValues.reserve(frequencyAxisHz.size());
    for (size_t index = 0; index < frequencyAxisHz.size(); ++index) {
        valueSet.leftValues.push_back(leftUnwrapped[index] * 180.0 / kPi);
        valueSet.rightValues.push_back(rightUnwrapped[index] * 180.0 / kPi);
    }
    return valueSet;
}

void applyTailFade(std::vector<double>& impulse) {
    const size_t fadeCount = std::min(impulse.size() / 8, size_t{4096});
    if (fadeCount == 0 || fadeCount >= impulse.size()) {
        return;
    }

    const size_t fadeStart = impulse.size() - fadeCount;
    for (size_t index = fadeStart; index < impulse.size(); ++index) {
        const double t = static_cast<double>(index - fadeStart) / static_cast<double>(fadeCount);
        impulse[index] *= std::cos(t * kPi * 0.5);
    }
}

std::vector<double> buildDirectImpulseWindow(const std::vector<double>& impulse,
                                             size_t preRollSamples,
                                             int sampleRate,
                                             const RoomSimulationSettings& settings) {
    if (impulse.empty()) {
        return {};
    }

    const double directWindowMs = clampValue(settings.earlyReflectionStartMs * 0.8, 5.0, 12.0);
    const size_t directLength = clampValue<size_t>(
        preRollSamples + static_cast<size_t>(std::lround((directWindowMs / 1000.0) * static_cast<double>(sampleRate))),
        std::min<size_t>(256, impulse.size()),
        impulse.size());
    std::vector<double> direct(impulse.begin(), impulse.begin() + static_cast<std::ptrdiff_t>(directLength));
    applyTailFade(direct);
    return direct;
}

std::vector<double> buildChannelImpulse(int sampleRate,
                                        size_t impulseLength,
                                        size_t preRollSamples,
                                        const RoomSimulationSettings& settings,
                                        bool rightChannel) {
    std::vector<double> impulse(impulseLength, 0.0);
    if (impulse.empty()) {
        return impulse;
    }

    std::mt19937 rng(static_cast<std::mt19937::result_type>(
        settings.seed + (rightChannel ? 10007 : 0)));
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    std::uniform_real_distribution<double> zeroToOne(0.0, 1.0);

    const int stereoShiftSamples =
        rightChannel ? static_cast<int>(std::lround((settings.stereoSkewMs / 1000.0) * static_cast<double>(sampleRate))) : 0;
    const int directIndex = static_cast<int>(preRollSamples) + stereoShiftSamples;
    if (directIndex >= 0 && static_cast<size_t>(directIndex) < impulse.size()) {
        impulse[static_cast<size_t>(directIndex)] = 1.0;
    }

    const double firstReflectionDb = -9.5 + (rightChannel ? -0.7 : 0.0);
    for (int reflection = 0; reflection < settings.earlyReflectionCount; ++reflection) {
        const double jitterMs = (unit(rng) * 0.35 * settings.earlyReflectionSpacingMs);
        const double delayMs = settings.earlyReflectionStartMs +
                               (static_cast<double>(reflection) * settings.earlyReflectionSpacingMs) +
                               jitterMs;
        const int sampleIndex = static_cast<int>(preRollSamples) +
                                stereoShiftSamples +
                                static_cast<int>(std::lround((delayMs / 1000.0) * static_cast<double>(sampleRate)));
        if (sampleIndex < 0 || static_cast<size_t>(sampleIndex) >= impulse.size()) {
            continue;
        }

        const double gainDb = firstReflectionDb -
                              (static_cast<double>(reflection) * settings.earlyReflectionDecayDbPerTap);
        impulse[static_cast<size_t>(sampleIndex)] += dbToAmplitude(gainDb) * (0.75 + (zeroToOne(rng) * 0.5));
    }

    const double tailStartMs = std::max(18.0,
                                        settings.earlyReflectionStartMs +
                                            (static_cast<double>(std::max(settings.earlyReflectionCount - 1, 0)) *
                                             settings.earlyReflectionSpacingMs));
    const size_t tailStartSample = std::min(
        impulse.size(),
        preRollSamples +
            static_cast<size_t>(std::lround((tailStartMs / 1000.0) * static_cast<double>(sampleRate))) +
            static_cast<size_t>(std::max(stereoShiftSamples, 0)));
    const double maxDurationSeconds = static_cast<double>(impulse.size()) / static_cast<double>(sampleRate);
    const int tailTapCount = tailStartSample >= impulse.size()
                                 ? 0
                                 : std::max(0,
                                            static_cast<int>(std::lround((settings.lateDensityPerSecond * maxDurationSeconds) *
                                                                         (rightChannel ? 1.04 : 1.0))));
    const double rt60Seconds = std::max(settings.lateDecayRt60Ms / 1000.0, 0.05);
    const double tailStartAmplitude = dbToAmplitude(settings.lateDecayStartDb + (rightChannel ? -0.4 : 0.0));

    for (int tap = 0; tap < tailTapCount; ++tap) {
        const size_t sampleIndex =
            tailStartSample + static_cast<size_t>(std::lround(zeroToOne(rng) * static_cast<double>(impulse.size() - tailStartSample - 1)));
        if (sampleIndex >= impulse.size()) {
            continue;
        }

        const double timeSeconds = static_cast<double>(sampleIndex - preRollSamples) / static_cast<double>(sampleRate);
        const double envelope = std::pow(10.0, (-3.0 * std::max(0.0, timeSeconds - (tailStartMs / 1000.0))) / rt60Seconds);
        impulse[sampleIndex] += tailStartAmplitude * envelope * unit(rng);
    }

    applyTailFade(impulse);
    return impulse;
}

void applySpectralShape(std::vector<double>& impulse,
                        int sampleRate,
                        const RoomSimulationSettings& settings,
                        bool rightChannel) {
    if (impulse.empty()) {
        return;
    }

    const size_t fftSize = nextPowerOfTwo(std::max(impulse.size(), size_t{4096}));
    std::vector<std::complex<double>> spectrum = fftOfRealSignal(impulse, fftSize);
    const double peakFrequencyHz = settings.modalPeakFrequencyHz * (rightChannel ? 1.025 : 0.985);
    const double nullFrequencyHz = settings.modalNullFrequencyHz * (rightChannel ? 0.975 : 1.015);
    for (size_t bin = 0; bin < spectrum.size(); ++bin) {
        const size_t mirroredBin = bin <= (spectrum.size() / 2) ? bin : (spectrum.size() - bin);
        const double frequencyHz = static_cast<double>(mirroredBin) * static_cast<double>(sampleRate) /
                                   static_cast<double>(std::max<size_t>(1, spectrum.size()));
        double gainDb = spectralTiltDb(frequencyHz, settings.spectralTiltDbPerOctave);
        gainDb += lowShelfDb(frequencyHz, settings.lowShelfCornerHz, settings.lowShelfGainDb);
        gainDb += gaussianBellDb(frequencyHz, peakFrequencyHz, settings.modalPeakQ, settings.modalPeakGainDb);
        gainDb += gaussianBellDb(frequencyHz, nullFrequencyHz, settings.modalNullQ, -settings.modalNullDepthDb);
        spectrum[bin] *= dbToAmplitude(gainDb);
    }

    fft(spectrum, true);
    for (size_t index = 0; index < impulse.size(); ++index) {
        impulse[index] = spectrum[index].real();
    }
}

void normalizeChannels(std::vector<double>& leftImpulse,
                       std::vector<double>& rightImpulse,
                       const RoomSimulationSettings& settings) {
    double peak = 0.0;
    for (const double value : leftImpulse) {
        peak = std::max(peak, std::abs(value));
    }
    for (const double value : rightImpulse) {
        peak = std::max(peak, std::abs(value));
    }
    if (peak > 1.0e-12) {
        for (double& value : leftImpulse) {
            value /= peak;
        }
        for (double& value : rightImpulse) {
            value /= peak;
        }
    }

    std::mt19937 rng(static_cast<std::mt19937::result_type>(settings.seed + 40001));
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    const double noiseAmplitude = dbToAmplitude(settings.noiseFloorDb);
    for (double& value : leftImpulse) {
        value += noiseAmplitude * unit(rng);
    }
    for (double& value : rightImpulse) {
        value += noiseAmplitude * unit(rng);
    }
}

MeasurementChannelMetrics buildChannelMetrics(const std::vector<double>& impulse,
                                              int sampleRate,
                                              size_t preRollSamples,
                                              size_t onsetSampleIndex,
                                              double noiseFloorDb) {
    MeasurementChannelMetrics metrics;
    metrics.available = !impulse.empty();
    metrics.detectedLatencySamples = static_cast<int>(onsetSampleIndex);
    metrics.onsetSampleIndex = static_cast<int>(onsetSampleIndex);
    metrics.onsetTimeSeconds =
        (static_cast<double>(onsetSampleIndex) - static_cast<double>(preRollSamples)) / static_cast<double>(std::max(sampleRate, 1));
    metrics.peakSampleIndex = static_cast<int>(maxAbsIndex(impulse));
    metrics.impulseStartSample = 0;
    metrics.impulseLengthSamples = static_cast<int>(impulse.size());
    metrics.preRollSamples = static_cast<int>(preRollSamples);
    metrics.analysisWindowStartSample = 0;
    metrics.analysisWindowLengthSamples = static_cast<int>(impulse.size());
    metrics.analysisWindowFadeSamples = static_cast<int>(std::min(impulse.size() / 8, size_t{4096}));
    metrics.impulsePeakAmplitude = impulse.empty() ? 0.0 : std::abs(impulse[maxAbsIndex(impulse)]);
    metrics.impulsePeakDb = amplitudeToDb(metrics.impulsePeakAmplitude);
    metrics.impulseRmsDb = amplitudeToDb(rmsAmplitude(impulse));
    metrics.capturePeakDb = metrics.impulsePeakDb;
    metrics.captureRmsDb = metrics.impulseRmsDb;
    metrics.noiseFloorDb = noiseFloorDb;
    metrics.impulsePeakToNoiseDb = metrics.impulsePeakDb - noiseFloorDb;
    return metrics;
}

}  // namespace

RoomSimulationSettings defaultRoomSimulationSettings() {
    return {};
}

void normalizeRoomSimulationSettings(RoomSimulationSettings& settings) {
    settings.stereoSkewMs = clampValue(settings.stereoSkewMs, 0.0, 10.0);
    settings.spectralTiltDbPerOctave = clampValue(settings.spectralTiltDbPerOctave, -6.0, 6.0);
    settings.lowShelfGainDb = clampValue(settings.lowShelfGainDb, -12.0, 18.0);
    settings.lowShelfCornerHz = clampValue(settings.lowShelfCornerHz, 30.0, 500.0);
    settings.modalPeakFrequencyHz = clampValue(settings.modalPeakFrequencyHz, 20.0, 400.0);
    settings.modalPeakGainDb = clampValue(settings.modalPeakGainDb, 0.0, 18.0);
    settings.modalPeakQ = clampValue(settings.modalPeakQ, 0.5, 30.0);
    settings.modalNullFrequencyHz = clampValue(settings.modalNullFrequencyHz, 20.0, 400.0);
    settings.modalNullDepthDb = clampValue(settings.modalNullDepthDb, 0.0, 24.0);
    settings.modalNullQ = clampValue(settings.modalNullQ, 0.5, 30.0);
    settings.earlyReflectionCount = clampValue(settings.earlyReflectionCount, 0, 24);
    settings.earlyReflectionStartMs = clampValue(settings.earlyReflectionStartMs, 1.0, 80.0);
    settings.earlyReflectionSpacingMs = clampValue(settings.earlyReflectionSpacingMs, 0.5, 30.0);
    settings.earlyReflectionDecayDbPerTap = clampValue(settings.earlyReflectionDecayDbPerTap, 0.0, 12.0);
    settings.lateDecayRt60Ms = clampValue(settings.lateDecayRt60Ms, 80.0, 3000.0);
    settings.lateDecayStartDb = clampValue(settings.lateDecayStartDb, -48.0, -6.0);
    settings.lateDensityPerSecond = clampValue(settings.lateDensityPerSecond, 0.0, 1500.0);
    settings.noiseFloorDb = clampValue(settings.noiseFloorDb, -120.0, -24.0);
    settings.seed = clampValue(settings.seed, 0, 1000000);
}

MeasurementResult buildSimulatedRoomMeasurement(const MeasurementSettings& measurement,
                                                const RoomSimulationSettings& simulation,
                                                std::string_view simulationName) {
    MeasurementResult result;
    RoomSimulationSettings normalized = simulation;
    normalizeRoomSimulationSettings(normalized);

    MeasurementSettings measurementSettings = measurement;
    syncDerivedMeasurementSettings(measurementSettings);
    const int sampleRate = std::max(8000, measurementSettings.sampleRate);
    const size_t impulseLength = static_cast<size_t>(std::max(measurementSettings.targetLengthSamples, 2048));
    const size_t preRollSamples = clampValue<size_t>(
        static_cast<size_t>(std::lround(static_cast<double>(sampleRate) * 0.004)),
        64,
        std::max<size_t>(64, impulseLength / 8));

    std::vector<double> leftImpulse = buildChannelImpulse(sampleRate, impulseLength, preRollSamples, normalized, false);
    std::vector<double> rightImpulse = buildChannelImpulse(sampleRate, impulseLength, preRollSamples, normalized, true);
    applySpectralShape(leftImpulse, sampleRate, normalized, false);
    applySpectralShape(rightImpulse, sampleRate, normalized, true);
    normalizeChannels(leftImpulse, rightImpulse, normalized);

    std::vector<double> roomLeftImpulse = leftImpulse;
    std::vector<double> roomRightImpulse = rightImpulse;
    applyTailFade(roomLeftImpulse);
    applyTailFade(roomRightImpulse);
    const std::vector<double> directLeftImpulse = buildDirectImpulseWindow(leftImpulse, preRollSamples, sampleRate, normalized);
    const std::vector<double> directRightImpulse = buildDirectImpulseWindow(rightImpulse, preRollSamples, sampleRate, normalized);

    const size_t fftSize = nextPowerOfTwo(std::max({roomLeftImpulse.size(),
                                                    roomRightImpulse.size(),
                                                    size_t{4096}}));
    const std::vector<std::complex<double>> leftRoomSpectrum = fftOfRealSignal(roomLeftImpulse, fftSize);
    const std::vector<std::complex<double>> rightRoomSpectrum = fftOfRealSignal(roomRightImpulse, fftSize);

    const double maxFrequencyHz = std::min(measurementSettings.endFrequencyHz, static_cast<double>(sampleRate) * 0.5);
    const size_t positiveBinCount = (fftSize / 2) + 1;
    const size_t displayPointCount = std::min(positiveBinCount,
                                              std::max<size_t>(256, std::min<size_t>(2048, positiveBinCount / 4)));
    const std::vector<double> displayFrequencyAxisHz =
        buildLogFrequencyAxis(std::max(measurementSettings.startFrequencyHz, 20.0), maxFrequencyHz, displayPointCount);
    const std::vector<double> timeAxisSeconds = makeTimeAxisSeconds(impulseLength, preRollSamples, sampleRate);

    appendIfValid(result,
                  buildImpulseValueSet("measurement.raw_impulse_response", timeAxisSeconds, leftImpulse, rightImpulse));
    appendIfValid(result,
                  buildImpulseValueSet("measurement.room_impulse_response", timeAxisSeconds, roomLeftImpulse, roomRightImpulse));
    appendIfValid(result,
                  buildImpulseValueSet("measurement.direct_impulse_response",
                                       makeTimeAxisSeconds(directLeftImpulse.size(), preRollSamples, sampleRate),
                                       directLeftImpulse,
                                       directRightImpulse));

    appendIfValid(result,
                  buildFullSpectrumValueSet("measurement.raw_magnitude_spectrum",
                                            "level",
                                            "dB",
                                            leftRoomSpectrum,
                                            rightRoomSpectrum,
                                            sampleRate,
                                            true));
    appendIfValid(result,
                  buildFullSpectrumValueSet("measurement.raw_phase_spectrum",
                                            "phase",
                                            "degrees",
                                            leftRoomSpectrum,
                                            rightRoomSpectrum,
                                            sampleRate,
                                            false));
    appendIfValid(result,
                  buildFullSpectrumValueSet("measurement.room_magnitude_spectrum",
                                            "level",
                                            "dB",
                                            leftRoomSpectrum,
                                            rightRoomSpectrum,
                                            sampleRate,
                                            true));
    appendIfValid(result,
                  buildFullSpectrumValueSet("measurement.room_phase_spectrum",
                                            "phase",
                                            "degrees",
                                            leftRoomSpectrum,
                                            rightRoomSpectrum,
                                            sampleRate,
                                            false));
    MeasurementValueSet rawMagnitudeResponse = buildMagnitudeResponseValueSet("measurement.raw_magnitude_response",
                                                                              displayFrequencyAxisHz,
                                                                              leftRoomSpectrum,
                                                                              rightRoomSpectrum,
                                                                              sampleRate);
    MeasurementValueSet magnitudeResponse = rawMagnitudeResponse;
    magnitudeResponse.key = "measurement.magnitude_response";
    appendIfValid(result, std::move(rawMagnitudeResponse));
    appendIfValid(result,
                  buildPhaseResponseValueSet("measurement.raw_phase_response",
                                             displayFrequencyAxisHz,
                                             leftRoomSpectrum,
                                             rightRoomSpectrum,
                                             sampleRate));
    appendIfValid(result, std::move(magnitudeResponse));
    appendIfValid(result,
                  buildMagnitudeResponseValueSet("measurement.room_magnitude_response",
                                                 displayFrequencyAxisHz,
                                                 leftRoomSpectrum,
                                                 rightRoomSpectrum,
                                                 sampleRate));
    appendIfValid(result,
                  buildPhaseResponseValueSet("measurement.room_phase_response",
                                             displayFrequencyAxisHz,
                                             leftRoomSpectrum,
                                             rightRoomSpectrum,
                                             sampleRate));
    MeasurementAnalysis& analysis = result.analysis;
    analysis.analyzerVersion = "room-sim-v1";
    analysis.measurementKind = "room-simulation";
    analysis.measurementTimestampUtc = currentTimestampUtc();
    analysis.backendName = "Simulation";
    analysis.backendInputDevice = "Synthetic room response";
    analysis.backendOutputDevice = simulationName.empty() ? "Room Simulation" : std::string(simulationName);
    analysis.requestedBackend = "simulation";
    analysis.requestedDriver = std::string(simulationName);
    analysis.routingSelectionHonored = true;
    analysis.routingNotes = "Synthetic room response generated from Room Simulation settings.";
    analysis.sampleRate = sampleRate;
    analysis.sweepDurationSeconds = measurementSettings.durationSeconds;
    analysis.fadeInSeconds = measurementSettings.fadeInSeconds;
    analysis.fadeOutSeconds = measurementSettings.fadeOutSeconds;
    analysis.startFrequencyHz = measurementSettings.startFrequencyHz;
    analysis.endFrequencyHz = measurementSettings.endFrequencyHz;
    analysis.targetLengthSamples = static_cast<int>(impulseLength);
    analysis.leadInSamples = measurementSettings.leadInSamples;
    analysis.outputVolumeDb = 0.0;
    analysis.playedSweepSamples = static_cast<int>(std::lround(measurementSettings.durationSeconds * static_cast<double>(sampleRate)));
    analysis.capturedSamples = static_cast<int>(impulseLength);
    analysis.alignmentSearchSamples = static_cast<int>(preRollSamples);
    analysis.alignmentMethod = "Synthetic direct-path alignment";
    analysis.windowType = "Synthetic room decay";
    analysis.inverseFilterLengthSamples = 0;
    analysis.inverseFilterPeakIndex = 0;
    analysis.fftSize = static_cast<int>(fftSize);
    analysis.displayPointCount = static_cast<int>(displayPointCount);
    analysis.captureClippingDetected = false;
    analysis.captureTooQuiet = false;
    analysis.capturePeakDb = std::max(amplitudeToDb(std::abs(leftImpulse[maxAbsIndex(leftImpulse)])),
                                      amplitudeToDb(std::abs(rightImpulse[maxAbsIndex(rightImpulse)])));
    analysis.captureRmsDb = amplitudeToDb(std::max(rmsAmplitude(leftImpulse), rmsAmplitude(rightImpulse)));
    analysis.captureNoiseFloorDb = normalized.noiseFloorDb;
    analysis.left = buildChannelMetrics(leftImpulse, sampleRate, preRollSamples, preRollSamples, normalized.noiseFloorDb);
    analysis.right = buildChannelMetrics(rightImpulse,
                                         sampleRate,
                                         preRollSamples,
                                         preRollSamples +
                                             static_cast<size_t>(std::max(0, static_cast<int>(std::lround((normalized.stereoSkewMs / 1000.0) *
                                                                                                         static_cast<double>(sampleRate))))),
                                         normalized.noiseFloorDb);
    return result;
}

}  // namespace wolfie::measurement
