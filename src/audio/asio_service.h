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

struct AsioChannel {
    int number = 0;
    std::wstring name;
};

struct AsioChannelListing {
    std::vector<AsioChannel> inputs;
    std::vector<AsioChannel> outputs;
    std::optional<std::wstring> errorMessage;
};

class AsioService {
public:
    std::vector<std::wstring> enumerateDrivers() const;
    AsioChannelListing enumerateChannels(HWND parentWindow, std::wstring_view driverName) const;
    std::optional<std::wstring> openControlPanel(HWND parentWindow, std::wstring_view driverName) const;
};

}  // namespace wolfie::audio
