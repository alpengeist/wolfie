#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "test_harness.h"

#include "core/models.h"
#include "measurement/response_analyzer.h"
#include "measurement/sweep_generator.h"
#include "measurement/waterfall_builder.h"

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

std::vector<int16_t> synthesizeMeasurementCapture(const wolfie::measurement::SweepPlaybackPlan& plan,
                                                  int leftDelaySamples,
                                                  int rightDelaySamples,
                                                  double leftGain,
                                                  double rightGain) {
    const size_t captureLength =
        plan.totalFrames + static_cast<size_t>(std::max(leftDelaySamples, rightDelaySamples)) + plan.postRollFrames + 1024;
    std::vector<int16_t> capture(captureLength, 0);

    const size_t leftSweepStart = plan.leadInFrames + static_cast<size_t>(leftDelaySamples);
    const size_t rightSweepStart = plan.segmentFrames + plan.leadInFrames + static_cast<size_t>(rightDelaySamples);
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

std::vector<double> buildLinearFrequencyAxis(double maxFrequencyHz, size_t pointCount) {
    std::vector<double> axis;
    axis.reserve(pointCount);
    for (size_t index = 0; index < pointCount; ++index) {
        const double t = pointCount <= 1 ? 0.0 : static_cast<double>(index) / static_cast<double>(pointCount - 1);
        axis.push_back(maxFrequencyHz * t);
    }
    return axis;
}

wolfie::MeasurementResult buildSyntheticReferenceResult(int sampleRate,
                                                        double magnitudeOffsetDb,
                                                        double phaseOffsetDegrees) {
    wolfie::MeasurementResult result;
    result.analysis.sampleRate = sampleRate;
    const std::vector<double> positiveAxisHz = buildLinearFrequencyAxis(static_cast<double>(sampleRate) * 0.5, 4097);

    auto makeSpectrum = [&](const std::string& magnitudeKey,
                            const std::string& phaseKey) {
        wolfie::MeasurementValueSet magnitude;
        magnitude.key = magnitudeKey;
        magnitude.xValues = positiveAxisHz;
        magnitude.leftValues.assign(positiveAxisHz.size(), magnitudeOffsetDb);
        magnitude.rightValues.assign(positiveAxisHz.size(), magnitudeOffsetDb);

        wolfie::MeasurementValueSet phase;
        phase.key = phaseKey;
        phase.yQuantity = "phase";
        phase.yUnit = "degrees";
        phase.xValues = positiveAxisHz;
        phase.leftValues.assign(positiveAxisHz.size(), phaseOffsetDegrees);
        phase.rightValues.assign(positiveAxisHz.size(), phaseOffsetDegrees);

        result.valueSets.push_back(std::move(magnitude));
        result.valueSets.push_back(std::move(phase));
    };

    makeSpectrum("measurement.raw_magnitude_spectrum", "measurement.raw_phase_spectrum");
    makeSpectrum("measurement.room_magnitude_spectrum", "measurement.room_phase_spectrum");
    makeSpectrum("measurement.direct_magnitude_spectrum", "measurement.direct_phase_spectrum");
    return result;
}

double meanBandDelta(const std::vector<double>& leftValues,
                     const std::vector<double>& rightValues,
                     const std::vector<double>& frequencyAxisHz,
                     double minFrequencyHz,
                     double maxFrequencyHz) {
    const size_t count = std::min({leftValues.size(), rightValues.size(), frequencyAxisHz.size()});
    double sum = 0.0;
    size_t used = 0;
    for (size_t index = 0; index < count; ++index) {
        if (frequencyAxisHz[index] < minFrequencyHz || frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        sum += rightValues[index] - leftValues[index];
        ++used;
    }
    return used == 0 ? 0.0 : sum / static_cast<double>(used);
}

double meanBandLevel(const std::vector<double>& values,
                     const std::vector<double>& frequencyAxisHz,
                     double minFrequencyHz,
                     double maxFrequencyHz) {
    const size_t count = std::min(values.size(), frequencyAxisHz.size());
    double sum = 0.0;
    size_t used = 0;
    for (size_t index = 0; index < count; ++index) {
        if (frequencyAxisHz[index] < minFrequencyHz || frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        sum += values[index];
        ++used;
    }
    return used == 0 ? 0.0 : sum / static_cast<double>(used);
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

    const wolfie::MeasurementValueSet* canonical = result.findValueSet("measurement.magnitude_response");
    if (canonical == nullptr || !canonical->valid()) {
        std::cerr << "measurement result is missing the canonical magnitude response\n";
        return false;
    }

    if (result.magnitudeResponse() != canonical ||
        canonical->xValues != magnitude->xValues ||
        canonical->leftValues != magnitude->leftValues ||
        canonical->rightValues != magnitude->rightValues) {
        std::cerr << "canonical magnitude response did not match the raw response without calibration\n";
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

bool expectMeasurementPublishesSeparateDirectAndRoomAnalysisProducts() {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.durationSeconds = 1.0;
    settings.fadeInSeconds = 0.05;
    settings.fadeOutSeconds = 0.05;
    settings.leadInSamples = 1024;
    settings.targetLengthSamples = 4096;

    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0);
    const std::vector<int16_t> capture = synthesizeMeasurementCapture(plan, 180, 0.8, 0.5);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings);

    const wolfie::MeasurementValueSet* directImpulse = result.findValueSet("measurement.direct_impulse_response");
    const wolfie::MeasurementValueSet* roomImpulse = result.findValueSet("measurement.room_impulse_response");
    const wolfie::MeasurementValueSet* directMagnitude = result.findValueSet("measurement.direct_magnitude_response");
    const wolfie::MeasurementValueSet* roomMagnitude = result.findValueSet("measurement.room_magnitude_response");
    const wolfie::MeasurementValueSet* directPhase = result.findValueSet("measurement.direct_phase_response");
    const wolfie::MeasurementValueSet* roomPhase = result.findValueSet("measurement.room_phase_response");
    if (directImpulse == nullptr || !directImpulse->valid() ||
        roomImpulse == nullptr || !roomImpulse->valid() ||
        directMagnitude == nullptr || !directMagnitude->valid() ||
        roomMagnitude == nullptr || !roomMagnitude->valid() ||
        directPhase == nullptr || !directPhase->valid() ||
        roomPhase == nullptr || !roomPhase->valid()) {
        std::cerr << "measurement result is missing direct/room analysis products\n";
        return false;
    }

    if (directImpulse->xValues.size() >= roomImpulse->xValues.size()) {
        std::cerr << "direct impulse window was not shorter than the room window\n";
        return false;
    }
    if (directMagnitude->xValues != roomMagnitude->xValues ||
        directPhase->xValues != roomPhase->xValues) {
        std::cerr << "direct and room transfer products do not share the display axis\n";
        return false;
    }

    return true;
}

bool expectReferenceLoopbackStaysFlatIntoLowBass() {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.durationSeconds = 15.0;
    settings.fadeInSeconds = 0.05;
    settings.fadeOutSeconds = 0.1;
    settings.leadInSamples = 6000;
    settings.targetLengthSamples = 65536;

    const int delaySamples = 180;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0, wolfie::MeasurementRunMode::Reference);
    const std::vector<int16_t> capture = synthesizeMeasurementCapture(plan,
                                                                      delaySamples,
                                                                      0.8,
                                                                      0.8);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings);

    const wolfie::MeasurementValueSet* magnitude = result.findValueSet("measurement.magnitude_response");
    if (magnitude == nullptr || !magnitude->valid()) {
        std::cerr << "reference loopback magnitude response is missing\n";
        return false;
    }

    const double midBandDb = meanBandLevel(magnitude->leftValues, magnitude->xValues, 200.0, 1000.0);
    const double bassBandDb = meanBandLevel(magnitude->leftValues, magnitude->xValues, 30.0, 100.0);
    const double edgeBandDb = meanBandLevel(magnitude->leftValues, magnitude->xValues, 20.0, 25.0);
    if (std::abs(bassBandDb - midBandDb) > 0.75) {
        std::cerr << "reference loopback lost low-bass flatness (bass-mid delta="
                  << (bassBandDb - midBandDb) << " dB)\n";
        return false;
    }
    if (std::abs(edgeBandDb - midBandDb) > 1.5) {
        std::cerr << "reference loopback rolled off at the sweep start (20-25 Hz delta="
                  << (edgeBandDb - midBandDb) << " dB)\n";
        return false;
    }

    return true;
}

bool expectMeasurementPublishesReferenceCompensatedTransferProducts() {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.durationSeconds = 1.0;
    settings.fadeInSeconds = 0.05;
    settings.fadeOutSeconds = 0.05;
    settings.leadInSamples = 1024;
    settings.targetLengthSamples = 4096;

    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0);
    const std::vector<int16_t> capture = synthesizeMeasurementCapture(plan, 180, 0.8, 0.5);
    const wolfie::MeasurementResult referenceResult =
        buildSyntheticReferenceResult(settings.sampleRate, 6.0, 0.0);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings,
                                                               &referenceResult);

    const wolfie::MeasurementValueSet* rawRoom = result.findValueSet("measurement.room_magnitude_response");
    const wolfie::MeasurementValueSet* compensatedRoom =
        result.findValueSet("measurement.reference_compensated_room_magnitude_response");
    const wolfie::MeasurementValueSet* compensatedDirect =
        result.findValueSet("measurement.reference_compensated_direct_magnitude_response");
    if (rawRoom == nullptr || !rawRoom->valid() ||
        compensatedRoom == nullptr || !compensatedRoom->valid() ||
        compensatedDirect == nullptr || !compensatedDirect->valid()) {
        std::cerr << "reference-compensated transfer products were not published\n";
        return false;
    }

    const double roomDeltaDb =
        meanBandDelta(rawRoom->leftValues, compensatedRoom->leftValues, rawRoom->xValues, 40.0, 5000.0);
    if (std::abs(roomDeltaDb + 6.0) > 0.4) {
        std::cerr << "reference compensation did not apply the expected gain offset (delta="
                  << roomDeltaDb << " dB)\n";
        return false;
    }

    return true;
}

bool expectMeasurementRetainsStereoArrivalMismatchForLaterAlignment() {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = 48000;
    settings.durationSeconds = 1.0;
    settings.fadeInSeconds = 0.05;
    settings.fadeOutSeconds = 0.05;
    settings.leadInSamples = 1024;
    settings.targetLengthSamples = 4096;

    const int leftDelaySamples = 180;
    const int rightDelaySamples = 252;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0);
    const std::vector<int16_t> capture =
        synthesizeMeasurementCapture(plan, leftDelaySamples, rightDelaySamples, 0.8, 0.5);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings);
    if (result.analysis.left.detectedLatencySamples != leftDelaySamples ||
        result.analysis.right.detectedLatencySamples != rightDelaySamples) {
        std::cerr << "measurement result did not retain the per-channel direct-arrival mismatch\n";
        return false;
    }

    return true;
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
    return wolfie::tests::runTestCases({
        {"expectMeasurementResultValueSets", expectMeasurementResultValueSets},
        {"expectSweepDeconvolutionProducesImpulseLikePeak", expectSweepDeconvolutionProducesImpulseLikePeak},
        {"expectMeasurementPublishesSeparateDirectAndRoomAnalysisProducts", expectMeasurementPublishesSeparateDirectAndRoomAnalysisProducts},
        {"expectReferenceLoopbackStaysFlatIntoLowBass", expectReferenceLoopbackStaysFlatIntoLowBass},
        {"expectMeasurementPublishesReferenceCompensatedTransferProducts", expectMeasurementPublishesReferenceCompensatedTransferProducts},
        {"expectMeasurementRetainsStereoArrivalMismatchForLaterAlignment", expectMeasurementRetainsStereoArrivalMismatchForLaterAlignment},
        {"expectWaterfallPlotData", expectWaterfallPlotData},
    });
}
