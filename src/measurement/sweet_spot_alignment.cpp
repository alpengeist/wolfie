#include "measurement/sweet_spot_alignment.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "measurement/impulse_windowing.h"

namespace wolfie::measurement {

namespace {

constexpr double kGraphHalfWindowMs = 0.65;
constexpr double kGraphDelayMarginMs = 0.35;
constexpr double kMinimumStableGraphSpanMs = 0.25;
constexpr double kMinimumFocusFrequencyHz = 1000.0;
constexpr double kMinimumPolarityCorrelation = -0.35;

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

std::vector<double> buildDisplayEnvelope(const std::vector<double>& values, size_t radius) {
    if (values.empty()) {
        return {};
    }

    const size_t safeRadius = std::max<size_t>(1, radius);
    std::vector<double> prefixEnergy(values.size() + 1, 0.0);
    for (size_t index = 0; index < values.size(); ++index) {
        prefixEnergy[index + 1] = prefixEnergy[index] + (values[index] * values[index]);
    }

    std::vector<double> envelope(values.size(), 0.0);
    for (size_t index = 0; index < values.size(); ++index) {
        const size_t begin = index > safeRadius ? index - safeRadius : 0;
        const size_t end = std::min(values.size(), index + safeRadius + 1);
        envelope[index] = prefixEnergy[end] - prefixEnergy[begin];
    }
    return normalizeImpulse(envelope);
}

double rmsValue(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    double energy = 0.0;
    for (const double value : values) {
        energy += value * value;
    }
    return std::sqrt(energy / static_cast<double>(values.size()));
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
    const MeasurementValueSet* roomImpulse = result.findValueSet("measurement.room_impulse_response");
    if (roomImpulse != nullptr && roomImpulse->valid()) {
        return roomImpulse;
    }
    return nullptr;
}

bool axisContainsTime(const std::vector<double>& axisMs, double timeMs, double toleranceMs) {
    if (axisMs.empty()) {
        return false;
    }
    return timeMs >= (axisMs.front() - toleranceMs) && timeMs <= (axisMs.back() + toleranceMs);
}

double localWaveformCorrelation(const std::vector<double>& leftTimeMs,
                                const std::vector<double>& leftValues,
                                const std::vector<double>& rightTimeMs,
                                const std::vector<double>& rightValues,
                                double windowHalfWidthMs,
                                double stepMs) {
    if (leftTimeMs.empty() || rightTimeMs.empty() ||
        leftTimeMs.size() != leftValues.size() || rightTimeMs.size() != rightValues.size()) {
        return 0.0;
    }

    double dot = 0.0;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (double x = -windowHalfWidthMs; x <= windowHalfWidthMs + (stepMs * 0.25); x += stepMs) {
        const double leftSample = interpolateLinear(leftTimeMs, leftValues, x);
        const double rightSample = interpolateLinear(rightTimeMs, rightValues, x);
        dot += leftSample * rightSample;
        leftEnergy += leftSample * leftSample;
        rightEnergy += rightSample * rightSample;
    }

    if (leftEnergy <= 1.0e-9 || rightEnergy <= 1.0e-9) {
        return 0.0;
    }
    return dot / std::sqrt(leftEnergy * rightEnergy);
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
    view.delayMismatchSamples = static_cast<int>(
        std::lround(view.delayMismatchMs * static_cast<double>(view.sampleRate) / 1000.0));
    view.pathMismatchCm = (view.delayMismatchMs / 1000.0) * 34300.0;
    view.suggestedMoveCm = std::abs(view.pathMismatchCm) * 0.5;
    view.confidenceDb = std::min(result.analysis.left.impulsePeakToNoiseDb,
                                 result.analysis.right.impulsePeakToNoiseDb);
    view.centeredToleranceMs =
        static_cast<double>(view.centeredToleranceSamples) * 1000.0 / static_cast<double>(std::max(view.sampleRate, 1));
    if (std::abs(view.delayMismatchSamples) > view.centeredToleranceSamples) {
        view.suggestedDirection = view.delayMismatchMs < 0.0
                                      ? SweetSpotMoveDirection::Right
                                      : SweetSpotMoveDirection::Left;
    }

    std::vector<double> baseTimeMs;
    baseTimeMs.reserve(impulse->xValues.size());
    for (const double value : impulse->xValues) {
        baseTimeMs.push_back(value * 1000.0);
    }

    if (baseTimeMs.empty() || impulse->leftValues.size() != baseTimeMs.size() || impulse->rightValues.size() != baseTimeMs.size()) {
        view.available = false;
        return view;
    }
    const size_t innerRadiusSamples =
        focusInnerRadiusSamples(result.analysis.sampleRate,
                                impulseFocusFrequencyHz(result, kMinimumFocusFrequencyHz));
    const size_t outerRadiusSamples = focusOuterRadiusSamples(innerRadiusSamples);
    const size_t leftPeakIndex = maxAbsIndex(impulse->leftValues);
    const size_t rightPeakIndex = maxAbsIndex(impulse->rightValues);
    const std::vector<double> focusedLeftImpulse =
        focusSamplesAroundPeak(impulse->leftValues, leftPeakIndex, innerRadiusSamples, outerRadiusSamples);
    const std::vector<double> focusedRightImpulse =
        focusSamplesAroundPeak(impulse->rightValues, rightPeakIndex, innerRadiusSamples, outerRadiusSamples);
    std::vector<double> leftImpulse = normalizeImpulse(focusedLeftImpulse);
    std::vector<double> rightImpulse = normalizeImpulse(focusedRightImpulse);

    const double midpointArrivalMs = (view.leftArrivalMs + view.rightArrivalMs) * 0.5;
    const double stepMs = 1000.0 / static_cast<double>(view.sampleRate);
    const bool absoluteTimeAxis =
        axisContainsTime(baseTimeMs, view.leftArrivalMs, stepMs) &&
        axisContainsTime(baseTimeMs, view.rightArrivalMs, stepMs);
    std::vector<double> leftTimeMs(baseTimeMs.size(), 0.0);
    std::vector<double> rightTimeMs(baseTimeMs.size(), 0.0);
    for (size_t index = 0; index < baseTimeMs.size(); ++index) {
        if (absoluteTimeAxis) {
            leftTimeMs[index] = baseTimeMs[index] - midpointArrivalMs;
            rightTimeMs[index] = baseTimeMs[index] - midpointArrivalMs;
        } else {
            leftTimeMs[index] = baseTimeMs[index] + view.leftArrivalMs - midpointArrivalMs;
            rightTimeMs[index] = baseTimeMs[index] + view.rightArrivalMs - midpointArrivalMs;
        }
    }

    std::vector<double> leftLocalTimeMs(baseTimeMs.size(), 0.0);
    std::vector<double> rightLocalTimeMs(baseTimeMs.size(), 0.0);
    for (size_t index = 0; index < baseTimeMs.size(); ++index) {
        if (absoluteTimeAxis) {
            leftLocalTimeMs[index] = baseTimeMs[index] - view.leftArrivalMs;
            rightLocalTimeMs[index] = baseTimeMs[index] - view.rightArrivalMs;
        } else {
            leftLocalTimeMs[index] = baseTimeMs[index];
            rightLocalTimeMs[index] = baseTimeMs[index];
        }
    }
    const double polarityWindowHalfWidthMs = static_cast<double>(innerRadiusSamples * 2) * stepMs;
    const double polarityCorrelation =
        localWaveformCorrelation(leftLocalTimeMs, leftImpulse, rightLocalTimeMs, rightImpulse, polarityWindowHalfWidthMs, stepMs);
    if (polarityCorrelation <= kMinimumPolarityCorrelation &&
        rmsValue(leftImpulse) > 0.02 && rmsValue(rightImpulse) > 0.02) {
        view.polarityMismatchDetected = true;
    }

    const size_t displayEnvelopeRadiusSamples =
        std::clamp<size_t>(std::max<size_t>(1, innerRadiusSamples / 4), size_t{1}, size_t{4});
    const std::vector<double> leftDisplayImpulse = buildDisplayEnvelope(leftImpulse, displayEnvelopeRadiusSamples);
    const std::vector<double> rightDisplayImpulse = buildDisplayEnvelope(rightImpulse, displayEnvelopeRadiusSamples);

    const double availableMinTimeMs = std::min(leftTimeMs.front(), rightTimeMs.front());
    const double availableMaxTimeMs = std::max(leftTimeMs.back(), rightTimeMs.back());
    const double graphHalfWindowMs =
        std::max(kGraphHalfWindowMs, (std::abs(view.delayMismatchMs) * 0.5) + kGraphDelayMarginMs);
    double minTimeMs = std::max(-graphHalfWindowMs, availableMinTimeMs);
    double maxTimeMs = std::min(graphHalfWindowMs, availableMaxTimeMs);
    if ((maxTimeMs - minTimeMs) < std::max(stepMs, kMinimumStableGraphSpanMs)) {
        minTimeMs = availableMinTimeMs;
        maxTimeMs = availableMaxTimeMs;
    }
    const size_t sampleCount = std::max<size_t>(
        2,
        static_cast<size_t>(std::ceil((maxTimeMs - minTimeMs) / std::max(stepMs, 1.0e-6))) + 1);

    view.timeAxisMs.resize(sampleCount, 0.0);
    view.leftImpulse.resize(sampleCount, 0.0);
    view.rightImpulse.resize(sampleCount, 0.0);
    for (size_t index = 0; index < sampleCount; ++index) {
        const double x = minTimeMs + (static_cast<double>(index) * stepMs);
        view.timeAxisMs[index] = x;
        view.leftImpulse[index] = interpolateLinear(leftTimeMs, leftDisplayImpulse, x);
        view.rightImpulse[index] = interpolateLinear(rightTimeMs, rightDisplayImpulse, x);
    }

    return view;
}

}  // namespace wolfie::measurement
