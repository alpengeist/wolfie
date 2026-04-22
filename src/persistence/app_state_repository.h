#pragma once

#include <filesystem>

#include "core/models.h"

namespace wolfie::persistence {

class AppStateRepository {
public:
    explicit AppStateRepository(std::filesystem::path path);

    AppState load() const;
    void save(const AppState& state) const;

private:
    std::filesystem::path path_;
};

}  // namespace wolfie::persistence
