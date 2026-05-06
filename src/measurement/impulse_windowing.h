#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

struct TimeDomainPair {
    std::vector<double> timeSeconds;
    std::vector<double> left;
    std::vector<double> right;

    [[nodiscard]] bool valid() const {
        return !timeSeconds.empty() &&
               left.size() == timeSeconds.size() &&
               right.size() == timeSeconds.size();
    }
};

TimeDomainPair loadImpulsePair(const MeasurementResult& result, std::string_view key);
TimeDomainPair loadWindowImpulsePair(const MeasurementResult& result, std::string_view window);
size_t maxAbsIndex(const std::vector<double>& values);
std::vector<double> extractCircularWindow(const std::vector<double>& impulse,
                                          size_t windowStart,
                                          size_t windowLength);
double impulseFocusFrequencyHz(const MeasurementResult& measurement,
                               double minimumFocusFrequencyHz = 1000.0);
double impulseFocusFrequencyHz(const MeasurementResult* result,
                               int sampleRate,
                               double minimumFocusFrequencyHz = 1000.0,
                               double defaultFocusFrequencyHz = 4000.0);
size_t focusInnerRadiusSamples(int sampleRate, double focusFrequencyHz);
size_t focusOuterRadiusSamples(size_t innerRadius);
std::vector<double> focusSamplesAroundPeak(const std::vector<double>& values,
                                           size_t peakIndex,
                                           size_t innerRadius,
                                           size_t outerRadius);
std::vector<double> focusSamplesAroundPeak(const std::vector<double>& values,
                                           size_t innerRadius,
                                           size_t outerRadius);
TimeDomainPair focusImpulsePair(const TimeDomainPair& impulse,
                                int sampleRate,
                                double focusFrequencyHz);

}  // namespace wolfie::measurement
