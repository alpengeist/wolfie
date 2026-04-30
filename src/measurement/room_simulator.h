#pragma once

#include "core/models.h"

namespace wolfie::measurement {

RoomSimulationSettings defaultRoomSimulationSettings();
void normalizeRoomSimulationSettings(RoomSimulationSettings& settings);
MeasurementResult buildSimulatedRoomMeasurement(const MeasurementSettings& measurement,
                                                const RoomSimulationSettings& simulation,
                                                std::string_view simulationName);

}  // namespace wolfie::measurement
