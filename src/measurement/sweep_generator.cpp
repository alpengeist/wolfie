#include "measurement/sweep_generator.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace wolfie::measurement {

namespace {

constexpr double kMutedOutputVolumeDb = -100.0;

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

std::vector<double> generateSweepSamples(const MeasurementSettings& settings, int sampleRate) {
    const int totalSamples = std::max(1, static_cast<int>(std::round(settings.durationSeconds * sampleRate)));
    const double duration = std::max(0.1, settings.durationSeconds);
    const double startHz = std::max(1.0, settings.startFrequencyHz);
    const double nyquist = std::max(2.0, static_cast<double>(sampleRate) * 0.5);
    const double endHz = clampValue(settings.endFrequencyHz, startHz, nyquist);
    const double logSpan = std::log(endHz / startHz);

    std::vector<double> samples(totalSamples, 0.0);
    constexpr double twoPi = 2.0 * 3.14159265358979323846;
    const double growth = logSpan > 1.0e-9 ? duration / logSpan : 0.0;
    const double phaseScale = logSpan > 1.0e-9 ? twoPi * startHz * growth : 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(sampleRate);
        const double phase = logSpan > 1.0e-9
            ? phaseScale * (std::exp(t / growth) - 1.0)
            : twoPi * startHz * t;

        double envelope = 1.0;
        if (t < settings.fadeInSeconds) {
            envelope = t / std::max(0.01, settings.fadeInSeconds);
        } else if (t > duration - settings.fadeOutSeconds) {
            envelope = std::max(0.0, (duration - t) / std::max(0.01, settings.fadeOutSeconds));
        }
        samples[i] = std::sin(phase) * envelope;
    }

    double peak = 0.0;
    for (const double sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    if (peak > 0.0) {
        for (double& sample : samples) {
            sample /= peak;
        }
    }
    return samples;
}

std::vector<double> scaleSweepSamples(const std::vector<double>& samples, double volumeDb) {
    if (volumeDb <= kMutedOutputVolumeDb) {
        return std::vector<double>(samples.size(), 0.0);
    }

    const double gain = clampValue(std::pow(10.0, volumeDb / 20.0), 0.0, 1.0);
    std::vector<double> scaled(samples.size(), 0.0);
    for (size_t i = 0; i < samples.size(); ++i) {
        scaled[i] = samples[i] * gain;
    }
    return scaled;
}

int16_t sampleToPcm16(double sample) {
    const double clamped = clampValue(sample, -1.0, 1.0);
    return static_cast<int16_t>(std::round(clamped * 32767.0));
}

std::vector<int16_t> buildStereoSweepPcm(const std::vector<double>& sweepSamples,
                                         int leadInSamples,
                                         size_t postRollFrames) {
    const size_t safeLeadIn = static_cast<size_t>(std::max(0, leadInSamples));
    const size_t segmentFrames = safeLeadIn + sweepSamples.size() + postRollFrames;
    const size_t totalFrames = segmentFrames * 2;
    std::vector<int16_t> pcm(totalFrames * 2, 0);

    for (size_t i = 0; i < sweepSamples.size(); ++i) {
        const size_t leftFrame = safeLeadIn + i;
        const size_t rightFrame = segmentFrames + safeLeadIn + i;
        pcm[leftFrame * 2] = sampleToPcm16(sweepSamples[i]);
        pcm[rightFrame * 2 + 1] = sampleToPcm16(sweepSamples[i]);
    }

    return pcm;
}

}  // namespace

double defaultSweepEndFrequencyHz(int sampleRate) {
    return sampleRate == 44100 ? 22050.0 : 24000.0;
}

void syncDerivedMeasurementSettings(MeasurementSettings& settings) {
    settings.endFrequencyHz = defaultSweepEndFrequencyHz(settings.sampleRate);
}

SweepPlaybackPlan buildSweepPlaybackPlan(const MeasurementSettings& settings, double outputVolumeDb) {
    const int sampleRate = std::max(8000, settings.sampleRate);

    SweepPlaybackPlan plan;
    plan.leadInFrames = static_cast<size_t>(std::max(0, settings.leadInSamples));
    plan.playedSweep = scaleSweepSamples(generateSweepSamples(settings, sampleRate), outputVolumeDb);
    plan.sweepFrames = plan.playedSweep.size();
    plan.postRollFrames = static_cast<size_t>(std::max(settings.targetLengthSamples, sampleRate / 5));
    plan.segmentFrames = plan.leadInFrames + plan.sweepFrames + plan.postRollFrames;
    plan.totalFrames = plan.segmentFrames * 2;
    plan.playbackPcm = buildStereoSweepPcm(plan.playedSweep, settings.leadInSamples, plan.postRollFrames);
    return plan;
}

bool writeStereoWaveFile(const std::filesystem::path& path,
                         const std::vector<int16_t>& interleavedSamples,
                         int sampleRate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const uint16_t channels = 2;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t dataSize = static_cast<uint32_t>(interleavedSamples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36 + dataSize;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    const uint32_t fmtSize = 16;
    const uint16_t formatTag = 1;
    out.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    out.write(reinterpret_cast<const char*>(&formatTag), sizeof(formatTag));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    out.write(reinterpret_cast<const char*>(interleavedSamples.data()), static_cast<std::streamsize>(dataSize));

    return static_cast<bool>(out);
}

bool writeMonoWaveFile(const std::filesystem::path& path,
                       const std::vector<int16_t>& samples,
                       int sampleRate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36 + dataSize;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    const uint32_t fmtSize = 16;
    const uint16_t formatTag = 1;
    out.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    out.write(reinterpret_cast<const char*>(&formatTag), sizeof(formatTag));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataSize));

    return static_cast<bool>(out);
}

}  // namespace wolfie::measurement
