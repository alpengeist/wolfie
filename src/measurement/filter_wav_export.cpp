#include "measurement/filter_wav_export.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "measurement/filter_designer.h"

namespace wolfie::measurement {

namespace {

bool writeStereoDoubleWaveFile(const std::filesystem::path& path,
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
    const uint16_t bitsPerSample = 64;
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
        const double left = leftSamples[index];
        const double right = rightSamples[index];
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

std::filesystem::path roonArchiveFilePath(const std::filesystem::path& directory) {
    return directory / "roon.zip";
}

void writeUint16(std::ofstream& out, uint16_t value) {
    const std::array<char, 2> bytes = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
    };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeUint32(std::ofstream& out, uint32_t value) {
    const std::array<char, 4> bytes = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 24) & 0xFF),
    };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

uint32_t crc32ForBytes(const std::vector<uint8_t>& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t value : bytes) {
        crc ^= static_cast<uint32_t>(value);
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (crc & 1u) != 0u ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

bool readFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    bytes.clear();

    std::error_code sizeError;
    const uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize > static_cast<uintmax_t>(0xFFFFFFFFu)) {
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    bytes.resize(static_cast<size_t>(fileSize));
    if (!bytes.empty()) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(in) || bytes.empty();
}

struct ZipEntry {
    std::filesystem::path sourcePath;
    std::string archiveName;
    std::vector<uint8_t> payload;
    uint32_t crc32 = 0;
    uint32_t localHeaderOffset = 0;
};

bool writeStoredZipArchive(const std::filesystem::path& zipPath,
                           const std::vector<std::filesystem::path>& sourceFiles) {
    std::vector<ZipEntry> entries;
    entries.reserve(sourceFiles.size());
    for (const std::filesystem::path& sourcePath : sourceFiles) {
        ZipEntry entry;
        entry.sourcePath = sourcePath;
        entry.archiveName = sourcePath.filename().string();
        if (entry.archiveName.empty() || !readFileBytes(sourcePath, entry.payload)) {
            return false;
        }
        entry.crc32 = crc32ForBytes(entry.payload);
        entries.push_back(std::move(entry));
    }

    std::ofstream out(zipPath, std::ios::binary);
    if (!out) {
        return false;
    }

    for (ZipEntry& entry : entries) {
        entry.localHeaderOffset = static_cast<uint32_t>(out.tellp());
        writeUint32(out, 0x04034B50u);
        writeUint16(out, 20);
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint32(out, entry.crc32);
        writeUint32(out, static_cast<uint32_t>(entry.payload.size()));
        writeUint32(out, static_cast<uint32_t>(entry.payload.size()));
        writeUint16(out, static_cast<uint16_t>(entry.archiveName.size()));
        writeUint16(out, 0);
        out.write(entry.archiveName.data(), static_cast<std::streamsize>(entry.archiveName.size()));
        if (!entry.payload.empty()) {
            out.write(reinterpret_cast<const char*>(entry.payload.data()), static_cast<std::streamsize>(entry.payload.size()));
        }
        if (!out) {
            return false;
        }
    }

    const uint32_t centralDirectoryOffset = static_cast<uint32_t>(out.tellp());
    for (const ZipEntry& entry : entries) {
        writeUint32(out, 0x02014B50u);
        writeUint16(out, 20);
        writeUint16(out, 20);
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint32(out, entry.crc32);
        writeUint32(out, static_cast<uint32_t>(entry.payload.size()));
        writeUint32(out, static_cast<uint32_t>(entry.payload.size()));
        writeUint16(out, static_cast<uint16_t>(entry.archiveName.size()));
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint16(out, 0);
        writeUint32(out, 0);
        writeUint32(out, entry.localHeaderOffset);
        out.write(entry.archiveName.data(), static_cast<std::streamsize>(entry.archiveName.size()));
        if (!out) {
            return false;
        }
    }

    const uint32_t centralDirectorySize = static_cast<uint32_t>(static_cast<uint32_t>(out.tellp()) - centralDirectoryOffset);
    writeUint32(out, 0x06054B50u);
    writeUint16(out, 0);
    writeUint16(out, 0);
    writeUint16(out, static_cast<uint16_t>(entries.size()));
    writeUint16(out, static_cast<uint16_t>(entries.size()));
    writeUint32(out, centralDirectorySize);
    writeUint32(out, centralDirectoryOffset);
    writeUint16(out, 0);

    return static_cast<bool>(out);
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

std::filesystem::path roonFilterArchivePath(const std::filesystem::path& directory) {
    return roonArchiveFilePath(directory);
}

bool exportRoonFilterWavSet(const std::filesystem::path& directory,
                            const SmoothedResponse& response,
                            const MeasurementSettings& measurement,
                            const TargetCurveSettings& targetCurve,
                            const FilterDesignSettings& filterSettings,
                            const std::vector<int>& sampleRates,
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
    if (sampleRates.empty()) {
        errorMessage = L"No export sample rates were selected.";
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        errorMessage = L"Could not create export directory: " + directory.wstring();
        return false;
    }

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
        if (!writeStereoDoubleWaveFile(wavPath,
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

    const std::filesystem::path zipPath = roonArchiveFilePath(directory);
    if (!writeStoredZipArchive(zipPath, generatedFiles)) {
        errorMessage = L"Could not write Roon archive: " + zipPath.wstring();
        return false;
    }
    generatedFiles.push_back(zipPath);

    return true;
}

}  // namespace wolfie::measurement
