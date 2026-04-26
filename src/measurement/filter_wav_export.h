#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

using RoonFilterExportProgressCallback = std::function<void(int sampleRate,
                                                            std::size_t sampleRateIndex,
                                                            std::size_t totalSampleRates)>;

[[nodiscard]] const std::vector<int>& roonCommonSampleRates();
[[nodiscard]] std::filesystem::path roonFilterWavPath(const std::filesystem::path& directory, int sampleRate);
[[nodiscard]] std::filesystem::path roonFilterConfigPath(const std::filesystem::path& directory, int sampleRate);

bool exportRoonFilterWavSet(const std::filesystem::path& directory,
                            const SmoothedResponse& response,
                            const MeasurementSettings& measurement,
                            const TargetCurveSettings& targetCurve,
                            const FilterDesignSettings& filterSettings,
                            std::vector<std::filesystem::path>& generatedFiles,
                            std::wstring& errorMessage,
                            const RoonFilterExportProgressCallback& progressCallback = {});

}  // namespace wolfie::measurement
