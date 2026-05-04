#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "test_harness.h"

#include "core/models.h"
#include "measurement/response_analyzer.h"
#include "measurement/sweet_spot_alignment.h"
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

std::vector<double> convolveSignal(const std::vector<double>& signal, const std::vector<double>& impulseResponse) {
    if (signal.empty() || impulseResponse.empty()) {
        return {};
    }

    std::vector<double> output(signal.size() + impulseResponse.size() - 1, 0.0);
    for (size_t signalIndex = 0; signalIndex < signal.size(); ++signalIndex) {
        for (size_t tapIndex = 0; tapIndex < impulseResponse.size(); ++tapIndex) {
            output[signalIndex + tapIndex] += signal[signalIndex] * impulseResponse[tapIndex];
        }
    }
    return output;
}

std::vector<int16_t> synthesizeFilteredAlignmentCapture(const wolfie::measurement::SweepPlaybackPlan& plan,
                                                        int leftDelaySamples,
                                                        int rightDelaySamples,
                                                        const std::vector<double>& leftImpulseResponse,
                                                        const std::vector<double>& rightImpulseResponse,
                                                        double leftGain,
                                                        double rightGain) {
    const std::vector<double> leftSignal = convolveSignal(plan.playedSweep, leftImpulseResponse);
    const std::vector<double> rightSignal = convolveSignal(plan.playedSweep, rightImpulseResponse);
    const size_t tailFrames = std::max(leftSignal.size(), rightSignal.size());
    const size_t captureLength =
        plan.totalFrames + static_cast<size_t>(std::max(leftDelaySamples, rightDelaySamples)) + tailFrames + 1024;
    std::vector<int16_t> capture(captureLength, 0);

    const size_t leftStart = plan.leadInFrames + static_cast<size_t>(leftDelaySamples);
    const size_t rightStart = plan.segmentFrames + plan.leadInFrames + static_cast<size_t>(rightDelaySamples);
    for (size_t index = 0; index < leftSignal.size(); ++index) {
        if (leftStart + index < capture.size()) {
            capture[leftStart + index] = sampleToPcm16(leftSignal[index] * leftGain);
        }
    }
    for (size_t index = 0; index < rightSignal.size(); ++index) {
        if (rightStart + index < capture.size()) {
            capture[rightStart + index] = sampleToPcm16(rightSignal[index] * rightGain);
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

size_t impulseHalfMagnitudeWidthSamples(const std::vector<double>& values) {
    if (values.empty()) {
        return 0;
    }

    size_t peakIndex = 0;
    double peakMagnitude = 0.0;
    for (size_t index = 0; index < values.size(); ++index) {
        const double magnitude = std::abs(values[index]);
        if (magnitude > peakMagnitude) {
            peakMagnitude = magnitude;
            peakIndex = index;
        }
    }
    if (peakMagnitude <= 1.0e-9) {
        return 0;
    }

    const double threshold = peakMagnitude * 0.5;
    size_t left = peakIndex;
    while (left > 0 && std::abs(values[left - 1]) >= threshold) {
        --left;
    }
    size_t right = peakIndex;
    while ((right + 1) < values.size() && std::abs(values[right + 1]) >= threshold) {
        ++right;
    }
    return right - left + 1;
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

wolfie::MeasurementSettings buildAlignmentSettings(int sampleRate) {
    wolfie::MeasurementSettings settings;
    settings.sampleRate = sampleRate;
    settings.durationSeconds = 0.0018;
    settings.fadeInSeconds = 0.00025;
    settings.fadeOutSeconds = 0.00025;
    settings.startFrequencyHz = 2800.0;
    settings.endFrequencyHz = 6200.0;
    settings.leadInSamples = std::max(sampleRate / 40, 1024);
    settings.targetLengthSamples = sampleRate >= 96000 ? 1024 : 512;
    return settings;
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

wolfie::MeasurementResult buildSyntheticReferenceResultWithWindowOffsets(int sampleRate,
                                                                         double roomMagnitudeOffsetDb,
                                                                         double directMagnitudeOffsetDb,
                                                                         double phaseOffsetDegrees) {
    wolfie::MeasurementResult result;
    result.analysis.sampleRate = sampleRate;
    result.analysis.measurementKind = "reference";
    const std::vector<double> positiveAxisHz = buildLinearFrequencyAxis(static_cast<double>(sampleRate) * 0.5, 4097);

    auto makeSpectrum = [&](const std::string& magnitudeKey,
                            const std::string& phaseKey,
                            double magnitudeOffsetDb) {
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

    makeSpectrum("measurement.raw_magnitude_spectrum", "measurement.raw_phase_spectrum", roomMagnitudeOffsetDb);
    makeSpectrum("measurement.room_magnitude_spectrum", "measurement.room_phase_spectrum", roomMagnitudeOffsetDb);
    makeSpectrum("measurement.direct_magnitude_spectrum", "measurement.direct_phase_spectrum", directMagnitudeOffsetDb);
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

double bandPeakToPeak(const std::vector<double>& values,
                      const std::vector<double>& frequencyAxisHz,
                      double minFrequencyHz,
                      double maxFrequencyHz) {
    const size_t count = std::min(values.size(), frequencyAxisHz.size());
    bool found = false;
    double minValue = 0.0;
    double maxValue = 0.0;
    for (size_t index = 0; index < count; ++index) {
        if (frequencyAxisHz[index] < minFrequencyHz || frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        if (!found) {
            minValue = values[index];
            maxValue = values[index];
            found = true;
            continue;
        }
        minValue = std::min(minValue, values[index]);
        maxValue = std::max(maxValue, values[index]);
    }
    return found ? (maxValue - minValue) : 0.0;
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
    const double lowBandRippleDb = bandPeakToPeak(magnitude->leftValues, magnitude->xValues, 25.0, 200.0);
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
    if (lowBandRippleDb > 0.25) {
        std::cerr << "reference loopback retained too much low-frequency ripple (25-200 Hz p-p="
                  << lowBandRippleDb << " dB)\n";
        return false;
    }

    return true;
}

bool expectReferenceMagnitudeResponsePrefersDirectWindow() {
    wolfie::MeasurementResult result;
    result.analysis.measurementKind = "reference";

    wolfie::MeasurementValueSet room;
    room.key = "measurement.magnitude_response";
    room.xValues = {20.0, 1000.0};
    room.leftValues = {-7.4, -7.0};
    room.rightValues = {-7.4, -7.0};

    wolfie::MeasurementValueSet direct;
    direct.key = "measurement.direct_magnitude_response";
    direct.xValues = {20.0, 1000.0};
    direct.leftValues = {-7.0, -7.0};
    direct.rightValues = {-7.0, -7.0};

    result.valueSets.push_back(room);
    result.valueSets.push_back(direct);

    const wolfie::MeasurementValueSet* selected = result.magnitudeResponse();
    if (selected != result.findValueSet("measurement.direct_magnitude_response")) {
        std::cerr << "reference magnitude response did not prefer the direct window\n";
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

bool expectReferenceCompensationUsesDirectReferenceSpectrum() {
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
        buildSyntheticReferenceResultWithWindowOffsets(settings.sampleRate, 6.0, 2.0, 0.0);
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
    if (rawRoom == nullptr || !rawRoom->valid() ||
        compensatedRoom == nullptr || !compensatedRoom->valid()) {
        std::cerr << "reference-compensated room response was not published\n";
        return false;
    }

    const double roomDeltaDb =
        meanBandDelta(rawRoom->leftValues, compensatedRoom->leftValues, rawRoom->xValues, 40.0, 5000.0);
    if (std::abs(roomDeltaDb + 2.0) > 0.4) {
        std::cerr << "reference compensation did not use the direct reference spectrum (delta="
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

bool expectSweetSpotAlignmentViewSuggestsMovingTowardLaterSpeaker() {
    const wolfie::MeasurementSettings settings = buildAlignmentSettings(48000);

    const int leftDelaySamples = 180;
    const int rightDelaySamples = 252;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0, wolfie::MeasurementRunMode::Alignment);
    const std::vector<int16_t> capture =
        synthesizeMeasurementCapture(plan, leftDelaySamples, rightDelaySamples, 0.8, 0.5);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings,
                                                               nullptr,
                                                               wolfie::MeasurementRunMode::Alignment);
    const wolfie::measurement::SweetSpotAlignmentView view =
        wolfie::measurement::buildSweetSpotAlignmentView(result);
    if (!view.available) {
        std::cerr << "sweet spot alignment view was not generated\n";
        return false;
    }
    if (view.suggestedDirection != wolfie::measurement::SweetSpotMoveDirection::Right) {
        std::cerr << "alignment view suggested the wrong movement direction\n";
        return false;
    }
    const double expectedDelayMs =
        (static_cast<double>(leftDelaySamples - rightDelaySamples) * 1000.0) / static_cast<double>(settings.sampleRate);
    if (std::abs(view.delayMismatchMs - expectedDelayMs) > 0.01) {
        std::cerr << "alignment view delay mismatch was incorrect\n";
        return false;
    }
    if (view.delayMismatchSamples != (leftDelaySamples - rightDelaySamples)) {
        std::cerr << "alignment view sample mismatch was incorrect\n";
        return false;
    }
    if (view.timeAxisMs.size() < 12 ||
        view.leftImpulse.size() != view.timeAxisMs.size() ||
        view.rightImpulse.size() != view.timeAxisMs.size()) {
        std::cerr << "alignment pulse plot data was incomplete\n";
        return false;
    }
    const auto leftPeak = std::max_element(view.leftImpulse.begin(),
                                           view.leftImpulse.end(),
                                           [](double left, double right) { return std::abs(left) < std::abs(right); });
    const auto rightPeak = std::max_element(view.rightImpulse.begin(),
                                            view.rightImpulse.end(),
                                            [](double left, double right) { return std::abs(left) < std::abs(right); });
    if (leftPeak == view.leftImpulse.end() || rightPeak == view.rightImpulse.end()) {
        std::cerr << "alignment pulse peaks were missing\n";
        return false;
    }
    const size_t leftPeakIndex = static_cast<size_t>(std::distance(view.leftImpulse.begin(), leftPeak));
    const size_t rightPeakIndex = static_cast<size_t>(std::distance(view.rightImpulse.begin(), rightPeak));
    const double plottedDelayMs = view.timeAxisMs[leftPeakIndex] - view.timeAxisMs[rightPeakIndex];
    if (std::abs(plottedDelayMs - view.delayMismatchMs) > 0.03) {
        std::cerr << "alignment overlay did not preserve the measured channel delay\n";
        return false;
    }

    return true;
}

bool expectAlignmentPlaybackBurstStartsAndEndsQuietly() {
    const wolfie::MeasurementSettings settings = buildAlignmentSettings(48000);
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0, wolfie::MeasurementRunMode::Alignment);
    if (plan.playedSweep.size() < 32) {
        std::cerr << "alignment playback burst is too short\n";
        return false;
    }

    const double firstMagnitude = std::abs(plan.playedSweep.front());
    const double lastMagnitude = std::abs(plan.playedSweep.back());
    if (firstMagnitude > 1.0e-3 || lastMagnitude > 1.0e-3) {
        std::cerr << "alignment playback burst does not taper to silence at the boundaries\n";
        return false;
    }

    const auto peak = std::max_element(plan.playedSweep.begin(),
                                       plan.playedSweep.end(),
                                       [](double left, double right) { return std::abs(left) < std::abs(right); });
    if (peak == plan.playedSweep.end()) {
        std::cerr << "alignment playback burst peak is missing\n";
        return false;
    }
    const size_t peakIndex = static_cast<size_t>(std::distance(plan.playedSweep.begin(), peak));
    if (peakIndex < 4 || peakIndex + 4 >= plan.playedSweep.size()) {
        std::cerr << "alignment playback burst peak is too close to the edges\n";
        return false;
    }

    return true;
}

bool expectSweetSpotAlignmentViewHandlesAbsoluteImpulseTimeAxis() {
    wolfie::MeasurementResult result;
    result.analysis.sampleRate = 48000;
    result.analysis.left.available = true;
    result.analysis.right.available = true;
    result.analysis.left.onsetTimeSeconds = 0.01225;
    result.analysis.right.onsetTimeSeconds = 0.01255;
    result.analysis.left.impulsePeakToNoiseDb = 36.0;
    result.analysis.right.impulsePeakToNoiseDb = 34.0;

    wolfie::MeasurementValueSet directImpulse;
    directImpulse.key = "measurement.direct_impulse_response";
    directImpulse.xQuantity = "time";
    directImpulse.xUnit = "seconds";
    const double sampleStepSeconds = 1.0 / static_cast<double>(result.analysis.sampleRate);
    const double startTimeSeconds = 0.0119;
    constexpr size_t sampleCount = 96;
    directImpulse.xValues.reserve(sampleCount);
    directImpulse.leftValues.reserve(sampleCount);
    directImpulse.rightValues.reserve(sampleCount);
    for (size_t index = 0; index < sampleCount; ++index) {
        const double timeSeconds = startTimeSeconds + (static_cast<double>(index) * sampleStepSeconds);
        const double leftDeltaMs = (timeSeconds - result.analysis.left.onsetTimeSeconds) * 1000.0;
        const double rightDeltaMs = (timeSeconds - result.analysis.right.onsetTimeSeconds) * 1000.0;
        directImpulse.xValues.push_back(timeSeconds);
        directImpulse.leftValues.push_back(std::exp(-(leftDeltaMs * leftDeltaMs) / 0.0012));
        directImpulse.rightValues.push_back(std::exp(-(rightDeltaMs * rightDeltaMs) / 0.0012));
    }
    result.valueSets.push_back(std::move(directImpulse));

    const wolfie::measurement::SweetSpotAlignmentView view =
        wolfie::measurement::buildSweetSpotAlignmentView(result);
    if (!view.available) {
        std::cerr << "alignment view did not accept an absolute impulse time axis\n";
        return false;
    }
    if (view.delayMismatchSamples != -14) {
        std::cerr << "alignment view did not quantize the absolute-axis mismatch to samples as expected\n";
        return false;
    }
    if (view.timeAxisMs.size() < 24 || view.timeAxisMs.front() >= 0.0 || view.timeAxisMs.back() <= 0.0) {
        std::cerr << "alignment view did not build a centered relative time axis\n";
        return false;
    }
    const auto leftPeak = std::max_element(view.leftImpulse.begin(),
                                           view.leftImpulse.end(),
                                           [](double left, double right) { return std::abs(left) < std::abs(right); });
    const auto rightPeak = std::max_element(view.rightImpulse.begin(),
                                            view.rightImpulse.end(),
                                            [](double left, double right) { return std::abs(left) < std::abs(right); });
    if (leftPeak == view.leftImpulse.end() || rightPeak == view.rightImpulse.end()) {
        std::cerr << "alignment view pulse peaks were missing\n";
        return false;
    }
    if (std::abs(*leftPeak) < 0.6 || std::abs(*rightPeak) < 0.6) {
        std::cerr << "alignment view collapsed the pulse peak when the source axis was absolute\n";
        return false;
    }
    const size_t leftPeakIndex = static_cast<size_t>(std::distance(view.leftImpulse.begin(), leftPeak));
    const size_t rightPeakIndex = static_cast<size_t>(std::distance(view.rightImpulse.begin(), rightPeak));
    const double plottedDelayMs = view.timeAxisMs[leftPeakIndex] - view.timeAxisMs[rightPeakIndex];
    if (std::abs(plottedDelayMs - view.delayMismatchMs) > 0.03) {
        std::cerr << "alignment view did not retain the absolute arrival offset\n";
        return false;
    }

    return true;
}

bool expectSweetSpotAlignmentViewUsesCenteredDeadband() {
    wolfie::MeasurementResult result;
    result.analysis.sampleRate = 48000;
    result.analysis.left.available = true;
    result.analysis.right.available = true;
    result.analysis.left.onsetTimeSeconds = 0.01230;
    result.analysis.right.onsetTimeSeconds = 0.01230;
    result.analysis.left.impulsePeakToNoiseDb = 36.0;
    result.analysis.right.impulsePeakToNoiseDb = 35.0;

    wolfie::MeasurementValueSet directImpulse;
    directImpulse.key = "measurement.direct_impulse_response";
    directImpulse.xQuantity = "time";
    directImpulse.xUnit = "seconds";
    const double sampleStepSeconds = 1.0 / static_cast<double>(result.analysis.sampleRate);
    const double startTimeSeconds = 0.0119;
    constexpr size_t sampleCount = 96;
    directImpulse.xValues.reserve(sampleCount);
    directImpulse.leftValues.reserve(sampleCount);
    directImpulse.rightValues.reserve(sampleCount);
    for (size_t index = 0; index < sampleCount; ++index) {
        const double timeSeconds = startTimeSeconds + (static_cast<double>(index) * sampleStepSeconds);
        const double leftDeltaMs = (timeSeconds - result.analysis.left.onsetTimeSeconds) * 1000.0;
        const double rightDeltaMs = (timeSeconds - result.analysis.right.onsetTimeSeconds) * 1000.0;
        directImpulse.xValues.push_back(timeSeconds);
        directImpulse.leftValues.push_back(std::exp(-(leftDeltaMs * leftDeltaMs) / 0.0012));
        directImpulse.rightValues.push_back(std::exp(-(rightDeltaMs * rightDeltaMs) / 0.0012));
    }
    result.valueSets.push_back(std::move(directImpulse));

    const wolfie::measurement::SweetSpotAlignmentView view =
        wolfie::measurement::buildSweetSpotAlignmentView(result);
    if (!view.available) {
        std::cerr << "alignment view was not generated for the deadband case\n";
        return false;
    }
    if (std::abs(view.delayMismatchMs) > 1.0e-9 || std::abs(view.centeredToleranceMs) > 1.0e-9) {
        std::cerr << "alignment exact-match test did not stay at zero milliseconds\n";
        return false;
    }
    if (view.delayMismatchSamples != 0 || view.centeredToleranceSamples != 0) {
        std::cerr << "alignment exact-match test did not stay at zero samples\n";
        return false;
    }
    if (view.suggestedDirection != wolfie::measurement::SweetSpotMoveDirection::None) {
        std::cerr << "alignment view still requested movement inside the centered tolerance\n";
        return false;
    }
    if (view.polarityMismatchDetected) {
        std::cerr << "alignment view reported a polarity mismatch inside the centered same-polarity case\n";
        return false;
    }

    return true;
}

bool expectSweetSpotAlignmentViewDetectsPolarityMismatch() {
    wolfie::MeasurementResult result;
    result.analysis.sampleRate = 48000;
    result.analysis.left.available = true;
    result.analysis.right.available = true;
    result.analysis.left.onsetTimeSeconds = 0.01230;
    result.analysis.right.onsetTimeSeconds = 0.01230;
    result.analysis.left.impulsePeakToNoiseDb = 36.0;
    result.analysis.right.impulsePeakToNoiseDb = 35.0;

    wolfie::MeasurementValueSet directImpulse;
    directImpulse.key = "measurement.direct_impulse_response";
    directImpulse.xQuantity = "time";
    directImpulse.xUnit = "seconds";
    const double sampleStepSeconds = 1.0 / static_cast<double>(result.analysis.sampleRate);
    const double startTimeSeconds = 0.0119;
    constexpr size_t sampleCount = 96;
    directImpulse.xValues.reserve(sampleCount);
    directImpulse.leftValues.reserve(sampleCount);
    directImpulse.rightValues.reserve(sampleCount);
    for (size_t index = 0; index < sampleCount; ++index) {
        const double timeSeconds = startTimeSeconds + (static_cast<double>(index) * sampleStepSeconds);
        const double deltaMs = (timeSeconds - result.analysis.left.onsetTimeSeconds) * 1000.0;
        const double pulse = std::exp(-(deltaMs * deltaMs) / 0.0012);
        directImpulse.xValues.push_back(timeSeconds);
        directImpulse.leftValues.push_back(pulse);
        directImpulse.rightValues.push_back(-pulse);
    }
    result.valueSets.push_back(std::move(directImpulse));

    const wolfie::measurement::SweetSpotAlignmentView view =
        wolfie::measurement::buildSweetSpotAlignmentView(result);
    if (!view.available) {
        std::cerr << "alignment view was not generated for the polarity mismatch case\n";
        return false;
    }
    if (!view.polarityMismatchDetected) {
        std::cerr << "alignment view did not detect the polarity mismatch\n";
        return false;
    }

    return true;
}

bool expectSweetSpotAlignmentViewSuppressesOuterSideLobes() {
    wolfie::MeasurementResult result;
    result.analysis.sampleRate = 48000;
    result.analysis.startFrequencyHz = 4000.0;
    result.analysis.endFrequencyHz = 6200.0;
    result.analysis.left.available = true;
    result.analysis.right.available = true;
    result.analysis.left.onsetTimeSeconds = 0.01230;
    result.analysis.right.onsetTimeSeconds = 0.01230;
    result.analysis.left.impulsePeakToNoiseDb = 36.0;
    result.analysis.right.impulsePeakToNoiseDb = 35.0;

    wolfie::MeasurementValueSet directImpulse;
    directImpulse.key = "measurement.direct_impulse_response";
    directImpulse.xQuantity = "time";
    directImpulse.xUnit = "seconds";
    const double sampleStepSeconds = 1.0 / static_cast<double>(result.analysis.sampleRate);
    const double startTimeSeconds = 0.0116;
    constexpr size_t sampleCount = 120;
    directImpulse.xValues.reserve(sampleCount);
    directImpulse.leftValues.reserve(sampleCount);
    directImpulse.rightValues.reserve(sampleCount);
    for (size_t index = 0; index < sampleCount; ++index) {
        const double timeSeconds = startTimeSeconds + (static_cast<double>(index) * sampleStepSeconds);
        const double deltaMs = (timeSeconds - result.analysis.left.onsetTimeSeconds) * 1000.0;
        const double mainLobe = std::exp(-(deltaMs * deltaMs) / 0.0008);
        const double sideLobe =
            (0.7 * std::exp(-((deltaMs - 0.46) * (deltaMs - 0.46)) / 0.0009)) +
            (0.7 * std::exp(-((deltaMs + 0.46) * (deltaMs + 0.46)) / 0.0009));
        const double value = mainLobe + sideLobe;
        directImpulse.xValues.push_back(timeSeconds);
        directImpulse.leftValues.push_back(value);
        directImpulse.rightValues.push_back(value);
    }
    result.valueSets.push_back(std::move(directImpulse));

    const wolfie::measurement::SweetSpotAlignmentView view =
        wolfie::measurement::buildSweetSpotAlignmentView(result);
    if (!view.available) {
        std::cerr << "alignment view was not generated for the side-lobe suppression case\n";
        return false;
    }

    double tailPeak = 0.0;
    double centerPeak = 0.0;
    for (size_t index = 0; index < view.timeAxisMs.size(); ++index) {
        const double magnitude = std::abs(view.leftImpulse[index]);
        if (std::abs(view.timeAxisMs[index]) > 0.35) {
            tailPeak = std::max(tailPeak, magnitude);
        } else {
            centerPeak = std::max(centerPeak, magnitude);
        }
    }
    if (centerPeak < 0.8) {
        std::cerr << "alignment view lost the dominant lobe while suppressing side lobes\n";
        return false;
    }
    if (tailPeak > 0.3) {
        std::cerr << "alignment view still shows strong outer side lobes\n";
        return false;
    }

    return true;
}

bool expectAlignmentMatchedCorrelationProducesSharpPeak() {
    const wolfie::MeasurementSettings settings = buildAlignmentSettings(48000);

    const int leftDelaySamples = 180;
    const int rightDelaySamples = 252;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0, wolfie::MeasurementRunMode::Alignment);
    const std::vector<int16_t> capture =
        synthesizeMeasurementCapture(plan, leftDelaySamples, rightDelaySamples, 0.8, 0.5);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings,
                                                               nullptr,
                                                               wolfie::MeasurementRunMode::Alignment);

    const wolfie::MeasurementValueSet* directImpulse = result.findValueSet("measurement.direct_impulse_response");
    if (directImpulse == nullptr || !directImpulse->valid()) {
        std::cerr << "alignment direct impulse response is missing\n";
        return false;
    }
    if (result.analysis.left.detectedLatencySamples != leftDelaySamples ||
        result.analysis.right.detectedLatencySamples != rightDelaySamples) {
        std::cerr << "alignment matched correlation did not retain the expected delay mismatch\n";
        return false;
    }

    const size_t leftWidthSamples = impulseHalfMagnitudeWidthSamples(directImpulse->leftValues);
    const size_t rightWidthSamples = impulseHalfMagnitudeWidthSamples(directImpulse->rightValues);
    if (leftWidthSamples == 0 || rightWidthSamples == 0) {
        std::cerr << "alignment impulse peak could not be measured\n";
        return false;
    }
    if (leftWidthSamples > 12 || rightWidthSamples > 12) {
        std::cerr << "alignment impulse peak is still too broad (left width="
                  << leftWidthSamples << ", right width=" << rightWidthSamples << ")\n";
        return false;
    }

    return true;
}

bool expectAlignmentMatchedCorrelationIgnoresCarrierPhaseBias() {
    const wolfie::MeasurementSettings settings = buildAlignmentSettings(48000);
    const int leftDelaySamples = 180;
    const int rightDelaySamples = 180;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0, wolfie::MeasurementRunMode::Alignment);
    const std::vector<int16_t> capture =
        synthesizeFilteredAlignmentCapture(plan,
                                           leftDelaySamples,
                                           rightDelaySamples,
                                           {1.0},
                                           {0.5, 0.5, -0.9},
                                           0.8,
                                           0.8);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings,
                                                               nullptr,
                                                               wolfie::MeasurementRunMode::Alignment);

    if (result.analysis.left.detectedLatencySamples != leftDelaySamples) {
        std::cerr << "alignment matched correlation moved the left arrival unexpectedly\n";
        return false;
    }
    if (std::abs(result.analysis.right.detectedLatencySamples - rightDelaySamples) > 1) {
        std::cerr << "alignment matched correlation was biased by carrier phase (right delay="
                  << result.analysis.right.detectedLatencySamples << ")\n";
        return false;
    }

    return true;
}

bool expectAlignmentMatchedCorrelationHandlesPolarityInversion() {
    const wolfie::MeasurementSettings settings = buildAlignmentSettings(48000);
    const int leftDelaySamples = 180;
    const int rightDelaySamples = 180;
    const wolfie::measurement::SweepPlaybackPlan plan =
        wolfie::measurement::buildSweepPlaybackPlan(settings, -12.0, wolfie::MeasurementRunMode::Alignment);
    const std::vector<int16_t> capture =
        synthesizeFilteredAlignmentCapture(plan,
                                           leftDelaySamples,
                                           rightDelaySamples,
                                           {1.0},
                                           {-1.0, 0.1, -0.04},
                                           0.8,
                                           0.8);
    const wolfie::MeasurementResult result =
        wolfie::measurement::buildMeasurementResultFromCapture(capture,
                                                               plan,
                                                               settings.sampleRate,
                                                               wolfie::AudioSettings{},
                                                               settings,
                                                               nullptr,
                                                               wolfie::MeasurementRunMode::Alignment);

    if (result.analysis.left.detectedLatencySamples != leftDelaySamples) {
        std::cerr << "alignment matched correlation moved the non-inverted channel unexpectedly\n";
        return false;
    }
    if (std::abs(result.analysis.right.detectedLatencySamples - rightDelaySamples) > 1) {
        std::cerr << "alignment matched correlation drifted under polarity inversion (right delay="
                  << result.analysis.right.detectedLatencySamples << ")\n";
        return false;
    }

    const wolfie::measurement::SweetSpotAlignmentView view =
        wolfie::measurement::buildSweetSpotAlignmentView(result);
    if (!view.available) {
        std::cerr << "alignment view was not generated for the polarity inversion case\n";
        return false;
    }
    if (!view.polarityMismatchDetected) {
        std::cerr << "alignment view did not flag the polarity inversion case\n";
        return false;
    }
    if (std::abs(view.delayMismatchSamples) > 1) {
        std::cerr << "alignment view showed a false delay mismatch under polarity inversion (samples="
                  << view.delayMismatchSamples << ")\n";
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
        {"expectReferenceMagnitudeResponsePrefersDirectWindow", expectReferenceMagnitudeResponsePrefersDirectWindow},
        {"expectMeasurementPublishesReferenceCompensatedTransferProducts", expectMeasurementPublishesReferenceCompensatedTransferProducts},
        {"expectReferenceCompensationUsesDirectReferenceSpectrum", expectReferenceCompensationUsesDirectReferenceSpectrum},
        {"expectMeasurementRetainsStereoArrivalMismatchForLaterAlignment", expectMeasurementRetainsStereoArrivalMismatchForLaterAlignment},
        {"expectSweetSpotAlignmentViewSuggestsMovingTowardLaterSpeaker", expectSweetSpotAlignmentViewSuggestsMovingTowardLaterSpeaker},
        {"expectAlignmentPlaybackBurstStartsAndEndsQuietly", expectAlignmentPlaybackBurstStartsAndEndsQuietly},
        {"expectSweetSpotAlignmentViewHandlesAbsoluteImpulseTimeAxis", expectSweetSpotAlignmentViewHandlesAbsoluteImpulseTimeAxis},
        {"expectSweetSpotAlignmentViewUsesCenteredDeadband", expectSweetSpotAlignmentViewUsesCenteredDeadband},
        {"expectSweetSpotAlignmentViewDetectsPolarityMismatch", expectSweetSpotAlignmentViewDetectsPolarityMismatch},
        {"expectSweetSpotAlignmentViewSuppressesOuterSideLobes", expectSweetSpotAlignmentViewSuppressesOuterSideLobes},
        {"expectAlignmentMatchedCorrelationProducesSharpPeak", expectAlignmentMatchedCorrelationProducesSharpPeak},
        {"expectAlignmentMatchedCorrelationIgnoresCarrierPhaseBias", expectAlignmentMatchedCorrelationIgnoresCarrierPhaseBias},
        {"expectAlignmentMatchedCorrelationHandlesPolarityInversion",
         expectAlignmentMatchedCorrelationHandlesPolarityInversion},
        {"expectWaterfallPlotData", expectWaterfallPlotData},
    });
}
