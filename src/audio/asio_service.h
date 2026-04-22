#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace wolfie::audio {

class AsioService {
public:
    std::vector<std::wstring> enumerateDrivers() const;
    std::optional<std::wstring> openControlPanel(HWND parentWindow, std::wstring_view driverName) const;
};

}  // namespace wolfie::audio
