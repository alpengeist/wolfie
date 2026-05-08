#pragma once

#include "core/models.h"

namespace wolfie::measurement {

void normalizeFilterDesignSettings(FilterDesignSettings& settings, int sampleRate);
std::vector<int> suggestPreRingingCompensationFrequencies(const FilterDesignResult& result,
                                                          const FilterDesignSettings& settings);
FilterDesignResult designFilters(const SmoothedResponse& response,
                                 const MeasurementSettings& measurement,
                                 const TargetCurveSettings& targetCurve,
                                 const FilterDesignSettings& settings,
                                 const MeasurementResult* sourceMeasurement = nullptr);
FilterDesignResult designFiltersForSampleRate(const SmoothedResponse& response,
                                              const MeasurementSettings& measurement,
                                              const TargetCurveSettings& targetCurve,
                                              const FilterDesignSettings& settings,
                                              int sampleRate,
                                              const MeasurementResult* sourceMeasurement = nullptr);

}  // namespace wolfie::measurement
