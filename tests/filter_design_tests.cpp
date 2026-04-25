#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "core/models.h"
#include "measurement/filter_designer.h"
#include "measurement/target_curve_designer.h"

namespace {

std::vector<double> buildLogAxis(double minFrequencyHz, double maxFrequencyHz, int pointCount) {
    std::vector<double> axis;
    axis.reserve(static_cast<size_t>(pointCount));
    const double logMin = std::log10(std::max(minFrequencyHz, 1.0));
    const double logMax = std::log10(std::max(maxFrequencyHz, minFrequencyHz + 1.0));
    for (int index = 0; index < pointCount; ++index) {
        const double t = pointCount == 1 ? 0.0 : static_cast<double>(index) / static_cast<double>(pointCount - 1);
        axis.push_back(std::pow(10.0, logMin + ((logMax - logMin) * t)));
    }
    return axis;
}

wolfie::SmoothedResponse buildSyntheticResponse() {
    wolfie::SmoothedResponse response;
    response.frequencyAxisHz = buildLogAxis(20.0, 20000.0, 512);
    response.leftChannelDb.reserve(response.frequencyAxisHz.size());
    response.rightChannelDb.reserve(response.frequencyAxisHz.size());
    for (const double frequencyHz : response.frequencyAxisHz) {
        const double logRatio = std::log10(frequencyHz / 1000.0);
        response.leftChannelDb.push_back((-4.0 * logRatio) + (2.5 * std::exp(-std::pow((frequencyHz - 75.0) / 55.0, 2.0))));
        response.rightChannelDb.push_back((-3.0 * logRatio) - (2.0 * std::exp(-std::pow((frequencyHz - 2800.0) / 1200.0, 2.0))));
    }
    return response;
}

wolfie::SmoothedResponse buildFlatResponse(double levelDb) {
    wolfie::SmoothedResponse response;
    response.frequencyAxisHz = buildLogAxis(20.0, 20000.0, 512);
    response.leftChannelDb.assign(response.frequencyAxisHz.size(), levelDb);
    response.rightChannelDb.assign(response.frequencyAxisHz.size(), levelDb);
    return response;
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

    const wolfie::SmoothedResponse response = buildSyntheticResponse();
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

bool expectExactTargetCurveEvaluationCapturesBellPeak() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::SmoothedResponse sparseResponse;
    sparseResponse.frequencyAxisHz = buildLogAxis(20.0, 20000.0, 16);
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

    const wolfie::SmoothedResponse response = buildFlatResponse(18.0);
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

}  // namespace

bool runFilterDesignTests() {
    return expectDesignedFilterLooksSane() &&
           expectExactTargetCurveEvaluationCapturesBellPeak() &&
           expectTargetCurveAnchorsToMeasuredLevel();
}
