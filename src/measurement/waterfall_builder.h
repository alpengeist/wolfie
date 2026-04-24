#pragma once

#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

struct WaterfallSlice {
    double timeMilliseconds = 0.0;
    std::vector<double> valuesDb;
};

struct WaterfallPlotData {
    std::vector<double> frequencyAxisHz;
    std::vector<WaterfallSlice> slices;
    double minDb = -72.0;
    double maxDb = 0.0;

    [[nodiscard]] bool valid() const {
        if (frequencyAxisHz.empty() || slices.empty()) {
            return false;
        }

        for (const WaterfallSlice& slice : slices) {
            if (slice.valuesDb.size() != frequencyAxisHz.size()) {
                return false;
            }
        }
        return true;
    }
};

WaterfallPlotData buildWaterfallPlotData(const MeasurementResult& result, MeasurementChannel channel);

}  // namespace wolfie::measurement
