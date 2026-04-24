#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/models.h"
#include "measurement/sweep_generator.h"

namespace wolfie::measurement {

struct LoopbackDelayEstimate {
    bool success = false;
    int latencySamples = 0;
    size_t peakIndex = 0;
    double peakAmplitude = 0.0;
    double peakToNoiseDb = 0.0;
    bool clippingDetected = false;
    bool tooQuiet = false;
};

double amplitudeDbFromPcm16(const int16_t* samples, size_t count);
double sweepFrequencyAtSample(const MeasurementSettings& settings,
                              int sampleRate,
                              size_t sampleIndex,
                              size_t totalSamples);
int configuredLoopbackLatencySamples(const MeasurementSettings& settings, int sampleRate);
LoopbackDelayEstimate estimateLoopbackDelayFromCapture(const std::vector<int16_t>& capturedSamples,
                                                       const std::vector<double>& referenceSignal,
                                                       size_t leadInFrames,
                                                       int sampleRate,
                                                       const MeasurementSettings& settings);
MeasurementResult buildMeasurementResultFromCapture(const std::vector<int16_t>& capturedSamples,
                                                    const SweepPlaybackPlan& playbackPlan,
                                                    int sampleRate,
                                                    const AudioSettings& audioSettings,
                                                    const MeasurementSettings& settings);

}  // namespace wolfie::measurement
