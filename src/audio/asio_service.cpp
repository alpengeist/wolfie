#include "audio/asio_service.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <sstream>

#include "audio/asio_sdk.h"

namespace wolfie::audio {

struct AsioControlPanelSession::Impl {
    DriverHandle handle;
};

AsioControlPanelSession::AsioControlPanelSession() = default;

AsioControlPanelSession::AsioControlPanelSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

AsioControlPanelSession::AsioControlPanelSession(AsioControlPanelSession&& other) noexcept = default;

AsioControlPanelSession& AsioControlPanelSession::operator=(AsioControlPanelSession&& other) noexcept = default;

AsioControlPanelSession::~AsioControlPanelSession() = default;

std::vector<std::wstring> AsioService::enumerateDrivers() const {
    std::vector<std::wstring> drivers;

    auto collect = [&](HKEY root, const wchar_t* subKey) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return;
        }

        DWORD index = 0;
        while (true) {
            std::array<wchar_t, 256> name{};
            DWORD nameLength = static_cast<DWORD>(name.size());
            const LONG result = RegEnumKeyExW(key, index, name.data(), &nameLength, nullptr, nullptr, nullptr, nullptr);
            if (result == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (result == ERROR_SUCCESS && nameLength > 0) {
                drivers.emplace_back(name.data(), nameLength);
            }
            ++index;
        }

        RegCloseKey(key);
    };

    collect(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO");
    collect(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\ASIO");

    std::sort(drivers.begin(), drivers.end());
    drivers.erase(std::unique(drivers.begin(), drivers.end()), drivers.end());
    return drivers;
}

AsioChannelListing AsioService::enumerateChannels(HWND parentWindow, std::wstring_view driverName) const {
    AsioChannelListing listing;
    DriverHandle handle;
    if (const auto error = openDriver(parentWindow, driverName, handle)) {
        listing.errorMessage = error;
        return listing;
    }

    long inputCount = 0;
    long outputCount = 0;
    const ASIOError channelResult = handle.driver->getChannels(&inputCount, &outputCount);
    if (channelResult != 0) {
        std::wstring message = asioDriverMessage(handle.driver);
        if (message.empty()) {
            std::wostringstream fallback;
            fallback << L"The selected ASIO driver returned error " << channelResult << L" while reading channel counts.";
            message = fallback.str();
        }
        listing.errorMessage = message;
        return listing;
    }

    auto collect = [&](bool isInput, long count, std::vector<AsioChannel>& channels) {
        for (long index = 0; index < count; ++index) {
            ASIOChannelInfo info{};
            info.channel = index;
            info.isInput = isInput ? 1 : 0;
            const ASIOError infoResult = handle.driver->getChannelInfo(&info);
            const int channelNumber = static_cast<int>(index + 1);
            std::wstring name;
            if (infoResult == 0) {
                name = asioChannelName(info, channelNumber);
            } else {
                std::wostringstream fallback;
                fallback << L"Channel " << channelNumber;
                name = fallback.str();
            }

            std::wostringstream label;
            label << channelNumber << L" - " << name;
            channels.push_back({channelNumber, label.str()});
        }
    };

    collect(true, inputCount, listing.inputs);
    collect(false, outputCount, listing.outputs);
    return listing;
}

std::optional<std::wstring> AsioService::openControlPanel(HWND parentWindow, std::wstring_view driverName) const {
    AsioControlPanelSession session;
    return startControlPanelSession(parentWindow, driverName, session);
}

std::optional<std::wstring> AsioService::startControlPanelSession(HWND parentWindow,
                                                                  std::wstring_view driverName,
                                                                  AsioControlPanelSession& session) const {
    DriverHandle handle;
    if (const auto error = openDriver(parentWindow, driverName, handle)) {
        return error;
    }

    const ASIOError result = handle.driver->controlPanel();
    if (result != 0) {
        std::wstring message = asioDriverMessage(handle.driver);
        if (message.empty()) {
            std::wostringstream fallback;
            fallback << L"The selected ASIO driver returned error " << result << L" while opening its control panel.";
            message = fallback.str();
        }
        return message;
    }

    session = AsioControlPanelSession(std::make_unique<AsioControlPanelSession::Impl>(
        AsioControlPanelSession::Impl{std::move(handle)}));
    return std::nullopt;
}

std::optional<std::wstring> AsioService::openControlPanelIsolated(std::wstring_view driverName) const {
    if (driverName.empty()) {
        return std::wstring(L"Select an installed ASIO driver first.");
    }

    std::array<wchar_t, MAX_PATH> modulePath{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (pathLength == 0 || pathLength >= modulePath.size()) {
        return std::wstring(L"Failed to locate the wolfie executable for ASIO helper launch.");
    }

    const std::wstring exePath(modulePath.data(), pathLength);
    std::wstring commandLine = L"\"";
    commandLine += exePath;
    commandLine += L"\" --asio-control-panel \"";
    for (const wchar_t ch : driverName) {
        if (ch == L'"') {
            commandLine += L'\\';
        }
        commandLine += ch;
    }
    commandLine += L"\"";

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring mutableCommandLine = commandLine;
    if (!CreateProcessW(exePath.c_str(),
                        mutableCommandLine.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        std::filesystem::path(exePath).parent_path().wstring().c_str(),
                        &startupInfo,
                        &processInfo)) {
        std::wostringstream message;
        message << L"Failed to launch the ASIO control-panel helper process (Win32 error "
                << GetLastError() << L").";
        return message.str();
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return std::nullopt;
}

}  // namespace wolfie::audio
