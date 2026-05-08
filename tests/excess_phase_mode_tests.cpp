#include <iostream>

#include "filter_test_support.h"
#include "test_harness.h"

#include "measurement/filter_designer.h"
#include "measurement/target_curve_designer.h"

namespace {

bool expectExcessLfModeLeavesMinimumPhaseInputAlone() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "excess-lf";

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid || result.phaseMode != "excess-lf") {
        std::cerr << "excess-lf minimum-phase baseline did not produce the expected mode\n";
        return false;
    }

    const double leftPredictedBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                    result.left.predictedExcessPhaseDegrees,
                                                                    20.0,
                                                                    300.0);
    const double rightPredictedBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                     result.right.predictedExcessPhaseDegrees,
                                                                     20.0,
                                                                     300.0);
    if (leftPredictedBandMean > 2.0 || rightPredictedBandMean > 2.0) {
        std::cerr << "excess-lf mode introduced excess phase on minimum-phase input (left="
                  << leftPredictedBandMean << ", right=" << rightPredictedBandMean << ")\n";
        return false;
    }

    const double magnitudeDeltaLeft = wolfie::tests::bandMeanAbsDelta(result.frequencyAxisHz,
                                                                      response.leftChannelDb,
                                                                      result.left.correctedResponseDb,
                                                                      20.0,
                                                                      20000.0);
    const double magnitudeDeltaRight = wolfie::tests::bandMeanAbsDelta(result.frequencyAxisHz,
                                                                       response.rightChannelDb,
                                                                       result.right.correctedResponseDb,
                                                                       20.0,
                                                                       20000.0);
    if (magnitudeDeltaLeft > 0.05 || magnitudeDeltaRight > 0.05) {
        std::cerr << "excess-lf mode changed magnitude on a phase-only baseline (left="
                  << magnitudeDeltaLeft << ", right=" << magnitudeDeltaRight << ")\n";
        return false;
    }

    return true;
}

bool expectExcessLfModeIgnoresBulkDelay() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "excess-lf";

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0065);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "excess-lf bulk-delay baseline did not produce a valid filter result\n";
        return false;
    }

    const double leftPredictedBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                    result.left.predictedExcessPhaseDegrees,
                                                                    20.0,
                                                                    300.0);
    const double rightPredictedBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                     result.right.predictedExcessPhaseDegrees,
                                                                     20.0,
                                                                     300.0);
    if (leftPredictedBandMean > 3.0 || rightPredictedBandMean > 3.0) {
        std::cerr << "excess-lf mode reacted to pure bulk delay (left="
                  << leftPredictedBandMean << ", right=" << rightPredictedBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectExcessLfModeContainsCorrectionToLowFrequencies() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "excess-lf";

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "excess-lf containment case did not produce a valid filter result\n";
        return false;
    }

    const double highBandDelta = wolfie::tests::bandMeanAbsDelta(result.frequencyAxisHz,
                                                                 result.left.inputExcessPhaseDegrees,
                                                                 result.left.predictedExcessPhaseDegrees,
                                                                 500.0,
                                                                 5000.0);
    if (highBandDelta > 5.0) {
        std::cerr << "excess-lf mode changed too much phase out of band (" << highBandDelta << " deg)\n";
        return false;
    }

    const double leftMagnitudeDelta = wolfie::tests::bandMeanAbsDelta(result.frequencyAxisHz,
                                                                      response.leftChannelDb,
                                                                      result.left.correctedResponseDb,
                                                                      20.0,
                                                                      20000.0);
    const double rightMagnitudeDelta = wolfie::tests::bandMeanAbsDelta(result.frequencyAxisHz,
                                                                       response.rightChannelDb,
                                                                       result.right.correctedResponseDb,
                                                                       20.0,
                                                                       20000.0);
    if (leftMagnitudeDelta > 0.05 || rightMagnitudeDelta > 0.05) {
        std::cerr << "excess-lf mode changed magnitude while still in isolated phase preview (left="
                  << leftMagnitudeDelta << ", right=" << rightMagnitudeDelta << ")\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    return wolfie::tests::runTestCases({
        {"expectExcessLfModeLeavesMinimumPhaseInputAlone", expectExcessLfModeLeavesMinimumPhaseInputAlone},
        {"expectExcessLfModeIgnoresBulkDelay", expectExcessLfModeIgnoresBulkDelay},
        {"expectExcessLfModeContainsCorrectionToLowFrequencies", expectExcessLfModeContainsCorrectionToLowFrequencies},
    });
}
