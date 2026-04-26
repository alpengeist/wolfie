#include "measurement/filter_wav_export.h"

#include <cstdint>
#include <fstream>
#include <string>

#include "measurement/filter_designer.h"

namespace wolfie::measurement {

namespace {

bool writeStereoFloatWaveFile(const std::filesystem::path& path,
                              const std::vector<double>& leftSamples,
                              const std::vector<double>& rightSamples,
                              int sampleRate) {
    if (leftSamples.empty() || leftSamples.size() != rightSamples.size()) {
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const uint16_t channels = 2;
    const uint16_t bitsPerSample = 32;
    const uint16_t formatTag = 3;  // IEEE float
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate) * blockAlign;
    const uint32_t dataSize = static_cast<uint32_t>(leftSamples.size() * blockAlign);
    const uint32_t riffSize = 36 + dataSize;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    const uint32_t fmtSize = 16;
    out.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    out.write(reinterpret_cast<const char*>(&formatTag), sizeof(formatTag));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));

    for (size_t index = 0; index < leftSamples.size(); ++index) {
        const float left = static_cast<float>(leftSamples[index]);
        const float right = static_cast<float>(rightSamples[index]);
        out.write(reinterpret_cast<const char*>(&left), sizeof(left));
        out.write(reinterpret_cast<const char*>(&right), sizeof(right));
    }

    return static_cast<bool>(out);
}

bool writeRoonConfigFile(const std::filesystem::path& path,
                         int sampleRate,
                         const std::filesystem::path& wavPath) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    out << sampleRate << " 2 2 0\n"
        << "0 0\n"
        << "0 0\n"
        << wavPath.filename().string() << "\n"
        << "0\n"
        << "0.0\n"
        << "0.0\n"
        << wavPath.filename().string() << "\n"
        << "1\n"
        << "1.0\n"
        << "1.0\n";
    return static_cast<bool>(out);
}

std::filesystem::path roonFilePath(const std::filesystem::path& directory, int sampleRate) {
    return directory / ("wolfie_stereo_" + std::to_string(sampleRate) + ".wav");
}

std::string roonConfigSampleRateToken(int sampleRate) {
    int token = sampleRate / 100;
    while (token > 0 && token % 10 == 0) {
        token /= 10;
    }
    return std::to_string(token);
}

std::filesystem::path roonConfigFilePath(const std::filesystem::path& directory, int sampleRate) {
    return directory / ("roon 2.0_" + roonConfigSampleRateToken(sampleRate) + ".cfg");
}

}  // namespace

const std::vector<int>& roonCommonSampleRates() {
    static const std::vector<int> sampleRates = {
        44100,
        48000,
        88200,
        96000,
        176400,
        192000,
        352800,
        384000,
        705600,
        768000,
    };
    return sampleRates;
}

std::filesystem::path roonFilterWavPath(const std::filesystem::path& directory, int sampleRate) {
    return roonFilePath(directory, sampleRate);
}

std::filesystem::path roonFilterConfigPath(const std::filesystem::path& directory, int sampleRate) {
    return roonConfigFilePath(directory, sampleRate);
}

bool exportRoonFilterWavSet(const std::filesystem::path& directory,
                            const SmoothedResponse& response,
                            const MeasurementSettings& measurement,
                            const TargetCurveSettings& targetCurve,
                            const FilterDesignSettings& filterSettings,
                            std::vector<std::filesystem::path>& generatedFiles,
                            std::wstring& errorMessage,
                            const RoonFilterExportProgressCallback& progressCallback) {
    generatedFiles.clear();
    errorMessage.clear();

    if (response.frequencyAxisHz.empty() ||
        response.leftChannelDb.size() != response.frequencyAxisHz.size() ||
        response.rightChannelDb.size() != response.frequencyAxisHz.size()) {
        errorMessage = L"No smoothed response is available for filter export.";
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        errorMessage = L"Could not create export directory: " + directory.wstring();
        return false;
    }

    const std::vector<int>& sampleRates = roonCommonSampleRates();
    const std::size_t totalSampleRates = sampleRates.size();
    for (std::size_t index = 0; index < totalSampleRates; ++index) {
        const int sampleRate = sampleRates[index];
        if (progressCallback) {
            progressCallback(sampleRate, index + 1, totalSampleRates);
        }

        const FilterDesignResult filterResult =
            designFiltersForSampleRate(response, measurement, targetCurve, filterSettings, sampleRate);
        if (!filterResult.valid ||
            filterResult.left.filterTaps.empty() ||
            filterResult.right.filterTaps.empty() ||
            filterResult.left.filterTaps.size() != filterResult.right.filterTaps.size()) {
            errorMessage = L"Filter design failed for " + std::to_wstring(sampleRate) + L" Hz.";
            return false;
        }

        const std::filesystem::path wavPath = roonFilePath(directory, sampleRate);
        if (!writeStereoFloatWaveFile(wavPath,
                                      filterResult.left.filterTaps,
                                      filterResult.right.filterTaps,
                                      sampleRate)) {
            errorMessage = L"Could not write filter WAV: " + wavPath.wstring();
            return false;
        }
        const std::filesystem::path cfgPath = roonConfigFilePath(directory, sampleRate);
        if (!writeRoonConfigFile(cfgPath, sampleRate, wavPath)) {
            errorMessage = L"Could not write Roon config: " + cfgPath.wstring();
            return false;
        }

        generatedFiles.push_back(wavPath);
        generatedFiles.push_back(cfgPath);
    }

    return true;
}

}  // namespace wolfie::measurement
