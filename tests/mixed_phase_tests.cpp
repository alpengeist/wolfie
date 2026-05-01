#include <cmath>
#include <iostream>

#include "filter_test_support.h"
#include "test_harness.h"

#include "measurement/filter_designer.h"
#include "measurement/target_curve_designer.h"

namespace {

double maxAdjacentBandAbsDelta(const std::vector<double>& frequencyAxisHz,
                               const std::vector<double>& values,
                               double minFrequencyHz,
                               double maxFrequencyHz) {
    const size_t count = std::min(frequencyAxisHz.size(), values.size());
    double maxDelta = 0.0;
    bool havePrevious = false;
    double previousValue = 0.0;
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index]) ||
            frequencyAxisHz[index] < minFrequencyHz ||
            frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        if (havePrevious) {
            maxDelta = std::max(maxDelta, std::abs(values[index] - previousValue));
        }
        previousValue = values[index];
        havePrevious = true;
    }
    return maxDelta;
}

bool expectMixedModeLeavesMinimumPhaseInputAlone() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0);
    const wolfie::FilterDesignResult minimumResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           minimumSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult mixedResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           mixedSettings,
                                           &phaseMeasurement);
    if (!minimumResult.valid || !mixedResult.valid || mixedResult.phaseMode != "mixed") {
        std::cerr << "mixed minimum-phase baseline did not produce valid results\n";
        return false;
    }

    const double leftPredictedBandMean = wolfie::tests::bandMeanAbs(mixedResult.frequencyAxisHz,
                                                                    mixedResult.left.predictedExcessPhaseDegrees,
                                                                    20.0,
                                                                    300.0);
    const double rightPredictedBandMean = wolfie::tests::bandMeanAbs(mixedResult.frequencyAxisHz,
                                                                     mixedResult.right.predictedExcessPhaseDegrees,
                                                                     20.0,
                                                                     300.0);
    if (leftPredictedBandMean > 2.0 || rightPredictedBandMean > 2.0) {
        std::cerr << "mixed mode introduced excess phase on a minimum-phase baseline (left="
                  << leftPredictedBandMean << ", right=" << rightPredictedBandMean << ")\n";
        return false;
    }

    const double leftMagnitudeDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                      minimumResult.left.correctedResponseDb,
                                                                      mixedResult.left.correctedResponseDb,
                                                                      20.0,
                                                                      20000.0);
    const double rightMagnitudeDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                       minimumResult.right.correctedResponseDb,
                                                                       mixedResult.right.correctedResponseDb,
                                                                       20.0,
                                                                       20000.0);
    if (leftMagnitudeDelta > 0.1 || rightMagnitudeDelta > 0.1) {
        std::cerr << "mixed mode changed magnitude on a minimum-phase baseline (left="
                  << leftMagnitudeDelta << ", right=" << rightMagnitudeDelta << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeIgnoresBulkDelay() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0065);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed bulk-delay baseline did not produce a valid filter result\n";
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
        std::cerr << "mixed mode reacted to pure bulk delay (left="
                  << leftPredictedBandMean << ", right=" << rightPredictedBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeReducesLowFrequencyExcessPhase() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed reduction case did not produce a valid filter result\n";
        return false;
    }

    const double leftInputBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                result.left.inputExcessPhaseDegrees,
                                                                20.0,
                                                                200.0);
    const double leftPredictedBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                    result.left.predictedExcessPhaseDegrees,
                                                                    20.0,
                                                                    200.0);
    if (leftInputBandMean < 8.0) {
        std::cerr << "synthetic mixed-mode fixture did not produce a meaningful LF phase error\n";
        return false;
    }
    if (leftPredictedBandMean > leftInputBandMean * 0.6) {
        std::cerr << "mixed mode did not materially reduce LF excess phase (before="
                  << leftInputBandMean << ", after=" << leftPredictedBandMean << ")\n";
        return false;
    }

    const double rightPredictedBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                     result.right.predictedExcessPhaseDegrees,
                                                                     20.0,
                                                                     200.0);
    if (rightPredictedBandMean > 2.0) {
        std::cerr << "mixed mode changed the clean channel while correcting the left channel (right="
                  << rightPredictedBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeContainsCorrectionToLowFrequencies() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed containment case did not produce a valid filter result\n";
        return false;
    }

    const double highBandDelta = wolfie::tests::bandMeanAbsDelta(result.frequencyAxisHz,
                                                                 result.left.inputExcessPhaseDegrees,
                                                                 result.left.predictedExcessPhaseDegrees,
                                                                 500.0,
                                                                 5000.0);
    if (highBandDelta > 5.0) {
        std::cerr << "mixed mode changed too much phase out of band (" << highBandDelta << " deg)\n";
        return false;
    }

    return true;
}

bool expectMixedModePreservesMagnitudeVsMinimum() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    targetCurve.lowGainDb = 2.0;
    targetCurve.midGainDb = 0.0;
    targetCurve.highGainDb = -1.5;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;
    minimumSettings.maxBoostDb = 6.0;
    minimumSettings.maxCutDb = 12.0;
    minimumSettings.highCorrectionHz = 18000.0;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = wolfie::tests::buildSyntheticResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult minimumResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           minimumSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult mixedResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           mixedSettings,
                                           &phaseMeasurement);
    if (!minimumResult.valid || !mixedResult.valid) {
        std::cerr << "mixed magnitude-preservation case did not produce valid filter results\n";
        return false;
    }

    const double leftMagnitudeDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                      minimumResult.left.correctedResponseDb,
                                                                      mixedResult.left.correctedResponseDb,
                                                                      20.0,
                                                                      20000.0);
    const double rightMagnitudeDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                       minimumResult.right.correctedResponseDb,
                                                                       mixedResult.right.correctedResponseDb,
                                                                       20.0,
                                                                       20000.0);
    if (leftMagnitudeDelta > 0.25 || rightMagnitudeDelta > 0.25) {
        std::cerr << "mixed mode changed magnitude too much versus minimum phase (left="
                  << leftMagnitudeDelta << ", right=" << rightMagnitudeDelta << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeStrengthZeroMatchesMinimum() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";
    mixedSettings.mixedPhaseStrength = 0.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult minimumResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           minimumSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult mixedResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           mixedSettings,
                                           &phaseMeasurement);
    if (!minimumResult.valid || !mixedResult.valid) {
        std::cerr << "mixed strength-zero case did not produce valid filter results\n";
        return false;
    }

    const double leftMagnitudeDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                      minimumResult.left.correctedResponseDb,
                                                                      mixedResult.left.correctedResponseDb,
                                                                      20.0,
                                                                      20000.0);
    const double rightMagnitudeDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                       minimumResult.right.correctedResponseDb,
                                                                       mixedResult.right.correctedResponseDb,
                                                                       20.0,
                                                                       20000.0);
    if (leftMagnitudeDelta > 0.1 || rightMagnitudeDelta > 0.1) {
        std::cerr << "mixed strength zero changed magnitude versus minimum phase (left="
                  << leftMagnitudeDelta << ", right=" << rightMagnitudeDelta << ")\n";
        return false;
    }

    const double leftResidualDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                     minimumResult.left.predictedExcessPhaseDegrees,
                                                                     mixedResult.left.predictedExcessPhaseDegrees,
                                                                     20.0,
                                                                     300.0);
    const double rightResidualDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                      minimumResult.right.predictedExcessPhaseDegrees,
                                                                      mixedResult.right.predictedExcessPhaseDegrees,
                                                                      20.0,
                                                                      300.0);
    if (leftResidualDelta > 2.0 || rightResidualDelta > 2.0) {
        std::cerr << "mixed strength zero changed predicted excess phase versus minimum phase (left="
                  << leftResidualDelta << ", right=" << rightResidualDelta << ")\n";
        return false;
    }
    if (std::abs(mixedResult.left.impulsePeakIndex - minimumResult.left.impulsePeakIndex) > 2 ||
        std::abs(mixedResult.right.impulsePeakIndex - minimumResult.right.impulsePeakIndex) > 2) {
        std::cerr << "mixed strength zero changed filter latency versus minimum phase (left="
                  << mixedResult.left.impulsePeakIndex << ", right=" << mixedResult.right.impulsePeakIndex << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModePhaseLimitDoesNotInventUpperBandCorrectionWithoutMagnitudeOpportunity() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings narrowSettings;
    narrowSettings.tapCount = 16384;
    narrowSettings.phaseMode = "mixed";
    narrowSettings.mixedPhaseMaxFrequencyHz = 120.0;

    wolfie::FilterDesignSettings wideSettings = narrowSettings;
    wideSettings.mixedPhaseMaxFrequencyHz = 320.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult narrowResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           narrowSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult wideResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           wideSettings,
                                           &phaseMeasurement);
    if (!narrowResult.valid || !wideResult.valid) {
        std::cerr << "mixed phase-limit case did not produce valid filter results\n";
        return false;
    }

    const double inputLowBand = wolfie::tests::bandMeanAbs(narrowResult.frequencyAxisHz,
                                                           narrowResult.left.inputExcessPhaseDegrees,
                                                           20.0,
                                                           90.0);
    const double narrowLowBand = wolfie::tests::bandMeanAbs(narrowResult.frequencyAxisHz,
                                                            narrowResult.left.predictedExcessPhaseDegrees,
                                                            20.0,
                                                            90.0);
    if (narrowLowBand > inputLowBand * 0.75) {
        std::cerr << "narrow mixed phase limit stopped useful low-band reduction (before="
                  << inputLowBand << ", after=" << narrowLowBand << ")\n";
        return false;
    }

    const double upperBandDelta = wolfie::tests::bandMeanAbsDelta(narrowResult.frequencyAxisHz,
                                                                  narrowResult.left.predictedExcessPhaseDegrees,
                                                                  wideResult.left.predictedExcessPhaseDegrees,
                                                                  110.0,
                                                                  220.0);
    if (upperBandDelta > 1.0) {
        std::cerr << "widening mixed phase limit changed a band without meaningful inversion opportunity (delta="
                  << upperBandDelta << " deg)\n";
        return false;
    }

    return true;
}

bool expectMixedModePhaseCapControlsLowFrequencyReduction() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings lowCapSettings;
    lowCapSettings.tapCount = 16384;
    lowCapSettings.phaseMode = "mixed";
    lowCapSettings.mixedPhaseMaxFrequencyHz = 220.0;
    lowCapSettings.mixedPhaseMaxCorrectionDegrees = 120.0;

    wolfie::FilterDesignSettings highCapSettings = lowCapSettings;
    highCapSettings.mixedPhaseMaxCorrectionDegrees = 360.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, 0.0);
    const wolfie::FilterDesignResult lowCapResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           lowCapSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult highCapResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           highCapSettings,
                                           &phaseMeasurement);
    if (!lowCapResult.valid || !highCapResult.valid) {
        std::cerr << "mixed phase-cap case did not produce valid filter results\n";
        return false;
    }

    const double inputLowBand = wolfie::tests::bandMeanAbs(lowCapResult.frequencyAxisHz,
                                                           lowCapResult.left.inputExcessPhaseDegrees,
                                                           20.0,
                                                           80.0);
    const double lowCapResidual = wolfie::tests::bandMeanAbs(lowCapResult.frequencyAxisHz,
                                                             lowCapResult.left.predictedExcessPhaseDegrees,
                                                             20.0,
                                                             80.0);
    const double highCapResidual = wolfie::tests::bandMeanAbs(highCapResult.frequencyAxisHz,
                                                              highCapResult.left.predictedExcessPhaseDegrees,
                                                              20.0,
                                                              80.0);
    if (inputLowBand < 40.0) {
        std::cerr << "synthetic mixed phase-cap fixture did not produce enough LF excess phase\n";
        return false;
    }
    if (highCapResidual > lowCapResidual * 0.9) {
        std::cerr << "raising mixed phase cap did not materially improve LF reduction (low="
                  << lowCapResidual << ", high=" << highCapResidual << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeStereoImpulsePeaksStayAlignedWithoutLargeBulkDelay() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";
    filterSettings.mixedPhaseMaxCorrectionDegrees = 720.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, -2.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed stereo-latency alignment case did not produce a valid filter result\n";
        return false;
    }

    const int stereoPeakDelta = std::abs(result.left.impulsePeakIndex - result.right.impulsePeakIndex);
    if (stereoPeakDelta > 1) {
        std::cerr << "mixed stereo filter peaks diverged in latency (left="
                  << result.left.impulsePeakIndex << ", right=" << result.right.impulsePeakIndex << ")\n";
        return false;
    }
    if (result.left.impulsePeakIndex > (filterSettings.tapCount / 8) ||
        result.right.impulsePeakIndex > (filterSettings.tapCount / 8)) {
        std::cerr << "mixed stereo filter peaks accumulated too much bulk delay (left="
                  << result.left.impulsePeakIndex << ", right=" << result.right.impulsePeakIndex << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModePreservesStereoLowFrequencyPhaseRelationship() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";
    mixedSettings.mixedPhaseMaxCorrectionDegrees = 720.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, -2.0);
    const wolfie::FilterDesignResult minimumResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           minimumSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult mixedResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           mixedSettings,
                                           &phaseMeasurement);
    if (!minimumResult.valid || !mixedResult.valid) {
        std::cerr << "mixed stereo phase-relationship case did not produce valid filter results\n";
        return false;
    }

    const double minimumStereoDelta = wolfie::tests::bandMeanAbsDelta(minimumResult.frequencyAxisHz,
                                                                      minimumResult.left.predictedExcessPhaseContinuousDegrees,
                                                                      minimumResult.right.predictedExcessPhaseContinuousDegrees,
                                                                      20.0,
                                                                      150.0);
    const double mixedStereoDelta = wolfie::tests::bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                                    mixedResult.left.predictedExcessPhaseContinuousDegrees,
                                                                    mixedResult.right.predictedExcessPhaseContinuousDegrees,
                                                                    20.0,
                                                                    150.0);
    if (std::abs(mixedStereoDelta - minimumStereoDelta) > 5.0) {
        std::cerr << "mixed mode changed low-frequency stereo phase relationship (minimum="
                  << minimumStereoDelta << ", mixed=" << mixedStereoDelta << ")\n";
        return false;
    }

    return true;
}

bool expectInputGroupDelayIsPublishedFromMeasuredPhase() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "input group-delay publication case did not produce a valid filter result\n";
        return false;
    }

    if (result.left.inputGroupDelayMs.size() != result.frequencyAxisHz.size() ||
        result.right.inputGroupDelayMs.size() != result.frequencyAxisHz.size()) {
        std::cerr << "measured input group delay was not published at display resolution\n";
        return false;
    }

    const double leftInputDelay = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                             result.left.inputGroupDelayMs,
                                                             20.0,
                                                             200.0);
    const double rightInputDelay = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                              result.right.inputGroupDelayMs,
                                                              20.0,
                                                              200.0);
    if (leftInputDelay < 0.02 || rightInputDelay > 0.01) {
        std::cerr << "measured input group delay was not preserved in the filter result (left="
                  << leftInputDelay << ", right=" << rightInputDelay << ")\n";
        return false;
    }

    return true;
}

bool expectContinuousExcessPhaseSeriesStaySmoothAcrossWraps() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";
    filterSettings.mixedPhaseMaxCorrectionDegrees = 360.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "continuous excess-phase publication case did not produce a valid filter result\n";
        return false;
    }

    const double inputContinuousJump = wolfie::tests::maxAdjacentAbsDelta(result.left.inputExcessPhaseContinuousDegrees);
    const double predictedContinuousJump = wolfie::tests::maxAdjacentAbsDelta(result.left.predictedExcessPhaseContinuousDegrees);
    if (inputContinuousJump > 120.0 || predictedContinuousJump > 120.0) {
        std::cerr << "continuous excess-phase series contains implausible wrap jumps (input="
                  << inputContinuousJump << ", predicted=" << predictedContinuousJump << ")\n";
        return false;
    }

    return true;
}

bool expectMixedPhasePredictedGroupDelayStaysSmoothOnDisplayAxis() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";
    filterSettings.lowCorrectionHz = 30.0;
    filterSettings.highCorrectionHz = 12000.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed group-delay smoothness case did not produce a valid filter result\n";
        return false;
    }

    const double leftPredictedAdjacentJump =
        maxAdjacentBandAbsDelta(result.frequencyAxisHz,
                                result.left.predictedGroupDelayMs,
                                25.0,
                                180.0);
    if (leftPredictedAdjacentJump > 8.0) {
        std::cerr << "mixed predicted group delay contains unstable display-axis oscillation (jump="
                  << leftPredictedAdjacentJump << " ms)\n";
        return false;
    }

    return true;
}

bool expectMixedModeSkipsPhaseOnlyCorrectionWhenMagnitudeInversionIsNegligible() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";
    filterSettings.mixedPhaseMaxCorrectionDegrees = 360.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed negligible-inversion case did not produce a valid filter result\n";
        return false;
    }

    const double leftResidualDelta =
        wolfie::tests::bandMeanAbsDelta(result.frequencyAxisHz,
                                        result.left.inputExcessPhaseDegrees,
                                        result.left.predictedExcessPhaseDegrees,
                                        20.0,
                                        180.0);
    if (leftResidualDelta > 2.0) {
        std::cerr << "mixed mode applied phase-only correction despite negligible inversion (delta="
                  << leftResidualDelta << " deg)\n";
        return false;
    }

    return true;
}

bool expectMixedModePredictedMagnitudeStaysSmoothInLowBand() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;
    minimumSettings.lowCorrectionHz = 30.0;
    minimumSettings.highCorrectionHz = 12000.0;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";
    mixedSettings.mixedPhaseMaxFrequencyHz = 220.0;
    mixedSettings.mixedPhaseMaxCorrectionDegrees = 360.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, -2.0);
    const wolfie::FilterDesignResult minimumResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           minimumSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult mixedResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           mixedSettings,
                                           &phaseMeasurement);
    if (!minimumResult.valid || !mixedResult.valid) {
        std::cerr << "mixed magnitude smoothness case did not produce valid filter results\n";
        return false;
    }

    const double minimumLeftAdjacentJump =
        maxAdjacentBandAbsDelta(minimumResult.frequencyAxisHz,
                                minimumResult.left.correctedResponseDb,
                                80.0,
                                180.0);
    const double mixedLeftAdjacentJump =
        maxAdjacentBandAbsDelta(mixedResult.frequencyAxisHz,
                                mixedResult.left.correctedResponseDb,
                                80.0,
                                180.0);
    const double minimumRightAdjacentJump =
        maxAdjacentBandAbsDelta(minimumResult.frequencyAxisHz,
                                minimumResult.right.correctedResponseDb,
                                80.0,
                                180.0);
    const double mixedRightAdjacentJump =
        maxAdjacentBandAbsDelta(mixedResult.frequencyAxisHz,
                                mixedResult.right.correctedResponseDb,
                                80.0,
                                180.0);
    if (mixedLeftAdjacentJump > std::max(1.5, minimumLeftAdjacentJump + 0.5) ||
        mixedRightAdjacentJump > std::max(1.5, minimumRightAdjacentJump + 0.5)) {
        std::cerr << "mixed mode introduced low-band magnitude ripple (left minimum="
                  << minimumLeftAdjacentJump << ", left mixed=" << mixedLeftAdjacentJump
                  << ", right minimum=" << minimumRightAdjacentJump
                  << ", right mixed=" << mixedRightAdjacentJump << ")\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    return wolfie::tests::runTestCases({
        {"expectMixedModeIgnoresBulkDelay", expectMixedModeIgnoresBulkDelay},
        {"expectMixedModeReducesLowFrequencyExcessPhase", expectMixedModeReducesLowFrequencyExcessPhase},
        {"expectMixedModeContainsCorrectionToLowFrequencies", expectMixedModeContainsCorrectionToLowFrequencies},
        {"expectMixedModePreservesMagnitudeVsMinimum", expectMixedModePreservesMagnitudeVsMinimum},
        {"expectMixedModeStrengthZeroMatchesMinimum", expectMixedModeStrengthZeroMatchesMinimum},
        {"expectMixedModePhaseLimitDoesNotInventUpperBandCorrectionWithoutMagnitudeOpportunity",
         expectMixedModePhaseLimitDoesNotInventUpperBandCorrectionWithoutMagnitudeOpportunity},
        {"expectMixedModePhaseCapControlsLowFrequencyReduction", expectMixedModePhaseCapControlsLowFrequencyReduction},
        {"expectMixedModeStereoImpulsePeaksStayAlignedWithoutLargeBulkDelay", expectMixedModeStereoImpulsePeaksStayAlignedWithoutLargeBulkDelay},
        {"expectMixedModePreservesStereoLowFrequencyPhaseRelationship", expectMixedModePreservesStereoLowFrequencyPhaseRelationship},
        {"expectInputGroupDelayIsPublishedFromMeasuredPhase", expectInputGroupDelayIsPublishedFromMeasuredPhase},
        {"expectContinuousExcessPhaseSeriesStaySmoothAcrossWraps", expectContinuousExcessPhaseSeriesStaySmoothAcrossWraps},
        {"expectMixedPhasePredictedGroupDelayStaysSmoothOnDisplayAxis", expectMixedPhasePredictedGroupDelayStaysSmoothOnDisplayAxis},
        {"expectMixedModeSkipsPhaseOnlyCorrectionWhenMagnitudeInversionIsNegligible", expectMixedModeSkipsPhaseOnlyCorrectionWhenMagnitudeInversionIsNegligible},
        {"expectMixedModePredictedMagnitudeStaysSmoothInLowBand", expectMixedModePredictedMagnitudeStaysSmoothInLowBand},
    });
}
