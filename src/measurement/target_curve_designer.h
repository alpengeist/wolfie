#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

struct TargetCurvePlotData {
    std::vector<double> frequencyAxisHz;
    std::vector<double> basicCurveDb;
    std::vector<double> targetCurveDb;
    std::vector<double> selectedBandContributionDb;
    double minFrequencyHz = 20.0;
    double maxFrequencyHz = 20000.0;
};

void normalizeTargetCurveSettings(TargetCurveSettings& settings, double minFrequencyHz, double maxFrequencyHz);
TargetEqBand makeDefaultTargetEqBand(double frequencyHz, int colorIndex);
std::vector<TargetEqBand> defaultTargetEqBands();
double evaluateTargetCurveDbAtFrequency(const MeasurementSettings& measurement,
                                        const TargetCurveSettings& settings,
                                        double minFrequencyHz,
                                        double maxFrequencyHz,
                                        double frequencyHz);
TargetCurvePlotData buildTargetCurvePlotData(const SmoothedResponse& response,
                                             const MeasurementSettings& measurement,
                                             const TargetCurveSettings& settings,
                                             std::optional<size_t> selectedBandIndex);

}  // namespace wolfie::measurement
