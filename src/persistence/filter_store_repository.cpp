#include "persistence/filter_store_repository.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <utility>

namespace wolfie::persistence {

namespace {

constexpr std::string_view kFilterStoreMagicV1 = "wolfie-filter-store-v1";
constexpr std::string_view kFilterStoreMagicV2 = "wolfie-filter-store-v2";
constexpr std::string_view kFilterStoreMagicV3 = "wolfie-filter-store-v3";
constexpr std::string_view kFilterStoreMagicV4 = "wolfie-filter-store-v4";
constexpr uint32_t kMaxStringBytes = 1u << 20;
constexpr uint32_t kMaxVectorElements = 1u << 22;
constexpr uint32_t kMaxProcessLogEntries = 1u << 14;

enum class FilterStoreVersion {
    V1,
    V2,
    V3,
    V4
};

std::filesystem::path filterStorePath(const std::filesystem::path& rootPath, std::string_view variant) {
    return rootPath / "filters" / (std::string(variant) + ".filter-store.bin");
}

template <typename T>
bool writeScalar(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(out);
}

template <typename T>
bool readScalar(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(in);
}

bool writeString(std::ofstream& out, const std::string& value) {
    const uint32_t size = static_cast<uint32_t>(value.size());
    return writeScalar(out, size) &&
           (size == 0 || static_cast<bool>(out.write(value.data(), static_cast<std::streamsize>(size))));
}

bool readString(std::ifstream& in, std::string& value) {
    uint32_t size = 0;
    if (!readScalar(in, size) || size > kMaxStringBytes) {
        return false;
    }

    value.assign(size, '\0');
    return size == 0 || static_cast<bool>(in.read(value.data(), static_cast<std::streamsize>(size)));
}

bool writeDoubleVector(std::ofstream& out, const std::vector<double>& values) {
    const uint32_t size = static_cast<uint32_t>(values.size());
    return writeScalar(out, size) &&
           (size == 0 || static_cast<bool>(out.write(reinterpret_cast<const char*>(values.data()),
                                                     static_cast<std::streamsize>(sizeof(double) * size))));
}

bool readDoubleVector(std::ifstream& in, std::vector<double>& values) {
    uint32_t size = 0;
    if (!readScalar(in, size) || size > kMaxVectorElements) {
        return false;
    }

    values.assign(size, 0.0);
    return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(values.data()),
                                                  static_cast<std::streamsize>(sizeof(double) * size)));
}

bool writeIntVector(std::ofstream& out, const std::vector<int>& values) {
    const uint32_t size = static_cast<uint32_t>(values.size());
    return writeScalar(out, size) &&
           (size == 0 || static_cast<bool>(out.write(reinterpret_cast<const char*>(values.data()),
                                                     static_cast<std::streamsize>(sizeof(int) * size))));
}

bool readIntVector(std::ifstream& in, std::vector<int>& values) {
    uint32_t size = 0;
    if (!readScalar(in, size) || size > kMaxVectorElements) {
        return false;
    }

    values.assign(size, 0);
    return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(values.data()),
                                                  static_cast<std::streamsize>(sizeof(int) * size)));
}

bool writeStringVector(std::ofstream& out, const std::vector<std::string>& values) {
    const uint32_t size = static_cast<uint32_t>(values.size());
    if (!writeScalar(out, size)) {
        return false;
    }
    for (const std::string& value : values) {
        if (!writeString(out, value)) {
            return false;
        }
    }
    return true;
}

bool readStringVector(std::ifstream& in, std::vector<std::string>& values) {
    uint32_t size = 0;
    if (!readScalar(in, size) || size > kMaxProcessLogEntries) {
        return false;
    }

    values.clear();
    values.reserve(size);
    for (uint32_t index = 0; index < size; ++index) {
        std::string value;
        if (!readString(in, value)) {
            return false;
        }
        values.push_back(std::move(value));
    }
    return true;
}

bool writeSettings(std::ofstream& out, const FilterDesignSettings& settings) {
    return writeScalar(out, static_cast<int32_t>(settings.tapCount)) &&
           writeScalar(out, settings.maxBoostDb) &&
           writeScalar(out, settings.maxCutDb) &&
           writeScalar(out, settings.smoothness) &&
           writeScalar(out, settings.lowCorrectionHz) &&
           writeScalar(out, settings.lowTaperOctaves) &&
           writeScalar(out, settings.highCorrectionHz) &&
           writeScalar(out, settings.highTaperOctaves) &&
           writeScalar(out, static_cast<int32_t>(settings.displayPointCount)) &&
           writeString(out, settings.phaseMode) &&
           writeScalar(out, settings.mixedPhaseMaxFrequencyHz) &&
           writeScalar(out, settings.excessPhaseWindowMs) &&
           writeScalar(out, settings.mixedPhaseStrength) &&
           writeScalar(out, settings.mixedPhaseMaxCorrectionDegrees) &&
           writeIntVector(out, settings.preRingingCompensationFrequenciesHz) &&
           writeScalar(out, settings.preRingingCompensationStrength);
}

bool readSettingsV1(std::ifstream& in, FilterDesignSettings& settings) {
    int32_t tapCount = 0;
    int32_t displayPointCount = 0;
    if (!readScalar(in, tapCount) ||
        !readScalar(in, settings.maxBoostDb) ||
        !readScalar(in, settings.maxCutDb) ||
        !readScalar(in, settings.smoothness) ||
        !readScalar(in, settings.lowCorrectionHz) ||
        !readScalar(in, settings.lowTaperOctaves) ||
        !readScalar(in, settings.highCorrectionHz) ||
        !readScalar(in, settings.highTaperOctaves) ||
        !readScalar(in, displayPointCount) ||
        !readString(in, settings.phaseMode) ||
        !readScalar(in, settings.mixedPhaseMaxFrequencyHz) ||
        !readScalar(in, settings.excessPhaseWindowMs) ||
        !readScalar(in, settings.mixedPhaseStrength) ||
        !readScalar(in, settings.mixedPhaseMaxCorrectionDegrees)) {
        return false;
    }

    settings.tapCount = std::max(tapCount, 0);
    settings.displayPointCount = std::max(displayPointCount, 0);
    return true;
}

bool readSettingsV2(std::ifstream& in, FilterDesignSettings& settings) {
    if (!readSettingsV1(in, settings) ||
        !readIntVector(in, settings.preRingingCompensationFrequenciesHz) ||
        !readScalar(in, settings.preRingingCompensationStrength)) {
        return false;
    }
    return true;
}

bool writeChannelResult(std::ofstream& out, const FilterDesignChannelResult& channel) {
    return writeDoubleVector(out, channel.correctionCurveDb) &&
           writeDoubleVector(out, channel.filterResponseDb) &&
           writeDoubleVector(out, channel.correctedResponseDb) &&
           writeDoubleVector(out, channel.inputGroupDelayMs) &&
           writeDoubleVector(out, channel.requestedMixedGroupDelayPreSolveMs) &&
           writeDoubleVector(out, channel.requestedMixedGroupDelayMs) &&
           writeDoubleVector(out, channel.groupDelayMs) &&
           writeDoubleVector(out, channel.inputExcessPhaseDegrees) &&
           writeDoubleVector(out, channel.inputExcessPhaseContinuousDegrees) &&
           writeDoubleVector(out, channel.predictedExcessPhaseDegrees) &&
           writeDoubleVector(out, channel.predictedExcessPhaseContinuousDegrees) &&
           writeDoubleVector(out, channel.predictedGroupDelayMs) &&
           writeDoubleVector(out, channel.impulseTimeMs) &&
           writeDoubleVector(out, channel.filterTaps) &&
           writeScalar(out, static_cast<int32_t>(channel.impulsePeakIndex)) &&
           writeScalar(out, channel.peakAmplitude);
}

bool readChannelResult(std::ifstream& in,
                       FilterDesignChannelResult& channel,
                       FilterStoreVersion version) {
    int32_t impulsePeakIndex = 0;
    if (!readDoubleVector(in, channel.correctionCurveDb) ||
        !readDoubleVector(in, channel.filterResponseDb) ||
        !readDoubleVector(in, channel.correctedResponseDb) ||
        !readDoubleVector(in, channel.inputGroupDelayMs) ||
        (version == FilterStoreVersion::V4 &&
         !readDoubleVector(in, channel.requestedMixedGroupDelayPreSolveMs)) ||
        ((version == FilterStoreVersion::V3 || version == FilterStoreVersion::V4) &&
         !readDoubleVector(in, channel.requestedMixedGroupDelayMs)) ||
        !readDoubleVector(in, channel.groupDelayMs) ||
        !readDoubleVector(in, channel.inputExcessPhaseDegrees) ||
        !readDoubleVector(in, channel.inputExcessPhaseContinuousDegrees) ||
        !readDoubleVector(in, channel.predictedExcessPhaseDegrees) ||
        !readDoubleVector(in, channel.predictedExcessPhaseContinuousDegrees) ||
        !readDoubleVector(in, channel.predictedGroupDelayMs) ||
        !readDoubleVector(in, channel.impulseTimeMs) ||
        !readDoubleVector(in, channel.filterTaps) ||
        !readScalar(in, impulsePeakIndex) ||
        !readScalar(in, channel.peakAmplitude)) {
        return false;
    }

    channel.impulsePeakIndex = impulsePeakIndex;
    return true;
}

bool writeStoredFilter(std::ofstream& out, const StoredFilterDesign& storedFilter) {
    const uint8_t available = storedFilter.available() ? 1 : 0;
    return writeScalar(out, available) &&
           (!storedFilter.available() ||
            (writeSettings(out, storedFilter.settings) &&
             writeScalar(out, static_cast<int32_t>(storedFilter.result.sampleRate)) &&
             writeScalar(out, static_cast<int32_t>(storedFilter.result.tapCount)) &&
             writeScalar(out, static_cast<int32_t>(storedFilter.result.fftSize)) &&
             writeScalar(out, static_cast<int32_t>(storedFilter.result.positiveBinCount)) &&
             writeString(out, storedFilter.result.phaseMode) &&
             writeString(out, storedFilter.result.phasePreparationSourceWindow) &&
             writeString(out, storedFilter.result.phasePreparationSourceKey) &&
             writeString(out, storedFilter.result.phasePreparationSeriesKind) &&
             writeScalar(out, storedFilter.result.phasePreparationBulkDelaySeconds) &&
             writeScalar(out, storedFilter.result.requestedMixedTransitionStartHz) &&
             writeScalar(out, storedFilter.result.requestedMixedTransitionEndHz) &&
             writeStringVector(out, storedFilter.result.processLog) &&
             writeDoubleVector(out, storedFilter.result.frequencyAxisHz) &&
             writeDoubleVector(out, storedFilter.result.targetCurveDb) &&
             writeChannelResult(out, storedFilter.result.left) &&
             writeChannelResult(out, storedFilter.result.right)));
}

bool readStoredFilter(std::ifstream& in,
                      StoredFilterDesign& storedFilter,
                      FilterStoreVersion version) {
    storedFilter = {};

    uint8_t available = 0;
    if (!readScalar(in, available)) {
        return false;
    }
    if (available == 0) {
        return true;
    }

    int32_t sampleRate = 0;
    int32_t tapCount = 0;
    int32_t fftSize = 0;
    int32_t positiveBinCount = 0;
    const bool settingsOk = version == FilterStoreVersion::V1
                                ? readSettingsV1(in, storedFilter.settings)
                                : readSettingsV2(in, storedFilter.settings);
    if (!settingsOk ||
        !readScalar(in, sampleRate) ||
        !readScalar(in, tapCount) ||
        !readScalar(in, fftSize) ||
        !readScalar(in, positiveBinCount) ||
        !readString(in, storedFilter.result.phaseMode) ||
        !readString(in, storedFilter.result.phasePreparationSourceWindow) ||
        !readString(in, storedFilter.result.phasePreparationSourceKey) ||
        !readString(in, storedFilter.result.phasePreparationSeriesKind) ||
        !readScalar(in, storedFilter.result.phasePreparationBulkDelaySeconds) ||
        (version == FilterStoreVersion::V4 &&
         (!readScalar(in, storedFilter.result.requestedMixedTransitionStartHz) ||
          !readScalar(in, storedFilter.result.requestedMixedTransitionEndHz))) ||
        !readStringVector(in, storedFilter.result.processLog) ||
        !readDoubleVector(in, storedFilter.result.frequencyAxisHz) ||
        !readDoubleVector(in, storedFilter.result.targetCurveDb) ||
        !readChannelResult(in, storedFilter.result.left, version) ||
        !readChannelResult(in, storedFilter.result.right, version)) {
        storedFilter = {};
        return false;
    }

    storedFilter.result.valid = true;
    storedFilter.result.sampleRate = sampleRate;
    storedFilter.result.tapCount = tapCount;
    storedFilter.result.fftSize = fftSize;
    storedFilter.result.positiveBinCount = positiveBinCount;
    return true;
}

bool saveVariantFile(const std::filesystem::path& rootPath,
                     std::string_view variant,
                     const StoredFilterDesign& storedFilter) {
    const std::filesystem::path path = filterStorePath(rootPath, variant);
    if (!storedFilter.available()) {
        std::filesystem::remove(path);
        return true;
    }

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    const std::array<char, kFilterStoreMagicV4.size()> magicBytes = [] {
        std::array<char, kFilterStoreMagicV4.size()> bytes{};
        std::copy(kFilterStoreMagicV4.begin(), kFilterStoreMagicV4.end(), bytes.begin());
        return bytes;
    }();

    return static_cast<bool>(out.write(magicBytes.data(), static_cast<std::streamsize>(magicBytes.size()))) &&
           writeStoredFilter(out, storedFilter);
}

bool loadVariantFile(const std::filesystem::path& rootPath,
                     std::string_view variant,
                     StoredFilterDesign& storedFilter) {
    storedFilter = {};
    const std::filesystem::path path = filterStorePath(rootPath, variant);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    std::array<char, kFilterStoreMagicV4.size()> magicBytes{};
    if (!in.read(magicBytes.data(), static_cast<std::streamsize>(magicBytes.size()))) {
        return false;
    }
    const bool isV4 =
        std::equal(magicBytes.begin(), magicBytes.end(), kFilterStoreMagicV4.begin(), kFilterStoreMagicV4.end());
    const bool isV3 =
        std::equal(magicBytes.begin(), magicBytes.end(), kFilterStoreMagicV3.begin(), kFilterStoreMagicV3.end());
    const bool isV2 =
        std::equal(magicBytes.begin(), magicBytes.end(), kFilterStoreMagicV2.begin(), kFilterStoreMagicV2.end());
    const bool isV1 =
        std::equal(magicBytes.begin(), magicBytes.end(), kFilterStoreMagicV1.begin(), kFilterStoreMagicV1.end());
    if (!isV1 && !isV2 && !isV3 && !isV4) {
        storedFilter = {};
        return false;
    }

    return readStoredFilter(in,
                            storedFilter,
                            isV4 ? FilterStoreVersion::V4
                                 : (isV3 ? FilterStoreVersion::V3
                                         : (isV2 ? FilterStoreVersion::V2 : FilterStoreVersion::V1)));
}

}  // namespace

void FilterStoreRepository::load(const std::filesystem::path& rootPath,
                                 StoredFilterDesign& minimumFilter,
                                 StoredFilterDesign& mixedFilter) const {
    minimumFilter = {};
    mixedFilter = {};
    if (rootPath.empty()) {
        return;
    }

    loadVariantFile(rootPath, "minimum", minimumFilter);
    loadVariantFile(rootPath, "mixed", mixedFilter);
}

void FilterStoreRepository::save(const std::filesystem::path& rootPath,
                                 const StoredFilterDesign& minimumFilter,
                                 const StoredFilterDesign& mixedFilter) const {
    if (rootPath.empty()) {
        return;
    }

    saveVariantFile(rootPath, "minimum", minimumFilter);
    saveVariantFile(rootPath, "mixed", mixedFilter);
    const std::filesystem::path filtersPath = rootPath / "filters";
    if (std::filesystem::exists(filtersPath) && std::filesystem::is_directory(filtersPath) &&
        std::filesystem::is_empty(filtersPath)) {
        std::filesystem::remove(filtersPath);
    }
}

}  // namespace wolfie::persistence
