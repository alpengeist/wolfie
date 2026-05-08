#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wolfie::persistence {

bool loadMonoPcm16WaveFile(const std::filesystem::path& path,
                           int& sampleRate,
                           std::vector<int16_t>& samples,
                           std::wstring& errorMessage);
bool loadMonoWaveFileNormalized(const std::filesystem::path& path,
                                int& sampleRate,
                                std::vector<double>& samples,
                                std::wstring& errorMessage);

}  // namespace wolfie::persistence
