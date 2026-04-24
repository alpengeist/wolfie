#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/models.h"
#include "measurement/response_analyzer.h"
#include "measurement/sweep_generator.h"

namespace {

int16_t sampleToPcm16(double sample) {
    const double clamped = std::max(-1.0, std::min(sample, 1.0));
    return static_cast<int16_t>(std::lround(clamped * 32767.0));
}

std::vector<int16_t> synthesizeLoopbackCapture(const wolfie::measurement::SweepPlaybackPlan& plan,
                                               size_t segmentOffset,
                                               int delaySamples,
                                               double gain) {
    std::vector<int16_t> capture(plan.totalFrames + static_cast<size_t>(delaySamples) + 256, 0);
    const size_t sweepStart = segmentOffset + plan.leadInFrames + static_cast<size_t>(delaySamples);
    for (size_t i = 0; i < plan.playedSweep.size() && sweepStart + i < capture.size(); ++i) {
        capture[sweepStart + i] = sampleToPcm16(plan.playedSweep[i] * gain);
    }
    return capture;
}

bool expectDelay(const std::string& name, size_t segmentOffset, int expectedDelay) {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.leadInSamples = 2048;

    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildLoopbackCalibrationPlaybackPlan(settings, -12.0);
    const std::vector<int16_t> capture = synthesizeLoopbackCapture(plan, segmentOffset, expectedDelay, 0.75);
    const wolfie::measurement::LoopbackDelayEstimate estimate =
        wolfie::measurement::estimateLoopbackDelayFromCapture(capture,
                                                              plan.playedSweep,
                                                              plan.leadInFrames,
                                                              settings.sampleRate,
                                                              settings);

    if (!estimate.success || std::abs(estimate.latencySamples - expectedDelay) > 1) {
        std::cerr << name << " failed: expected " << expectedDelay
                  << " samples, got " << estimate.latencySamples
                  << ", success=" << estimate.success
                  << ", peakToNoiseDb=" << estimate.peakToNoiseDb << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.loopbackLatencySamples = 441;
    settings.loopbackLatencySampleRate = 44100;
    if (wolfie::measurement::configuredLoopbackLatencySamples(settings, 48000) != 480) {
        std::cerr << "sample-rate-scaled latency failed\n";
        return 1;
    }

    wolfie::MeasurementSettings loopbackSettings;
    loopbackSettings.sampleRate = 48000;
    loopbackSettings.leadInSamples = 2048;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildLoopbackCalibrationPlaybackPlan(loopbackSettings, -12.0);

    bool ok = true;
    ok = expectDelay("left loopback delay", 0, 317) && ok;
    ok = expectDelay("right loopback delay", plan.segmentFrames, 913) && ok;
    return ok ? 0 : 1;
}
