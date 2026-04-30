#pragma once

#include <filesystem>
#include <vector>

#include "core/models.h"

namespace wolfie::persistence {

class RoomSimulationRepository {
public:
    std::vector<RoomSimulationDefinition> loadAll(const std::filesystem::path& workspaceRoot) const;
    void save(const std::filesystem::path& workspaceRoot, const RoomSimulationDefinition& simulation) const;

private:
    static bool isValidSimulationName(std::string_view name);
};

}  // namespace wolfie::persistence
