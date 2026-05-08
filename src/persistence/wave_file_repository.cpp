#include "persistence/wave_file_repository.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

namespace wolfie::persistence {

namespace {

constexpr uint16_t kWaveFormatPcm = 1;
constexpr uint16_t kWaveFormatIeeeFloat = 3;
constexpr uint16_t kWaveFormatExtensible = 0xFFFE;

template <typename T>
bool readValue(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

bool readTag(std::ifstream& in, std::array<char, 4>& tag) {
    in.read(tag.data(), static_cast<std::streamsize>(tag.size()));
    return static_cast<bool>(in);
}

int32_t readSigned24(const uint8_t* bytes) {
    const int32_t value = static_cast<int32_t>(bytes[0]) |
                          (static_cast<int32_t>(bytes[1]) << 8) |
                          (static_cast<int32_t>(bytes[2]) << 16);
    return (value & 0x00800000) != 0 ? (value | ~0x00FFFFFF) : value;
}

struct MonoWaveFormat {
    uint16_t formatTag = 0;
    uint16_t channelCount = 0;
    uint16_t bitsPerSample = 0;
    uint16_t validBitsPerSample = 0;
    uint32_t sampleRate = 0;
    bool isFloat = false;

    [[nodiscard]] bool supportsNormalizedDecode() const {
        if (channelCount != 1 || sampleRate == 0) {
            return false;
        }
        if (isFloat) {
            return bitsPerSample == 32 || bitsPerSample == 64;
        }
        return bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32;
    }

    [[nodiscard]] size_t bytesPerSample() const {
        return static_cast<size_t>(bitsPerSample / 8);
    }
};

bool finalizeFormatTag(MonoWaveFormat& format,
                       uint16_t rawFormatTag,
                       uint16_t extensibleValidBits,
                       uint32_t extensibleSubFormatCode) {
    format.formatTag = rawFormatTag;
    format.validBitsPerSample = format.bitsPerSample;
    format.isFloat = false;

    if (rawFormatTag == kWaveFormatPcm) {
        return true;
    }
    if (rawFormatTag == kWaveFormatIeeeFloat) {
        format.isFloat = true;
        return true;
    }
    if (rawFormatTag != kWaveFormatExtensible) {
        return false;
    }

    format.validBitsPerSample = extensibleValidBits != 0 ? extensibleValidBits : format.bitsPerSample;
    if (extensibleSubFormatCode == kWaveFormatPcm) {
        format.formatTag = kWaveFormatPcm;
        return true;
    }
    if (extensibleSubFormatCode == kWaveFormatIeeeFloat) {
        format.formatTag = kWaveFormatIeeeFloat;
        format.isFloat = true;
        return true;
    }
    return false;
}

bool parseFormatChunk(std::ifstream& in,
                      uint32_t chunkSize,
                      MonoWaveFormat& format,
                      std::wstring_view pathText,
                      std::wstring& errorMessage) {
    uint16_t rawFormatTag = 0;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    if (chunkSize < 16 ||
        !readValue(in, rawFormatTag) ||
        !readValue(in, format.channelCount) ||
        !readValue(in, format.sampleRate) ||
        !readValue(in, byteRate) ||
        !readValue(in, blockAlign) ||
        !readValue(in, format.bitsPerSample)) {
        errorMessage = L"Invalid WAV format chunk: " + std::wstring(pathText);
        return false;
    }

    uint16_t extensibleValidBits = 0;
    uint32_t extensibleSubFormatCode = 0;
    uint32_t consumedBytes = 16;
    if (chunkSize > consumedBytes) {
        uint16_t extraSize = 0;
        if (!readValue(in, extraSize)) {
            errorMessage = L"Invalid WAV format extension: " + std::wstring(pathText);
            return false;
        }
        consumedBytes += 2;

        const uint32_t remainingExtraBytes = std::min<uint32_t>(extraSize, chunkSize - consumedBytes);
        if (rawFormatTag == kWaveFormatExtensible && remainingExtraBytes >= 22) {
            uint16_t samplesUnion = 0;
            uint32_t channelMask = 0;
            if (!readValue(in, samplesUnion) ||
                !readValue(in, channelMask) ||
                !readValue(in, extensibleSubFormatCode)) {
                errorMessage = L"Invalid extensible WAV format: " + std::wstring(pathText);
                return false;
            }
            extensibleValidBits = samplesUnion;
            std::array<char, 12> ignoredGuidTail{};
            in.read(ignoredGuidTail.data(), static_cast<std::streamsize>(ignoredGuidTail.size()));
            if (!in) {
                errorMessage = L"Invalid extensible WAV format: " + std::wstring(pathText);
                return false;
            }
            consumedBytes += 22;
        }

        if (chunkSize > consumedBytes) {
            in.seekg(static_cast<std::streamoff>(chunkSize - consumedBytes), std::ios::cur);
        }
    }

    if (!finalizeFormatTag(format, rawFormatTag, extensibleValidBits, extensibleSubFormatCode)) {
        errorMessage = L"Unsupported WAV format: " + std::wstring(pathText);
        return false;
    }
    return static_cast<bool>(in);
}

bool readWaveHeader(std::ifstream& in,
                    std::wstring_view pathText,
                    MonoWaveFormat& format,
                    std::vector<uint8_t>& data,
                    std::wstring& errorMessage) {
    std::array<char, 4> riffTag{};
    uint32_t riffSize = 0;
    std::array<char, 4> waveTag{};
    if (!readTag(in, riffTag) || !readValue(in, riffSize) || !readTag(in, waveTag) ||
        riffTag != std::array<char, 4>{'R', 'I', 'F', 'F'} ||
        waveTag != std::array<char, 4>{'W', 'A', 'V', 'E'}) {
        errorMessage = L"Invalid WAV header: " + std::wstring(pathText);
        return false;
    }

    bool formatSeen = false;
    bool dataSeen = false;
    while (in && !(formatSeen && dataSeen)) {
        std::array<char, 4> chunkTag{};
        uint32_t chunkSize = 0;
        if (!readTag(in, chunkTag) || !readValue(in, chunkSize)) {
            break;
        }

        if (chunkTag == std::array<char, 4>{'f', 'm', 't', ' '}) {
            if (!parseFormatChunk(in, chunkSize, format, pathText, errorMessage)) {
                return false;
            }
            formatSeen = true;
        } else if (chunkTag == std::array<char, 4>{'d', 'a', 't', 'a'}) {
            data.resize(chunkSize);
            if (!data.empty()) {
                in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
            }
            dataSeen = static_cast<bool>(in) || data.empty();
        } else {
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }

        if ((chunkSize & 1u) != 0u) {
            in.seekg(1, std::ios::cur);
        }
    }

    if (!formatSeen || !dataSeen) {
        errorMessage = L"Required WAV chunks were missing: " + std::wstring(pathText);
        return false;
    }
    return true;
}

double decodeNormalizedSample(const uint8_t* bytes, const MonoWaveFormat& format) {
    if (format.isFloat) {
        if (format.bitsPerSample == 32) {
            float value = 0.0f;
            std::memcpy(&value, bytes, sizeof(value));
            return static_cast<double>(value);
        }
        if (format.bitsPerSample == 64) {
            double value = 0.0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }
        return 0.0;
    }

    switch (format.bitsPerSample) {
    case 8:
        return (static_cast<double>(*bytes) - 128.0) / 128.0;
    case 16: {
        int16_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<double>(value) / 32768.0;
    }
    case 24:
        return static_cast<double>(readSigned24(bytes)) / 8388608.0;
    case 32: {
        int32_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        const int validBits = std::clamp<int>(format.validBitsPerSample, 1, 32);
        const int shift = 32 - validBits;
        const int32_t shifted = shift > 0 ? (value >> shift) : value;
        return static_cast<double>(shifted) / static_cast<double>(int64_t{1} << (validBits - 1));
    }
    default:
        return 0.0;
    }
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

    MonoWaveFormat format;
    std::vector<uint8_t> data;
    if (!readWaveHeader(in, path.wstring(), format, data, errorMessage)) {
        return false;
    }
    if (format.formatTag != kWaveFormatPcm || format.channelCount != 1 || format.bitsPerSample != 16 || format.sampleRate == 0) {
        errorMessage = L"Unsupported WAV format for raw capture: " + path.wstring();
        return false;
    }
    if (data.size() % sizeof(int16_t) != 0) {
        errorMessage = L"Unexpected WAV data size: " + path.wstring();
        return false;
    }

    samples.resize(data.size() / sizeof(int16_t));
    if (!samples.empty()) {
        std::memcpy(samples.data(), data.data(), data.size());
    }
    sampleRate = static_cast<int>(format.sampleRate);
    return true;
}

bool loadMonoWaveFileNormalized(const std::filesystem::path& path,
                                int& sampleRate,
                                std::vector<double>& samples,
                                std::wstring& errorMessage) {
    sampleRate = 0;
    samples.clear();
    errorMessage.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        errorMessage = L"Could not open WAV file: " + path.wstring();
        return false;
    }

    MonoWaveFormat format;
    std::vector<uint8_t> data;
    if (!readWaveHeader(in, path.wstring(), format, data, errorMessage)) {
        return false;
    }
    if (!format.supportsNormalizedDecode()) {
        errorMessage = L"Unsupported WAV format for raw capture: " + path.wstring();
        return false;
    }

    const size_t bytesPerSample = format.bytesPerSample();
    if (bytesPerSample == 0 || data.size() % bytesPerSample != 0) {
        errorMessage = L"Unexpected WAV data size: " + path.wstring();
        return false;
    }

    const size_t sampleCount = data.size() / bytesPerSample;
    samples.resize(sampleCount, 0.0);
    for (size_t index = 0; index < sampleCount; ++index) {
        const uint8_t* sampleBytes = data.data() + (index * bytesPerSample);
        samples[index] = decodeNormalizedSample(sampleBytes, format);
    }

    sampleRate = static_cast<int>(format.sampleRate);
    return true;
}

}  // namespace wolfie::persistence
