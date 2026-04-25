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

}  // namespace

bool runFilterDesignTests() {
    return expectDesignedFilterLooksSane();
}
