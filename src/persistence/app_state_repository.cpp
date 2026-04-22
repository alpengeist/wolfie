#include "persistence/app_state_repository.h"

#include <fstream>
#include <optional>
#include <sstream>

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

std::vector<std::string> findJsonStringArray(const std::string& source, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const size_t keyPos = source.find(pattern);
    if (keyPos == std::string::npos) {
        return {};
    }

    const size_t arrayStart = source.find('[', keyPos + pattern.size());
    const size_t arrayEnd = source.find(']', arrayStart + 1);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos) {
        return {};
    }

    std::vector<std::string> values;
    size_t cursor = arrayStart + 1;
    while (cursor < arrayEnd) {
        const size_t quoteStart = source.find('"', cursor);
        if (quoteStart == std::string::npos || quoteStart >= arrayEnd) {
            break;
        }
        std::string value;
        for (size_t i = quoteStart + 1; i < arrayEnd; ++i) {
            const char ch = source[i];
            if (ch == '\\' && i + 1 < arrayEnd) {
                value.push_back(source[i + 1]);
                ++i;
                continue;
            }
            if (ch == '"') {
                values.push_back(value);
                cursor = i + 1;
                break;
            }
            value.push_back(ch);
        }
        ++cursor;
    }
    return values;
}

}  // namespace

AppStateRepository::AppStateRepository(std::filesystem::path path) : path_(std::move(path)) {}

AppState AppStateRepository::load() const {
    AppState state;
    const auto content = readTextFile(path_);
    if (!content) {
        return state;
    }

    if (const auto lastWorkspace = findJsonString(*content, "lastWorkspace")) {
        state.lastWorkspace = std::filesystem::path(*lastWorkspace);
    }

    for (const auto& recent : findJsonStringArray(*content, "recentWorkspaces")) {
        state.recentWorkspaces.emplace_back(recent);
    }

    return state;
}

void AppStateRepository::save(const AppState& state) const {
    std::ostringstream out;
    out << "{\n"
        << "  \"lastWorkspace\": \"" << escapeJson(state.lastWorkspace.generic_string()) << "\",\n"
        << "  \"recentWorkspaces\": [";
    for (size_t i = 0; i < state.recentWorkspaces.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "\"" << escapeJson(state.recentWorkspaces[i].generic_string()) << "\"";
    }
    out << "]\n}\n";
    writeTextFile(path_, out.str());
}

}  // namespace wolfie::persistence
