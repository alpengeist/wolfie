#include <iostream>

#include "filter_test_support.h"
#include "test_harness.h"

#include "measurement/filter_analysis.h"
#include "measurement/filter_designer.h"
#include "measurement/stereo_diagnostics.h"
#include "measurement/target_curve_designer.h"

namespace {

bool expectFilterAnalysisPublishesImprovedPostFilterDiagnostics() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult filterResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!filterResult.valid) {
        std::cerr << "filter-analysis fixture did not produce a valid filter result\n";
        return false;
    }

    const wolfie::StereoDiagnosticsResult before =
        wolfie::measurement::buildStereoDiagnostics(phaseMeasurement, "room");
    const wolfie::FilterAnalysisResult analysis =
        wolfie::measurement::buildFilterAnalysis(phaseMeasurement, filterResult);
    if (!before.available || !before.summary.available ||
        !analysis.available || !analysis.room.available || !analysis.room.summary.available) {
        std::cerr << "filter analysis did not publish a usable before/after result\n";
        return false;
    }

    if (analysis.direct.window != "direct" || analysis.room.window != "room") {
        std::cerr << "filter analysis published unexpected window metadata\n";
        return false;
    }

    if (analysis.room.frequencyAxisHz.empty() ||
        analysis.room.phaseDeltaDegrees.size() != analysis.room.frequencyAxisHz.size() ||
        analysis.room.magnitudeDeltaDb.size() != analysis.room.frequencyAxisHz.size()) {
        std::cerr << "filter analysis did not publish plotted room diagnostics\n";
        return false;
    }

    if (analysis.room.summary.lowBandPhaseRmsDegrees > before.summary.lowBandPhaseRmsDegrees * 0.8) {
        std::cerr << "filter analysis room phase RMS did not improve enough (before="
                  << before.summary.lowBandPhaseRmsDegrees << ", after="
                  << analysis.room.summary.lowBandPhaseRmsDegrees << ")\n";
        return false;
    }

    if (analysis.room.summary.phaseSimilarity < before.summary.phaseSimilarity) {
        std::cerr << "filter analysis room phase similarity regressed (before="
                  << before.summary.phaseSimilarity << ", after="
                  << analysis.room.summary.phaseSimilarity << ")\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    return wolfie::tests::runTestCases({
        {"expectFilterAnalysisPublishesImprovedPostFilterDiagnostics", expectFilterAnalysisPublishesImprovedPostFilterDiagnostics},
    });
}
