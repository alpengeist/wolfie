#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "core/models.h"
#include "measurement/response_analyzer.h"
#include "measurement/sweep_generator.h"
#include "measurement/waterfall_builder.h"

bool runFilterDesignTests();

namespace {

int16_t sampleToPcm16(double sample) {
    const double clamped = std::max(-1.0, std::min(sample, 1.0));
    return static_cast<int16_t>(std::lround(clamped * 32767.0));
}

std::vector<int16_t> synthesizeMeasurementCapture(const wolfie::measurement::SweepPlaybackPlan& plan,
                                                  int delaySamples,
                                                  double leftGain,
                                                  double rightGain) {
    const size_t captureLength = plan.totalFrames + static_cast<size_t>(delaySamples) + plan.postRollFrames + 1024;
    std::vector<int16_t> capture(captureLength, 0);

    const size_t leftSweepStart = plan.leadInFrames + static_cast<size_t>(delaySamples);
    const size_t rightSweepStart = plan.segmentFrames + plan.leadInFrames + static_cast<size_t>(delaySamples);
    for (size_t i = 0; i < plan.playedSweep.size(); ++i) {
        if (leftSweepStart + i < capture.size()) {
            capture[leftSweepStart + i] = sampleToPcm16(plan.playedSweep[i] * leftGain);
        }
        if (rightSweepStart + i < capture.size()) {
            capture[rightSweepStart + i] = sampleToPcm16(plan.playedSweep[i] * rightGain);
        }
    }

    return capture;
}

struct ImpulseSharpnessMetrics {
    size_t peakIndex = 0;
    double peakMagnitude = 0.0;
    double sideRms = 0.0;
    double sideEnergyFraction = 1.0;
};

ImpulseSharpnessMetrics analyzeImpulseSharpness(const std::vector<double>& values, size_t exclusionRadius) {
    ImpulseSharpnessMetrics metrics;
    if (values.empty()) {
        return metrics;
    }

    double totalEnergy = 0.0;
    for (size_t index = 0; index < values.size(); ++index) {
        const double magnitude = std::abs(values[index]);
        if (magnitude > metrics.peakMagnitude) {
            metrics.peakMagnitude = magnitude;
            metrics.peakIndex = index;
        }
        totalEnergy += values[index] * values[index];
    }

    const size_t exclusionBegin = metrics.peakIndex > exclusionRadius ? metrics.peakIndex - exclusionRadius : 0;
    const size_t exclusionEnd = std::min(values.size(), metrics.peakIndex + exclusionRadius + 1);
    double sideEnergy = 0.0;
    size_t sideCount = 0;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index >= exclusionBegin && index < exclusionEnd) {
            continue;
        }
        sideEnergy += values[index] * values[index];
        ++sideCount;
    }

    metrics.sideRms = sideCount == 0 ? 0.0 : std::sqrt(sideEnergy / static_cast<double>(sideCount));
    metrics.sideEnergyFraction = totalEnergy <= 1.0e-12 ? 0.0 : sideEnergy / totalEnergy;
    return metrics;
}

bool expectMeasurementResultValueSets() {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.durationSeconds = 1.0;
    settings.fadeInSeconds = 0.05;
    settings.fadeOutSeconds = 0.05;
    settings.leadInSamples = 1024;
    settings.targetLengthSamples = 4096;

    const int delaySamples = 180;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0);
    const std::vector<int16_t> capture = synthesizeMeasurementCapture(plan,
                                                                      delaySamples,
                                                                      0.8,
                                                                      0.5);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings);

    const wolfie::MeasurementValueSet* impulse = result.findValueSet("measurement.raw_impulse_response");
    const wolfie::MeasurementValueSet* magnitude = result.findValueSet("measurement.raw_magnitude_response");
    const wolfie::MeasurementValueSet* phase = result.findValueSet("measurement.raw_phase_response");
    if (impulse == nullptr || !impulse->valid() ||
        magnitude == nullptr || !magnitude->valid() ||
        phase == nullptr || !phase->valid()) {
        std::cerr << "measurement result is missing expected value sets\n";
        return false;
    }

    const auto leftPeak = std::max_element(impulse->leftValues.begin(), impulse->leftValues.end(), [](double a, double b) {
        return std::abs(a) < std::abs(b);
    });
    if (leftPeak == impulse->leftValues.end()) {
        std::cerr << "measurement impulse response has no peak\n";
        return false;
    }

    const size_t peakIndex = static_cast<size_t>(std::distance(impulse->leftValues.begin(), leftPeak));
    const double peakTimeSeconds = impulse->xValues[peakIndex];
    if (std::abs(peakTimeSeconds) > 0.01) {
        std::cerr << "measurement impulse peak is not aligned near zero: " << peakTimeSeconds << " s\n";
        return false;
    }

    if (result.analysis.left.detectedLatencySamples != delaySamples ||
        result.analysis.right.detectedLatencySamples != delaySamples) {
        std::cerr << "detected latency did not match expected synthetic delay\n";
        return false;
    }

    if (result.preferredMagnitudeResponse() != magnitude) {
        std::cerr << "preferred magnitude response did not select the raw response\n";
        return false;
    }

    return true;
}

bool expectSweepDeconvolutionProducesImpulseLikePeak() {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.durationSeconds = 1.0;
    settings.fadeInSeconds = 0.05;
    settings.fadeOutSeconds = 0.05;
    settings.leadInSamples = 1024;
    settings.targetLengthSamples = 4096;

    const int delaySamples = 180;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0);
    const std::vector<int16_t> capture = synthesizeMeasurementCapture(plan,
                                                                      delaySamples,
                                                                      0.8,
                                                                      0.5);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings);

    const wolfie::MeasurementValueSet* impulse = result.findValueSet("measurement.raw_impulse_response");
    if (impulse == nullptr || !impulse->valid()) {
        std::cerr << "deconvolution impulse response is missing\n";
        return false;
    }

    constexpr size_t kImpulseMainLobeRadius = 8;
    const ImpulseSharpnessMetrics leftMetrics =
        analyzeImpulseSharpness(impulse->leftValues, kImpulseMainLobeRadius);
    const ImpulseSharpnessMetrics rightMetrics =
        analyzeImpulseSharpness(impulse->rightValues, kImpulseMainLobeRadius);

    auto channelLooksImpulseLike = [](const ImpulseSharpnessMetrics& metrics, const char* label) {
        if (metrics.peakMagnitude <= 1.0e-6) {
            std::cerr << label << " deconvolution peak was too small\n";
            return false;
        }

        const double sideToPeakRatio = metrics.sideRms / metrics.peakMagnitude;
        if (sideToPeakRatio > 0.01) {
            std::cerr << label << " deconvolution left too much off-peak energy (ratio="
                      << sideToPeakRatio << ")\n";
            return false;
        }
        if (metrics.sideEnergyFraction > 0.01) {
            std::cerr << label << " deconvolution spread too much energy away from the main impulse (fraction="
                      << metrics.sideEnergyFraction << ")\n";
            return false;
        }
        return true;
    };

    return channelLooksImpulseLike(leftMetrics, "left") &&
           channelLooksImpulseLike(rightMetrics, "right");
}

bool expectWaterfallPlotData() {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.durationSeconds = 1.0;
    settings.fadeInSeconds = 0.05;
    settings.fadeOutSeconds = 0.05;
    settings.leadInSamples = 1024;
    settings.targetLengthSamples = 8192;

    const int delaySamples = 120;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0);
    const std::vector<int16_t> capture = synthesizeMeasurementCapture(plan,
                                                                      delaySamples,
                                                                      0.8,
                                                                      0.6);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings);
    const wolfie::measurement::WaterfallPlotData plot =
        wolfie::measurement::buildWaterfallPlotData(result, wolfie::MeasurementChannel::Left);
    if (!plot.valid()) {
        std::cerr << "waterfall plot data was not generated\n";
        return false;
    }
    if (plot.slices.size() < 6 || plot.frequencyAxisHz.size() < 64) {
        std::cerr << "waterfall plot data is too sparse\n";
        return false;
    }
    if (plot.slices.front().timeMilliseconds != 0.0) {
        std::cerr << "waterfall did not start at zero milliseconds\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    return expectMeasurementResultValueSets() &&
           expectSweepDeconvolutionProducesImpulseLikePeak() &&
           expectWaterfallPlotData() &&
           runFilterDesignTests() ? 0 : 1;
}
