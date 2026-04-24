#include "persistence/workspace_repository.h"

#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "persistence/microphone_calibration_repository.h"
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
    if (const auto value = findJsonNumber(content, "smoothingGraphExtraRangeDb")) {
        ui.smoothingGraphExtraRangeDb = *value;
    }
    if (const auto value = findJsonNumber(content, "smoothingGraphVerticalOffsetDb")) {
        ui.smoothingGraphVerticalOffsetDb = *value;
    }
    if (const auto value = findJsonNumber(content, "targetCurveGraphExtraRangeDb")) {
        ui.targetCurveGraphExtraRangeDb = *value;
    }
    if (const auto value = findJsonNumber(content, "targetCurveGraphVerticalOffsetDb")) {
        ui.targetCurveGraphVerticalOffsetDb = *value;
    }
}

constexpr char kMeasurementResultFileMagic[] = "wolfie-result-values-v1";

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
        if (const auto value = findJsonNumber(*content, "loopbackLatencySamples")) {
            workspace.measurement.loopbackLatencySamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "loopbackLatencySampleRate")) {
            workspace.measurement.loopbackLatencySampleRate = static_cast<int>(*value);
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
    }

    if (const auto uiContent = readTextFile(path / "ui.json")) {
        loadUiSettingsFromJson(*uiContent, workspace.ui);
    }

    measurement::syncDerivedMeasurementSettings(workspace.measurement);
    measurement::normalizeResponseSmoothingSettings(workspace.smoothing);
    std::wstring calibrationError;
    loadMicrophoneCalibration(workspace.audio, calibrationError);
    loadMeasurementResultFile(workspace);
    loadTargetCurveBandsFile(workspace);
    const auto targetPlot = measurement::buildTargetCurvePlotData(workspace.smoothedResponse,
                                                                  workspace.measurement,
                                                                  workspace.targetCurve,
                                                                  std::nullopt);
    measurement::normalizeTargetCurveSettings(workspace.targetCurve, targetPlot.minFrequencyHz, targetPlot.maxFrequencyHz);
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
                  << "    \"leadInSamples\": " << workspace.measurement.leadInSamples << ",\n"
                  << "    \"loopbackLatencySamples\": " << workspace.measurement.loopbackLatencySamples << ",\n"
                  << "    \"loopbackLatencySampleRate\": " << workspace.measurement.loopbackLatencySampleRate << "\n"
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
                  << "  \"ui\": {\n"
                  << "    \"measurementSectionHeight\": " << workspace.ui.measurementSectionHeight << ",\n"
                  << "    \"resultSectionHeight\": " << workspace.ui.resultSectionHeight << ",\n"
                  << "    \"processLogHeight\": " << workspace.ui.processLogHeight << ",\n"
                  << "    \"measurementGraphExtraRangeDb\": " << workspace.ui.measurementGraphExtraRangeDb << ",\n"
                  << "    \"measurementGraphVerticalOffsetDb\": " << workspace.ui.measurementGraphVerticalOffsetDb << ",\n"
                  << "    \"smoothingGraphExtraRangeDb\": " << workspace.ui.smoothingGraphExtraRangeDb << ",\n"
                  << "    \"smoothingGraphVerticalOffsetDb\": " << workspace.ui.smoothingGraphVerticalOffsetDb << ",\n"
                  << "    \"targetCurveGraphExtraRangeDb\": " << workspace.ui.targetCurveGraphExtraRangeDb << ",\n"
                  << "    \"targetCurveGraphVerticalOffsetDb\": " << workspace.ui.targetCurveGraphVerticalOffsetDb << "\n"
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
           << "  \"smoothingGraphExtraRangeDb\": " << workspace.ui.smoothingGraphExtraRangeDb << ",\n"
           << "  \"smoothingGraphVerticalOffsetDb\": " << workspace.ui.smoothingGraphVerticalOffsetDb << ",\n"
           << "  \"targetCurveGraphExtraRangeDb\": " << workspace.ui.targetCurveGraphExtraRangeDb << ",\n"
           << "  \"targetCurveGraphVerticalOffsetDb\": " << workspace.ui.targetCurveGraphVerticalOffsetDb << "\n"
           << "}\n";
    writeTextFile(workspace.rootPath / "ui.json", uiJson.str());

    saveMeasurementResultFile(workspace);
    saveTargetCurveBandsFile(workspace);
}

}  // namespace wolfie::persistence
