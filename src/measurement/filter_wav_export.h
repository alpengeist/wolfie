#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/models.h"

namespace wolfie::measurement {

using RoonFilterExportProgressCallback = std::function<void(int sampleRate,
                                                            std::size_t sampleRateIndex,
                                                            std::size_t totalSampleRates)>;

[[nodiscard]] const std::vector<int>& roonCommonSampleRates();
[[nodiscard]] std::filesystem::path roonFilterWavPath(const std::filesystem::path& directory, int sampleRate);
[[nodiscard]] std::filesystem::path roonFilterConfigPath(const std::filesystem::path& directory, int sampleRate);
[[nodiscard]] std::filesystem::path roonFilterParametersPath(const std::filesystem::path& directory);
[[nodiscard]] std::filesystem::path roonFilterArchivePath(const std::filesystem::path& directory,
                                                          std::string_view archiveBaseName = "roon");

bool exportRoonFilterWavSet(const std::filesystem::path& directory,
                            const SmoothedResponse& response,
                            const MeasurementSettings& measurement,
                            const TargetCurveSettings& targetCurve,
                            const FilterDesignSettings& filterSettings,
                            const MeasurementResult* sourceMeasurement,
                            const std::vector<int>& sampleRates,
                            std::vector<std::filesystem::path>& generatedFiles,
                            std::wstring& errorMessage,
                            const RoonFilterExportProgressCallback& progressCallback = {},
                            std::string_view archiveBaseName = "roon",
                            std::string_view parametersText = "");

}  // namespace wolfie::measurement
