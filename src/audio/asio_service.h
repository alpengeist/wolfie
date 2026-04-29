#pragma once

#include <memory>
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

class AsioControlPanelSession {
public:
    AsioControlPanelSession();
    AsioControlPanelSession(const AsioControlPanelSession&) = delete;
    AsioControlPanelSession& operator=(const AsioControlPanelSession&) = delete;
    AsioControlPanelSession(AsioControlPanelSession&& other) noexcept;
    AsioControlPanelSession& operator=(AsioControlPanelSession&& other) noexcept;
    ~AsioControlPanelSession();

private:
    struct Impl;

    explicit AsioControlPanelSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class AsioService;
};

class AsioService {
public:
    std::vector<std::wstring> enumerateDrivers() const;
    AsioChannelListing enumerateChannels(HWND parentWindow, std::wstring_view driverName) const;
    std::optional<std::wstring> openControlPanel(HWND parentWindow, std::wstring_view driverName) const;
    std::optional<std::wstring> startControlPanelSession(HWND parentWindow,
                                                         std::wstring_view driverName,
                                                         AsioControlPanelSession& session) const;
    std::optional<std::wstring> openControlPanelIsolated(std::wstring_view driverName) const;
};

}  // namespace wolfie::audio
