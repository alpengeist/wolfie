#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

[[nodiscard]] const std::vector<int>& roonCommonSampleRates();
[[nodiscard]] std::filesystem::path roonFilterWavPath(const std::filesystem::path& directory, int sampleRate);
[[nodiscard]] std::filesystem::path roonFilterConfigPath(const std::filesystem::path& directory, int sampleRate);

bool exportRoonFilterWavSet(const std::filesystem::path& directory,
                            const SmoothedResponse& response,
                            const MeasurementSettings& measurement,
                            const TargetCurveSettings& targetCurve,
                            const FilterDesignSettings& filterSettings,
                            std::vector<std::filesystem::path>& generatedFiles,
                            std::wstring& errorMessage);

}  // namespace wolfie::measurement
