#pragma once

#include <filesystem>

#include "core/models.h"

namespace wolfie::persistence {

class FilterStoreRepository {
public:
    void load(const std::filesystem::path& rootPath,
              StoredFilterDesign& minimumFilter,
              StoredFilterDesign& mixedFilter) const;
    void save(const std::filesystem::path& rootPath,
              const StoredFilterDesign& minimumFilter,
              const StoredFilterDesign& mixedFilter) const;
};

}  // namespace wolfie::persistence
