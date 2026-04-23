#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

struct SweepPlaybackPlan {
    std::vector<double> playedSweep;
    std::vector<int16_t> playbackPcm;
    size_t leadInFrames = 0;
    size_t sweepFrames = 0;
    size_t segmentFrames = 0;
    size_t totalFrames = 0;
};

double defaultSweepEndFrequencyHz(int sampleRate);
void syncDerivedMeasurementSettings(MeasurementSettings& settings);
SweepPlaybackPlan buildSweepPlaybackPlan(const MeasurementSettings& settings, double outputVolumeDb);
bool writeStereoWaveFile(const std::filesystem::path& path,
                         const std::vector<int16_t>& interleavedSamples,
                         int sampleRate);

}  // namespace wolfie::measurement
