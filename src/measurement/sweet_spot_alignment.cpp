#include "measurement/sweet_spot_alignment.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wolfie::measurement {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double maxAbsValue(const std::vector<double>& values) {
    double peak = 0.0;
    for (const double value : values) {
        peak = std::max(peak, std::abs(value));
    }
    return peak;
}

std::vector<double> normalizeImpulse(const std::vector<double>& values) {
    std::vector<double> normalized = values;
    const double peak = maxAbsValue(values);
    if (peak <= 1.0e-12) {
        return normalized;
    }

    for (double& value : normalized) {
        value /= peak;
    }
    return normalized;
}

double interpolateLinear(const std::vector<double>& xValues,
                         const std::vector<double>& yValues,
                         double x) {
    if (xValues.empty() || xValues.size() != yValues.size()) {
        return 0.0;
    }
    if (x <= xValues.front()) {
        return yValues.front();
    }
    if (x >= xValues.back()) {
        return yValues.back();
    }

    const auto upper = std::lower_bound(xValues.begin(), xValues.end(), x);
    if (upper == xValues.begin()) {
        return yValues.front();
    }
    if (upper == xValues.end()) {
        return yValues.back();
    }

    const size_t upperIndex = static_cast<size_t>(upper - xValues.begin());
    const size_t lowerIndex = upperIndex - 1;
    const double x0 = xValues[lowerIndex];
    const double x1 = xValues[upperIndex];
    const double span = std::max(x1 - x0, 1.0e-9);
    const double blend = clampValue((x - x0) / span, 0.0, 1.0);
    return yValues[lowerIndex] + ((yValues[upperIndex] - yValues[lowerIndex]) * blend);
}

const MeasurementValueSet* findValidImpulse(const MeasurementResult& result) {
    const MeasurementValueSet* directImpulse = result.findValueSet("measurement.direct_impulse_response");
    if (directImpulse != nullptr && directImpulse->valid()) {
        return directImpulse;
    }
    const MeasurementValueSet* rawImpulse = result.findValueSet("measurement.raw_impulse_response");
    if (rawImpulse != nullptr && rawImpulse->valid()) {
        return rawImpulse;
    }
    return nullptr;
}

}  // namespace

SweetSpotAlignmentView buildSweetSpotAlignmentView(const MeasurementResult& result) {
    SweetSpotAlignmentView view;
    const MeasurementValueSet* impulse = findValidImpulse(result);
    if (impulse == nullptr || result.analysis.sampleRate <= 0 ||
        !result.analysis.left.available || !result.analysis.right.available) {
        return view;
    }

    view.available = true;
    view.captureTooQuiet = result.analysis.captureTooQuiet;
    view.captureClippingDetected = result.analysis.captureClippingDetected;
    view.sampleRate = result.analysis.sampleRate;
    view.leftArrivalMs = result.analysis.left.onsetTimeSeconds * 1000.0;
    view.rightArrivalMs = result.analysis.right.onsetTimeSeconds * 1000.0;
    view.delayMismatchMs = view.leftArrivalMs - view.rightArrivalMs;
    view.pathMismatchCm = (view.delayMismatchMs / 1000.0) * 34300.0;
    view.suggestedMoveCm = std::abs(view.pathMismatchCm) * 0.5;
    view.confidenceDb = std::min(result.analysis.left.impulsePeakToNoiseDb,
                                 result.analysis.right.impulsePeakToNoiseDb);
    if (std::abs(view.delayMismatchMs) > view.centeredToleranceMs) {
        view.suggestedDirection = view.delayMismatchMs < 0.0
                                      ? SweetSpotMoveDirection::Right
                                      : SweetSpotMoveDirection::Left;
    }

    std::vector<double> baseTimeMs;
    baseTimeMs.reserve(impulse->xValues.size());
    for (const double value : impulse->xValues) {
        baseTimeMs.push_back(value * 1000.0);
    }

    std::vector<double> leftImpulse = normalizeImpulse(impulse->leftValues);
    std::vector<double> rightImpulse = normalizeImpulse(impulse->rightValues);
    if (baseTimeMs.empty() || leftImpulse.size() != baseTimeMs.size() || rightImpulse.size() != baseTimeMs.size()) {
        view.available = false;
        return view;
    }

    const double midpointArrivalMs = (view.leftArrivalMs + view.rightArrivalMs) * 0.5;
    const double leftOffsetMs = view.leftArrivalMs - midpointArrivalMs;
    const double rightOffsetMs = view.rightArrivalMs - midpointArrivalMs;

    std::vector<double> shiftedLeftTimeMs(baseTimeMs.size(), 0.0);
    std::vector<double> shiftedRightTimeMs(baseTimeMs.size(), 0.0);
    for (size_t index = 0; index < baseTimeMs.size(); ++index) {
        shiftedLeftTimeMs[index] = baseTimeMs[index] + leftOffsetMs;
        shiftedRightTimeMs[index] = baseTimeMs[index] + rightOffsetMs;
    }

    const double minTimeMs = std::min(shiftedLeftTimeMs.front(), shiftedRightTimeMs.front()) - 0.2;
    const double maxTimeMs = std::max(shiftedLeftTimeMs.back(), shiftedRightTimeMs.back()) + 0.2;
    const double stepMs = 1000.0 / static_cast<double>(view.sampleRate);
    const size_t sampleCount = std::max<size_t>(
        2,
        static_cast<size_t>(std::ceil((maxTimeMs - minTimeMs) / std::max(stepMs, 1.0e-6))) + 1);

    view.timeAxisMs.resize(sampleCount, 0.0);
    view.leftImpulse.resize(sampleCount, 0.0);
    view.rightImpulse.resize(sampleCount, 0.0);
    for (size_t index = 0; index < sampleCount; ++index) {
        const double x = minTimeMs + (static_cast<double>(index) * stepMs);
        view.timeAxisMs[index] = x;
        view.leftImpulse[index] = interpolateLinear(shiftedLeftTimeMs, leftImpulse, x);
        view.rightImpulse[index] = interpolateLinear(shiftedRightTimeMs, rightImpulse, x);
    }

    return view;
}

}  // namespace wolfie::measurement
