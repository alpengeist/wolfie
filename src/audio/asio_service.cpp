#include "audio/asio_service.h"

#include <algorithm>
#include <array>
#include <sstream>

#include "audio/asio_sdk.h"

namespace wolfie::audio {

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
    DriverHandle handle;
    if (const auto error = openDriver(parentWindow, driverName, handle)) {
        return error;
    }

    const ASIOError result = handle.driver->controlPanel();
    std::wstring message;
    if (result != 0) {
        message = asioDriverMessage(handle.driver);
        if (message.empty()) {
            std::wostringstream fallback;
            fallback << L"The selected ASIO driver returned error " << result << L" while opening its control panel.";
            message = fallback.str();
        }
    }

    if (!message.empty()) {
        return message;
    }
    return std::nullopt;
}

}  // namespace wolfie::audio
