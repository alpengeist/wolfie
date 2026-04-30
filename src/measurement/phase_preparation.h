#pragma once

#include <string>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

struct PreparedPhaseChannel {
    std::vector<double> nativeFrequencyAxisHz;
    std::vector<double> measuredMagnitudeDb;
    std::vector<double> smoothedMagnitudeDb;
    std::vector<double> measuredPhaseRadians;
    std::vector<double> delayCorrectedPhaseRadians;
    std::vector<double> minimumPhaseRadians;
    std::vector<double> excessPhaseRadians;
    double bulkDelaySeconds = 0.0;
    std::string sourceKey;

    [[nodiscard]] bool valid() const {
        return !nativeFrequencyAxisHz.empty() &&
               measuredMagnitudeDb.size() == nativeFrequencyAxisHz.size() &&
               smoothedMagnitudeDb.size() == nativeFrequencyAxisHz.size() &&
               measuredPhaseRadians.size() == nativeFrequencyAxisHz.size() &&
               delayCorrectedPhaseRadians.size() == nativeFrequencyAxisHz.size() &&
               minimumPhaseRadians.size() == nativeFrequencyAxisHz.size() &&
               excessPhaseRadians.size() == nativeFrequencyAxisHz.size();
    }
};

struct PreparedPhaseData {
    bool valid = false;
    PreparedPhaseChannel left;
    PreparedPhaseChannel right;
    std::string sourceWindow;
    std::string sourceKey;
};

struct PreparedPhaseView {
    std::vector<double> frequencyAxisHz;
    std::vector<double> delayCorrectedPhaseRadians;
    std::vector<double> minimumPhaseRadians;
    std::vector<double> excessPhaseRadians;
    std::vector<double> wrappedExcessPhaseDegrees;
    std::vector<double> continuousExcessPhaseDegrees;
    std::vector<double> groupDelayMs;

    [[nodiscard]] bool valid() const {
        return !frequencyAxisHz.empty() &&
               delayCorrectedPhaseRadians.size() == frequencyAxisHz.size() &&
               minimumPhaseRadians.size() == frequencyAxisHz.size() &&
               excessPhaseRadians.size() == frequencyAxisHz.size() &&
               wrappedExcessPhaseDegrees.size() == frequencyAxisHz.size() &&
               continuousExcessPhaseDegrees.size() == frequencyAxisHz.size() &&
               groupDelayMs.size() == frequencyAxisHz.size();
    }
};

PreparedPhaseData preparePhaseData(const MeasurementResult* result,
                                   const ResponseSmoothingSettings& smoothingSettings,
                                   int sampleRate,
                                   int fftSize);
PreparedPhaseChannel prepareMatchedPhaseChannel(const std::vector<double>& nativeFrequencyAxisHz,
                                                const std::vector<double>& measuredMagnitudeDb,
                                                const std::vector<double>& unwrappedPhaseRadians,
                                                double bulkDelaySeconds,
                                                const ResponseSmoothingSettings& smoothingSettings,
                                                int sampleRate,
                                                int fftSize,
                                                std::string sourceKey);
PreparedPhaseView resamplePreparedPhaseChannel(const PreparedPhaseChannel& channel,
                                               const std::vector<double>& displayFrequencyAxisHz);

}  // namespace wolfie::measurement
