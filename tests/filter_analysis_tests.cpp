#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

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

bool expectStereoDiagnosticsAnchorsIaccWindowsToImpulseArrival() {
    wolfie::measurement::StereoDiagnosticsInput input;
    input.sampleRate = 48000;
    input.delayMismatchMs = 0.0;
    input.frequencyAxisHz = {20.0, 100.0, 1000.0};
    input.leftSpectrum = {{1.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}};
    input.rightSpectrum = {{1.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}};

    constexpr size_t kSampleCount = 6000;
    constexpr double kStartTimeSeconds = 0.100;
    const double sampleStepSeconds = 1.0 / static_cast<double>(input.sampleRate);
    input.directImpulseTimeSeconds.reserve(kSampleCount);
    input.windowImpulseTimeSeconds.reserve(kSampleCount);
    input.directImpulseLeft.assign(kSampleCount, 0.0);
    input.directImpulseRight.assign(kSampleCount, 0.0);
    input.windowImpulseLeft.assign(kSampleCount, 0.0);
    input.windowImpulseRight.assign(kSampleCount, 0.0);
    for (size_t index = 0; index < kSampleCount; ++index) {
        const double timeSeconds = kStartTimeSeconds + (static_cast<double>(index) * sampleStepSeconds);
        input.directImpulseTimeSeconds.push_back(timeSeconds);
        input.windowImpulseTimeSeconds.push_back(timeSeconds);
    }

    input.directImpulseLeft[0] = 1.0;
    input.directImpulseRight[0] = 1.0;
    input.directImpulseLeft[16] = 0.25;
    input.directImpulseRight[16] = 0.25;

    input.windowImpulseLeft[0] = 1.0;
    input.windowImpulseRight[0] = 1.0;
    input.windowImpulseLeft[120] = 0.35;
    input.windowImpulseRight[120] = 0.35;
    input.windowImpulseLeft[4000] = 0.15;
    input.windowImpulseRight[4000] = 0.15;
    input.windowImpulseLeft[5000] = 0.08;
    input.windowImpulseRight[5000] = 0.08;

    const wolfie::StereoDiagnosticsResult diagnostics =
        wolfie::measurement::buildStereoDiagnostics(input, "room");
    if (!diagnostics.available || !diagnostics.summary.available) {
        std::cerr << "shifted IACC fixture did not produce an available diagnostics result\n";
        return false;
    }

    if (!std::isfinite(diagnostics.summary.iacc10) ||
        !std::isfinite(diagnostics.summary.iacc20) ||
        !std::isfinite(diagnostics.summary.iacc80) ||
        !std::isfinite(diagnostics.summary.iaccLate)) {
        std::cerr << "IACC windows were not anchored to the shifted impulse arrival\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    return wolfie::tests::runTestCases({
        {"expectFilterAnalysisPublishesImprovedPostFilterDiagnostics", expectFilterAnalysisPublishesImprovedPostFilterDiagnostics},
        {"expectStereoDiagnosticsAnchorsIaccWindowsToImpulseArrival", expectStereoDiagnosticsAnchorsIaccWindowsToImpulseArrival},
    });
}
