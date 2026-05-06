#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "filter_test_support.h"
#include "test_harness.h"

#include "measurement/filter_designer.h"
#include "measurement/target_curve_designer.h"

namespace {

double interpolateLogFrequency(const std::vector<double>& frequencyAxisHz,
                               const std::vector<double>& values,
                               double frequencyHz) {
    if (frequencyAxisHz.empty() || values.empty()) {
        return 0.0;
    }

    const size_t count = std::min(frequencyAxisHz.size(), values.size());
    if (count == 1 || frequencyHz <= frequencyAxisHz.front()) {
        return values.front();
    }
    if (frequencyHz >= frequencyAxisHz[count - 1]) {
        return values[count - 1];
    }

    const auto upper = std::lower_bound(frequencyAxisHz.begin(),
                                        frequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count),
                                        frequencyHz);
    const size_t upperIndex = static_cast<size_t>(std::distance(frequencyAxisHz.begin(), upper));
    const size_t lowerIndex = upperIndex - 1;
    const double x0 = std::log10(std::max(frequencyAxisHz[lowerIndex], 1.0));
    const double x1 = std::log10(std::max(frequencyAxisHz[upperIndex], 1.0));
    const double x = std::log10(std::max(frequencyHz, 1.0));
    const double y0 = values[lowerIndex];
    const double y1 = values[upperIndex];
    const double t = std::clamp((x - x0) / std::max(x1 - x0, 1.0e-9), 0.0, 1.0);
    return y0 + ((y1 - y0) * t);
}

wolfie::SmoothedResponse buildBoostLimitedDipResponse(double centerFrequencyHz,
                                                      double depthDb,
                                                      double logWidth) {
    wolfie::SmoothedResponse response;
    response.frequencyAxisHz = wolfie::tests::buildLogAxis(20.0, 20000.0, 512);
    response.leftChannelDb.reserve(response.frequencyAxisHz.size());
    response.rightChannelDb.reserve(response.frequencyAxisHz.size());
    const double safeCenterHz = std::max(centerFrequencyHz, 1.0);
    const double safeWidth = std::max(logWidth, 1.0e-6);
    for (const double frequencyHz : response.frequencyAxisHz) {
        const double logDistance =
            (std::log10(std::max(frequencyHz, 1.0)) - std::log10(safeCenterHz)) / safeWidth;
        const double responseDb = -depthDb * std::exp(-(logDistance * logDistance));
        response.leftChannelDb.push_back(responseDb);
        response.rightChannelDb.push_back(responseDb);
    }
    return response;
}

wolfie::SmoothedResponse buildCutLimitedPeakResponse(double centerFrequencyHz,
                                                     double heightDb,
                                                     double logWidth) {
    wolfie::SmoothedResponse response;
    response.frequencyAxisHz = wolfie::tests::buildLogAxis(20.0, 20000.0, 512);
    response.leftChannelDb.reserve(response.frequencyAxisHz.size());
    response.rightChannelDb.reserve(response.frequencyAxisHz.size());
    const double safeCenterHz = std::max(centerFrequencyHz, 1.0);
    const double safeWidth = std::max(logWidth, 1.0e-6);
    for (const double frequencyHz : response.frequencyAxisHz) {
        const double logDistance =
            (std::log10(std::max(frequencyHz, 1.0)) - std::log10(safeCenterHz)) / safeWidth;
        const double responseDb = heightDb * std::exp(-(logDistance * logDistance));
        response.leftChannelDb.push_back(responseDb);
        response.rightChannelDb.push_back(responseDb);
    }
    return response;
}

double gaussianOnLogFrequency(double frequencyHz, double centerFrequencyHz, double logWidth) {
    const double safeCenterHz = std::max(centerFrequencyHz, 1.0);
    const double safeWidth = std::max(logWidth, 1.0e-6);
    const double logDistance =
        (std::log10(std::max(frequencyHz, 1.0)) - std::log10(safeCenterHz)) / safeWidth;
    return std::exp(-(logDistance * logDistance));
}

wolfie::SmoothedResponse buildBroadPeakRegressionResponse() {
    wolfie::SmoothedResponse response;
    response.frequencyAxisHz = wolfie::tests::buildLogAxis(20.0, 20000.0, 512);
    response.leftChannelDb.reserve(response.frequencyAxisHz.size());
    response.rightChannelDb.reserve(response.frequencyAxisHz.size());
    for (const double frequencyHz : response.frequencyAxisHz) {
        const double responseDb =
            (-3.5 * gaussianOnLogFrequency(frequencyHz, 26.0, 0.16)) +
            (7.25 * gaussianOnLogFrequency(frequencyHz, 115.0, 0.22)) +
            (-4.0 * gaussianOnLogFrequency(frequencyHz, 320.0, 0.13));
        response.leftChannelDb.push_back(responseDb);
        response.rightChannelDb.push_back(responseDb);
    }
    return response;
}

double maxFiniteValueInBand(const std::vector<double>& frequencyAxisHz,
                            const std::vector<double>& values,
                            double minFrequencyHz,
                            double maxFrequencyHz) {
    double maximum = -std::numeric_limits<double>::infinity();
    for (size_t index = 0; index < frequencyAxisHz.size() && index < values.size(); ++index) {
        if (!std::isfinite(values[index]) ||
            frequencyAxisHz[index] < minFrequencyHz ||
            frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        maximum = std::max(maximum, values[index]);
    }
    return maximum;
}

bool expectDesignedFilterLooksSane() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    targetCurve.lowGainDb = 2.0;
    targetCurve.midGainDb = 0.0;
    targetCurve.highGainDb = -1.5;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 6.0;
    filterSettings.maxCutDb = 12.0;
    filterSettings.highCorrectionHz = 18000.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildSyntheticResponse();
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response, measurement, targetCurve, filterSettings);
    if (!result.valid) {
        std::cerr << "filter design did not produce a valid result\n";
        return false;
    }
    if (result.tapCount != 16384 || result.fftSize != 65536) {
        std::cerr << "filter design used unexpected tap or fft size\n";
        return false;
    }
    if (result.left.filterTaps.size() != static_cast<size_t>(result.tapCount) ||
        result.right.filterTaps.size() != static_cast<size_t>(result.tapCount)) {
        std::cerr << "filter design returned the wrong tap count\n";
        return false;
    }
    if (result.frequencyAxisHz.size() != static_cast<size_t>(filterSettings.displayPointCount)) {
        std::cerr << "filter design returned the wrong display resolution\n";
        return false;
    }

    const auto maxLeftCorrection = std::max_element(result.left.correctionCurveDb.begin(),
                                                    result.left.correctionCurveDb.end());
    const auto minLeftCorrection = std::min_element(result.left.correctionCurveDb.begin(),
                                                    result.left.correctionCurveDb.end());
    if (maxLeftCorrection == result.left.correctionCurveDb.end() ||
        *maxLeftCorrection > 6.2 || *minLeftCorrection < -12.2) {
        std::cerr << "filter design did not respect boost/cut limits\n";
        return false;
    }

    double leftError = 0.0;
    double rightError = 0.0;
    for (size_t index = 0; index < result.frequencyAxisHz.size(); ++index) {
        leftError += std::abs(result.left.correctedResponseDb[index] - result.targetCurveDb[index]);
        rightError += std::abs(result.right.correctedResponseDb[index] - result.targetCurveDb[index]);
    }
    leftError /= static_cast<double>(result.frequencyAxisHz.size());
    rightError /= static_cast<double>(result.frequencyAxisHz.size());
    if (leftError > 1.5 || rightError > 1.5) {
        std::cerr << "predicted corrected response is too far from target\n";
        return false;
    }

    return true;
}

bool expectExactTargetCurveEvaluationCapturesBellPeak() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::SmoothedResponse sparseResponse;
    sparseResponse.frequencyAxisHz = wolfie::tests::buildLogAxis(20.0, 20000.0, 16);
    sparseResponse.leftChannelDb.assign(sparseResponse.frequencyAxisHz.size(), 0.0);
    sparseResponse.rightChannelDb.assign(sparseResponse.frequencyAxisHz.size(), 0.0);

    wolfie::TargetCurveSettings targetCurve;
    targetCurve.eqBands = {wolfie::measurement::makeDefaultTargetEqBand(1000.0, 0)};
    targetCurve.eqBands.front().enabled = true;
    targetCurve.eqBands.front().gainDb = 9.0;
    targetCurve.eqBands.front().q = 6.0;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    const wolfie::measurement::TargetCurvePlotData plot =
        wolfie::measurement::buildTargetCurvePlotData(sparseResponse, measurement, targetCurve, 0);
    const double exactDb =
        wolfie::measurement::evaluateTargetCurveDbAtFrequency(measurement, targetCurve, plot.minFrequencyHz, plot.maxFrequencyHz, 1000.0);
    const double interpolatedDb = interpolateLogFrequency(plot.frequencyAxisHz, plot.targetCurveDb, 1000.0);
    if (exactDb < 8.5) {
        std::cerr << "exact target-curve evaluation did not preserve the bell peak\n";
        return false;
    }
    if (interpolatedDb > exactDb - 1.0) {
        std::cerr << "sparse plot interpolation unexpectedly matched the bell peak\n";
        return false;
    }

    return true;
}

bool expectTargetCurveAnchorsToMeasuredLevel() {
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

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(18.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response, measurement, targetCurve, filterSettings);
    if (!result.valid) {
        std::cerr << "level-anchoring test did not produce a valid filter result\n";
        return false;
    }

    double correctionMeanAbs = 0.0;
    double targetMeanDb = 0.0;
    for (size_t index = 0; index < result.frequencyAxisHz.size(); ++index) {
        correctionMeanAbs += std::abs(result.left.correctionCurveDb[index]);
        targetMeanDb += result.targetCurveDb[index];
    }
    correctionMeanAbs /= static_cast<double>(std::max<size_t>(result.frequencyAxisHz.size(), 1));
    targetMeanDb /= static_cast<double>(std::max<size_t>(result.frequencyAxisHz.size(), 1));
    if (correctionMeanAbs > 0.25) {
        std::cerr << "flat offset response still produced a non-trivial correction\n";
        return false;
    }
    if (std::abs(targetMeanDb - 18.0) > 0.25) {
        std::cerr << "target curve was not anchored to the measured response level\n";
        return false;
    }

    return true;
}

bool expectLowCorrectionBoundChangesBassCorrectionShape() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings lowBound30;
    lowBound30.tapCount = 16384;
    lowBound30.maxBoostDb = 18.0;
    lowBound30.maxCutDb = 12.0;
    lowBound30.smoothness = 0.5;
    lowBound30.lowCorrectionHz = 30.0;
    lowBound30.highCorrectionHz = 18000.0;

    wolfie::FilterDesignSettings lowBound50 = lowBound30;
    lowBound50.lowCorrectionHz = 50.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::FilterDesignResult lowBound30Result =
        wolfie::measurement::designFilters(response, measurement, targetCurve, lowBound30);
    const wolfie::FilterDesignResult lowBound50Result =
        wolfie::measurement::designFilters(response, measurement, targetCurve, lowBound50);
    if (!lowBound30Result.valid || !lowBound50Result.valid) {
        std::cerr << "low correction bound regression case did not produce valid filter results\n";
        return false;
    }

    const double bassBandDelta = wolfie::tests::bandMeanAbsDelta(lowBound30Result.frequencyAxisHz,
                                                                 lowBound30Result.left.correctionCurveDb,
                                                                 lowBound50Result.left.correctionCurveDb,
                                                                 30.0,
                                                                 55.0);
    if (bassBandDelta < 1.0) {
        std::cerr << "changing low correction bound had too little effect on bass correction (delta="
                  << bassBandDelta << ")\n";
        return false;
    }

    const double lowBandCorrection = wolfie::tests::bandMeanAbs(lowBound30Result.frequencyAxisHz,
                                                                lowBound30Result.left.correctionCurveDb,
                                                                30.0,
                                                                38.0);
    const double upperBassCorrection = wolfie::tests::bandMeanAbs(lowBound30Result.frequencyAxisHz,
                                                                  lowBound30Result.left.correctionCurveDb,
                                                                  45.0,
                                                                  55.0);
    if (lowBandCorrection < upperBassCorrection + 1.0) {
        std::cerr << "low correction taper flattened the 30-55 Hz correction shape (low="
                  << lowBandCorrection << ", upper=" << upperBassCorrection << ")\n";
        return false;
    }

    return true;
}

bool expectDefaultSettingsUseNoBoostCeiling() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings defaultSettings;
    defaultSettings.tapCount = 16384;
    defaultSettings.maxCutDb = 12.0;
    defaultSettings.highCorrectionHz = 18000.0;

    wolfie::FilterDesignSettings boostedSettings = defaultSettings;
    boostedSettings.maxBoostDb = 6.0;

    const wolfie::SmoothedResponse response = wolfie::tests::buildLowFrequencyRollOffResponse();
    const wolfie::FilterDesignResult defaultResult =
        wolfie::measurement::designFilters(response, measurement, targetCurve, defaultSettings);
    const wolfie::FilterDesignResult boostedResult =
        wolfie::measurement::designFilters(response, measurement, targetCurve, boostedSettings);
    if (!defaultResult.valid || !boostedResult.valid) {
        std::cerr << "no-boost ceiling regression case did not produce valid filter results\n";
        return false;
    }

    const auto maxCorrection = std::max_element(defaultResult.left.correctionCurveDb.begin(),
                                                defaultResult.left.correctionCurveDb.end());
    if (maxCorrection == defaultResult.left.correctionCurveDb.end() ||
        *maxCorrection > 0.05) {
        std::cerr << "default filter design did not stay at the 0 dB no-boost ceiling (max="
                  << (maxCorrection == defaultResult.left.correctionCurveDb.end() ? 0.0 : *maxCorrection) << ")\n";
        return false;
    }

    const double defaultBassCorrection = wolfie::tests::bandMeanAbs(defaultResult.frequencyAxisHz,
                                                                    defaultResult.left.correctionCurveDb,
                                                                    25.0,
                                                                    80.0);
    const double boostedBassCorrection = wolfie::tests::bandMeanAbs(boostedResult.frequencyAxisHz,
                                                                    boostedResult.left.correctionCurveDb,
                                                                    25.0,
                                                                    80.0);
    if (boostedBassCorrection < defaultBassCorrection + 2.0) {
        std::cerr << "default no-boost ceiling did not materially reduce bass boost demand (default="
                  << defaultBassCorrection << ", boosted=" << boostedBassCorrection << ")\n";
        return false;
    }

    return true;
}

bool expectTargetCurveLevelOffsetSurvivesAnchoring() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    targetCurve.levelOffsetDb = 3.0;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    const wolfie::SmoothedResponse response = wolfie::tests::buildFlatResponse(18.0);
    const wolfie::measurement::TargetCurvePlotData plot =
        wolfie::measurement::buildTargetCurvePlotData(response, measurement, targetCurve, std::nullopt);
    if (plot.frequencyAxisHz.empty() || plot.targetCurveDb.empty()) {
        std::cerr << "level-offset target-curve plot came back empty\n";
        return false;
    }

    double targetMeanDb = 0.0;
    for (const double value : plot.targetCurveDb) {
        targetMeanDb += value;
    }
    targetMeanDb /= static_cast<double>(plot.targetCurveDb.size());
    if (std::abs(targetMeanDb - 21.0) > 0.25) {
        std::cerr << "target-curve level offset was lost after anchoring\n";
        return false;
    }

    return true;
}

bool expectBoostCeilingHasInertiaForNarrowDips() {
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
    filterSettings.lowCorrectionHz = 30.0;
    filterSettings.highCorrectionHz = 18000.0;
    filterSettings.smoothness = 1.0;

    const wolfie::SmoothedResponse narrowDip = buildBoostLimitedDipResponse(48.0, 12.0, 0.045);
    const wolfie::SmoothedResponse broadDip = buildBoostLimitedDipResponse(48.0, 12.0, 0.14);
    const wolfie::FilterDesignResult narrowResult =
        wolfie::measurement::designFilters(narrowDip, measurement, targetCurve, filterSettings);
    const wolfie::FilterDesignResult broadResult =
        wolfie::measurement::designFilters(broadDip, measurement, targetCurve, filterSettings);
    if (!narrowResult.valid || !broadResult.valid) {
        std::cerr << "boost-ceiling inertia regression case did not produce valid filter results\n";
        return false;
    }

    const double narrowPeak = maxFiniteValueInBand(narrowResult.frequencyAxisHz,
                                                   narrowResult.left.correctionCurveDb,
                                                   30.0,
                                                   90.0);
    const double broadPeak = maxFiniteValueInBand(broadResult.frequencyAxisHz,
                                                  broadResult.left.correctionCurveDb,
                                                  30.0,
                                                  90.0);
    if (!std::isfinite(narrowPeak) || !std::isfinite(broadPeak)) {
        std::cerr << "boost-ceiling inertia regression case did not publish usable correction values\n";
        return false;
    }

    if (narrowPeak > 4.5) {
        std::cerr << "narrow boost-limited dip still ran too hard into the ceiling (peak="
                  << narrowPeak << ")\n";
        return false;
    }

    const double narrowBandMean = wolfie::tests::bandMeanAbs(narrowResult.frequencyAxisHz,
                                                             narrowResult.left.correctionCurveDb,
                                                             35.0,
                                                             75.0);
    const double broadBandMean = wolfie::tests::bandMeanAbs(broadResult.frequencyAxisHz,
                                                            broadResult.left.correctionCurveDb,
                                                            35.0,
                                                            75.0);
    if (broadBandMean < narrowBandMean + 0.35) {
        std::cerr << "broad dip did not draw materially more boost than the narrow dip (narrow="
                  << narrowBandMean << ", broad=" << broadBandMean << ")\n";
        return false;
    }

    if (broadPeak < 3.25) {
        std::cerr << "broad dip no longer made meaningful use of the explicit boost budget (peak="
                  << broadPeak << ")\n";
        return false;
    }

    return true;
}

bool expectCutsUseConfiguredBudgetMoreLiterally() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    targetCurve.lowGainDb = -6.0;
    targetCurve.midGainDb = -6.0;
    targetCurve.highGainDb = -6.0;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 2.0;
    filterSettings.maxCutDb = 12.0;
    filterSettings.lowCorrectionHz = 30.0;
    filterSettings.highCorrectionHz = 18000.0;
    filterSettings.smoothness = 4.0;

    const wolfie::SmoothedResponse peakResponse = buildCutLimitedPeakResponse(50.0, 11.0, 0.15);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(peakResponse, measurement, targetCurve, filterSettings);
    if (!result.valid) {
        std::cerr << "literal cut-budget regression case did not produce a valid filter result\n";
        return false;
    }

    const double centerCorrection =
        interpolateLogFrequency(result.frequencyAxisHz, result.left.correctionCurveDb, 50.0);
    if (centerCorrection > -8.8) {
        std::cerr << "cut budget still compressed too much before the solver (center="
                  << centerCorrection << ")\n";
        return false;
    }

    if (centerCorrection < -12.05) {
        std::cerr << "cut budget exceeded the configured limit (center=" << centerCorrection << ")\n";
        return false;
    }

    return true;
}

bool expectBroadPeakCutsLeaveSmoothHeadroom() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 0.0;
    filterSettings.maxCutDb = 12.0;
    filterSettings.lowCorrectionHz = 30.0;
    filterSettings.highCorrectionHz = 18000.0;
    filterSettings.smoothness = 1.25;

    const wolfie::SmoothedResponse response = buildBroadPeakRegressionResponse();
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response, measurement, targetCurve, filterSettings);
    if (!result.valid) {
        std::cerr << "broad-peak headroom regression case did not produce a valid filter result\n";
        return false;
    }

    double worstUnderswingDb = 0.0;
    double sourceAboveTargetMeanDb = 0.0;
    double correctedAboveTargetMeanDb = 0.0;
    size_t used = 0;
    for (size_t index = 0; index < result.frequencyAxisHz.size(); ++index) {
        const double frequencyHz = result.frequencyAxisHz[index];
        if (frequencyHz < 55.0 || frequencyHz > 210.0) {
            continue;
        }

        const double targetDb = result.targetCurveDb[index];
        const double sourceDb =
            interpolateLogFrequency(response.frequencyAxisHz, response.leftChannelDb, frequencyHz);
        const double correctedDb = result.left.correctedResponseDb[index];
        worstUnderswingDb = std::min(worstUnderswingDb, correctedDb - targetDb);
        sourceAboveTargetMeanDb += std::max(sourceDb - targetDb, 0.0);
        correctedAboveTargetMeanDb += std::max(correctedDb - targetDb, 0.0);
        ++used;
    }

    if (used == 0) {
        std::cerr << "broad-peak headroom regression case had no usable band samples\n";
        return false;
    }

    sourceAboveTargetMeanDb /= static_cast<double>(used);
    correctedAboveTargetMeanDb /= static_cast<double>(used);

    if (correctedAboveTargetMeanDb > sourceAboveTargetMeanDb - 1.5) {
        std::cerr << "broad peak was not reduced enough (source=" << sourceAboveTargetMeanDb
                  << ", corrected=" << correctedAboveTargetMeanDb << ")\n";
        return false;
    }

    if (worstUnderswingDb < -0.8) {
        std::cerr << "broad peak correction still swung below the target too hard (underswing="
                  << worstUnderswingDb << ")\n";
        return false;
    }

    const double centerTargetDb =
        interpolateLogFrequency(result.frequencyAxisHz, result.targetCurveDb, 115.0);
    const double centerCorrectedDb =
        interpolateLogFrequency(result.frequencyAxisHz, result.left.correctedResponseDb, 115.0);
    const double centerOffsetDb = centerCorrectedDb - centerTargetDb;
    if (centerOffsetDb < -0.5 || centerOffsetDb > 1.5) {
        std::cerr << "broad peak correction did not settle into a small smooth headroom window (offset="
                  << centerOffsetDb << ")\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    return wolfie::tests::runTestCases({
        {"expectDesignedFilterLooksSane", expectDesignedFilterLooksSane},
        {"expectLowCorrectionBoundChangesBassCorrectionShape", expectLowCorrectionBoundChangesBassCorrectionShape},
        {"expectDefaultSettingsUseNoBoostCeiling", expectDefaultSettingsUseNoBoostCeiling},
        {"expectBoostCeilingHasInertiaForNarrowDips", expectBoostCeilingHasInertiaForNarrowDips},
        {"expectCutsUseConfiguredBudgetMoreLiterally", expectCutsUseConfiguredBudgetMoreLiterally},
        {"expectBroadPeakCutsLeaveSmoothHeadroom", expectBroadPeakCutsLeaveSmoothHeadroom},
        {"expectExactTargetCurveEvaluationCapturesBellPeak", expectExactTargetCurveEvaluationCapturesBellPeak},
        {"expectTargetCurveAnchorsToMeasuredLevel", expectTargetCurveAnchorsToMeasuredLevel},
        {"expectTargetCurveLevelOffsetSurvivesAnchoring", expectTargetCurveLevelOffsetSurvivesAnchoring},
    });
}
