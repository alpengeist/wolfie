#pragma once

#include <filesystem>

#include "core/models.h"

namespace wolfie::persistence {

class WorkspaceRepository {
public:
    WorkspaceState load(const std::filesystem::path& path) const;
    void save(const WorkspaceState& workspace) const;
};

}  // namespace wolfie::persistence
