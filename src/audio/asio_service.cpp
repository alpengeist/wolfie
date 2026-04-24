#include "audio/asio_service.h"

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>

#include <objbase.h>

namespace wolfie::audio {

namespace {

using ASIOBool = long;
using ASIOSampleRate = double;
using ASIOError = long;
using ASIOSampleType = long;

struct ASIOSamples;
struct ASIOTimeStamp;
struct ASIOClockSource;
struct ASIOBufferInfo;
struct ASIOCallbacks;

struct ASIOChannelInfo {
    long channel = 0;
    ASIOBool isInput = 0;
    ASIOBool isActive = 0;
    long channelGroup = 0;
    ASIOSampleType type = 0;
    char name[32]{};
};

struct IASIO : public IUnknown {
    virtual ASIOBool STDMETHODCALLTYPE init(void* sysHandle) = 0;
    virtual void STDMETHODCALLTYPE getDriverName(char* name) = 0;
    virtual long STDMETHODCALLTYPE getDriverVersion() = 0;
    virtual void STDMETHODCALLTYPE getErrorMessage(char* string) = 0;
    virtual ASIOError STDMETHODCALLTYPE start() = 0;
    virtual ASIOError STDMETHODCALLTYPE stop() = 0;
    virtual ASIOError STDMETHODCALLTYPE getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError STDMETHODCALLTYPE getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError STDMETHODCALLTYPE getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError STDMETHODCALLTYPE canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError STDMETHODCALLTYPE getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError STDMETHODCALLTYPE setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError STDMETHODCALLTYPE getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError STDMETHODCALLTYPE setClockSource(long reference) = 0;
    virtual ASIOError STDMETHODCALLTYPE getSamplePosition(ASIOSamples* samplePosition, ASIOTimeStamp* timeStamp) = 0;
    virtual ASIOError STDMETHODCALLTYPE getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError STDMETHODCALLTYPE createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError STDMETHODCALLTYPE disposeBuffers() = 0;
    virtual ASIOError STDMETHODCALLTYPE controlPanel() = 0;
    virtual ASIOError STDMETHODCALLTYPE future(long selector, void* opt) = 0;
    virtual ASIOError STDMETHODCALLTYPE outputReady() = 0;
};

std::wstring trimLineBreaks(std::wstring value) {
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n')) {
        value.pop_back();
    }
    return value;
}

std::wstring formatHResultMessage(HRESULT hr) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = length > 0 && buffer != nullptr ? trimLineBreaks(std::wstring(buffer, length)) : L"Unknown COM error";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    std::wostringstream formatted;
    formatted << message << L" (0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << L')';
    return formatted.str();
}

std::optional<std::wstring> findAsioDriverClsid(std::wstring_view driverName) {
    constexpr std::array registryRoots{
        L"SOFTWARE\\ASIO\\",
        L"SOFTWARE\\WOW6432Node\\ASIO\\"
    };

    for (const auto* registryRoot : registryRoots) {
        const std::wstring keyPath = std::wstring(registryRoot) + std::wstring(driverName);
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keyPath.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
            bytes < sizeof(wchar_t)) {
            continue;
        }

        std::wstring clsid(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keyPath.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid.data(), &bytes) == ERROR_SUCCESS) {
            if (!clsid.empty() && clsid.back() == L'\0') {
                clsid.pop_back();
            }
            if (!clsid.empty()) {
                return clsid;
            }
        }
    }

    return std::nullopt;
}

std::wstring asioDriverMessage(IASIO* driver) {
    std::array<char, 512> buffer{};
    driver->getErrorMessage(buffer.data());
    if (buffer.front() == '\0') {
        return {};
    }

    const int wideLength = MultiByteToWideChar(CP_ACP, 0, buffer.data(), -1, nullptr, 0);
    if (wideLength <= 1) {
        return {};
    }

    std::wstring message(wideLength, L'\0');
    MultiByteToWideChar(CP_ACP, 0, buffer.data(), -1, message.data(), wideLength);
    if (!message.empty() && message.back() == L'\0') {
        message.pop_back();
    }
    return message;
}

std::wstring asioChannelName(const ASIOChannelInfo& info, int channelNumber) {
    if (info.name[0] == '\0') {
        std::wostringstream fallback;
        fallback << L"Channel " << channelNumber;
        return fallback.str();
    }

    std::array<char, 33> nameBuffer{};
    std::copy_n(info.name, 32, nameBuffer.data());

    const int wideLength = MultiByteToWideChar(CP_ACP, 0, nameBuffer.data(), -1, nullptr, 0);
    if (wideLength <= 1) {
        std::wostringstream fallback;
        fallback << L"Channel " << channelNumber;
        return fallback.str();
    }

    std::wstring name(wideLength, L'\0');
    MultiByteToWideChar(CP_ACP, 0, nameBuffer.data(), -1, name.data(), wideLength);
    if (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }
    return name;
}

struct DriverHandle {
    IASIO* driver = nullptr;
    bool shouldUninitialize = false;

    DriverHandle() = default;
    DriverHandle(const DriverHandle&) = delete;
    DriverHandle& operator=(const DriverHandle&) = delete;

    ~DriverHandle() {
        if (driver != nullptr) {
            driver->Release();
        }
        if (shouldUninitialize) {
            CoUninitialize();
        }
    }
};

std::optional<std::wstring> openDriver(HWND parentWindow, std::wstring_view driverName, DriverHandle& handle) {
    if (driverName.empty()) {
        return std::wstring(L"Select an installed ASIO driver first.");
    }

    const auto clsidText = findAsioDriverClsid(driverName);
    if (!clsidText) {
        return std::wstring(L"Could not locate the selected ASIO driver in the registry.");
    }

    CLSID clsid{};
    const HRESULT clsidHr = CLSIDFromString(const_cast<wchar_t*>(clsidText->c_str()), &clsid);
    if (FAILED(clsidHr)) {
        return std::wstring(L"The selected ASIO driver has an invalid CLSID: ") + formatHResultMessage(clsidHr);
    }

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    handle.shouldUninitialize = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
        return std::wstring(L"COM initialization failed while opening the ASIO driver: ") + formatHResultMessage(initHr);
    }

    void* rawDriver = nullptr;
    const HRESULT createHr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &rawDriver);
    if (FAILED(createHr) || rawDriver == nullptr) {
        return std::wstring(L"Failed to create the selected ASIO driver: ") + formatHResultMessage(createHr);
    }

    handle.driver = static_cast<IASIO*>(rawDriver);
    const ASIOBool initialized = handle.driver->init(parentWindow != nullptr ? parentWindow : GetDesktopWindow());
    if (!initialized) {
        std::wstring message = asioDriverMessage(handle.driver);
        if (message.empty()) {
            message = L"The selected ASIO driver rejected initialization.";
        }
        return message;
    }

    return std::nullopt;
}

}  // namespace

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
