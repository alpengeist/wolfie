#include "persistence/workspace_repository.h"

#include <array>
#include <fstream>
#include <optional>
#include <sstream>

#include "measurement/response_smoother.h"
#include "measurement/sweep_generator.h"

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
        if (const auto value = findJsonNumber(*content, "measurementSectionHeight")) {
            workspace.ui.measurementSectionHeight = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "resultSectionHeight")) {
            workspace.ui.resultSectionHeight = static_cast<int>(*value);
        }
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
    }

    measurement::syncDerivedMeasurementSettings(workspace.measurement);
    measurement::normalizeResponseSmoothingSettings(workspace.smoothing);
    loadMeasurementResultFile(workspace);
    return workspace;
}

void WorkspaceRepository::save(const WorkspaceState& workspace) const {
    if (workspace.rootPath.empty()) {
        return;
    }

    std::filesystem::create_directories(workspace.rootPath / "measurement");

    std::ostringstream workspaceJson;
    workspaceJson << "{\n"
                  << "  \"audio\": {\n"
                  << "    \"driver\": \"" << escapeJson(workspace.audio.driver) << "\",\n"
                  << "    \"micInputChannel\": " << workspace.audio.micInputChannel << ",\n"
                  << "    \"leftOutputChannel\": " << workspace.audio.leftOutputChannel << ",\n"
                  << "    \"rightOutputChannel\": " << workspace.audio.rightOutputChannel << ",\n"
                  << "    \"outputVolumeDb\": " << workspace.audio.outputVolumeDb << "\n"
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
                  << "  \"ui\": {\n"
                  << "    \"measurementSectionHeight\": " << workspace.ui.measurementSectionHeight << ",\n"
                  << "    \"resultSectionHeight\": " << workspace.ui.resultSectionHeight << "\n"
                  << "  }\n"
                  << "}\n";
    writeTextFile(workspace.rootPath / "workspace.json", workspaceJson.str());

    std::ostringstream uiJson;
    uiJson << "{\n"
           << "  \"measurementSectionHeight\": " << workspace.ui.measurementSectionHeight << ",\n"
           << "  \"resultSectionHeight\": " << workspace.ui.resultSectionHeight << "\n"
           << "}\n";
    writeTextFile(workspace.rootPath / "ui.json", uiJson.str());

    saveMeasurementResultFile(workspace);
}

}  // namespace wolfie::persistence
