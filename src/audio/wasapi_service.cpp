#include "audio/wasapi_service.h"

#include <algorithm>
#include <string>

#include <audioclient.h>
#include <propidl.h>
#include <mmdeviceapi.h>

#include "core/text_utils.h"

namespace wolfie::audio {

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ~ComPtr() {
        reset();
    }

    [[nodiscard]] T* get() const {
        return ptr_;
    }

    [[nodiscard]] T** put() {
        reset();
        return &ptr_;
    }

    [[nodiscard]] T* operator->() const {
        return ptr_;
    }

    void reset(T* value = nullptr) {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
        ptr_ = value;
    }

private:
    T* ptr_ = nullptr;
};

class CoInitScope {
public:
    explicit CoInitScope(DWORD flags) {
        result_ = CoInitializeEx(nullptr, flags);
        shouldUninitialize_ = SUCCEEDED(result_);
    }

    ~CoInitScope() {
        if (shouldUninitialize_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const {
        return result_;
    }

private:
    HRESULT result_ = E_FAIL;
    bool shouldUninitialize_ = false;
};

constexpr PROPERTYKEY kDeviceFriendlyNamePropertyKey{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};

std::wstring deviceFriendlyName(IMMDevice* device) {
    if (device == nullptr) {
        return L"Audio device";
    }

    ComPtr<IPropertyStore> propertyStore;
    if (FAILED(device->OpenPropertyStore(STGM_READ, propertyStore.put()))) {
        return L"Audio device";
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = L"Audio device";
    if (SUCCEEDED(propertyStore->GetValue(kDeviceFriendlyNamePropertyKey, &value)) &&
        value.vt == VT_LPWSTR &&
        value.pwszVal != nullptr &&
        value.pwszVal[0] != L'\0') {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

int deviceChannelCount(IMMDevice* device) {
    if (device == nullptr) {
        return 0;
    }

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient),
                                CLSCTX_INPROC_SERVER,
                                nullptr,
                                reinterpret_cast<void**>(client.put())))) {
        return 0;
    }

    WAVEFORMATEX* rawFormat = nullptr;
    if (FAILED(client->GetMixFormat(&rawFormat)) || rawFormat == nullptr) {
        return 0;
    }

    const int channelCount = static_cast<int>(rawFormat->nChannels);
    CoTaskMemFree(rawFormat);
    return channelCount;
}

std::vector<WasapiDevice> enumerateDevices(EDataFlow flow) {
    std::vector<WasapiDevice> devices;

    CoInitScope coInit(COINIT_MULTITHREADED);
    if (FAILED(coInit.result()) && coInit.result() != RPC_E_CHANGED_MODE) {
        return devices;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.put())))) {
        return devices;
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()))) {
        return devices;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return devices;
    }

    devices.reserve(static_cast<size_t>(count));
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, device.put()))) {
            continue;
        }

        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || rawId == nullptr) {
            continue;
        }

        const std::wstring id = rawId;
        CoTaskMemFree(rawId);
        devices.push_back({toUtf8(id), deviceFriendlyName(device.get()), deviceChannelCount(device.get())});
    }

    std::sort(devices.begin(), devices.end(), [](const WasapiDevice& left, const WasapiDevice& right) {
        return left.name < right.name;
    });
    return devices;
}

}  // namespace

std::vector<WasapiDevice> WasapiService::enumerateInputDevices() const {
    return enumerateDevices(eCapture);
}

std::vector<WasapiDevice> WasapiService::enumerateOutputDevices() const {
    return enumerateDevices(eRender);
}

}  // namespace wolfie::audio
