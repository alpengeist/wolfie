#include "persistence/workspace_repository.h"

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
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

void loadMeasurementResultFile(WorkspaceState& workspace) {
    workspace.result = {};
    if (workspace.rootPath.empty()) {
        return;
    }

    const auto response = readTextFile(workspace.rootPath / "measurement" / "response.csv");
    if (!response) {
        return;
    }

    std::istringstream in(*response);
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("frequency")) {
            continue;
        }

        std::istringstream row(line);
        std::string cell;
        std::array<double, 3> values{};
        int index = 0;
        while (std::getline(row, cell, ',') && index < 3) {
            values[index++] = std::stod(cell);
        }
        if (index == 3) {
            workspace.result.frequencyAxisHz.push_back(values[0]);
            workspace.result.leftChannelDb.push_back(values[1]);
            workspace.result.rightChannelDb.push_back(values[2]);
        }
    }
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
    std::ostringstream responseCsv;
    responseCsv << "frequency,left,right\n";
    for (size_t i = 0; i < workspace.result.frequencyAxisHz.size(); ++i) {
        responseCsv << workspace.result.frequencyAxisHz[i] << ','
                    << workspace.result.leftChannelDb[i] << ','
                    << workspace.result.rightChannelDb[i] << '\n';
    }
    writeTextFile(workspace.rootPath / "measurement" / "response.csv", responseCsv.str());
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
