#include "measurement/response_analyzer.h"

#include <algorithm>
#include <cmath>

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

    long double energy = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const long double normalized = static_cast<long double>(samples[i]) / 32768.0L;
        energy += normalized * normalized;
    }
    return std::sqrt(static_cast<double>(energy / static_cast<long double>(count)));
}

double rmsFromFloat(const float* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0;
    }

    long double energy = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const long double normalized = static_cast<long double>(samples[i]);
        energy += normalized * normalized;
    }
    return std::sqrt(static_cast<double>(energy / static_cast<long double>(count)));
}

double amplitudeToDb(double amplitude) {
    if (amplitude <= 1.0e-6) {
        return -90.0;
    }
    return clampValue(20.0 * std::log10(amplitude), -90.0, 24.0);
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

MeasurementResult buildMeasurementResultFromCapture(const std::vector<int16_t>& capturedSamples,
                                                    const std::vector<float>& playedSweep,
                                                    size_t leadInFrames,
                                                    int sampleRate,
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
    const size_t pointCount = std::min<size_t>(analysisFrames, clampValue<size_t>(analysisFrames / 256, 128, 1024));
    result.frequencyAxisHz.reserve(pointCount);
    result.leftChannelDb.reserve(pointCount);
    result.rightChannelDb.reserve(pointCount);

    auto channelDb = [&](size_t segmentOffset, size_t begin, size_t end) {
        if (end <= begin) {
            return -90.0;
        }

        const size_t captureStart = segmentOffset + leadInFrames + begin;
        const size_t captureEnd = std::min(segmentOffset + leadInFrames + end, capturedSamples.size());
        if (captureEnd <= captureStart) {
            return -90.0;
        }

        const double measured = rmsFromPcm16(capturedSamples.data() + captureStart, captureEnd - captureStart);
        const double reference = rmsFromFloat(playedSweep.data() + begin, end - begin);
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

    return result;
}

}  // namespace wolfie::measurement
