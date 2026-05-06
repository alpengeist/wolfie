#include <cmath>
#include <iostream>

#include "filter_test_support.h"
#include "test_harness.h"

#include "measurement/filter_designer.h"
#include "measurement/phase_preparation.h"
#include "measurement/response_smoother.h"
#include "measurement/target_curve_designer.h"

namespace {

bool expectMinimumPhaseInputNeedsNoExcessCorrection() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 6.0;
    filterSettings.maxCutDb = 12.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "minimum-phase baseline did not produce a valid filter result\n";
        return false;
    }
    if (result.left.inputExcessPhaseDegrees.size() != result.frequencyAxisHz.size() ||
        result.right.inputExcessPhaseDegrees.size() != result.frequencyAxisHz.size()) {
        std::cerr << "minimum-phase baseline did not populate excess-phase diagnostics\n";
        return false;
    }

    const double leftBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                           result.left.inputExcessPhaseDegrees,
                                                           20.0,
                                                           300.0);
    const double rightBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                            result.right.inputExcessPhaseDegrees,
                                                            20.0,
                                                            300.0);
    if (leftBandMean > 2.0 || rightBandMean > 2.0) {
        std::cerr << "minimum-phase baseline reported unexpected excess phase (left="
                  << leftBandMean << ", right=" << rightBandMean << ")\n";
        return false;
    }

    const double predictedLeftBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                    result.left.predictedExcessPhaseDegrees,
                                                                    20.0,
                                                                    300.0);
    const double predictedRightBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                     result.right.predictedExcessPhaseDegrees,
                                                                     20.0,
                                                                     300.0);
    if (predictedLeftBandMean > 2.0 || predictedRightBandMean > 2.0) {
        std::cerr << "minimum-phase baseline predicted unexpected excess phase (left="
                  << predictedLeftBandMean << ", right=" << predictedRightBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectBulkDelayIsNotTreatedAsExcessPhase() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 6.0;
    filterSettings.maxCutDb = 12.0;

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
        std::cerr << "bulk-delay baseline did not produce a valid filter result\n";
        return false;
    }

    const double leftBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                           result.left.inputExcessPhaseDegrees,
                                                           20.0,
                                                           300.0);
    const double rightBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                            result.right.inputExcessPhaseDegrees,
                                                            20.0,
                                                            300.0);
    if (leftBandMean > 3.0 || rightBandMean > 3.0) {
        std::cerr << "bulk delay was misclassified as excess phase (left="
                  << leftBandMean << ", right=" << rightBandMean << ")\n";
        return false;
    }

    const double predictedLeftBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                    result.left.predictedExcessPhaseDegrees,
                                                                    20.0,
                                                                    300.0);
    const double predictedRightBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                                     result.right.predictedExcessPhaseDegrees,
                                                                     20.0,
                                                                     300.0);
    if (predictedLeftBandMean > 3.0 || predictedRightBandMean > 3.0) {
        std::cerr << "bulk-delay baseline predicted excess phase after delay removal (left="
                  << predictedLeftBandMean << ", right=" << predictedRightBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectExcessPhasePreparationIgnoresDisplayResponseShape() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;

    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0035, 2.0, -1.0);
    const wolfie::FilterDesignResult flatResult =
        wolfie::measurement::designFilters(wolfie::tests::buildFlatResponse(0.0),
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult shapedResult =
        wolfie::measurement::designFilters(wolfie::tests::buildSyntheticResponse(),
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!flatResult.valid || !shapedResult.valid) {
        std::cerr << "display-response invariance case did not produce valid filter results\n";
        return false;
    }

    const double leftInputDelta = wolfie::tests::bandMeanAbsDelta(flatResult.frequencyAxisHz,
                                                                  flatResult.left.inputExcessPhaseContinuousDegrees,
                                                                  shapedResult.left.inputExcessPhaseContinuousDegrees,
                                                                  20.0,
                                                                  300.0);
    const double rightInputDelta = wolfie::tests::bandMeanAbsDelta(flatResult.frequencyAxisHz,
                                                                   flatResult.right.inputExcessPhaseContinuousDegrees,
                                                                   shapedResult.right.inputExcessPhaseContinuousDegrees,
                                                                   20.0,
                                                                   300.0);
    if (leftInputDelta > 1.0 || rightInputDelta > 1.0) {
        std::cerr << "excess-phase preparation still depends on display response shape (left="
                  << leftInputDelta << ", right=" << rightInputDelta << ")\n";
        return false;
    }

    const double leftPredictedDelta = wolfie::tests::bandMeanAbsDelta(flatResult.frequencyAxisHz,
                                                                      flatResult.left.predictedExcessPhaseContinuousDegrees,
                                                                      shapedResult.left.predictedExcessPhaseContinuousDegrees,
                                                                      20.0,
                                                                      300.0);
    const double rightPredictedDelta = wolfie::tests::bandMeanAbsDelta(flatResult.frequencyAxisHz,
                                                                       flatResult.right.predictedExcessPhaseContinuousDegrees,
                                                                       shapedResult.right.predictedExcessPhaseContinuousDegrees,
                                                                       20.0,
                                                                       300.0);
    if (leftPredictedDelta > 1.0 || rightPredictedDelta > 1.0) {
        std::cerr << "predicted excess phase still depends on display response shape (left="
                  << leftPredictedDelta << ", right=" << rightPredictedDelta << ")\n";
        return false;
    }

    return true;
}

bool expectPhasePreparationPrefersDirectSource() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(0.0);
    const wolfie::MeasurementResult directPreferredMeasurement =
        wolfie::tests::buildPhaseMeasurementWithSourceAvailability(measurement.sampleRate,
                                                                   0.0,
                                                                   true,
                                                                   true,
                                                                   true,
                                                                   2.5,
                                                                   0.0,
                                                                   -1.5,
                                                                   0.0,
                                                                   -2.0,
                                                                   0.0);
    const wolfie::MeasurementResult directOnlyMeasurement =
        wolfie::tests::buildPhaseMeasurementWithSourceAvailability(measurement.sampleRate,
                                                                   0.0,
                                                                   true,
                                                                   false,
                                                                   false,
                                                                   2.5,
                                                                   0.0,
                                                                   0.0,
                                                                   0.0,
                                                                   0.0,
                                                                   0.0);
    const wolfie::MeasurementResult roomOnlyMeasurement =
        wolfie::tests::buildPhaseMeasurementWithSourceAvailability(measurement.sampleRate,
                                                                   0.0,
                                                                   false,
                                                                   true,
                                                                   false,
                                                                   0.0,
                                                                   0.0,
                                                                   -1.5,
                                                                   0.0,
                                                                   0.0,
                                                                   0.0);

    const wolfie::FilterDesignResult preferredResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &directPreferredMeasurement);
    const wolfie::FilterDesignResult directResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &directOnlyMeasurement);
    const wolfie::FilterDesignResult roomResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &roomOnlyMeasurement);
    if (!preferredResult.valid || !directResult.valid || !roomResult.valid) {
        std::cerr << "phase-source selection case did not produce valid filter results\n";
        return false;
    }

    const double preferredVsDirect = wolfie::tests::bandMeanAbsDelta(preferredResult.frequencyAxisHz,
                                                                     preferredResult.left.inputExcessPhaseContinuousDegrees,
                                                                     directResult.left.inputExcessPhaseContinuousDegrees,
                                                                     20.0,
                                                                     300.0);
    const double preferredVsRoom = wolfie::tests::bandMeanAbsDelta(preferredResult.frequencyAxisHz,
                                                                   preferredResult.left.inputExcessPhaseContinuousDegrees,
                                                                   roomResult.left.inputExcessPhaseContinuousDegrees,
                                                                   20.0,
                                                                   300.0);
    if (preferredVsDirect > 1.0 || preferredVsRoom < 15.0) {
        std::cerr << "phase preparation did not prefer the direct source as expected (direct delta="
                  << preferredVsDirect << ", room delta=" << preferredVsRoom << ")\n";
        return false;
    }

    return true;
}

bool expectPhasePreparationFallsBackToRawSource() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;

    const wolfie::MeasurementResult rawOnlyMeasurement =
        wolfie::tests::buildPhaseMeasurementWithSourceAvailability(measurement.sampleRate,
                                                                   0.0,
                                                                   false,
                                                                   false,
                                                                   true,
                                                                   0.0,
                                                                   0.0,
                                                                   0.0,
                                                                   0.0,
                                                                   2.0,
                                                                   -2.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(wolfie::tests::buildFlatResponse(0.0),
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &rawOnlyMeasurement);
    if (!result.valid) {
        std::cerr << "raw-source fallback case did not produce a valid filter result\n";
        return false;
    }

    const double leftBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                           result.left.inputExcessPhaseDegrees,
                                                           20.0,
                                                           300.0);
    const double rightBandMean = wolfie::tests::bandMeanAbs(result.frequencyAxisHz,
                                                            result.right.inputExcessPhaseDegrees,
                                                            20.0,
                                                            300.0);
    if (leftBandMean < 15.0 || rightBandMean < 15.0) {
        std::cerr << "raw-source fallback did not publish the expected excess-phase content (left="
                  << leftBandMean << ", right=" << rightBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectExcessPhaseWindowReducesLateTailContribution() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings fullWindowSettings;
    fullWindowSettings.tapCount = 16384;
    fullWindowSettings.phaseMode = "mixed";

    wolfie::FilterDesignSettings shortWindowSettings = fullWindowSettings;
    shortWindowSettings.excessPhaseWindowMs = 4.0;

    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildImpulsePhaseMeasurement(measurement.sampleRate,
                                                    0.0,
                                                    static_cast<int>(measurement.sampleRate * 0.012),
                                                    0.7);
    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(0.0);
    const wolfie::FilterDesignResult fullWindowResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           fullWindowSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult shortWindowResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           shortWindowSettings,
                                           &phaseMeasurement);
    if (!fullWindowResult.valid || !shortWindowResult.valid) {
        std::cerr << "excess-phase window case did not produce valid filter results\n";
        return false;
    }

    const double fullBandMean = wolfie::tests::bandMeanAbs(fullWindowResult.frequencyAxisHz,
                                                           fullWindowResult.left.inputExcessPhaseContinuousDegrees,
                                                           20.0,
                                                           300.0);
    const double shortBandMean = wolfie::tests::bandMeanAbs(shortWindowResult.frequencyAxisHz,
                                                            shortWindowResult.left.inputExcessPhaseContinuousDegrees,
                                                            20.0,
                                                            300.0);
    if (!std::isfinite(fullBandMean) || !std::isfinite(shortBandMean) || shortBandMean >= fullBandMean * 0.75) {
        std::cerr << "excess-phase window did not reduce late-tail contribution enough (full="
                  << fullBandMean << ", short=" << shortBandMean << ")\n";
        return false;
    }

    if (!wolfie::tests::processLogContains(shortWindowResult, "Applied excess-phase window 4.0 ms")) {
        std::cerr << "process log did not mention the configured excess-phase window\n";
        return false;
    }

    return true;
}

bool expectExcessPhaseWindowPreservesResponseAxisDensity() {
    wolfie::MeasurementResult measurement;
    const std::vector<double> responseAxisHz = wolfie::tests::buildLogAxis(20.0, 20000.0, 512);
    measurement.valueSets.push_back(wolfie::tests::buildImpulseValueSet(0.0));
    measurement.valueSets.back().key = "measurement.direct_impulse_response";
    measurement.valueSets.push_back(
        wolfie::tests::buildFlatMagnitudeSpectrum(responseAxisHz, "measurement.direct_magnitude_response"));
    measurement.valueSets.push_back(
        wolfie::tests::buildWrappedPhaseSpectrum(responseAxisHz,
                                                "measurement.direct_phase_response",
                                                0.0,
                                                1.5,
                                                -1.0));

    wolfie::ResponseSmoothingSettings smoothing;
    wolfie::measurement::normalizeResponseSmoothingSettings(smoothing);

    const wolfie::measurement::PreparedPhaseData prepared =
        wolfie::measurement::preparePhaseData(&measurement,
                                              smoothing,
                                              48000,
                                              262144,
                                              15.0);
    if (!prepared.valid) {
        std::cerr << "response-axis preservation case did not produce valid prepared phase data\n";
        return false;
    }
    if (prepared.left.nativeFrequencyAxisHz.size() != responseAxisHz.size() ||
        prepared.right.nativeFrequencyAxisHz.size() != responseAxisHz.size()) {
        std::cerr << "excess-phase window promoted response data to an unusable axis density (left="
                  << prepared.left.nativeFrequencyAxisHz.size() << ", right="
                  << prepared.right.nativeFrequencyAxisHz.size() << ")\n";
        return false;
    }

    return true;
}

bool expectFilterDesignPublishesPhasePreparationMetadataAndProcessLog() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurementWithSourceAvailability(measurement.sampleRate,
                                                                   0.0035,
                                                                   true,
                                                                   true,
                                                                   true,
                                                                   2.0,
                                                                   -1.0,
                                                                   -1.5,
                                                                   0.0,
                                                                   -2.0,
                                                                   0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(wolfie::tests::buildFlatResponse(0.0),
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "phase-preparation metadata case did not produce a valid filter result\n";
        return false;
    }

    if (result.phasePreparationSourceWindow != "direct" ||
        result.phasePreparationSourceKey != "measurement.direct" ||
        result.phasePreparationSeriesKind != "spectrum") {
        std::cerr << "filter result did not publish the expected phase-preparation metadata (window="
                  << result.phasePreparationSourceWindow << ", key=" << result.phasePreparationSourceKey
                  << ", kind=" << result.phasePreparationSeriesKind << ")\n";
        return false;
    }
    if (result.phasePreparationBulkDelaySeconds < 0.003 || result.phasePreparationBulkDelaySeconds > 0.004) {
        std::cerr << "filter result published an unexpected prepared bulk delay value ("
                  << result.phasePreparationBulkDelaySeconds << " s)\n";
        return false;
    }

    if (result.processLog.size() < 6 ||
        !wolfie::tests::processLogContains(result, "Starting filter design") ||
        !wolfie::tests::processLogContains(result, "Prepared matched phase data from measurement.direct") ||
        (!wolfie::tests::processLogContains(result, "Built per-channel low-frequency mixed-phase correction") &&
         !wolfie::tests::processLogContains(result, "Built shared low-frequency mixed-phase correction")) ||
        !wolfie::tests::processLogContains(result, "Designed left filter channel") ||
        !wolfie::tests::processLogContains(result, "Designed right filter channel") ||
        !wolfie::tests::processLogContains(result, "Completed filter design")) {
        std::cerr << "filter result process log did not publish the expected major steps\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    return wolfie::tests::runTestCases({
        {"expectMinimumPhaseInputNeedsNoExcessCorrection", expectMinimumPhaseInputNeedsNoExcessCorrection},
        {"expectBulkDelayIsNotTreatedAsExcessPhase", expectBulkDelayIsNotTreatedAsExcessPhase},
        {"expectExcessPhasePreparationIgnoresDisplayResponseShape", expectExcessPhasePreparationIgnoresDisplayResponseShape},
        {"expectPhasePreparationPrefersDirectSource", expectPhasePreparationPrefersDirectSource},
        {"expectPhasePreparationFallsBackToRawSource", expectPhasePreparationFallsBackToRawSource},
        {"expectExcessPhaseWindowReducesLateTailContribution", expectExcessPhaseWindowReducesLateTailContribution},
        {"expectExcessPhaseWindowPreservesResponseAxisDensity", expectExcessPhaseWindowPreservesResponseAxisDensity},
        {"expectFilterDesignPublishesPhasePreparationMetadataAndProcessLog", expectFilterDesignPublishesPhasePreparationMetadataAndProcessLog},
    });
}
