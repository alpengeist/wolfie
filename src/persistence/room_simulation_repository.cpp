#include "persistence/room_simulation_repository.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "measurement/room_simulator.h"

namespace wolfie::persistence {

namespace {

constexpr char kRoomSimulationFileMagic[] = "wolfie-room-sim-v1";

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

std::filesystem::path roomSimDirectoryPath(const std::filesystem::path& workspaceRoot) {
    return workspaceRoot / "roomsim";
}

std::filesystem::path roomSimFilePath(const std::filesystem::path& workspaceRoot, std::string_view name) {
    return roomSimDirectoryPath(workspaceRoot) / std::filesystem::path(std::string(name));
}

void assignIfMatchingKey(std::string_view key, std::string_view value, RoomSimulationSettings& settings) {
    if (key == "stereoSkewMs") {
        settings.stereoSkewMs = std::stod(std::string(value));
    } else if (key == "spectralTiltDbPerOctave") {
        settings.spectralTiltDbPerOctave = std::stod(std::string(value));
    } else if (key == "lowShelfGainDb") {
        settings.lowShelfGainDb = std::stod(std::string(value));
    } else if (key == "lowShelfCornerHz") {
        settings.lowShelfCornerHz = std::stod(std::string(value));
    } else if (key == "modalPeakFrequencyHz") {
        settings.modalPeakFrequencyHz = std::stod(std::string(value));
    } else if (key == "modalPeakGainDb") {
        settings.modalPeakGainDb = std::stod(std::string(value));
    } else if (key == "modalPeakQ") {
        settings.modalPeakQ = std::stod(std::string(value));
    } else if (key == "modalNullFrequencyHz") {
        settings.modalNullFrequencyHz = std::stod(std::string(value));
    } else if (key == "modalNullDepthDb") {
        settings.modalNullDepthDb = std::stod(std::string(value));
    } else if (key == "modalNullQ") {
        settings.modalNullQ = std::stod(std::string(value));
    } else if (key == "earlyReflectionCount") {
        settings.earlyReflectionCount = std::stoi(std::string(value));
    } else if (key == "earlyReflectionStartMs") {
        settings.earlyReflectionStartMs = std::stod(std::string(value));
    } else if (key == "earlyReflectionSpacingMs") {
        settings.earlyReflectionSpacingMs = std::stod(std::string(value));
    } else if (key == "earlyReflectionDecayDbPerTap") {
        settings.earlyReflectionDecayDbPerTap = std::stod(std::string(value));
    } else if (key == "lateDecayRt60Ms") {
        settings.lateDecayRt60Ms = std::stod(std::string(value));
    } else if (key == "lateDecayStartDb") {
        settings.lateDecayStartDb = std::stod(std::string(value));
    } else if (key == "lateDensityPerSecond") {
        settings.lateDensityPerSecond = std::stod(std::string(value));
    } else if (key == "noiseFloorDb") {
        settings.noiseFloorDb = std::stod(std::string(value));
    } else if (key == "seed") {
        settings.seed = std::stoi(std::string(value));
    }
}

std::optional<RoomSimulationDefinition> loadSimulationFile(const std::filesystem::path& path) {
    const auto content = readTextFile(path);
    if (!content) {
        return std::nullopt;
    }

    std::istringstream in(*content);
    std::string line;
    if (!std::getline(in, line) || trimAscii(line) != kRoomSimulationFileMagic) {
        return std::nullopt;
    }

    RoomSimulationDefinition simulation;
    simulation.name = path.filename().string();
    while (std::getline(in, line)) {
        const std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const size_t equalsPos = trimmed.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }
        assignIfMatchingKey(trimmed.substr(0, equalsPos), trimmed.substr(equalsPos + 1), simulation.settings);
    }
    measurement::normalizeRoomSimulationSettings(simulation.settings);
    return simulation;
}

}  // namespace

std::vector<RoomSimulationDefinition> RoomSimulationRepository::loadAll(const std::filesystem::path& workspaceRoot) const {
    std::vector<RoomSimulationDefinition> simulations;
    const std::filesystem::path directory = roomSimDirectoryPath(workspaceRoot);
    if (!std::filesystem::exists(directory)) {
        return simulations;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (const auto simulation = loadSimulationFile(entry.path())) {
            simulations.push_back(*simulation);
        }
    }

    std::sort(simulations.begin(), simulations.end(), [](const RoomSimulationDefinition& left, const RoomSimulationDefinition& right) {
        return left.name < right.name;
    });
    return simulations;
}

void RoomSimulationRepository::save(const std::filesystem::path& workspaceRoot,
                                    const RoomSimulationDefinition& simulation) const {
    if (workspaceRoot.empty() || simulation.name.empty() || !isValidSimulationName(simulation.name)) {
        return;
    }

    std::filesystem::create_directories(roomSimDirectoryPath(workspaceRoot));

    RoomSimulationSettings normalized = simulation.settings;
    measurement::normalizeRoomSimulationSettings(normalized);

    std::ostringstream out;
    out << kRoomSimulationFileMagic << '\n'
        << "stereoSkewMs=" << normalized.stereoSkewMs << '\n'
        << "spectralTiltDbPerOctave=" << normalized.spectralTiltDbPerOctave << '\n'
        << "lowShelfGainDb=" << normalized.lowShelfGainDb << '\n'
        << "lowShelfCornerHz=" << normalized.lowShelfCornerHz << '\n'
        << "modalPeakFrequencyHz=" << normalized.modalPeakFrequencyHz << '\n'
        << "modalPeakGainDb=" << normalized.modalPeakGainDb << '\n'
        << "modalPeakQ=" << normalized.modalPeakQ << '\n'
        << "modalNullFrequencyHz=" << normalized.modalNullFrequencyHz << '\n'
        << "modalNullDepthDb=" << normalized.modalNullDepthDb << '\n'
        << "modalNullQ=" << normalized.modalNullQ << '\n'
        << "earlyReflectionCount=" << normalized.earlyReflectionCount << '\n'
        << "earlyReflectionStartMs=" << normalized.earlyReflectionStartMs << '\n'
        << "earlyReflectionSpacingMs=" << normalized.earlyReflectionSpacingMs << '\n'
        << "earlyReflectionDecayDbPerTap=" << normalized.earlyReflectionDecayDbPerTap << '\n'
        << "lateDecayRt60Ms=" << normalized.lateDecayRt60Ms << '\n'
        << "lateDecayStartDb=" << normalized.lateDecayStartDb << '\n'
        << "lateDensityPerSecond=" << normalized.lateDensityPerSecond << '\n'
        << "noiseFloorDb=" << normalized.noiseFloorDb << '\n'
        << "seed=" << normalized.seed << '\n';
    writeTextFile(roomSimFilePath(workspaceRoot, simulation.name), out.str());
}

bool RoomSimulationRepository::isValidSimulationName(std::string_view name) {
    if (name.empty()) {
        return false;
    }

    if (name.back() == ' ' || name.back() == '.') {
        return false;
    }

    for (const char ch : name) {
        if (ch < 32 || ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            return false;
        }
    }
    return true;
}

}  // namespace wolfie::persistence
