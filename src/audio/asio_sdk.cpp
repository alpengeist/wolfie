#include "audio/asio_sdk.h"

#include <algorithm>
#include <array>
#include <utility>

#include <objbase.h>

namespace wolfie::audio {

namespace {

std::wstring trimLineBreaks(std::wstring value) {
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n')) {
        value.pop_back();
    }
    return value;
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

}  // namespace

DriverHandle::DriverHandle(DriverHandle&& other) noexcept
    : driver(other.driver),
      shouldUninitialize(other.shouldUninitialize) {
    other.driver = nullptr;
    other.shouldUninitialize = false;
}

DriverHandle& DriverHandle::operator=(DriverHandle&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (driver != nullptr) {
        driver->Release();
    }
    if (shouldUninitialize) {
        CoUninitialize();
    }

    driver = other.driver;
    shouldUninitialize = other.shouldUninitialize;
    other.driver = nullptr;
    other.shouldUninitialize = false;
    return *this;
}

DriverHandle::~DriverHandle() {
    if (driver != nullptr) {
        driver->Release();
    }
    if (shouldUninitialize) {
        CoUninitialize();
    }
}

std::wstring formatHResultMessage(HRESULT hr) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = length > 0 && buffer != nullptr ? trimLineBreaks(std::wstring(buffer, length)) : L"Unknown COM error";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    std::wstring formatted = message;
    formatted += L" (0x";
    wchar_t hexBuffer[16]{};
    _snwprintf_s(hexBuffer, _TRUNCATE, L"%lX", static_cast<unsigned long>(hr));
    formatted += hexBuffer;
    formatted += L')';
    return formatted;
}

std::wstring asioDriverMessage(IASIO* driver) {
    if (driver == nullptr) {
        return {};
    }

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
        return L"Channel " + std::to_wstring(channelNumber);
    }

    std::array<char, 33> nameBuffer{};
    std::copy_n(info.name, 32, nameBuffer.data());

    const int wideLength = MultiByteToWideChar(CP_ACP, 0, nameBuffer.data(), -1, nullptr, 0);
    if (wideLength <= 1) {
        return L"Channel " + std::to_wstring(channelNumber);
    }

    std::wstring name(wideLength, L'\0');
    MultiByteToWideChar(CP_ACP, 0, nameBuffer.data(), -1, name.data(), wideLength);
    if (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }
    return name;
}

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

bool isSupportedAsioSampleType(ASIOSampleType sampleType) {
    switch (sampleType) {
    case kAsioSampleInt16Lsb:
    case kAsioSampleInt24Lsb:
    case kAsioSampleInt32Lsb:
    case kAsioSampleFloat32Lsb:
    case kAsioSampleFloat64Lsb:
    case kAsioSampleInt32Lsb16:
    case kAsioSampleInt32Lsb18:
    case kAsioSampleInt32Lsb20:
    case kAsioSampleInt32Lsb24:
        return true;
    default:
        return false;
    }
}

std::wstring asioSampleTypeName(ASIOSampleType sampleType) {
    switch (sampleType) {
    case kAsioSampleInt16Lsb:
        return L"Int16 LSB";
    case kAsioSampleInt24Lsb:
        return L"Int24 LSB";
    case kAsioSampleInt32Lsb:
        return L"Int32 LSB";
    case kAsioSampleFloat32Lsb:
        return L"Float32 LSB";
    case kAsioSampleFloat64Lsb:
        return L"Float64 LSB";
    case kAsioSampleInt32Lsb16:
        return L"Int32 LSB 16-bit";
    case kAsioSampleInt32Lsb18:
        return L"Int32 LSB 18-bit";
    case kAsioSampleInt32Lsb20:
        return L"Int32 LSB 20-bit";
    case kAsioSampleInt32Lsb24:
        return L"Int32 LSB 24-bit";
    default:
        return L"Unsupported";
    }
}

}  // namespace wolfie::audio
