#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

double amplitudeDbFromPcm16(const int16_t* samples, size_t count);
double sweepFrequencyAtSample(const MeasurementSettings& settings,
                              int sampleRate,
                              size_t sampleIndex,
                              size_t totalSamples);
MeasurementResult buildMeasurementResultFromCapture(const std::vector<int16_t>& capturedSamples,
                                                    const std::vector<float>& playedSweep,
                                                    size_t leadInFrames,
                                                    int sampleRate,
                                                    const MeasurementSettings& settings);

}  // namespace wolfie::measurement
