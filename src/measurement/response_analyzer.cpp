#include "measurement/response_analyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

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

void applyMicrophoneCalibration(const AudioSettings& audioSettings, MeasurementResult& result) {
    if (audioSettings.microphoneCalibrationFrequencyHz.size() < 2 ||
        audioSettings.microphoneCalibrationFrequencyHz.size() != audioSettings.microphoneCalibrationCorrectionDb.size()) {
        return;
    }

    for (size_t i = 0; i < result.frequencyAxisHz.size() &&
                       i < result.leftChannelDb.size() &&
                       i < result.rightChannelDb.size(); ++i) {
        const double correctionDb = interpolateMicrophoneCalibrationDb(audioSettings, result.frequencyAxisHz[i]);
        result.leftChannelDb[i] += correctionDb;
        result.rightChannelDb[i] += correctionDb;
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
                                                    const std::vector<double>& playedSweep,
                                                    size_t leadInFrames,
                                                    int sampleRate,
                                                    const AudioSettings& audioSettings,
                                                    const MeasurementSettings& settings) {
    MeasurementResult result;
    if (playedSweep.empty()) {
        return result;
    }

    const size_t sweepFrames = playedSweep.size();
    const size_t fadeInFrames = clampValue<size_t>(
        static_cast<size_t>(std::round(std::max(0.0, settings.fadeInSeconds) * sampleRate)),
        0,
        sweepFrames);
    const size_t fadeOutFrames = clampValue<size_t>(
        static_cast<size_t>(std::round(std::max(0.0, settings.fadeOutSeconds) * sampleRate)),
        0,
        sweepFrames - fadeInFrames);
    const size_t analysisBegin = fadeInFrames;
    const size_t analysisEnd = sweepFrames - fadeOutFrames;
    if (analysisEnd <= analysisBegin) {
        return result;
    }

    const size_t analysisFrames = analysisEnd - analysisBegin;
    const size_t segmentFrames = leadInFrames + sweepFrames;
    const size_t loopbackLatencyFrames = static_cast<size_t>(
        configuredLoopbackLatencySamples(settings, sampleRate));
    const size_t pointCount = std::min<size_t>(analysisFrames, clampValue<size_t>(analysisFrames / 256, 128, 1024));
    result.frequencyAxisHz.reserve(pointCount);
    result.leftChannelDb.reserve(pointCount);
    result.rightChannelDb.reserve(pointCount);

    auto channelDb = [&](size_t segmentOffset, size_t begin, size_t end) {
        if (end <= begin) {
            return -90.0;
        }

        const size_t captureStart = segmentOffset + leadInFrames + loopbackLatencyFrames + begin;
        const size_t captureEnd = std::min(segmentOffset + leadInFrames + loopbackLatencyFrames + end, capturedSamples.size());
        if (captureEnd <= captureStart) {
            return -90.0;
        }

        const double measured = rmsFromPcm16(capturedSamples.data() + captureStart, captureEnd - captureStart);
        const double reference = rmsFromDouble(playedSweep.data() + begin, end - begin);
        return amplitudeToDb(measured / std::max(reference, 1.0e-6));
    };

    for (size_t i = 0; i < pointCount; ++i) {
        const size_t begin = analysisBegin + ((i * analysisFrames) / pointCount);
        const size_t end = std::max(begin + 1, analysisBegin + (((i + 1) * analysisFrames) / pointCount));
        const size_t center = begin + ((end - begin) / 2);

        result.frequencyAxisHz.push_back(sweepFrequencyAtSample(settings, sampleRate, center, sweepFrames));
        result.leftChannelDb.push_back(channelDb(0, begin, end));
        result.rightChannelDb.push_back(channelDb(segmentFrames, begin, end));
    }

    applyMicrophoneCalibration(audioSettings, result);
    return result;
}

}  // namespace wolfie::measurement
