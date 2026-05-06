#include "measurement/impulse_windowing.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace wolfie::measurement {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

const MeasurementValueSet* findValidValueSet(const MeasurementResult& result, std::string_view key) {
    const MeasurementValueSet* valueSet = result.findValueSet(key);
    return valueSet != nullptr && valueSet->valid() ? valueSet : nullptr;
}

}  // namespace

TimeDomainPair loadImpulsePair(const MeasurementResult& result, std::string_view key) {
    TimeDomainPair pair;
    const MeasurementValueSet* impulse = findValidValueSet(result, key);
    if (impulse == nullptr) {
        return pair;
    }

    pair.timeSeconds = impulse->xValues;
    pair.left = impulse->leftValues;
    pair.right = impulse->rightValues;
    return pair;
}

TimeDomainPair loadWindowImpulsePair(const MeasurementResult& result, std::string_view window) {
    return loadImpulsePair(result, "measurement." + std::string(window) + "_impulse_response");
}

size_t maxAbsIndex(const std::vector<double>& values) {
    size_t bestIndex = 0;
    double bestMagnitude = 0.0;
    for (size_t index = 0; index < values.size(); ++index) {
        const double magnitude = std::abs(values[index]);
        if (magnitude > bestMagnitude) {
            bestMagnitude = magnitude;
            bestIndex = index;
        }
    }
    return bestIndex;
}

std::vector<double> extractCircularWindow(const std::vector<double>& impulse,
                                          size_t windowStart,
                                          size_t windowLength) {
    std::vector<double> window;
    if (impulse.empty() || windowLength == 0) {
        return window;
    }

    const size_t extractedLength = std::min(windowLength, impulse.size());
    window.reserve(extractedLength);
    for (size_t index = 0; index < extractedLength; ++index) {
        window.push_back(impulse[(windowStart + index) % impulse.size()]);
    }
    return window;
}

double impulseFocusFrequencyHz(const MeasurementResult& measurement,
                               double minimumFocusFrequencyHz) {
    const double startHz = std::max(minimumFocusFrequencyHz, measurement.analysis.startFrequencyHz);
    const double endHz = std::max(startHz + 1.0, measurement.analysis.endFrequencyHz);
    return std::sqrt(startHz * endHz);
}

double impulseFocusFrequencyHz(const MeasurementResult* result,
                               int sampleRate,
                               double minimumFocusFrequencyHz,
                               double defaultFocusFrequencyHz) {
    if (result != nullptr) {
        return impulseFocusFrequencyHz(*result, minimumFocusFrequencyHz);
    }

    const double nyquistHz = static_cast<double>(std::max(sampleRate, 1)) * 0.5;
    return clampValue(defaultFocusFrequencyHz,
                      minimumFocusFrequencyHz,
                      std::max(minimumFocusFrequencyHz, nyquistHz - 1.0));
}

size_t focusInnerRadiusSamples(int sampleRate, double focusFrequencyHz) {
    const double framesPerCycle = static_cast<double>(std::max(sampleRate, 1)) / std::max(focusFrequencyHz, 1.0);
    return std::clamp<size_t>(static_cast<size_t>(std::lround(framesPerCycle)), size_t{3}, size_t{48});
}

size_t focusOuterRadiusSamples(size_t innerRadius) {
    return std::clamp<size_t>(innerRadius * 2, size_t{6}, size_t{96});
}

std::vector<double> focusSamplesAroundPeak(const std::vector<double>& values,
                                           size_t peakIndex,
                                           size_t innerRadius,
                                           size_t outerRadius) {
    std::vector<double> focused(values.size(), 0.0);
    if (values.empty()) {
        return focused;
    }

    const size_t safePeakIndex = std::min(peakIndex, values.size() - 1);
    const size_t safeInnerRadius = std::min(innerRadius, outerRadius);
    const size_t safeOuterRadius = std::max(outerRadius, safeInnerRadius);
    for (size_t index = 0; index < values.size(); ++index) {
        const size_t distance = index > safePeakIndex ? index - safePeakIndex : safePeakIndex - index;
        if (distance > safeOuterRadius) {
            continue;
        }

        double weight = 1.0;
        if (distance > safeInnerRadius && safeOuterRadius > safeInnerRadius) {
            const double t = static_cast<double>(distance - safeInnerRadius) /
                             static_cast<double>(safeOuterRadius - safeInnerRadius);
            weight = 0.5 * (1.0 + std::cos(t * std::numbers::pi_v<double>));
        }
        focused[index] = values[index] * weight;
    }
    return focused;
}

std::vector<double> focusSamplesAroundPeak(const std::vector<double>& values,
                                           size_t innerRadius,
                                           size_t outerRadius) {
    return focusSamplesAroundPeak(values, maxAbsIndex(values), innerRadius, outerRadius);
}

TimeDomainPair focusImpulsePair(const TimeDomainPair& impulse,
                                int sampleRate,
                                double focusFrequencyHz) {
    if (!impulse.valid()) {
        return {};
    }

    const size_t innerRadius = focusInnerRadiusSamples(sampleRate, focusFrequencyHz);
    const size_t outerRadius = focusOuterRadiusSamples(innerRadius);
    TimeDomainPair focused;
    focused.timeSeconds = impulse.timeSeconds;
    focused.left = focusSamplesAroundPeak(impulse.left, innerRadius, outerRadius);
    focused.right = focusSamplesAroundPeak(impulse.right, innerRadius, outerRadius);
    return focused;
}

}  // namespace wolfie::measurement
