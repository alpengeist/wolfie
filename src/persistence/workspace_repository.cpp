#include "persistence/workspace_repository.h"

#include <cctype>
#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "persistence/microphone_calibration_repository.h"
#include "measurement/filter_designer.h"
#include "measurement/response_smoother.h"
#include "measurement/sweep_generator.h"
#include "measurement/target_curve_designer.h"

namespace wolfie::persistence {

namespace {

std::string escapeJson(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::optional<std::string> readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool writeTextFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    out << content;
    return static_cast<bool>(out);
}

std::optional<std::string> findJsonString(const std::string& source, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const size_t keyPos = source.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t colonPos = source.find(':', keyPos + pattern.size());
    const size_t firstQuote = source.find('"', colonPos + 1);
    if (colonPos == std::string::npos || firstQuote == std::string::npos) {
        return std::nullopt;
    }

    std::string value;
    for (size_t cursor = firstQuote + 1; cursor < source.size(); ++cursor) {
        const char ch = source[cursor];
        if (ch == '\\' && cursor + 1 < source.size()) {
            value.push_back(source[cursor + 1]);
            ++cursor;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

std::optional<double> findJsonNumber(const std::string& source, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const size_t keyPos = source.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t colonPos = source.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t valueStart = source.find_first_of("-0123456789", colonPos + 1);
    if (valueStart == std::string::npos) {
        return std::nullopt;
    }
    const size_t valueEnd = source.find_first_not_of("0123456789+-.eE", valueStart);
    return std::stod(source.substr(valueStart, valueEnd - valueStart));
}

std::optional<bool> findJsonBool(const std::string& source, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const size_t keyPos = source.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t colonPos = source.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t valueStart = source.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valueStart == std::string::npos) {
        return std::nullopt;
    }
    if (source.compare(valueStart, 4, "true") == 0) {
        return true;
    }
    if (source.compare(valueStart, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<std::vector<int>> findJsonIntArray(const std::string& source, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const size_t keyPos = source.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t colonPos = source.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t arrayStart = source.find('[', colonPos + 1);
    const size_t arrayEnd = source.find(']', arrayStart + 1);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos) {
        return std::nullopt;
    }

    std::vector<int> values;
    size_t cursor = arrayStart + 1;
    while (cursor < arrayEnd) {
        cursor = source.find_first_of("-0123456789", cursor);
        if (cursor == std::string::npos || cursor >= arrayEnd) {
            break;
        }
        const size_t valueEnd = source.find_first_not_of("0123456789+-", cursor);
        values.push_back(std::stoi(source.substr(cursor, valueEnd - cursor)));
        if (valueEnd == std::string::npos || valueEnd >= arrayEnd) {
            break;
        }
        cursor = valueEnd + 1;
    }

    return values;
}

void loadUiSettingsFromJson(const std::string& content, UiSettings& ui) {
    if (const auto value = findJsonNumber(content, "measurementSectionHeight")) {
        ui.measurementSectionHeight = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(content, "resultSectionHeight")) {
        ui.resultSectionHeight = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(content, "processLogHeight")) {
        ui.processLogHeight = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(content, "measurementGraphExtraRangeDb")) {
        ui.measurementGraphExtraRangeDb = *value;
    }
    if (const auto value = findJsonNumber(content, "measurementGraphVerticalOffsetDb")) {
        ui.measurementGraphVerticalOffsetDb = *value;
    }
    if (const auto value = findJsonBool(content, "measurementGraphHasCustomFrequencyRange")) {
        ui.measurementGraphHasCustomFrequencyRange = *value;
    }
    if (const auto value = findJsonNumber(content, "measurementGraphVisibleMinFrequencyHz")) {
        ui.measurementGraphVisibleMinFrequencyHz = *value;
    }
    if (const auto value = findJsonNumber(content, "measurementGraphVisibleMaxFrequencyHz")) {
        ui.measurementGraphVisibleMaxFrequencyHz = *value;
    }
    if (const auto value = findJsonString(content, "measurementPlotMode")) {
        ui.measurementPlotMode = *value;
    }
    if (const auto value = findJsonString(content, "measurementWaterfallChannel")) {
        ui.measurementWaterfallChannel = *value;
    }
    if (const auto value = findJsonBool(content, "measurementMetadataCollapsed")) {
        ui.measurementMetadataCollapsed = *value;
    }
    if (const auto value = findJsonNumber(content, "smoothingGraphExtraRangeDb")) {
        ui.smoothingGraphExtraRangeDb = *value;
    }
    if (const auto value = findJsonNumber(content, "smoothingGraphVerticalOffsetDb")) {
        ui.smoothingGraphVerticalOffsetDb = *value;
    }
    if (const auto value = findJsonBool(content, "smoothingGraphHasCustomFrequencyRange")) {
        ui.smoothingGraphHasCustomFrequencyRange = *value;
    }
    if (const auto value = findJsonNumber(content, "smoothingGraphVisibleMinFrequencyHz")) {
        ui.smoothingGraphVisibleMinFrequencyHz = *value;
    }
    if (const auto value = findJsonNumber(content, "smoothingGraphVisibleMaxFrequencyHz")) {
        ui.smoothingGraphVisibleMaxFrequencyHz = *value;
    }
    if (const auto value = findJsonNumber(content, "targetCurveGraphExtraRangeDb")) {
        ui.targetCurveGraphExtraRangeDb = *value;
    }
    if (const auto value = findJsonNumber(content, "targetCurveGraphVerticalOffsetDb")) {
        ui.targetCurveGraphVerticalOffsetDb = *value;
    }
    if (const auto value = findJsonBool(content, "targetCurveGraphHasCustomVisibleDbRange")) {
        ui.targetCurveGraphHasCustomVisibleDbRange = *value;
    }
    if (const auto value = findJsonNumber(content, "targetCurveGraphVisibleMinDb")) {
        ui.targetCurveGraphVisibleMinDb = *value;
    }
    if (const auto value = findJsonNumber(content, "targetCurveGraphVisibleMaxDb")) {
        ui.targetCurveGraphVisibleMaxDb = *value;
    }
    if (const auto value = findJsonIntArray(content, "exportSampleRatesHz")) {
        ui.exportSampleRatesHz = *value;
        ui.exportSampleRatesCustomized = true;
    }
}

constexpr char kMeasurementResultFileMagic[] = "wolfie-result-values-v1";
constexpr char kTargetCurveProfileFileMagic[] = "wolfie-target-curve-v1";
constexpr char kTargetCurveProfileFileExtension[] = ".target-curve.txt";

std::string trimAscii(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::filesystem::path measurementResultFilePath(const std::filesystem::path& rootPath) {
    return rootPath / "measurement" / "result-values.txt";
}

std::filesystem::path measurementAnalysisFilePath(const std::filesystem::path& rootPath) {
    return rootPath / "measurement" / "analysis.json";
}

bool hasSuffix(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size(), suffix.size()) == suffix;
}

std::string sanitizeFileComponent(std::string_view value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    for (const char ch : value) {
        const bool invalid = ch < 32 || ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
                             ch == '"' || ch == '<' || ch == '>' || ch == '|';
        sanitized.push_back(invalid ? '_' : ch);
    }
    while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.')) {
        sanitized.pop_back();
    }
    size_t begin = 0;
    while (begin < sanitized.size() && sanitized[begin] == ' ') {
        ++begin;
    }
    sanitized.erase(0, begin);
    if (sanitized.empty()) {
        sanitized = "Default";
    }
    return sanitized;
}

std::filesystem::path targetCurveProfileFilePath(const std::filesystem::path& rootPath, std::string_view profileName) {
    return rootPath / (sanitizeFileComponent(profileName) + kTargetCurveProfileFileExtension);
}

std::optional<TargetCurveProfile> loadTargetCurveProfileFile(const std::filesystem::path& path) {
    const auto content = readTextFile(path);
    if (!content) {
        return std::nullopt;
    }

    std::istringstream in(*content);
    std::string line;
    if (!std::getline(in, line) || trimAscii(line) != kTargetCurveProfileFileMagic) {
        return std::nullopt;
    }

    TargetCurveProfile profile;
    bool readingComment = false;
    bool readingBands = false;
    std::string comment;
    while (std::getline(in, line)) {
        if (readingComment) {
            if (line == "comment_end") {
                readingComment = false;
                profile.comment = comment;
                continue;
            }
            if (!comment.empty()) {
                comment += '\n';
            }
            comment += line;
            continue;
        }
        if (readingBands) {
            if (line == "bands_end") {
                readingBands = false;
                continue;
            }

            std::istringstream row(line);
            std::string cell;
            std::vector<std::string> values;
            while (std::getline(row, cell, ',')) {
                values.push_back(cell);
            }
            if (values.size() < 5) {
                continue;
            }

            TargetEqBand band;
            band.enabled = values[0] == "1";
            band.colorIndex = std::stoi(values[1]);
            band.frequencyHz = std::stod(values[2]);
            band.gainDb = std::stod(values[3]);
            band.q = std::stod(values[4]);
            profile.curve.eqBands.push_back(band);
            continue;
        }

        if (line == "comment_begin") {
            readingComment = true;
            comment.clear();
            continue;
        }
        if (line == "bands_begin") {
            readingBands = true;
            continue;
        }
        if (line.rfind("name=", 0) == 0) {
            profile.name = line.substr(5);
            continue;
        }
        if (line.rfind("lowGainDb=", 0) == 0) {
            profile.curve.lowGainDb = std::stod(line.substr(10));
            continue;
        }
        if (line.rfind("midFrequencyHz=", 0) == 0) {
            profile.curve.midFrequencyHz = std::stod(line.substr(15));
            continue;
        }
        if (line.rfind("midGainDb=", 0) == 0) {
            profile.curve.midGainDb = std::stod(line.substr(10));
            continue;
        }
        if (line.rfind("highGainDb=", 0) == 0) {
            profile.curve.highGainDb = std::stod(line.substr(11));
            continue;
        }
        if (line.rfind("bypassEqBands=", 0) == 0) {
            profile.curve.bypassEqBands = trimAscii(line.substr(14)) == "1";
            continue;
        }
    }

    if (profile.name.empty()) {
        profile.name = path.stem().string();
    }
    return profile;
}

void saveTargetCurveProfileFile(const std::filesystem::path& rootPath, const TargetCurveProfile& profile) {
    std::ostringstream out;
    out << kTargetCurveProfileFileMagic << '\n'
        << "name=" << profile.name << '\n'
        << "lowGainDb=" << profile.curve.lowGainDb << '\n'
        << "midFrequencyHz=" << profile.curve.midFrequencyHz << '\n'
        << "midGainDb=" << profile.curve.midGainDb << '\n'
        << "highGainDb=" << profile.curve.highGainDb << '\n'
        << "bypassEqBands=" << (profile.curve.bypassEqBands ? 1 : 0) << '\n'
        << "comment_begin\n"
        << profile.comment << '\n'
        << "comment_end\n"
        << "bands_begin\n";
    for (const TargetEqBand& band : profile.curve.eqBands) {
        out << (band.enabled ? 1 : 0) << ','
            << band.colorIndex << ','
            << band.frequencyHz << ','
            << band.gainDb << ','
            << band.q << '\n';
    }
    out << "bands_end\n";
    writeTextFile(targetCurveProfileFilePath(rootPath, profile.name), out.str());
}

void writeJsonIntArray(std::ostringstream& out, const std::vector<int>& values) {
    out << '[';
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            out << ", ";
        }
        out << values[index];
    }
    out << ']';
}

bool parseMeasurementDataRow(std::string line, double& xValue, double& leftValue, double& rightValue) {
    for (char& ch : line) {
        if (ch == ',' || ch == ';' || ch == '\t') {
            ch = ' ';
        }
    }

    std::istringstream row(line);
    return static_cast<bool>(row >> xValue >> leftValue >> rightValue);
}

void appendMeasurementValueSetIfValid(MeasurementResult& result, MeasurementValueSet& valueSet) {
    if (!valueSet.valid()) {
        valueSet = {};
        return;
    }

    result.valueSets.push_back(std::move(valueSet));
    valueSet = {};
}

void loadMeasurementResultFile(WorkspaceState& workspace) {
    workspace.result = {};
    if (workspace.rootPath.empty()) {
        return;
    }

    const auto content = readTextFile(measurementResultFilePath(workspace.rootPath));
    if (!content) {
        return;
    }

    std::istringstream in(*content);
    std::string line;
    bool headerSeen = false;
    bool inSeries = false;
    bool inData = false;
    MeasurementValueSet currentValueSet;
    while (std::getline(in, line)) {
        const std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (!headerSeen) {
            if (trimmed != kMeasurementResultFileMagic) {
                return;
            }
            headerSeen = true;
            continue;
        }

        if (inData) {
            if (trimmed == "data_end") {
                inData = false;
                continue;
            }

            double xValue = 0.0;
            double leftValue = 0.0;
            double rightValue = 0.0;
            if (parseMeasurementDataRow(trimmed, xValue, leftValue, rightValue)) {
                currentValueSet.xValues.push_back(xValue);
                currentValueSet.leftValues.push_back(leftValue);
                currentValueSet.rightValues.push_back(rightValue);
            }
            continue;
        }

        if (trimmed.starts_with("[series ") && trimmed.ends_with(']')) {
            appendMeasurementValueSetIfValid(workspace.result, currentValueSet);
            currentValueSet = {};
            currentValueSet.key = trimAscii(trimmed.substr(8, trimmed.size() - 9));
            inSeries = !currentValueSet.key.empty();
            continue;
        }

        if (trimmed == "[/series]") {
            appendMeasurementValueSetIfValid(workspace.result, currentValueSet);
            inSeries = false;
            inData = false;
            continue;
        }

        if (!inSeries) {
            continue;
        }

        if (trimmed == "data_begin") {
            inData = true;
            continue;
        }

        const size_t equalsPos = trimmed.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }

        const std::string key = trimAscii(trimmed.substr(0, equalsPos));
        const std::string value = trimAscii(trimmed.substr(equalsPos + 1));
        if (key == "x_quantity") {
            currentValueSet.xQuantity = value;
        } else if (key == "x_unit") {
            currentValueSet.xUnit = value;
        } else if (key == "y_quantity") {
            currentValueSet.yQuantity = value;
        } else if (key == "y_unit") {
            currentValueSet.yUnit = value;
        }
    }

    appendMeasurementValueSetIfValid(workspace.result, currentValueSet);
}

void loadMeasurementArtifact(const std::string& content,
                             MeasurementAnalysis& analysis,
                             const char* jsonKey,
                             const char* artifactKey) {
    if (const auto value = findJsonString(content, jsonKey)) {
        if (value->empty()) {
            return;
        }
        analysis.artifacts.push_back({artifactKey, std::filesystem::path(*value)});
    }
}

void loadMeasurementAnalysisFile(WorkspaceState& workspace) {
    workspace.result.analysis = {};
    if (workspace.rootPath.empty()) {
        return;
    }

    const auto content = readTextFile(measurementAnalysisFilePath(workspace.rootPath));
    if (!content) {
        return;
    }

    MeasurementAnalysis& analysis = workspace.result.analysis;
    if (const auto value = findJsonString(*content, "analyzerVersion")) {
        analysis.analyzerVersion = *value;
    }
    if (const auto value = findJsonString(*content, "measurementTimestampUtc")) {
        analysis.measurementTimestampUtc = *value;
    }
    if (const auto value = findJsonString(*content, "backendName")) {
        analysis.backendName = *value;
    }
    if (const auto value = findJsonString(*content, "backendInputDevice")) {
        analysis.backendInputDevice = *value;
    }
    if (const auto value = findJsonString(*content, "backendOutputDevice")) {
        analysis.backendOutputDevice = *value;
    }
    if (const auto value = findJsonString(*content, "requestedDriver")) {
        analysis.requestedDriver = *value;
    }
    if (const auto value = findJsonNumber(*content, "requestedMicInputChannel")) {
        analysis.requestedMicInputChannel = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "requestedLeftOutputChannel")) {
        analysis.requestedLeftOutputChannel = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "requestedRightOutputChannel")) {
        analysis.requestedRightOutputChannel = static_cast<int>(*value);
    }
    if (const auto value = findJsonBool(*content, "routingSelectionHonored")) {
        analysis.routingSelectionHonored = *value;
    }
    if (const auto value = findJsonString(*content, "routingNotes")) {
        analysis.routingNotes = *value;
    }
    if (const auto value = findJsonNumber(*content, "sampleRate")) {
        analysis.sampleRate = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "sweepDurationSeconds")) {
        analysis.sweepDurationSeconds = *value;
    }
    if (const auto value = findJsonNumber(*content, "fadeInSeconds")) {
        analysis.fadeInSeconds = *value;
    }
    if (const auto value = findJsonNumber(*content, "fadeOutSeconds")) {
        analysis.fadeOutSeconds = *value;
    }
    if (const auto value = findJsonNumber(*content, "startFrequencyHz")) {
        analysis.startFrequencyHz = *value;
    }
    if (const auto value = findJsonNumber(*content, "endFrequencyHz")) {
        analysis.endFrequencyHz = *value;
    }
    if (const auto value = findJsonNumber(*content, "targetLengthSamples")) {
        analysis.targetLengthSamples = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "leadInSamples")) {
        analysis.leadInSamples = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "outputVolumeDb")) {
        analysis.outputVolumeDb = *value;
    }
    if (const auto value = findJsonNumber(*content, "playedSweepSamples")) {
        analysis.playedSweepSamples = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "capturedSamples")) {
        analysis.capturedSamples = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "alignmentSearchSamples")) {
        analysis.alignmentSearchSamples = static_cast<int>(*value);
    }
    if (const auto value = findJsonString(*content, "alignmentMethod")) {
        analysis.alignmentMethod = *value;
    }
    if (const auto value = findJsonString(*content, "windowType")) {
        analysis.windowType = *value;
    }
    if (const auto value = findJsonNumber(*content, "inverseFilterLengthSamples")) {
        analysis.inverseFilterLengthSamples = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "inverseFilterPeakIndex")) {
        analysis.inverseFilterPeakIndex = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "fftSize")) {
        analysis.fftSize = static_cast<int>(*value);
    }
    if (const auto value = findJsonNumber(*content, "displayPointCount")) {
        analysis.displayPointCount = static_cast<int>(*value);
    }
    if (const auto value = findJsonBool(*content, "captureClippingDetected")) {
        analysis.captureClippingDetected = *value;
    }
    if (const auto value = findJsonBool(*content, "captureTooQuiet")) {
        analysis.captureTooQuiet = *value;
    }
    if (const auto value = findJsonNumber(*content, "capturePeakDb")) {
        analysis.capturePeakDb = *value;
    }
    if (const auto value = findJsonNumber(*content, "captureRmsDb")) {
        analysis.captureRmsDb = *value;
    }
    if (const auto value = findJsonNumber(*content, "captureNoiseFloorDb")) {
        analysis.captureNoiseFloorDb = *value;
    }

    auto loadChannel = [&](MeasurementChannelMetrics& channel, std::string_view prefix) {
        const std::string stem(prefix);
        if (const auto value = findJsonBool(*content, stem + "Available")) {
            channel.available = *value;
        }
        if (const auto value = findJsonNumber(*content, stem + "DetectedLatencySamples")) {
            channel.detectedLatencySamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "OnsetSampleIndex")) {
            channel.onsetSampleIndex = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "OnsetTimeSeconds")) {
            channel.onsetTimeSeconds = *value;
        }
        if (const auto value = findJsonNumber(*content, stem + "PeakSampleIndex")) {
            channel.peakSampleIndex = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "ImpulseStartSample")) {
            channel.impulseStartSample = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "ImpulseLengthSamples")) {
            channel.impulseLengthSamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "PreRollSamples")) {
            channel.preRollSamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "AnalysisWindowStartSample")) {
            channel.analysisWindowStartSample = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "AnalysisWindowLengthSamples")) {
            channel.analysisWindowLengthSamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "AnalysisWindowFadeSamples")) {
            channel.analysisWindowFadeSamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, stem + "CapturePeakDb")) {
            channel.capturePeakDb = *value;
        }
        if (const auto value = findJsonNumber(*content, stem + "CaptureRmsDb")) {
            channel.captureRmsDb = *value;
        }
        if (const auto value = findJsonNumber(*content, stem + "NoiseFloorDb")) {
            channel.noiseFloorDb = *value;
        }
        if (const auto value = findJsonNumber(*content, stem + "ImpulsePeakAmplitude")) {
            channel.impulsePeakAmplitude = *value;
        }
        if (const auto value = findJsonNumber(*content, stem + "ImpulsePeakDb")) {
            channel.impulsePeakDb = *value;
        }
        if (const auto value = findJsonNumber(*content, stem + "ImpulseRmsDb")) {
            channel.impulseRmsDb = *value;
        }
        if (const auto value = findJsonNumber(*content, stem + "ImpulsePeakToNoiseDb")) {
            channel.impulsePeakToNoiseDb = *value;
        }
    };

    loadChannel(analysis.left, "left");
    loadChannel(analysis.right, "right");
    loadMeasurementArtifact(*content, analysis, "artifactGeneratedSweepWav", "generated_sweep_wav");
    loadMeasurementArtifact(*content, analysis, "artifactRawCaptureWav", "raw_capture_wav");
    loadMeasurementArtifact(*content, analysis, "artifactResultValuesTxt", "result_values_txt");
    loadMeasurementArtifact(*content, analysis, "artifactAnalysisJson", "analysis_json");
}

void loadTargetCurveBandsFile(WorkspaceState& workspace) {
    workspace.targetCurve.eqBands.clear();
    if (workspace.rootPath.empty()) {
        return;
    }

    const auto content = readTextFile(workspace.rootPath / "target-curve" / "bands.csv");
    if (!content) {
        return;
    }

    std::istringstream in(*content);
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("enabled") || line.empty()) {
            continue;
        }

        std::istringstream row(line);
        std::string cell;
        std::vector<std::string> values;
        while (std::getline(row, cell, ',')) {
            values.push_back(cell);
        }
        if (values.size() < 5) {
            continue;
        }

        TargetEqBand band;
        band.enabled = values[0] == "1";
        band.colorIndex = std::stoi(values[1]);
        band.frequencyHz = std::stod(values[2]);
        band.gainDb = std::stod(values[3]);
        band.q = std::stod(values[4]);
        workspace.targetCurve.eqBands.push_back(band);
    }
}

void loadTargetCurveProfiles(WorkspaceState& workspace) {
    workspace.targetCurveProfiles.clear();
    if (workspace.rootPath.empty()) {
        return;
    }

    std::vector<std::filesystem::path> profileFiles;
    for (const auto& entry : std::filesystem::directory_iterator(workspace.rootPath)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (hasSuffix(filename, kTargetCurveProfileFileExtension)) {
            profileFiles.push_back(entry.path());
        }
    }
    std::sort(profileFiles.begin(), profileFiles.end());
    for (const auto& path : profileFiles) {
        if (const auto profile = loadTargetCurveProfileFile(path)) {
            workspace.targetCurveProfiles.push_back(*profile);
        }
    }

    if (workspace.targetCurveProfiles.empty()) {
        loadTargetCurveBandsFile(workspace);
        TargetCurveProfile fallbackProfile;
        fallbackProfile.name = workspace.activeTargetCurveProfileName.empty() ? "Default" : workspace.activeTargetCurveProfileName;
        fallbackProfile.comment = workspace.activeTargetCurveComment;
        fallbackProfile.curve = workspace.targetCurve;
        workspace.targetCurveProfiles.push_back(fallbackProfile);
    }

    if (workspace.activeTargetCurveProfileName.empty()) {
        workspace.activeTargetCurveProfileName = workspace.targetCurveProfiles.front().name;
    }
    auto selected = std::find_if(workspace.targetCurveProfiles.begin(),
                                 workspace.targetCurveProfiles.end(),
                                 [&](const TargetCurveProfile& profile) {
                                     return profile.name == workspace.activeTargetCurveProfileName;
                                 });
    if (selected == workspace.targetCurveProfiles.end()) {
        selected = workspace.targetCurveProfiles.begin();
        workspace.activeTargetCurveProfileName = selected->name;
    }

    workspace.activeTargetCurveComment = selected->comment;
    workspace.targetCurve = selected->curve;
}

void saveMeasurementResultFile(const WorkspaceState& workspace) {
    if (workspace.rootPath.empty()) {
        return;
    }

    std::filesystem::create_directories(workspace.rootPath / "measurement");
    std::ostringstream out;
    out << kMeasurementResultFileMagic << '\n';
    for (const MeasurementValueSet& valueSet : workspace.result.valueSets) {
        if (!valueSet.valid() || valueSet.key.empty()) {
            continue;
        }

        out << '\n';
        out << "[series " << valueSet.key << "]\n";
        out << "x_quantity=" << valueSet.xQuantity << '\n';
        out << "x_unit=" << valueSet.xUnit << '\n';
        out << "y_quantity=" << valueSet.yQuantity << '\n';
        out << "y_unit=" << valueSet.yUnit << '\n';
        out << "data_begin\n";
        for (size_t i = 0; i < valueSet.xValues.size(); ++i) {
            out << valueSet.xValues[i] << ','
                << valueSet.leftValues[i] << ','
                << valueSet.rightValues[i] << '\n';
        }
        out << "data_end\n";
        out << "[/series]\n";
    }
    writeTextFile(measurementResultFilePath(workspace.rootPath), out.str());
    std::filesystem::remove(workspace.rootPath / "measurement" / "response.csv");
}

void saveMeasurementAnalysisFile(const WorkspaceState& workspace) {
    if (workspace.rootPath.empty()) {
        return;
    }

    const MeasurementAnalysis& analysis = workspace.result.analysis;
    if (!workspace.result.hasAnyValues() &&
        analysis.measurementTimestampUtc.empty() &&
        analysis.artifacts.empty()) {
        std::filesystem::remove(measurementAnalysisFilePath(workspace.rootPath));
        return;
    }

    auto artifactPath = [&](std::string_view key) -> std::string {
        if (const MeasurementArtifact* artifact = analysis.findArtifact(key)) {
            const auto utf8 = artifact->path.generic_u8string();
            return std::string(utf8.begin(), utf8.end());
        }
        return {};
    };

    auto writeChannel = [](std::ostringstream& out, const MeasurementChannelMetrics& channel, std::string_view prefix) {
        out << "  \"" << prefix << "Available\": " << (channel.available ? "true" : "false") << ",\n"
            << "  \"" << prefix << "DetectedLatencySamples\": " << channel.detectedLatencySamples << ",\n"
            << "  \"" << prefix << "OnsetSampleIndex\": " << channel.onsetSampleIndex << ",\n"
            << "  \"" << prefix << "OnsetTimeSeconds\": " << channel.onsetTimeSeconds << ",\n"
            << "  \"" << prefix << "PeakSampleIndex\": " << channel.peakSampleIndex << ",\n"
            << "  \"" << prefix << "ImpulseStartSample\": " << channel.impulseStartSample << ",\n"
            << "  \"" << prefix << "ImpulseLengthSamples\": " << channel.impulseLengthSamples << ",\n"
            << "  \"" << prefix << "PreRollSamples\": " << channel.preRollSamples << ",\n"
            << "  \"" << prefix << "AnalysisWindowStartSample\": " << channel.analysisWindowStartSample << ",\n"
            << "  \"" << prefix << "AnalysisWindowLengthSamples\": " << channel.analysisWindowLengthSamples << ",\n"
            << "  \"" << prefix << "AnalysisWindowFadeSamples\": " << channel.analysisWindowFadeSamples << ",\n"
            << "  \"" << prefix << "CapturePeakDb\": " << channel.capturePeakDb << ",\n"
            << "  \"" << prefix << "CaptureRmsDb\": " << channel.captureRmsDb << ",\n"
            << "  \"" << prefix << "NoiseFloorDb\": " << channel.noiseFloorDb << ",\n"
            << "  \"" << prefix << "ImpulsePeakAmplitude\": " << channel.impulsePeakAmplitude << ",\n"
            << "  \"" << prefix << "ImpulsePeakDb\": " << channel.impulsePeakDb << ",\n"
            << "  \"" << prefix << "ImpulseRmsDb\": " << channel.impulseRmsDb << ",\n"
            << "  \"" << prefix << "ImpulsePeakToNoiseDb\": " << channel.impulsePeakToNoiseDb << ",\n";
    };

    std::ostringstream out;
    out << "{\n"
        << "  \"analyzerVersion\": \"" << escapeJson(analysis.analyzerVersion) << "\",\n"
        << "  \"measurementTimestampUtc\": \"" << escapeJson(analysis.measurementTimestampUtc) << "\",\n"
        << "  \"backendName\": \"" << escapeJson(analysis.backendName) << "\",\n"
        << "  \"backendInputDevice\": \"" << escapeJson(analysis.backendInputDevice) << "\",\n"
        << "  \"backendOutputDevice\": \"" << escapeJson(analysis.backendOutputDevice) << "\",\n"
        << "  \"requestedDriver\": \"" << escapeJson(analysis.requestedDriver) << "\",\n"
        << "  \"requestedMicInputChannel\": " << analysis.requestedMicInputChannel << ",\n"
        << "  \"requestedLeftOutputChannel\": " << analysis.requestedLeftOutputChannel << ",\n"
        << "  \"requestedRightOutputChannel\": " << analysis.requestedRightOutputChannel << ",\n"
        << "  \"routingSelectionHonored\": " << (analysis.routingSelectionHonored ? "true" : "false") << ",\n"
        << "  \"routingNotes\": \"" << escapeJson(analysis.routingNotes) << "\",\n"
        << "  \"sampleRate\": " << analysis.sampleRate << ",\n"
        << "  \"sweepDurationSeconds\": " << analysis.sweepDurationSeconds << ",\n"
        << "  \"fadeInSeconds\": " << analysis.fadeInSeconds << ",\n"
        << "  \"fadeOutSeconds\": " << analysis.fadeOutSeconds << ",\n"
        << "  \"startFrequencyHz\": " << analysis.startFrequencyHz << ",\n"
        << "  \"endFrequencyHz\": " << analysis.endFrequencyHz << ",\n"
        << "  \"targetLengthSamples\": " << analysis.targetLengthSamples << ",\n"
        << "  \"leadInSamples\": " << analysis.leadInSamples << ",\n"
        << "  \"outputVolumeDb\": " << analysis.outputVolumeDb << ",\n"
        << "  \"playedSweepSamples\": " << analysis.playedSweepSamples << ",\n"
        << "  \"capturedSamples\": " << analysis.capturedSamples << ",\n"
        << "  \"alignmentSearchSamples\": " << analysis.alignmentSearchSamples << ",\n"
        << "  \"alignmentMethod\": \"" << escapeJson(analysis.alignmentMethod) << "\",\n"
        << "  \"windowType\": \"" << escapeJson(analysis.windowType) << "\",\n"
        << "  \"inverseFilterLengthSamples\": " << analysis.inverseFilterLengthSamples << ",\n"
        << "  \"inverseFilterPeakIndex\": " << analysis.inverseFilterPeakIndex << ",\n"
        << "  \"fftSize\": " << analysis.fftSize << ",\n"
        << "  \"displayPointCount\": " << analysis.displayPointCount << ",\n"
        << "  \"captureClippingDetected\": " << (analysis.captureClippingDetected ? "true" : "false") << ",\n"
        << "  \"captureTooQuiet\": " << (analysis.captureTooQuiet ? "true" : "false") << ",\n"
        << "  \"capturePeakDb\": " << analysis.capturePeakDb << ",\n"
        << "  \"captureRmsDb\": " << analysis.captureRmsDb << ",\n"
        << "  \"captureNoiseFloorDb\": " << analysis.captureNoiseFloorDb << ",\n";
    writeChannel(out, analysis.left, "left");
    writeChannel(out, analysis.right, "right");
    out << "  \"artifactGeneratedSweepWav\": \"" << escapeJson(artifactPath("generated_sweep_wav")) << "\",\n"
        << "  \"artifactRawCaptureWav\": \"" << escapeJson(artifactPath("raw_capture_wav")) << "\",\n"
        << "  \"artifactResultValuesTxt\": \"" << escapeJson(artifactPath("result_values_txt")) << "\",\n"
        << "  \"artifactAnalysisJson\": \"" << escapeJson(artifactPath("analysis_json")) << "\"\n"
        << "}\n";
    writeTextFile(measurementAnalysisFilePath(workspace.rootPath), out.str());
}

void saveTargetCurveBandsFile(const WorkspaceState& workspace) {
    if (workspace.rootPath.empty()) {
        return;
    }

    std::filesystem::create_directories(workspace.rootPath / "target-curve");
    std::ostringstream bandsCsv;
    bandsCsv << "enabled,colorIndex,frequencyHz,gainDb,q\n";
    for (const TargetEqBand& band : workspace.targetCurve.eqBands) {
        bandsCsv << (band.enabled ? 1 : 0) << ','
                 << band.colorIndex << ','
                 << band.frequencyHz << ','
                 << band.gainDb << ','
                 << band.q << '\n';
    }
    writeTextFile(workspace.rootPath / "target-curve" / "bands.csv", bandsCsv.str());
}

void saveTargetCurveProfiles(const WorkspaceState& workspace) {
    if (workspace.rootPath.empty()) {
        return;
    }

    std::vector<TargetCurveProfile> profiles = workspace.targetCurveProfiles;
    if (profiles.empty()) {
        TargetCurveProfile profile;
        profile.name = workspace.activeTargetCurveProfileName.empty() ? "Default" : workspace.activeTargetCurveProfileName;
        profile.comment = workspace.activeTargetCurveComment;
        profile.curve = workspace.targetCurve;
        profiles.push_back(profile);
    }

    bool foundActive = false;
    for (TargetCurveProfile& profile : profiles) {
        if (profile.name == workspace.activeTargetCurveProfileName) {
            profile.comment = workspace.activeTargetCurveComment;
            profile.curve = workspace.targetCurve;
            foundActive = true;
        }
    }
    if (!foundActive) {
        TargetCurveProfile profile;
        profile.name = workspace.activeTargetCurveProfileName.empty() ? "Default" : workspace.activeTargetCurveProfileName;
        profile.comment = workspace.activeTargetCurveComment;
        profile.curve = workspace.targetCurve;
        profiles.push_back(profile);
    }

    std::vector<std::string> desiredFileNames;
    desiredFileNames.reserve(profiles.size());
    for (const TargetCurveProfile& profile : profiles) {
        desiredFileNames.push_back(targetCurveProfileFilePath(workspace.rootPath, profile.name).filename().string());
        saveTargetCurveProfileFile(workspace.rootPath, profile);
    }

    for (const auto& entry : std::filesystem::directory_iterator(workspace.rootPath)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (!hasSuffix(filename, kTargetCurveProfileFileExtension)) {
            continue;
        }
        if (std::find(desiredFileNames.begin(), desiredFileNames.end(), filename) == desiredFileNames.end()) {
            std::filesystem::remove(entry.path());
        }
    }

    std::filesystem::remove(workspace.rootPath / "target-curve" / "bands.csv");
    std::filesystem::remove(workspace.rootPath / "target-curve");
}

}  // namespace

WorkspaceState WorkspaceRepository::load(const std::filesystem::path& path) const {
    WorkspaceState workspace;
    workspace.rootPath = path;
    measurement::syncDerivedMeasurementSettings(workspace.measurement);
    measurement::normalizeResponseSmoothingSettings(workspace.smoothing);

    const auto content = readTextFile(path / "workspace.json");
    if (content) {
        if (const auto driver = findJsonString(*content, "driver")) {
            workspace.audio.driver = *driver;
        }
        if (const auto value = findJsonNumber(*content, "micInputChannel")) {
            workspace.audio.micInputChannel = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "leftOutputChannel")) {
            workspace.audio.leftOutputChannel = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "rightOutputChannel")) {
            workspace.audio.rightOutputChannel = static_cast<int>(*value);
        }
        if (const auto value = findJsonString(*content, "microphoneCalibrationPath")) {
            workspace.audio.microphoneCalibrationPath = std::filesystem::path(*value);
        }
        if (const auto value = findJsonNumber(*content, "sampleRate")) {
            workspace.measurement.sampleRate = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "outputVolumeDb")) {
            workspace.audio.outputVolumeDb = *value;
        }
        if (const auto value = findJsonNumber(*content, "fadeInSeconds")) {
            workspace.measurement.fadeInSeconds = *value;
        }
        if (const auto value = findJsonNumber(*content, "fadeOutSeconds")) {
            workspace.measurement.fadeOutSeconds = *value;
        }
        if (const auto value = findJsonNumber(*content, "durationSeconds")) {
            workspace.measurement.durationSeconds = *value;
        }
        if (const auto value = findJsonNumber(*content, "startFrequencyHz")) {
            workspace.measurement.startFrequencyHz = *value;
        }
        if (const auto value = findJsonNumber(*content, "targetLengthSamples")) {
            workspace.measurement.targetLengthSamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "leadInSamples")) {
            workspace.measurement.leadInSamples = static_cast<int>(*value);
        }
        loadUiSettingsFromJson(*content, workspace.ui);
        if (const auto model = findJsonString(*content, "psychoacousticModel")) {
            workspace.smoothing.psychoacousticModel = *model;
        }
        if (const auto value = findJsonNumber(*content, "resolutionPercent")) {
            workspace.smoothing.resolutionPercent = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "lowFrequencyWindowCycles")) {
            workspace.smoothing.lowFrequencyWindowCycles = *value;
        }
        if (const auto value = findJsonNumber(*content, "highFrequencyWindowCycles")) {
            workspace.smoothing.highFrequencyWindowCycles = *value;
        }
        if (const auto value = findJsonNumber(*content, "highFrequencySlopeCutoffHz")) {
            workspace.smoothing.highFrequencySlopeCutoffHz = *value;
        }
        if (const auto value = findJsonNumber(*content, "lowGainDb")) {
            workspace.targetCurve.lowGainDb = *value;
        }
        if (const auto value = findJsonNumber(*content, "midFrequencyHz")) {
            workspace.targetCurve.midFrequencyHz = *value;
        }
        if (const auto value = findJsonNumber(*content, "midGainDb")) {
            workspace.targetCurve.midGainDb = *value;
        }
        if (const auto value = findJsonNumber(*content, "highGainDb")) {
            workspace.targetCurve.highGainDb = *value;
        }
        if (const auto value = findJsonBool(*content, "bypassEqBands")) {
            workspace.targetCurve.bypassEqBands = *value;
        }
        if (const auto value = findJsonString(*content, "activeTargetCurveProfileName")) {
            workspace.activeTargetCurveProfileName = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterTapCount")) {
            workspace.filters.tapCount = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "filterMaxBoostDb")) {
            workspace.filters.maxBoostDb = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterMaxCutDb")) {
            workspace.filters.maxCutDb = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterSmoothness")) {
            workspace.filters.smoothness = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterLowCorrectionHz")) {
            workspace.filters.lowCorrectionHz = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterLowTaperOctaves")) {
            workspace.filters.lowTaperOctaves = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterHighCorrectionHz")) {
            workspace.filters.highCorrectionHz = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterHighTaperOctaves")) {
            workspace.filters.highTaperOctaves = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterDisplayPointCount")) {
            workspace.filters.displayPointCount = static_cast<int>(*value);
        }
        if (const auto value = findJsonString(*content, "filterPhaseMode")) {
            workspace.filters.phaseMode = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterMixedPhaseMaxFrequencyHz")) {
            workspace.filters.mixedPhaseMaxFrequencyHz = *value;
        }
        if (const auto value = findJsonNumber(*content, "filterMixedPhaseStrength")) {
            workspace.filters.mixedPhaseStrength = *value;
        }
    }

    if (const auto uiContent = readTextFile(path / "ui.json")) {
        loadUiSettingsFromJson(*uiContent, workspace.ui);
    }

    measurement::syncDerivedMeasurementSettings(workspace.measurement);
    measurement::normalizeResponseSmoothingSettings(workspace.smoothing);
    measurement::normalizeFilterDesignSettings(workspace.filters, workspace.measurement.sampleRate);
    std::wstring calibrationError;
    loadMicrophoneCalibration(workspace.audio, calibrationError);
    loadMeasurementResultFile(workspace);
    loadMeasurementAnalysisFile(workspace);
    loadTargetCurveProfiles(workspace);
    const auto targetPlot = measurement::buildTargetCurvePlotData(workspace.smoothedResponse,
                                                                  workspace.measurement,
                                                                  workspace.targetCurve,
                                                                  std::nullopt);
    measurement::normalizeTargetCurveSettings(workspace.targetCurve, targetPlot.minFrequencyHz, targetPlot.maxFrequencyHz);
    for (TargetCurveProfile& profile : workspace.targetCurveProfiles) {
        measurement::normalizeTargetCurveSettings(profile.curve, targetPlot.minFrequencyHz, targetPlot.maxFrequencyHz);
        if (profile.name == workspace.activeTargetCurveProfileName) {
            workspace.targetCurve = profile.curve;
            workspace.activeTargetCurveComment = profile.comment;
        }
    }
    return workspace;
}

void WorkspaceRepository::save(const WorkspaceState& workspace) const {
    if (workspace.rootPath.empty()) {
        return;
    }

    std::filesystem::create_directories(workspace.rootPath / "measurement");

    std::ostringstream workspaceJson;
    const auto microphoneCalibrationPathUtf8 = workspace.audio.microphoneCalibrationPath.generic_u8string();
    workspaceJson << "{\n"
                  << "  \"audio\": {\n"
                  << "    \"driver\": \"" << escapeJson(workspace.audio.driver) << "\",\n"
                  << "    \"micInputChannel\": " << workspace.audio.micInputChannel << ",\n"
                  << "    \"leftOutputChannel\": " << workspace.audio.leftOutputChannel << ",\n"
                  << "    \"rightOutputChannel\": " << workspace.audio.rightOutputChannel << ",\n"
                  << "    \"outputVolumeDb\": " << workspace.audio.outputVolumeDb << ",\n"
                  << "    \"microphoneCalibrationPath\": \""
                  << escapeJson(std::string(microphoneCalibrationPathUtf8.begin(), microphoneCalibrationPathUtf8.end()))
                  << "\"\n"
                  << "  },\n"
                  << "  \"measurement\": {\n"
                  << "    \"sampleRate\": " << workspace.measurement.sampleRate << ",\n"
                  << "    \"fadeInSeconds\": " << workspace.measurement.fadeInSeconds << ",\n"
                  << "    \"fadeOutSeconds\": " << workspace.measurement.fadeOutSeconds << ",\n"
                  << "    \"durationSeconds\": " << workspace.measurement.durationSeconds << ",\n"
                  << "    \"startFrequencyHz\": " << workspace.measurement.startFrequencyHz << ",\n"
                  << "    \"endFrequencyHz\": " << workspace.measurement.endFrequencyHz << ",\n"
                  << "    \"targetLengthSamples\": " << workspace.measurement.targetLengthSamples << ",\n"
                  << "    \"leadInSamples\": " << workspace.measurement.leadInSamples << "\n"
                  << "  },\n"
                  << "  \"smoothing\": {\n"
                  << "    \"psychoacousticModel\": \"" << escapeJson(workspace.smoothing.psychoacousticModel) << "\",\n"
                  << "    \"resolutionPercent\": " << workspace.smoothing.resolutionPercent << ",\n"
                  << "    \"lowFrequencyWindowCycles\": " << workspace.smoothing.lowFrequencyWindowCycles << ",\n"
                  << "    \"highFrequencyWindowCycles\": " << workspace.smoothing.highFrequencyWindowCycles << ",\n"
                  << "    \"highFrequencySlopeCutoffHz\": " << workspace.smoothing.highFrequencySlopeCutoffHz << "\n"
                  << "  },\n"
                  << "  \"targetCurve\": {\n"
                  << "    \"lowGainDb\": " << workspace.targetCurve.lowGainDb << ",\n"
                  << "    \"midFrequencyHz\": " << workspace.targetCurve.midFrequencyHz << ",\n"
                  << "    \"midGainDb\": " << workspace.targetCurve.midGainDb << ",\n"
                  << "    \"highGainDb\": " << workspace.targetCurve.highGainDb << ",\n"
                  << "    \"bypassEqBands\": " << (workspace.targetCurve.bypassEqBands ? "true" : "false") << "\n"
                  << "  },\n"
                  << "  \"activeTargetCurveProfileName\": \"" << escapeJson(workspace.activeTargetCurveProfileName) << "\",\n"
                  << "  \"filters\": {\n"
                  << "    \"filterTapCount\": " << workspace.filters.tapCount << ",\n"
                  << "    \"filterMaxBoostDb\": " << workspace.filters.maxBoostDb << ",\n"
                  << "    \"filterMaxCutDb\": " << workspace.filters.maxCutDb << ",\n"
                  << "    \"filterSmoothness\": " << workspace.filters.smoothness << ",\n"
                  << "    \"filterLowCorrectionHz\": " << workspace.filters.lowCorrectionHz << ",\n"
                  << "    \"filterLowTaperOctaves\": " << workspace.filters.lowTaperOctaves << ",\n"
                  << "    \"filterHighCorrectionHz\": " << workspace.filters.highCorrectionHz << ",\n"
                  << "    \"filterHighTaperOctaves\": " << workspace.filters.highTaperOctaves << ",\n"
                  << "    \"filterDisplayPointCount\": " << workspace.filters.displayPointCount << ",\n"
                  << "    \"filterPhaseMode\": \"" << escapeJson(workspace.filters.phaseMode) << "\",\n"
                  << "    \"filterMixedPhaseMaxFrequencyHz\": " << workspace.filters.mixedPhaseMaxFrequencyHz << ",\n"
                  << "    \"filterMixedPhaseStrength\": " << workspace.filters.mixedPhaseStrength << "\n"
                  << "  },\n"
                  << "  \"ui\": {\n"
                  << "    \"measurementSectionHeight\": " << workspace.ui.measurementSectionHeight << ",\n"
                  << "    \"resultSectionHeight\": " << workspace.ui.resultSectionHeight << ",\n"
                  << "    \"processLogHeight\": " << workspace.ui.processLogHeight << ",\n"
                  << "    \"measurementGraphExtraRangeDb\": " << workspace.ui.measurementGraphExtraRangeDb << ",\n"
                  << "    \"measurementGraphVerticalOffsetDb\": " << workspace.ui.measurementGraphVerticalOffsetDb << ",\n"
                  << "    \"measurementGraphHasCustomFrequencyRange\": "
                  << (workspace.ui.measurementGraphHasCustomFrequencyRange ? "true" : "false") << ",\n"
                  << "    \"measurementGraphVisibleMinFrequencyHz\": " << workspace.ui.measurementGraphVisibleMinFrequencyHz << ",\n"
                  << "    \"measurementGraphVisibleMaxFrequencyHz\": " << workspace.ui.measurementGraphVisibleMaxFrequencyHz << ",\n"
                  << "    \"measurementPlotMode\": \"" << escapeJson(workspace.ui.measurementPlotMode) << "\",\n"
                  << "    \"measurementWaterfallChannel\": \"" << escapeJson(workspace.ui.measurementWaterfallChannel)
                  << "\",\n"
                  << "    \"measurementMetadataCollapsed\": "
                  << (workspace.ui.measurementMetadataCollapsed ? "true" : "false") << ",\n"
                  << "    \"smoothingGraphExtraRangeDb\": " << workspace.ui.smoothingGraphExtraRangeDb << ",\n"
                  << "    \"smoothingGraphVerticalOffsetDb\": " << workspace.ui.smoothingGraphVerticalOffsetDb << ",\n"
                  << "    \"smoothingGraphHasCustomFrequencyRange\": "
                  << (workspace.ui.smoothingGraphHasCustomFrequencyRange ? "true" : "false") << ",\n"
                  << "    \"smoothingGraphVisibleMinFrequencyHz\": " << workspace.ui.smoothingGraphVisibleMinFrequencyHz << ",\n"
                  << "    \"smoothingGraphVisibleMaxFrequencyHz\": " << workspace.ui.smoothingGraphVisibleMaxFrequencyHz << ",\n"
                  << "    \"targetCurveGraphExtraRangeDb\": " << workspace.ui.targetCurveGraphExtraRangeDb << ",\n"
                  << "    \"targetCurveGraphVerticalOffsetDb\": " << workspace.ui.targetCurveGraphVerticalOffsetDb << ",\n"
                  << "    \"targetCurveGraphHasCustomVisibleDbRange\": "
                  << (workspace.ui.targetCurveGraphHasCustomVisibleDbRange ? "true" : "false") << ",\n"
                  << "    \"targetCurveGraphVisibleMinDb\": " << workspace.ui.targetCurveGraphVisibleMinDb << ",\n"
                  << "    \"targetCurveGraphVisibleMaxDb\": " << workspace.ui.targetCurveGraphVisibleMaxDb << ",\n"
                  << "    \"exportSampleRatesHz\": ";
    writeJsonIntArray(workspaceJson, workspace.ui.exportSampleRatesHz);
    workspaceJson << "\n"
                  << "  }\n"
                  << "}\n";
    writeTextFile(workspace.rootPath / "workspace.json", workspaceJson.str());

    std::ostringstream uiJson;
    uiJson << "{\n"
           << "  \"measurementSectionHeight\": " << workspace.ui.measurementSectionHeight << ",\n"
           << "  \"resultSectionHeight\": " << workspace.ui.resultSectionHeight << ",\n"
           << "  \"processLogHeight\": " << workspace.ui.processLogHeight << ",\n"
           << "  \"measurementGraphExtraRangeDb\": " << workspace.ui.measurementGraphExtraRangeDb << ",\n"
           << "  \"measurementGraphVerticalOffsetDb\": " << workspace.ui.measurementGraphVerticalOffsetDb << ",\n"
           << "  \"measurementGraphHasCustomFrequencyRange\": "
           << (workspace.ui.measurementGraphHasCustomFrequencyRange ? "true" : "false") << ",\n"
           << "  \"measurementGraphVisibleMinFrequencyHz\": " << workspace.ui.measurementGraphVisibleMinFrequencyHz << ",\n"
           << "  \"measurementGraphVisibleMaxFrequencyHz\": " << workspace.ui.measurementGraphVisibleMaxFrequencyHz << ",\n"
           << "  \"measurementPlotMode\": \"" << escapeJson(workspace.ui.measurementPlotMode) << "\",\n"
           << "  \"measurementWaterfallChannel\": \"" << escapeJson(workspace.ui.measurementWaterfallChannel)
           << "\",\n"
           << "  \"measurementMetadataCollapsed\": "
           << (workspace.ui.measurementMetadataCollapsed ? "true" : "false") << ",\n"
           << "  \"smoothingGraphExtraRangeDb\": " << workspace.ui.smoothingGraphExtraRangeDb << ",\n"
           << "  \"smoothingGraphVerticalOffsetDb\": " << workspace.ui.smoothingGraphVerticalOffsetDb << ",\n"
           << "  \"smoothingGraphHasCustomFrequencyRange\": "
           << (workspace.ui.smoothingGraphHasCustomFrequencyRange ? "true" : "false") << ",\n"
           << "  \"smoothingGraphVisibleMinFrequencyHz\": " << workspace.ui.smoothingGraphVisibleMinFrequencyHz << ",\n"
           << "  \"smoothingGraphVisibleMaxFrequencyHz\": " << workspace.ui.smoothingGraphVisibleMaxFrequencyHz << ",\n"
           << "  \"targetCurveGraphExtraRangeDb\": " << workspace.ui.targetCurveGraphExtraRangeDb << ",\n"
           << "  \"targetCurveGraphVerticalOffsetDb\": " << workspace.ui.targetCurveGraphVerticalOffsetDb << ",\n"
           << "  \"targetCurveGraphHasCustomVisibleDbRange\": "
           << (workspace.ui.targetCurveGraphHasCustomVisibleDbRange ? "true" : "false") << ",\n"
           << "  \"targetCurveGraphVisibleMinDb\": " << workspace.ui.targetCurveGraphVisibleMinDb << ",\n"
           << "  \"targetCurveGraphVisibleMaxDb\": " << workspace.ui.targetCurveGraphVisibleMaxDb << ",\n"
           << "  \"exportSampleRatesHz\": ";
    writeJsonIntArray(uiJson, workspace.ui.exportSampleRatesHz);
    uiJson << "\n"
           << "}\n";
    writeTextFile(workspace.rootPath / "ui.json", uiJson.str());

    saveMeasurementResultFile(workspace);
    saveMeasurementAnalysisFile(workspace);
    saveTargetCurveProfiles(workspace);
}

}  // namespace wolfie::persistence
