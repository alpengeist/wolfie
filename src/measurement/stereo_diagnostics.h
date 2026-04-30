#pragma once

#include <string_view>

#include "core/models.h"

namespace wolfie::measurement {

StereoDiagnosticsResult buildStereoDiagnostics(const MeasurementResult& result,
                                               std::string_view window);

}  // namespace wolfie::measurement
