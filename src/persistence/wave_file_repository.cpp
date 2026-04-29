#include "persistence/wave_file_repository.h"

#include <array>
#include <fstream>

namespace wolfie::persistence {

namespace {

template <typename T>
bool readValue(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

bool readTag(std::ifstream& in, std::array<char, 4>& tag) {
    in.read(tag.data(), static_cast<std::streamsize>(tag.size()));
    return static_cast<bool>(in);
}

}  // namespace

bool loadMonoPcm16WaveFile(const std::filesystem::path& path,
                           int& sampleRate,
                           std::vector<int16_t>& samples,
                           std::wstring& errorMessage) {
    sampleRate = 0;
    samples.clear();
    errorMessage.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        errorMessage = L"Could not open WAV file: " + path.wstring();
        return false;
    }

    std::array<char, 4> riffTag{};
    uint32_t riffSize = 0;
    std::array<char, 4> waveTag{};
    if (!readTag(in, riffTag) || !readValue(in, riffSize) || !readTag(in, waveTag) ||
        riffTag != std::array<char, 4>{'R', 'I', 'F', 'F'} ||
        waveTag != std::array<char, 4>{'W', 'A', 'V', 'E'}) {
        errorMessage = L"Invalid WAV header: " + path.wstring();
        return false;
    }

    bool formatSeen = false;
    bool dataSeen = false;
    uint16_t formatTag = 0;
    uint16_t channelCount = 0;
    uint16_t bitsPerSample = 0;
    uint32_t fileSampleRate = 0;
    std::vector<int16_t> fileSamples;

    while (in && !(formatSeen && dataSeen)) {
        std::array<char, 4> chunkTag{};
        uint32_t chunkSize = 0;
        if (!readTag(in, chunkTag) || !readValue(in, chunkSize)) {
            break;
        }

        if (chunkTag == std::array<char, 4>{'f', 'm', 't', ' '}) {
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            if (chunkSize < 16 ||
                !readValue(in, formatTag) ||
                !readValue(in, channelCount) ||
                !readValue(in, fileSampleRate) ||
                !readValue(in, byteRate) ||
                !readValue(in, blockAlign) ||
                !readValue(in, bitsPerSample)) {
                errorMessage = L"Invalid WAV format chunk: " + path.wstring();
                return false;
            }

            const std::streamoff remainingBytes = static_cast<std::streamoff>(chunkSize) - 16;
            if (remainingBytes > 0) {
                in.seekg(remainingBytes, std::ios::cur);
            }
            formatSeen = static_cast<bool>(in);
        } else if (chunkTag == std::array<char, 4>{'d', 'a', 't', 'a'}) {
            if (chunkSize % sizeof(int16_t) != 0) {
                errorMessage = L"Unexpected WAV data size: " + path.wstring();
                return false;
            }

            fileSamples.resize(chunkSize / sizeof(int16_t));
            in.read(reinterpret_cast<char*>(fileSamples.data()), static_cast<std::streamsize>(chunkSize));
            dataSeen = static_cast<bool>(in);
        } else {
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }

        if ((chunkSize & 1u) != 0u) {
            in.seekg(1, std::ios::cur);
        }
    }

    if (!formatSeen || !dataSeen) {
        errorMessage = L"Required WAV chunks were missing: " + path.wstring();
        return false;
    }
    if (formatTag != 1 || channelCount != 1 || bitsPerSample != 16 || fileSampleRate == 0) {
        errorMessage = L"Unsupported WAV format for raw capture: " + path.wstring();
        return false;
    }

    sampleRate = static_cast<int>(fileSampleRate);
    samples = std::move(fileSamples);
    return true;
}

}  // namespace wolfie::persistence
