#pragma once

#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

enum class SweetSpotMoveDirection {
    None,
    Left,
    Right
};

struct SweetSpotAlignmentView {
    bool available = false;
    bool captureTooQuiet = false;
    bool captureClippingDetected = false;
    bool polarityMismatchDetected = false;
    int sampleRate = 0;
    int delayMismatchSamples = 0;
    double leftArrivalMs = 0.0;
    double rightArrivalMs = 0.0;
    double delayMismatchMs = 0.0;
    double pathMismatchCm = 0.0;
    double suggestedMoveCm = 0.0;
    double confidenceDb = 0.0;
    int centeredToleranceSamples = 0;
    double centeredToleranceMs = 0.0;
    SweetSpotMoveDirection suggestedDirection = SweetSpotMoveDirection::None;
    std::vector<double> timeAxisMs;
    std::vector<double> leftImpulse;
    std::vector<double> rightImpulse;
};

SweetSpotAlignmentView buildSweetSpotAlignmentView(const MeasurementResult& result);

}  // namespace wolfie::measurement
