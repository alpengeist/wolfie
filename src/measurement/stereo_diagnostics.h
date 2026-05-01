#pragma once

#include <complex>
#include <functional>
#include <string_view>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

struct StereoDiagnosticsInput {
    int sampleRate = 0;
    double delayMismatchMs = 0.0;
    std::vector<double> frequencyAxisHz;
    std::vector<std::complex<double>> leftSpectrum;
    std::vector<std::complex<double>> rightSpectrum;
    std::vector<double> directImpulseTimeSeconds;
    std::vector<double> directImpulseLeft;
    std::vector<double> directImpulseRight;
    std::vector<double> windowImpulseTimeSeconds;
    std::vector<double> windowImpulseLeft;
    std::vector<double> windowImpulseRight;
};

using StereoDiagnosticsProgressCallback = std::function<void(std::string_view metricLabel)>;

StereoDiagnosticsResult buildStereoDiagnostics(const MeasurementResult& result,
                                               std::string_view window,
                                               const StereoDiagnosticsProgressCallback& progressCallback = {});
StereoDiagnosticsResult buildStereoDiagnostics(const StereoDiagnosticsInput& input,
                                               std::string_view window,
                                               const StereoDiagnosticsProgressCallback& progressCallback = {});

}  // namespace wolfie::measurement
