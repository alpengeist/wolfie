#pragma once

#include "core/models.h"

namespace wolfie::measurement {

void normalizeFilterDesignSettings(FilterDesignSettings& settings, int sampleRate);
FilterDesignResult designFilters(const SmoothedResponse& response,
                                 const MeasurementSettings& measurement,
                                 const TargetCurveSettings& targetCurve,
                                 const FilterDesignSettings& settings);

}  // namespace wolfie::measurement
