#pragma once

#include <functional>
#include <string_view>

#include "core/models.h"

namespace wolfie::measurement {

using FilterAnalysisProgressCallback = std::function<void(std::string_view window, std::string_view statusLabel)>;

FilterAnalysisResult buildFilterAnalysis(const MeasurementResult& measurement,
                                         const FilterDesignResult& filterResult,
                                         const FilterAnalysisProgressCallback& progressCallback = {});

}  // namespace wolfie::measurement
