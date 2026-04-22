#pragma once

#include "core/models.h"

namespace wolfie::measurement {

void normalizeResponseSmoothingSettings(ResponseSmoothingSettings& settings);
double smoothingResolutionFactor(const ResponseSmoothingSettings& settings);
int effectiveLowWindowCycles(const ResponseSmoothingSettings& settings);
int effectiveHighWindowCycles(const ResponseSmoothingSettings& settings);
int effectiveSlidingOctaveDenominator(const ResponseSmoothingSettings& settings);
SmoothedResponse buildSmoothedResponse(const MeasurementResult& result,
                                       const ResponseSmoothingSettings& settings);

}  // namespace wolfie::measurement
