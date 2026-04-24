#include "measurement/response_analyzer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <utility>

namespace wolfie::measurement {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double rmsFromPcm16(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0;
    }

    double energy = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double normalized = static_cast<double>(samples[i]) / 32768.0;
        energy += normalized * normalized;
    }
    return std::sqrt(energy / static_cast<double>(count));
}

double rmsFromDouble(const double* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0;
    }

    double energy = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double normalized = samples[i];
        energy += normalized * normalized;
    }
    return std::sqrt(energy / static_cast<double>(count));
}

double amplitudeToDb(double amplitude) {
    if (amplitude <= 1.0e-6) {
        return -90.0;
    }
    return clampValue(20.0 * std::log10(amplitude), -90.0, 24.0);
}

double normalizedPcm16(int16_t sample) {
    return static_cast<double>(sample) / 32768.0;
}

std::vector<double> pcm16ToDouble(const std::vector<int16_t>& samples) {
    std::vector<double> converted(samples.size(), 0.0);
    for (size_t i = 0; i < samples.size(); ++i) {
        converted[i] = normalizedPcm16(samples[i]);
    }
    return converted;
}

bool containsClipping(const std::vector<int16_t>& samples) {
    return std::any_of(samples.begin(), samples.end(), [](int16_t sample) {
        return sample <= -32767 || sample >= 32767;
    });
}

struct ReferencePulse {
    size_t index = 0;
    double amplitude = 0.0;
};

struct CorrelationPeak {
    bool valid = false;
    int delaySamples = 0;
    size_t peakIndex = 0;
    double score = 0.0;
    double peakToNoiseDb = 0.0;
};

struct InverseSweepFilter {
    std::vector<double> samples;
    size_t referencePeakIndex = 0;
};

std::vector<ReferencePulse> findReferencePulses(const std::vector<double>& reference) {
    std::vector<ReferencePulse> pulses;
    for (size_t i = 0; i < reference.size(); ++i) {
        if (std::abs(reference[i]) > 1.0e-9) {
            pulses.push_back({i, reference[i]});
        }
    }
    return pulses;
}

double maxAbsSample(const std::vector<double>& samples) {
    double peak = 0.0;
    for (const double sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

double correlationNoiseRms(const std::vector<double>& scores, size_t peakIndex, size_t excludeRadius) {
    double energy = 0.0;
    size_t count = 0;
    const size_t excludeBegin = peakIndex > excludeRadius ? peakIndex - excludeRadius : 0;
    const size_t excludeEnd = std::min(scores.size(), peakIndex + excludeRadius + 1);
    for (size_t i = 0; i < scores.size(); ++i) {
        if (i >= excludeBegin && i < excludeEnd) {
            continue;
        }
        energy += scores[i] * scores[i];
        ++count;
    }
    return count == 0 ? 0.0 : std::sqrt(energy / static_cast<double>(count));
}

CorrelationPeak correlatePulseTrainAtSegment(const std::vector<double>& captured,
                                             const std::vector<ReferencePulse>& pulses,
                                             size_t segmentStart,
                                             int maxLatencySamples) {
    CorrelationPeak peak;
    if (captured.empty() || pulses.empty() || segmentStart >= captured.size() || maxLatencySamples <= 0) {
        return peak;
    }

    const size_t maxDelay = std::min(static_cast<size_t>(maxLatencySamples),
                                     captured.size() - segmentStart - 1);
    std::vector<double> scores(maxDelay + 1, 0.0);
    for (size_t delay = 0; delay <= maxDelay; ++delay) {
        double score = 0.0;
        double referenceEnergy = 0.0;
        size_t matchedPulses = 0;
        for (const ReferencePulse& pulse : pulses) {
            const size_t captureIndex = segmentStart + delay + pulse.index;
            if (captureIndex >= captured.size()) {
                continue;
            }
            score += captured[captureIndex] * pulse.amplitude;
            referenceEnergy += pulse.amplitude * pulse.amplitude;
            ++matchedPulses;
        }
        if (matchedPulses >= std::max<size_t>(3, pulses.size() / 2) && referenceEnergy > 0.0) {
            scores[delay] = std::abs(score) / std::sqrt(referenceEnergy);
        }
    }

    const auto best = std::max_element(scores.begin(), scores.end());
    if (best == scores.end() || *best <= 0.0) {
        return peak;
    }

    const size_t bestDelay = static_cast<size_t>(std::distance(scores.begin(), best));
    const double noiseRms = correlationNoiseRms(scores, bestDelay, 8);
    peak.valid = true;
    peak.delaySamples = static_cast<int>(bestDelay);
    peak.peakIndex = segmentStart + bestDelay;
    peak.score = *best;
    peak.peakToNoiseDb = noiseRms <= 1.0e-12 ? 120.0 : 20.0 * std::log10(*best / noiseRms);
    return peak;
}

double absolutePeakDb(const std::vector<double>& samples) {
    return amplitudeToDb(maxAbsSample(samples));
}

double rmsDbFromSamples(const std::vector<double>& samples) {
    return amplitudeToDb(rmsFromDouble(samples.data(), samples.size()));
}

double rmsDbFromPcmVector(const std::vector<int16_t>& samples) {
    return amplitudeToDb(rmsFromPcm16(samples.data(), samples.size()));
}

double rmsExcludingRange(const std::vector<double>& samples, size_t excludedCenter, size_t excludedRadius) {
    if (samples.empty()) {
        return 0.0;
    }

    const size_t excludeBegin = excludedCenter > excludedRadius ? excludedCenter - excludedRadius : 0;
    const size_t excludeEnd = std::min(samples.size(), excludedCenter + excludedRadius + 1);
    double energy = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (i >= excludeBegin && i < excludeEnd) {
            continue;
        }
        energy += samples[i] * samples[i];
        ++count;
    }
    return count == 0 ? 0.0 : std::sqrt(energy / static_cast<double>(count));
}

double noiseFloorDb(const std::vector<double>& samples, size_t excludedCenter = 0, size_t excludedRadius = 0) {
    return amplitudeToDb(rmsExcludingRange(samples, excludedCenter, excludedRadius));
}

double interpolateMicrophoneCalibrationDb(const AudioSettings& audioSettings, double frequencyHz) {
    const auto& frequencyAxisHz = audioSettings.microphoneCalibrationFrequencyHz;
    const auto& correctionDb = audioSettings.microphoneCalibrationCorrectionDb;
    if (frequencyAxisHz.size() < 2 || frequencyAxisHz.size() != correctionDb.size()) {
        return 0.0;
    }

    const double clampedFrequencyHz = std::max(1.0, frequencyHz);
    if (clampedFrequencyHz <= frequencyAxisHz.front()) {
        return correctionDb.front();
    }
    if (clampedFrequencyHz >= frequencyAxisHz.back()) {
        return correctionDb.back();
    }

    for (size_t i = 1; i < frequencyAxisHz.size(); ++i) {
        if (frequencyAxisHz[i] < clampedFrequencyHz) {
            continue;
        }

        const double lowFrequencyHz = std::max(1.0, frequencyAxisHz[i - 1]);
        const double highFrequencyHz = std::max(lowFrequencyHz + 1.0e-9, frequencyAxisHz[i]);
        const double lowLog = std::log(lowFrequencyHz);
        const double highLog = std::log(highFrequencyHz);
        const double targetLog = std::log(clampedFrequencyHz);
        const double blend = clampValue((targetLog - lowLog) / std::max(1.0e-9, highLog - lowLog), 0.0, 1.0);
        return correctionDb[i - 1] + ((correctionDb[i] - correctionDb[i - 1]) * blend);
    }

    return correctionDb.back();
}

void applyMicrophoneCalibration(const AudioSettings& audioSettings, MeasurementValueSet& valueSet) {
    if (audioSettings.microphoneCalibrationFrequencyHz.size() < 2 ||
        audioSettings.microphoneCalibrationFrequencyHz.size() != audioSettings.microphoneCalibrationCorrectionDb.size()) {
        return;
    }

    for (size_t i = 0; i < valueSet.xValues.size() &&
                       i < valueSet.leftValues.size() &&
                       i < valueSet.rightValues.size(); ++i) {
        const double correctionDb = interpolateMicrophoneCalibrationDb(audioSettings, valueSet.xValues[i]);
        valueSet.leftValues[i] += correctionDb;
        valueSet.rightValues[i] += correctionDb;
    }
}

constexpr double kPi = 3.14159265358979323846;

size_t nextPowerOfTwo(size_t value) {
    size_t power = 1;
    while (power < value) {
        power <<= 1U;
    }
    return power;
}

void fft(std::vector<std::complex<double>>& samples, bool inverse) {
    const size_t count = samples.size();
    if (count <= 1) {
        return;
    }

    for (size_t i = 1, j = 0; i < count; ++i) {
        size_t bit = count >> 1U;
        while ((j & bit) != 0) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
        if (i < j) {
            std::swap(samples[i], samples[j]);
        }
    }

    for (size_t length = 2; length <= count; length <<= 1U) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(length);
        const std::complex<double> phaseStep(std::cos(angle), std::sin(angle));
        for (size_t offset = 0; offset < count; offset += length) {
            std::complex<double> phase(1.0, 0.0);
            const size_t halfLength = length >> 1U;
            for (size_t i = 0; i < halfLength; ++i) {
                const std::complex<double> even = samples[offset + i];
                const std::complex<double> odd = samples[offset + i + halfLength] * phase;
                samples[offset + i] = even + odd;
                samples[offset + i + halfLength] = even - odd;
                phase *= phaseStep;
            }
        }
    }

    if (inverse) {
        const double scale = 1.0 / static_cast<double>(count);
        for (std::complex<double>& sample : samples) {
            sample *= scale;
        }
    }
}

std::vector<double> convolveReal(const std::vector<double>& left, const std::vector<double>& right) {
    if (left.empty() || right.empty()) {
        return {};
    }

    const size_t outputSize = left.size() + right.size() - 1;
    const size_t fftSize = nextPowerOfTwo(outputSize);
    std::vector<std::complex<double>> leftSpectrum(fftSize, std::complex<double>(0.0, 0.0));
    std::vector<std::complex<double>> rightSpectrum(fftSize, std::complex<double>(0.0, 0.0));
    for (size_t i = 0; i < left.size(); ++i) {
        leftSpectrum[i] = std::complex<double>(left[i], 0.0);
    }
    for (size_t i = 0; i < right.size(); ++i) {
        rightSpectrum[i] = std::complex<double>(right[i], 0.0);
    }

    fft(leftSpectrum, false);
    fft(rightSpectrum, false);
    for (size_t i = 0; i < fftSize; ++i) {
        leftSpectrum[i] *= rightSpectrum[i];
    }
    fft(leftSpectrum, true);

    std::vector<double> output(outputSize, 0.0);
    for (size_t i = 0; i < outputSize; ++i) {
        output[i] = leftSpectrum[i].real();
    }
    return output;
}

std::vector<std::complex<double>> fftOfRealSignal(const std::vector<double>& samples, size_t fftSize) {
    std::vector<std::complex<double>> spectrum(fftSize, std::complex<double>(0.0, 0.0));
    for (size_t i = 0; i < samples.size() && i < fftSize; ++i) {
        spectrum[i] = std::complex<double>(samples[i], 0.0);
    }
    fft(spectrum, false);
    return spectrum;
}

size_t maxAbsIndex(const std::vector<double>& samples) {
    if (samples.empty()) {
        return 0;
    }

    double bestMagnitude = 0.0;
    size_t bestIndex = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double magnitude = std::abs(samples[i]);
        if (magnitude > bestMagnitude) {
            bestMagnitude = magnitude;
            bestIndex = i;
        }
    }
    return bestIndex;
}

InverseSweepFilter buildInverseSweepFilter(const std::vector<double>& sweepSamples,
                                           const MeasurementSettings& settings,
                                           int sampleRate) {
    InverseSweepFilter filter;
    if (sweepSamples.empty()) {
        return filter;
    }

    const double durationSeconds = static_cast<double>(sweepSamples.size()) / static_cast<double>(std::max(sampleRate, 1));
    const double startHz = std::max(1.0, settings.startFrequencyHz);
    const double endHz = std::max(startHz + 1.0e-6, settings.endFrequencyHz);
    const double logSpan = std::log(endHz / startHz);
    const double growth = logSpan > 1.0e-9 ? durationSeconds / logSpan : 0.0;

    filter.samples.assign(sweepSamples.size(), 0.0);
    for (size_t i = 0; i < sweepSamples.size(); ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(std::max(sampleRate, 1));
        const double weight = growth > 0.0 ? std::exp(-t / growth) : 1.0;
        filter.samples[i] = sweepSamples[sweepSamples.size() - 1 - i] * weight;
    }

    const std::vector<double> selfDeconvolved = convolveReal(sweepSamples, filter.samples);
    filter.referencePeakIndex = maxAbsIndex(selfDeconvolved);
    const double peakValue =
        filter.referencePeakIndex < selfDeconvolved.size() ? selfDeconvolved[filter.referencePeakIndex] : 0.0;
    if (std::abs(peakValue) > 1.0e-9) {
        const double scale = 1.0 / peakValue;
        for (double& sample : filter.samples) {
            sample *= scale;
        }
    }
    return filter;
}

std::vector<double> extractCaptureSegment(const std::vector<int16_t>& capturedSamples,
                                          size_t segmentOffset,
                                          const SweepPlaybackPlan& playbackPlan,
                                          size_t extraTailFrames) {
    const size_t captureStart = segmentOffset;
    if (captureStart >= capturedSamples.size()) {
        return {};
    }

    const size_t availableFrames = std::min(playbackPlan.segmentFrames + extraTailFrames,
                                            capturedSamples.size() - captureStart);
    if (availableFrames == 0) {
        return {};
    }

    std::vector<double> segment(availableFrames, 0.0);
    for (size_t i = 0; i < availableFrames; ++i) {
        segment[i] = normalizedPcm16(capturedSamples[captureStart + i]);
    }
    return segment;
}

std::vector<double> makeTimeAxisSeconds(size_t sampleCount, size_t preRollFrames, int sampleRate) {
    std::vector<double> axis;
    axis.reserve(sampleCount);
    const double rate = static_cast<double>(std::max(sampleRate, 1));
    for (size_t i = 0; i < sampleCount; ++i) {
        axis.push_back((static_cast<double>(i) - static_cast<double>(preRollFrames)) / rate);
    }
    return axis;
}

size_t applyAnalysisWindow(std::vector<double>& samples, size_t preRollFrames) {
    if (samples.size() < 64) {
        return 0;
    }

    const size_t fadeFrames = std::clamp(samples.size() / 16, size_t{32}, size_t{2048});
    const size_t leadingFade = std::min(preRollFrames, fadeFrames);
    if (leadingFade > 1) {
        for (size_t i = 0; i < leadingFade; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(leadingFade - 1);
            const double weight = 0.5 - (0.5 * std::cos(kPi * t));
            samples[i] *= weight;
        }
    }

    if (fadeFrames > 1) {
        for (size_t i = 0; i < fadeFrames; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(fadeFrames - 1);
            const double weight = 0.5 * (1.0 + std::cos(kPi * t));
            samples[samples.size() - fadeFrames + i] *= weight;
        }
    }
    return fadeFrames;
}

std::vector<double> unwrapPhaseRadians(const std::vector<double>& phaseRadians) {
    std::vector<double> unwrapped = phaseRadians;
    if (unwrapped.empty()) {
        return unwrapped;
    }

    double offset = 0.0;
    for (size_t i = 1; i < unwrapped.size(); ++i) {
        const double delta = unwrapped[i] - unwrapped[i - 1];
        if (delta > kPi) {
            offset -= 2.0 * kPi;
        } else if (delta < -kPi) {
            offset += 2.0 * kPi;
        }
        unwrapped[i] += offset;
    }
    return unwrapped;
}

std::vector<double> buildLogFrequencyAxis(const MeasurementSettings& settings,
                                          int sampleRate,
                                          size_t pointCount) {
    const double minFrequencyHz = std::max(10.0, settings.startFrequencyHz);
    const double maxFrequencyHz = clampValue(settings.endFrequencyHz,
                                             minFrequencyHz + 1.0,
                                             static_cast<double>(std::max(sampleRate, 1)) * 0.5);
    const double logMin = std::log10(minFrequencyHz);
    const double logMax = std::log10(maxFrequencyHz);

    std::vector<double> axis;
    axis.reserve(pointCount);
    for (size_t i = 0; i < pointCount; ++i) {
        const double t = pointCount <= 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(pointCount - 1);
        axis.push_back(std::pow(10.0, logMin + ((logMax - logMin) * t)));
    }
    return axis;
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
    for (size_t i = 0; i < frequencyAxisHz.size(); ++i) {
        valueSet.leftValues.push_back(leftUnwrapped[i] * 180.0 / kPi);
        valueSet.rightValues.push_back(rightUnwrapped[i] * 180.0 / kPi);
    }
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
    for (size_t i = 0; i < positiveBinCount; ++i) {
        const double frequencyHz = static_cast<double>(i) * static_cast<double>(sampleRate) /
                                   static_cast<double>(std::max<size_t>(1, leftSpectrum.size()));
        valueSet.xValues.push_back(frequencyHz);
        if (magnitude) {
            valueSet.leftValues.push_back(amplitudeToDb(std::abs(leftSpectrum[i])));
            valueSet.rightValues.push_back(amplitudeToDb(std::abs(rightSpectrum[i])));
        } else {
            valueSet.leftValues.push_back(std::arg(leftSpectrum[i]) * 180.0 / kPi);
            valueSet.rightValues.push_back(std::arg(rightSpectrum[i]) * 180.0 / kPi);
        }
    }
    return valueSet;
}

struct ChannelAnalysis {
    std::vector<double> captureSegment;
    std::vector<double> impulseResponse;
    int detectedLatencySamples = 0;
    size_t peakSampleIndex = 0;
    size_t impulseStartSample = 0;
    size_t preRollFrames = 0;
    size_t analysisWindowLengthFrames = 0;
    size_t analysisWindowFadeFrames = 0;
    double capturePeakDb = -90.0;
    double captureRmsDb = -90.0;
    double noiseFloorDb = -90.0;
    double impulsePeakAmplitude = 0.0;
    double impulsePeakDb = -90.0;
    double impulseRmsDb = -90.0;
    double impulsePeakToNoiseDb = 0.0;
};

ChannelAnalysis analyzeSweepSegment(const std::vector<int16_t>& capturedSamples,
                                    size_t segmentOffset,
                                    const SweepPlaybackPlan& playbackPlan,
                                    size_t alignmentSearchFrames,
                                    const InverseSweepFilter& inverseFilter,
                                    int sampleRate,
                                    const MeasurementSettings& settings) {
    ChannelAnalysis analysis;
    analysis.captureSegment = extractCaptureSegment(capturedSamples,
                                                    segmentOffset,
                                                    playbackPlan,
                                                    alignmentSearchFrames);
    if (analysis.captureSegment.empty() || inverseFilter.samples.empty()) {
        return analysis;
    }

    analysis.capturePeakDb = absolutePeakDb(analysis.captureSegment);
    analysis.captureRmsDb = rmsDbFromSamples(analysis.captureSegment);
    const size_t noiseFrames = std::min<size_t>(std::max<size_t>(playbackPlan.leadInFrames, 64),
                                                analysis.captureSegment.size());
    if (noiseFrames > 0) {
        const std::vector<double> leadingNoise(analysis.captureSegment.begin(),
                                               analysis.captureSegment.begin() + static_cast<std::ptrdiff_t>(noiseFrames));
        analysis.noiseFloorDb = rmsDbFromSamples(leadingNoise);
    }

    const std::vector<double> deconvolved = convolveReal(analysis.captureSegment, inverseFilter.samples);
    if (deconvolved.empty()) {
        return analysis;
    }

    const size_t peakIndex = maxAbsIndex(deconvolved);
    analysis.peakSampleIndex = peakIndex;
    const int latency = static_cast<int>(peakIndex) - static_cast<int>(inverseFilter.referencePeakIndex) -
                        static_cast<int>(playbackPlan.leadInFrames);
    analysis.detectedLatencySamples = std::max(0, latency);
    const size_t requestedTargetLength = static_cast<size_t>(std::max(settings.targetLengthSamples, 512));
    const size_t targetLength = std::min(requestedTargetLength, deconvolved.size());
    if (targetLength == 0) {
        return analysis;
    }
    analysis.preRollFrames = std::min<size_t>(std::min(targetLength / 8, size_t{512}), peakIndex);
    const size_t impulseStart = peakIndex - analysis.preRollFrames;
    analysis.impulseStartSample = impulseStart;
    const size_t availableLength = std::min(targetLength, deconvolved.size() - impulseStart);
    analysis.impulseResponse.assign(deconvolved.begin() + static_cast<std::ptrdiff_t>(impulseStart),
                                    deconvolved.begin() + static_cast<std::ptrdiff_t>(impulseStart + availableLength));
    analysis.analysisWindowLengthFrames = analysis.impulseResponse.size();
    analysis.impulsePeakAmplitude = maxAbsSample(analysis.impulseResponse);
    analysis.impulsePeakDb = absolutePeakDb(analysis.impulseResponse);
    analysis.impulseRmsDb = rmsDbFromSamples(analysis.impulseResponse);
    const size_t excludeRadius = std::clamp(analysis.impulseResponse.size() / 64, size_t{16}, size_t{2048});
    const size_t impulsePeakIndex = maxAbsIndex(analysis.impulseResponse);
    const double noiseRms = rmsExcludingRange(analysis.impulseResponse, impulsePeakIndex, excludeRadius);
    analysis.impulsePeakToNoiseDb =
        noiseRms <= 1.0e-12 ? 120.0 : 20.0 * std::log10(std::max(analysis.impulsePeakAmplitude, 1.0e-12) / noiseRms);
    return analysis;
}

void trimImpulseToPreRoll(std::vector<double>& impulseResponse,
                          size_t& preRollFrames,
                          size_t targetPreRollFrames) {
    if (preRollFrames <= targetPreRollFrames || impulseResponse.empty()) {
        return;
    }

    const size_t trimFrames = std::min(preRollFrames - targetPreRollFrames, impulseResponse.size());
    impulseResponse.erase(impulseResponse.begin(),
                          impulseResponse.begin() + static_cast<std::ptrdiff_t>(trimFrames));
    preRollFrames -= trimFrames;
}

void trimChannelAnalysisToPreRoll(ChannelAnalysis& analysis, size_t targetPreRollFrames) {
    if (analysis.preRollFrames <= targetPreRollFrames || analysis.impulseResponse.empty()) {
        return;
    }

    const size_t trimFrames = std::min(analysis.preRollFrames - targetPreRollFrames, analysis.impulseResponse.size());
    analysis.impulseResponse.erase(analysis.impulseResponse.begin(),
                                   analysis.impulseResponse.begin() + static_cast<std::ptrdiff_t>(trimFrames));
    analysis.preRollFrames -= trimFrames;
    analysis.impulseStartSample += trimFrames;
}

void appendValueSetIfValid(MeasurementResult& result, MeasurementValueSet valueSet) {
    if (!valueSet.key.empty() && valueSet.valid()) {
        result.valueSets.push_back(std::move(valueSet));
    }
}

}  // namespace

double amplitudeDbFromPcm16(const int16_t* samples, size_t count) {
    return amplitudeToDb(rmsFromPcm16(samples, count));
}

double sweepFrequencyAtSample(const MeasurementSettings& settings,
                              int sampleRate,
                              size_t sampleIndex,
                              size_t totalSamples) {
    if (totalSamples == 0) {
        return std::max(1.0, settings.startFrequencyHz);
    }

    const double startHz = std::max(1.0, settings.startFrequencyHz);
    const double nyquist = std::max(2.0, static_cast<double>(sampleRate) * 0.5);
    const double endHz = clampValue(settings.endFrequencyHz, startHz, nyquist);
    const double position = clampValue(static_cast<double>(sampleIndex) / static_cast<double>(totalSamples), 0.0, 1.0);
    if (endHz <= startHz + 1.0e-9) {
        return startHz;
    }
    return startHz * std::pow(endHz / startHz, position);
}

int configuredLoopbackLatencySamples(const MeasurementSettings& settings, int sampleRate) {
    if (settings.loopbackLatencySamples <= 0) {
        return 0;
    }
    if (settings.loopbackLatencySampleRate <= 0 || settings.loopbackLatencySampleRate == sampleRate) {
        return settings.loopbackLatencySamples;
    }

    const double latencySeconds = static_cast<double>(settings.loopbackLatencySamples) /
                                  static_cast<double>(settings.loopbackLatencySampleRate);
    return std::max(0, static_cast<int>(std::lround(latencySeconds * static_cast<double>(sampleRate))));
}

LoopbackDelayEstimate estimateLoopbackDelayFromCapture(const std::vector<int16_t>& capturedSamples,
                                                       const std::vector<double>& referenceSignal,
                                                       size_t leadInFrames,
                                                       int sampleRate,
                                                       const MeasurementSettings& settings) {
    (void)settings;
    LoopbackDelayEstimate estimate;
    estimate.clippingDetected = containsClipping(capturedSamples);
    if (capturedSamples.empty() || referenceSignal.empty()) {
        estimate.tooQuiet = true;
        return estimate;
    }

    const std::vector<double> captured = pcm16ToDouble(capturedSamples);
    estimate.tooQuiet = maxAbsSample(captured) < 1.0e-4;

    const std::vector<ReferencePulse> pulses = findReferencePulses(referenceSignal);
    if (pulses.empty()) {
        return estimate;
    }

    const size_t sweepFrames = referenceSignal.size();
    const size_t segmentFrames = leadInFrames + sweepFrames;
    const int maxLatencySamples = std::max(sampleRate / 2, sampleRate / 10);

    const CorrelationPeak leftPeak = correlatePulseTrainAtSegment(captured,
                                                                  pulses,
                                                                  leadInFrames,
                                                                  maxLatencySamples);
    const CorrelationPeak rightPeak = correlatePulseTrainAtSegment(captured,
                                                                   pulses,
                                                                   segmentFrames + leadInFrames,
                                                                   maxLatencySamples);
    const CorrelationPeak bestPeak = rightPeak.score > leftPeak.score ? rightPeak : leftPeak;

    estimate.peakIndex = bestPeak.peakIndex;
    estimate.peakAmplitude = bestPeak.score;
    estimate.peakToNoiseDb = bestPeak.peakToNoiseDb;
    estimate.latencySamples = bestPeak.delaySamples;
    if (!bestPeak.valid || estimate.clippingDetected || estimate.tooQuiet || estimate.peakToNoiseDb < 20.0) {
        return estimate;
    }

    estimate.success = true;
    return estimate;
}

MeasurementResult buildMeasurementResultFromCapture(const std::vector<int16_t>& capturedSamples,
                                                    const SweepPlaybackPlan& playbackPlan,
                                                    int sampleRate,
                                                    const AudioSettings& audioSettings,
                                                    const MeasurementSettings& settings) {
    MeasurementResult result;
    result.analysis.requestedDriver = audioSettings.driver;
    result.analysis.requestedMicInputChannel = audioSettings.micInputChannel;
    result.analysis.requestedLeftOutputChannel = audioSettings.leftOutputChannel;
    result.analysis.requestedRightOutputChannel = audioSettings.rightOutputChannel;
    result.analysis.sampleRate = sampleRate;
    result.analysis.sweepDurationSeconds = settings.durationSeconds;
    result.analysis.fadeInSeconds = settings.fadeInSeconds;
    result.analysis.fadeOutSeconds = settings.fadeOutSeconds;
    result.analysis.startFrequencyHz = settings.startFrequencyHz;
    result.analysis.endFrequencyHz = settings.endFrequencyHz;
    result.analysis.targetLengthSamples = settings.targetLengthSamples;
    result.analysis.leadInSamples = settings.leadInSamples;
    result.analysis.outputVolumeDb = audioSettings.outputVolumeDb;
    result.analysis.configuredLoopbackLatencySamples = configuredLoopbackLatencySamples(settings, sampleRate);
    result.analysis.configuredLoopbackLatencySampleRate = settings.loopbackLatencySampleRate;
    result.analysis.playedSweepSamples = static_cast<int>(playbackPlan.playedSweep.size());
    result.analysis.capturedSamples = static_cast<int>(capturedSamples.size());
    result.analysis.alignmentMethod = "Deconvolved impulse peak per sweep segment";
    result.analysis.windowType = "Cosine-tapered analysis window";
    if (playbackPlan.playedSweep.empty()) {
        return result;
    }

    const std::vector<double> captured = pcm16ToDouble(capturedSamples);
    result.analysis.captureClippingDetected = containsClipping(capturedSamples);
    result.analysis.captureTooQuiet = maxAbsSample(captured) < 1.0e-4;
    result.analysis.capturePeakDb = absolutePeakDb(captured);
    result.analysis.captureRmsDb = rmsDbFromPcmVector(capturedSamples);
    const size_t captureNoiseFrames = std::min<size_t>(std::max<size_t>(playbackPlan.leadInFrames, 64), captured.size());
    if (captureNoiseFrames > 0) {
        const std::vector<double> leadingNoise(captured.begin(), captured.begin() + static_cast<std::ptrdiff_t>(captureNoiseFrames));
        result.analysis.captureNoiseFloorDb = rmsDbFromSamples(leadingNoise);
    }

    const InverseSweepFilter inverseFilter = buildInverseSweepFilter(playbackPlan.playedSweep,
                                                                     settings,
                                                                     sampleRate);
    result.analysis.inverseFilterLengthSamples = static_cast<int>(inverseFilter.samples.size());
    result.analysis.inverseFilterPeakIndex = static_cast<int>(inverseFilter.referencePeakIndex);
    if (inverseFilter.samples.empty()) {
        return result;
    }

    const size_t alignmentSearchFrames =
        static_cast<size_t>(std::max(sampleRate / 4, result.analysis.configuredLoopbackLatencySamples + (sampleRate / 20)));
    result.analysis.alignmentSearchSamples = static_cast<int>(alignmentSearchFrames);
    ChannelAnalysis leftAnalysis = analyzeSweepSegment(capturedSamples,
                                                       0,
                                                       playbackPlan,
                                                       alignmentSearchFrames,
                                                       inverseFilter,
                                                       sampleRate,
                                                       settings);
    ChannelAnalysis rightAnalysis = analyzeSweepSegment(capturedSamples,
                                                        playbackPlan.segmentFrames,
                                                        playbackPlan,
                                                        alignmentSearchFrames,
                                                        inverseFilter,
                                                        sampleRate,
                                                        settings);
    if (leftAnalysis.impulseResponse.empty() || rightAnalysis.impulseResponse.empty()) {
        return result;
    }

    const size_t commonPreRollFrames = std::min(leftAnalysis.preRollFrames, rightAnalysis.preRollFrames);
    trimChannelAnalysisToPreRoll(leftAnalysis, commonPreRollFrames);
    trimChannelAnalysisToPreRoll(rightAnalysis, commonPreRollFrames);

    const size_t impulseLength = std::min(leftAnalysis.impulseResponse.size(), rightAnalysis.impulseResponse.size());
    if (impulseLength == 0) {
        return result;
    }

    leftAnalysis.impulseResponse.resize(impulseLength);
    rightAnalysis.impulseResponse.resize(impulseLength);

    MeasurementValueSet rawImpulseResponse;
    rawImpulseResponse.key = "measurement.raw_impulse_response";
    rawImpulseResponse.xQuantity = "time";
    rawImpulseResponse.xUnit = "seconds";
    rawImpulseResponse.yQuantity = "amplitude";
    rawImpulseResponse.yUnit = "linear";
    rawImpulseResponse.xValues = makeTimeAxisSeconds(impulseLength, commonPreRollFrames, sampleRate);
    rawImpulseResponse.leftValues = leftAnalysis.impulseResponse;
    rawImpulseResponse.rightValues = rightAnalysis.impulseResponse;
    appendValueSetIfValid(result, std::move(rawImpulseResponse));

    std::vector<double> leftWindowedImpulse = leftAnalysis.impulseResponse;
    std::vector<double> rightWindowedImpulse = rightAnalysis.impulseResponse;
    leftAnalysis.analysisWindowFadeFrames = applyAnalysisWindow(leftWindowedImpulse, commonPreRollFrames);
    rightAnalysis.analysisWindowFadeFrames = applyAnalysisWindow(rightWindowedImpulse, commonPreRollFrames);
    leftAnalysis.analysisWindowLengthFrames = leftWindowedImpulse.size();
    rightAnalysis.analysisWindowLengthFrames = rightWindowedImpulse.size();

    const size_t fftSize = nextPowerOfTwo(std::max({leftWindowedImpulse.size(), rightWindowedImpulse.size(), size_t{4096}}));
    const std::vector<std::complex<double>> leftSpectrum = fftOfRealSignal(leftWindowedImpulse, fftSize);
    const std::vector<std::complex<double>> rightSpectrum = fftOfRealSignal(rightWindowedImpulse, fftSize);
    if (leftSpectrum.empty() || rightSpectrum.empty()) {
        return result;
    }

    appendValueSetIfValid(result,
                          buildFullSpectrumValueSet("measurement.raw_magnitude_spectrum",
                                                    "level",
                                                    "dB",
                                                    leftSpectrum,
                                                    rightSpectrum,
                                                    sampleRate,
                                                    true));
    appendValueSetIfValid(result,
                          buildFullSpectrumValueSet("measurement.raw_phase_spectrum",
                                                    "phase",
                                                    "degrees",
                                                    leftSpectrum,
                                                    rightSpectrum,
                                                    sampleRate,
                                                    false));

    const size_t positiveBinCount = (std::min(leftSpectrum.size(), rightSpectrum.size()) / 2) + 1;
    if (positiveBinCount == 0) {
        return result;
    }

    const size_t displayPointCount = std::min(positiveBinCount,
                                              std::max<size_t>(256,
                                                               std::min<size_t>(2048, positiveBinCount / 4)));
    result.analysis.fftSize = static_cast<int>(fftSize);
    result.analysis.displayPointCount = static_cast<int>(displayPointCount);
    const std::vector<double> displayFrequencyAxisHz = buildLogFrequencyAxis(settings,
                                                                             sampleRate,
                                                                             displayPointCount);

    MeasurementValueSet rawMagnitudeResponse = buildMagnitudeResponseValueSet("measurement.raw_magnitude_response",
                                                                              displayFrequencyAxisHz,
                                                                              leftSpectrum,
                                                                              rightSpectrum,
                                                                              sampleRate);
    MeasurementValueSet rawPhaseResponse = buildPhaseResponseValueSet("measurement.raw_phase_response",
                                                                      displayFrequencyAxisHz,
                                                                      leftSpectrum,
                                                                      rightSpectrum,
                                                                      sampleRate);
    appendValueSetIfValid(result, rawMagnitudeResponse);
    appendValueSetIfValid(result, rawPhaseResponse);

    const bool hasMicrophoneCalibration =
        audioSettings.microphoneCalibrationFrequencyHz.size() >= 2 &&
        audioSettings.microphoneCalibrationFrequencyHz.size() == audioSettings.microphoneCalibrationCorrectionDb.size();
    if (hasMicrophoneCalibration) {
        MeasurementValueSet calibratedMagnitudeSpectrum =
            buildFullSpectrumValueSet("measurement.calibrated_magnitude_spectrum",
                                      "level",
                                      "dB",
                                      leftSpectrum,
                                      rightSpectrum,
                                      sampleRate,
                                      true);
        applyMicrophoneCalibration(audioSettings, calibratedMagnitudeSpectrum);
        appendValueSetIfValid(result, std::move(calibratedMagnitudeSpectrum));

        MeasurementValueSet calibratedMagnitudeResponse = rawMagnitudeResponse;
        calibratedMagnitudeResponse.key = "measurement.calibrated_magnitude_response";
        applyMicrophoneCalibration(audioSettings, calibratedMagnitudeResponse);
        appendValueSetIfValid(result, std::move(calibratedMagnitudeResponse));
    }

    auto fillChannelMetrics = [&](MeasurementChannelMetrics& metrics, const ChannelAnalysis& analysis) {
        metrics.available = !analysis.impulseResponse.empty();
        metrics.detectedLatencySamples = analysis.detectedLatencySamples;
        metrics.onsetSampleIndex = static_cast<int>(playbackPlan.leadInFrames) + analysis.detectedLatencySamples;
        metrics.onsetTimeSeconds =
            static_cast<double>(metrics.onsetSampleIndex) / static_cast<double>(std::max(sampleRate, 1));
        metrics.peakSampleIndex = static_cast<int>(analysis.peakSampleIndex);
        metrics.impulseStartSample = static_cast<int>(analysis.impulseStartSample);
        metrics.impulseLengthSamples = static_cast<int>(analysis.impulseResponse.size());
        metrics.preRollSamples = static_cast<int>(analysis.preRollFrames);
        metrics.analysisWindowStartSample = static_cast<int>(analysis.impulseStartSample);
        metrics.analysisWindowLengthSamples = static_cast<int>(analysis.analysisWindowLengthFrames);
        metrics.analysisWindowFadeSamples = static_cast<int>(analysis.analysisWindowFadeFrames);
        metrics.capturePeakDb = analysis.capturePeakDb;
        metrics.captureRmsDb = analysis.captureRmsDb;
        metrics.noiseFloorDb = analysis.noiseFloorDb;
        metrics.impulsePeakAmplitude = analysis.impulsePeakAmplitude;
        metrics.impulsePeakDb = analysis.impulsePeakDb;
        metrics.impulseRmsDb = analysis.impulseRmsDb;
        metrics.impulsePeakToNoiseDb = analysis.impulsePeakToNoiseDb;
    };

    fillChannelMetrics(result.analysis.left, leftAnalysis);
    fillChannelMetrics(result.analysis.right, rightAnalysis);

    return result;
}

}  // namespace wolfie::measurement
