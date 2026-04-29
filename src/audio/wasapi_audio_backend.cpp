#include "audio/wasapi_audio_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include "core/text_utils.h"
#include "measurement/response_analyzer.h"
#include "measurement/sweep_generator.h"

namespace wolfie::audio {

namespace {

constexpr REFERENCE_TIME kSharedBufferDurationHns = 2'000'000;
constexpr DWORD kWorkerSleepMs = 5;
constexpr PROPERTYKEY kDeviceFriendlyNamePropertyKey{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};
constexpr GUID kSubFormatPcm{
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};
constexpr GUID kSubFormatIeeeFloat{
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};

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

struct WaveFormatHolder {
    std::vector<BYTE> bytes;

    [[nodiscard]] WAVEFORMATEX* get() {
        return bytes.empty() ? nullptr : reinterpret_cast<WAVEFORMATEX*>(bytes.data());
    }

    [[nodiscard]] const WAVEFORMATEX* get() const {
        return bytes.empty() ? nullptr : reinterpret_cast<const WAVEFORMATEX*>(bytes.data());
    }
};

struct AudioDataFormat {
    WORD channelCount = 0;
    DWORD sampleRate = 0;
    WORD bytesPerChannel = 0;
    WORD validBitsPerSample = 0;
    bool isFloat = false;
};

std::wstring trimLineBreaks(std::wstring value) {
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n')) {
        value.pop_back();
    }
    return value;
}

std::wstring knownHResultMessage(HRESULT hr) {
    switch (hr) {
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return L"The audio engine does not support the requested format";
    case AUDCLNT_E_DEVICE_IN_USE:
        return L"The audio device is already in use";
    case AUDCLNT_E_DEVICE_INVALIDATED:
        return L"The audio device is no longer available";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return L"The Windows audio service is not running";
    case AUDCLNT_E_RESOURCES_INVALIDATED:
        return L"The audio stream resources were invalidated";
    case AUDCLNT_E_WRONG_ENDPOINT_TYPE:
        return L"The selected endpoint type is not valid for this stream";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        return L"The Windows audio endpoint could not be created";
    case AUDCLNT_E_NOT_STOPPED:
        return L"The audio client must be stopped before it can be reinitialized";
    case AUDCLNT_E_EVENTHANDLE_NOT_SET:
        return L"The audio client requires an event handle before start";
    default:
        return {};
    }
}

std::wstring formatHResultMessageLocal(HRESULT hr) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = length > 0 && buffer != nullptr ? trimLineBreaks(std::wstring(buffer, length)) : knownHResultMessage(hr);
    if (message.empty()) {
        message = L"Unknown COM error";
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    wchar_t hexBuffer[16]{};
    _snwprintf_s(hexBuffer, _TRUNCATE, L"%lX", static_cast<unsigned long>(hr));
    return message + L" (0x" + hexBuffer + L')';
}

std::wstring formatWasapiError(std::wstring_view operation, HRESULT hr) {
    std::wstring message = operation.empty() ? L"WASAPI operation failed" : std::wstring(operation);
    message += L": ";
    message += formatHResultMessageLocal(hr);
    return message;
}

WaveFormatHolder copyWaveFormat(const WAVEFORMATEX* source) {
    WaveFormatHolder holder;
    if (source == nullptr) {
        return holder;
    }

    const size_t size = sizeof(WAVEFORMATEX) + source->cbSize;
    holder.bytes.resize(size);
    std::memcpy(holder.bytes.data(), source, size);
    return holder;
}

void setWaveFormatSampleRate(WAVEFORMATEX* format, DWORD sampleRate) {
    if (format == nullptr || sampleRate == 0) {
        return;
    }

    format->nSamplesPerSec = sampleRate;
    format->nAvgBytesPerSec = format->nBlockAlign * format->nSamplesPerSec;
}

std::wstring formatWaveFormatSummary(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return L"unknown format";
    }

    std::wstring sampleType = L"unknown";
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        sampleType = L"PCM";
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        sampleType = L"float";
    } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               format->cbSize >= static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (extensible->SubFormat == kSubFormatPcm) {
            sampleType = L"PCM";
        } else if (extensible->SubFormat == kSubFormatIeeeFloat) {
            sampleType = L"float";
        }
    }

    return std::to_wstring(format->nSamplesPerSec) + L" Hz, " +
           std::to_wstring(format->nChannels) + L" ch, " +
           std::to_wstring(format->wBitsPerSample) + L"-bit " + sampleType;
}

bool validateChannelSelection(int selectedChannel,
                              WORD availableChannels,
                              std::wstring_view role,
                              std::wstring_view deviceName,
                              std::wstring& errorMessage) {
    if (selectedChannel < 1 || selectedChannel > static_cast<int>(availableChannels)) {
        errorMessage = L"The selected Windows ";
        errorMessage += std::wstring(role);
        errorMessage += L" channel ";
        errorMessage += std::to_wstring(selectedChannel);
        errorMessage += L" is not available on ";
        if (deviceName.empty()) {
            errorMessage += L"the selected device";
        } else {
            errorMessage += L"\"";
            errorMessage += std::wstring(deviceName);
            errorMessage += L"\"";
        }
        errorMessage += L". Available channels: ";
        errorMessage += std::to_wstring(availableChannels);
        errorMessage += L".";
        return false;
    }
    return true;
}

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

bool resolveDevice(IMMDeviceEnumerator* enumerator,
                   EDataFlow flow,
                   const std::string& deviceIdUtf8,
                   ComPtr<IMMDevice>& device,
                   std::wstring& deviceName,
                   std::wstring& errorMessage) {
    if (enumerator == nullptr) {
        errorMessage = L"WASAPI device enumeration is unavailable.";
        return false;
    }

    HRESULT result = E_FAIL;
    if (deviceIdUtf8.empty()) {
        result = enumerator->GetDefaultAudioEndpoint(flow, eConsole, device.put());
    } else {
        const std::wstring deviceId = toWide(deviceIdUtf8);
        result = enumerator->GetDevice(deviceId.c_str(), device.put());
    }

    if (FAILED(result)) {
        errorMessage = formatWasapiError(flow == eCapture ? L"Could not open the selected input device"
                                                          : L"Could not open the selected output device",
                                         result);
        return false;
    }

    deviceName = deviceFriendlyName(device.get());
    return true;
}

bool activateAudioClient(IMMDevice* device, ComPtr<IAudioClient>& client, std::wstring& errorMessage) {
    if (device == nullptr) {
        errorMessage = L"Missing WASAPI device.";
        return false;
    }

    const HRESULT result =
        device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(client.put()));
    if (FAILED(result)) {
        errorMessage = formatWasapiError(L"Could not activate the selected Windows audio device", result);
        return false;
    }
    return true;
}

bool queryMixFormat(IAudioClient* client, WaveFormatHolder& formatHolder, std::wstring& errorMessage) {
    if (client == nullptr) {
        errorMessage = L"Windows audio client is unavailable.";
        return false;
    }

    WAVEFORMATEX* rawFormat = nullptr;
    const HRESULT result = client->GetMixFormat(&rawFormat);
    if (FAILED(result) || rawFormat == nullptr) {
        errorMessage = formatWasapiError(L"Could not query the Windows audio mix format", result);
        return false;
    }

    formatHolder = copyWaveFormat(rawFormat);
    CoTaskMemFree(rawFormat);
    return formatHolder.get() != nullptr;
}

bool isSharedFormatSupported(IAudioClient* client, const WAVEFORMATEX* format, HRESULT& supportResult) {
    if (client == nullptr || format == nullptr) {
        supportResult = E_POINTER;
        return false;
    }

    WAVEFORMATEX* closestMatch = nullptr;
    supportResult = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, format, &closestMatch);
    if (closestMatch != nullptr) {
        CoTaskMemFree(closestMatch);
    }
    return supportResult == S_OK;
}

std::vector<DWORD> buildSharedSampleRateCandidates(int requestedSampleRate,
                                                   const WAVEFORMATEX* captureMixFormat,
                                                   const WAVEFORMATEX* renderMixFormat) {
    std::vector<DWORD> sampleRates;
    sampleRates.reserve(6);

    const auto pushUnique = [&sampleRates](DWORD sampleRate) {
        if (sampleRate == 0) {
            return;
        }
        if (std::find(sampleRates.begin(), sampleRates.end(), sampleRate) == sampleRates.end()) {
            sampleRates.push_back(sampleRate);
        }
    };

    pushUnique(static_cast<DWORD>(std::max(0, requestedSampleRate)));
    pushUnique(renderMixFormat != nullptr ? renderMixFormat->nSamplesPerSec : 0);
    pushUnique(captureMixFormat != nullptr ? captureMixFormat->nSamplesPerSec : 0);
    pushUnique(48000);
    pushUnique(44100);
    pushUnique(96000);
    return sampleRates;
}

bool negotiateSharedFormats(IAudioClient* captureClient,
                            IAudioClient* renderClient,
                            int requestedSampleRate,
                            std::wstring_view captureDeviceName,
                            std::wstring_view renderDeviceName,
                            WaveFormatHolder& captureFormatHolder,
                            WaveFormatHolder& renderFormatHolder,
                            DWORD& selectedSampleRate,
                            std::wstring& errorMessage) {
    const WAVEFORMATEX* captureMixFormat = captureFormatHolder.get();
    const WAVEFORMATEX* renderMixFormat = renderFormatHolder.get();
    if (captureMixFormat == nullptr || renderMixFormat == nullptr) {
        errorMessage = L"Could not negotiate a Windows audio stream format because the device mix format is unavailable.";
        return false;
    }

    const std::vector<DWORD> candidates =
        buildSharedSampleRateCandidates(requestedSampleRate, captureMixFormat, renderMixFormat);
    for (const DWORD candidateSampleRate : candidates) {
        WaveFormatHolder captureCandidate = copyWaveFormat(captureMixFormat);
        WaveFormatHolder renderCandidate = copyWaveFormat(renderMixFormat);
        setWaveFormatSampleRate(captureCandidate.get(), candidateSampleRate);
        setWaveFormatSampleRate(renderCandidate.get(), candidateSampleRate);

        HRESULT captureSupport = E_FAIL;
        HRESULT renderSupport = E_FAIL;
        if (!isSharedFormatSupported(captureClient, captureCandidate.get(), captureSupport)) {
            continue;
        }
        if (!isSharedFormatSupported(renderClient, renderCandidate.get(), renderSupport)) {
            continue;
        }

        captureFormatHolder = std::move(captureCandidate);
        renderFormatHolder = std::move(renderCandidate);
        selectedSampleRate = candidateSampleRate;
        return true;
    }

    std::wstring requestedText = requestedSampleRate > 0
        ? std::to_wstring(requestedSampleRate) + L" Hz"
        : L"the requested sample rate";
    errorMessage =
        L"Could not find a shared Windows audio format for input \"" + std::wstring(captureDeviceName) +
        L"\" and output \"" + std::wstring(renderDeviceName) + L"\" at " + requestedText +
        L". Input mix format: " + formatWaveFormatSummary(captureMixFormat) +
        L". Output mix format: " + formatWaveFormatSummary(renderMixFormat) +
        L". Try 48 kHz or 44.1 kHz, or choose a different device pair.";
    return false;
}

bool decodeAudioDataFormat(const WAVEFORMATEX* format, AudioDataFormat& dataFormat) {
    if (format == nullptr || format->nChannels == 0) {
        return false;
    }

    dataFormat = {};
    dataFormat.channelCount = format->nChannels;
    dataFormat.sampleRate = format->nSamplesPerSec;

    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        dataFormat.bytesPerChannel = static_cast<WORD>(format->wBitsPerSample / 8);
        dataFormat.validBitsPerSample = format->wBitsPerSample;
        dataFormat.isFloat = false;
        return dataFormat.bytesPerChannel > 0;
    }

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        dataFormat.bytesPerChannel = static_cast<WORD>(format->wBitsPerSample / 8);
        dataFormat.validBitsPerSample = format->wBitsPerSample;
        dataFormat.isFloat = true;
        return dataFormat.bytesPerChannel == 4 || dataFormat.bytesPerChannel == 8;
    }

    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        return false;
    }

    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    dataFormat.bytesPerChannel = static_cast<WORD>(format->wBitsPerSample / 8);
    dataFormat.validBitsPerSample = extensible->Samples.wValidBitsPerSample != 0
        ? extensible->Samples.wValidBitsPerSample
        : format->wBitsPerSample;
    if (extensible->SubFormat == kSubFormatPcm) {
        dataFormat.isFloat = false;
        return dataFormat.bytesPerChannel > 0;
    }
    if (extensible->SubFormat == kSubFormatIeeeFloat) {
        dataFormat.isFloat = true;
        return dataFormat.bytesPerChannel == 4 || dataFormat.bytesPerChannel == 8;
    }
    return false;
}

bool initializeAudioClient(IAudioClient* client,
                           const WAVEFORMATEX* format,
                           std::wstring_view deviceRole,
                           std::wstring_view deviceName,
                           UINT32& bufferFrames,
                           std::wstring& errorMessage) {
    if (client == nullptr || format == nullptr) {
        errorMessage = L"Windows audio client is unavailable.";
        return false;
    }

    const DWORD streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const HRESULT result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, kSharedBufferDurationHns, 0, format, nullptr);
    if (FAILED(result)) {
        std::wstring operation = L"Could not initialize the Windows ";
        operation += std::wstring(deviceRole);
        operation += L" device";
        if (!deviceName.empty()) {
            operation += L" \"";
            operation += std::wstring(deviceName);
            operation += L"\"";
        }
        operation += L" for ";
        operation += formatWaveFormatSummary(format);
        errorMessage = formatWasapiError(operation, result);
        return false;
    }

    const HRESULT bufferResult = client->GetBufferSize(&bufferFrames);
    if (FAILED(bufferResult)) {
        errorMessage = formatWasapiError(L"Could not query the Windows audio buffer size", bufferResult);
        return false;
    }
    return true;
}

int32_t readSigned24(const uint8_t* bytes) {
    const int32_t value = static_cast<int32_t>(bytes[0]) |
                          (static_cast<int32_t>(bytes[1]) << 8) |
                          (static_cast<int32_t>(bytes[2]) << 16);
    return (value & 0x00800000) != 0 ? (value | ~0x00FFFFFF) : value;
}

void writeSigned24(uint8_t* bytes, int32_t value) {
    bytes[0] = static_cast<uint8_t>(value & 0xFF);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
}

double readNormalizedSample(const uint8_t* bytes, const AudioDataFormat& format) {
    if (format.isFloat) {
        if (format.bytesPerChannel == 4) {
            return static_cast<double>(*reinterpret_cast<const float*>(bytes));
        }
        if (format.bytesPerChannel == 8) {
            return *reinterpret_cast<const double*>(bytes);
        }
        return 0.0;
    }

    switch (format.bytesPerChannel) {
    case 2:
        return static_cast<double>(*reinterpret_cast<const int16_t*>(bytes)) / 32768.0;
    case 3:
        return static_cast<double>(readSigned24(bytes)) / 8388608.0;
    case 4: {
        const int32_t value = *reinterpret_cast<const int32_t*>(bytes);
        const int shift = std::max(0, 32 - static_cast<int>(format.validBitsPerSample));
        return static_cast<double>(value >> shift) /
               static_cast<double>(int64_t{1} << (format.validBitsPerSample - 1));
    }
    default:
        return 0.0;
    }
}

void writeNormalizedSample(uint8_t* bytes, const AudioDataFormat& format, double sample) {
    const double clamped = std::clamp(sample, -1.0, 1.0);
    if (format.isFloat) {
        if (format.bytesPerChannel == 4) {
            *reinterpret_cast<float*>(bytes) = static_cast<float>(clamped);
        } else if (format.bytesPerChannel == 8) {
            *reinterpret_cast<double*>(bytes) = clamped;
        }
        return;
    }

    switch (format.bytesPerChannel) {
    case 2:
        *reinterpret_cast<int16_t*>(bytes) = static_cast<int16_t>(std::lround(clamped * 32767.0));
        break;
    case 3: {
        const int32_t value = static_cast<int32_t>(std::lround(clamped * 8388607.0));
        writeSigned24(bytes, value);
        break;
    }
    case 4: {
        const int shift = std::max(0, 32 - static_cast<int>(format.validBitsPerSample));
        const double scale = static_cast<double>(int64_t{1} << (format.validBitsPerSample - 1)) - 1.0;
        const int32_t value = static_cast<int32_t>(std::lround(clamped * scale)) << shift;
        *reinterpret_cast<int32_t*>(bytes) = value;
        break;
    }
    default:
        break;
    }
}

class WasapiMeasurementSession final : public IAudioMeasurementSession {
public:
    ~WasapiMeasurementSession() override {
        AudioLevels levels{};
        stop(levels);
    }

    bool open(const AudioSettings& settings,
              const MeasurementSettings& measurementSettings,
              MeasurementRunMode runMode,
              std::wstring& errorMessage) {
        settings_ = settings;
        measurementSettings_ = measurementSettings;
        runMode_ = runMode;
        stopRequested_.store(false);
        playbackDone_.store(false);
        closed_.store(false);
        playbackFramesQueued_ = 0;
        currentAmplitudeDb_.store(-90.0);
        peakAmplitudeDb_.store(-90.0);
        startupComplete_ = false;
        startupSucceeded_ = false;
        startupError_.clear();
        playbackPlan_ = {};
        capturedSamples_.clear();
        workerThread_ = std::thread(&WasapiMeasurementSession::runWorker, this);

        std::unique_lock<std::mutex> lock(startupMutex_);
        startupCv_.wait(lock, [this]() { return startupComplete_; });
        if (!startupSucceeded_) {
            lock.unlock();
            if (workerThread_.joinable()) {
                workerThread_.join();
            }
            errorMessage = startupError_;
            return false;
        }
        return true;
    }

    void poll(AudioLevels& levels) override {
        levels.currentAmplitudeDb = currentAmplitudeDb_.load();
        levels.peakAmplitudeDb = peakAmplitudeDb_.load();
    }

    bool playbackDone() const override {
        return playbackDone_.load();
    }

    const measurement::SweepPlaybackPlan& playbackPlan() const override {
        return playbackPlan_;
    }

    const std::vector<int16_t>& capturedSamples() const override {
        return capturedSamples_;
    }

    int sampleRate() const override {
        return sampleRate_;
    }

    SessionDetails details() const override {
        return sessionDetails_;
    }

    void stop(AudioLevels& levels) override {
        if (closed_.exchange(true)) {
            levels.currentAmplitudeDb = currentAmplitudeDb_.load();
            levels.peakAmplitudeDb = peakAmplitudeDb_.load();
            return;
        }

        stopRequested_.store(true);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        playbackDone_.store(true);
        levels.currentAmplitudeDb = currentAmplitudeDb_.load();
        levels.peakAmplitudeDb = peakAmplitudeDb_.load();
    }

private:
    void signalStartup(bool success, const std::wstring& errorMessage) {
        std::lock_guard<std::mutex> lock(startupMutex_);
        startupSucceeded_ = success;
        startupError_ = errorMessage;
        startupComplete_ = true;
        startupCv_.notify_one();
    }

    void updatePeak(double currentDb) {
        double peakDb = peakAmplitudeDb_.load();
        while (currentDb > peakDb && !peakAmplitudeDb_.compare_exchange_weak(peakDb, currentDb)) {
        }
    }

    void fillRenderBuffer(IAudioRenderClient* renderClient, UINT32 frameCount) {
        if (renderClient == nullptr || frameCount == 0) {
            return;
        }

        BYTE* rawBuffer = nullptr;
        if (FAILED(renderClient->GetBuffer(frameCount, &rawBuffer)) || rawBuffer == nullptr) {
            return;
        }

        bool allSilent = true;
        const size_t frameStride = static_cast<size_t>(renderFormat_.channelCount) * renderFormat_.bytesPerChannel;
        for (UINT32 frame = 0; frame < frameCount; ++frame) {
            double leftSample = 0.0;
            double rightSample = 0.0;
            if (playbackFramesQueued_ < playbackPlan_.totalFrames) {
                const size_t baseIndex = playbackFramesQueued_ * 2;
                leftSample = static_cast<double>(playbackPlan_.playbackPcm[baseIndex]) / 32768.0;
                rightSample = static_cast<double>(playbackPlan_.playbackPcm[baseIndex + 1]) / 32768.0;
                ++playbackFramesQueued_;
                allSilent = false;
            }

            uint8_t* frameBytes = rawBuffer + (static_cast<size_t>(frame) * frameStride);
            for (WORD channel = 0; channel < renderFormat_.channelCount; ++channel) {
                double sample = 0.0;
                if (channel == leftOutputChannelIndex_) {
                    sample = leftSample;
                } else if (channel == rightOutputChannelIndex_) {
                    sample = rightSample;
                }
                writeNormalizedSample(frameBytes + (static_cast<size_t>(channel) * renderFormat_.bytesPerChannel), renderFormat_, sample);
            }
        }

        renderClient->ReleaseBuffer(frameCount, allSilent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }

    void serviceRender(IAudioClient* audioClient, IAudioRenderClient* renderClient, UINT32 bufferFrames) {
        if (audioClient == nullptr || renderClient == nullptr || bufferFrames == 0) {
            return;
        }

        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding)) || padding >= bufferFrames) {
            return;
        }

        fillRenderBuffer(renderClient, bufferFrames - padding);
        if (playbackFramesQueued_ >= playbackPlan_.totalFrames) {
            playbackDone_.store(true);
        }
    }

    void serviceCapture(IAudioCaptureClient* captureClient) {
        if (captureClient == nullptr) {
            return;
        }

        while (!stopRequested_.load()) {
            UINT32 packetFrames = 0;
            const HRESULT packetResult = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(packetResult) || packetFrames == 0) {
                return;
            }

            BYTE* rawBuffer = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient->GetBuffer(&rawBuffer, &frames, &flags, nullptr, nullptr))) {
                return;
            }

            const size_t captureOffset = capturedSamples_.size();
            capturedSamples_.resize(captureOffset + frames);
            auto* destination = capturedSamples_.data() + captureOffset;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || rawBuffer == nullptr) {
                std::fill(destination, destination + frames, 0);
            } else {
                const size_t frameStride = static_cast<size_t>(captureFormat_.channelCount) * captureFormat_.bytesPerChannel;
                for (UINT32 frame = 0; frame < frames; ++frame) {
                    const uint8_t* frameBytes = rawBuffer + (static_cast<size_t>(frame) * frameStride);
                    const uint8_t* channelBytes =
                        frameBytes + (static_cast<size_t>(captureChannelIndex_) * captureFormat_.bytesPerChannel);
                    const double normalized = readNormalizedSample(channelBytes, captureFormat_);
                    destination[frame] = static_cast<int16_t>(std::lround(std::clamp(normalized, -1.0, 1.0) * 32767.0));
                }
            }
            captureClient->ReleaseBuffer(frames);

            const double currentDb = measurement::amplitudeDbFromPcm16(destination, frames);
            currentAmplitudeDb_.store(currentDb);
            updatePeak(currentDb);
        }
    }

    void runWorker() {
        CoInitScope coInit(COINIT_MULTITHREADED);
        if (FAILED(coInit.result()) && coInit.result() != RPC_E_CHANGED_MODE) {
            signalStartup(false, L"COM initialization failed for Windows audio: " + formatHResultMessageLocal(coInit.result()));
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        const HRESULT enumeratorResult =
            CoCreateInstance(__uuidof(MMDeviceEnumerator),
                             nullptr,
                             CLSCTX_INPROC_SERVER,
                             __uuidof(IMMDeviceEnumerator),
                             reinterpret_cast<void**>(enumerator.put()));
        if (FAILED(enumeratorResult)) {
            signalStartup(false, formatWasapiError(L"Could not initialize Windows audio device enumeration", enumeratorResult));
            return;
        }

        ComPtr<IMMDevice> captureDevice;
        ComPtr<IMMDevice> renderDevice;
        std::wstring captureDeviceName;
        std::wstring renderDeviceName;
        std::wstring errorMessage;
        if (!resolveDevice(enumerator.get(), eCapture, settings_.windowsInputDeviceId, captureDevice, captureDeviceName, errorMessage) ||
            !resolveDevice(enumerator.get(), eRender, settings_.windowsOutputDeviceId, renderDevice, renderDeviceName, errorMessage)) {
            signalStartup(false, errorMessage);
            return;
        }

        ComPtr<IAudioClient> captureAudioClient;
        ComPtr<IAudioClient> renderAudioClient;
        if (!activateAudioClient(captureDevice.get(), captureAudioClient, errorMessage) ||
            !activateAudioClient(renderDevice.get(), renderAudioClient, errorMessage)) {
            signalStartup(false, errorMessage);
            return;
        }

        WaveFormatHolder captureFormatHolder;
        WaveFormatHolder renderFormatHolder;
        if (!queryMixFormat(captureAudioClient.get(), captureFormatHolder, errorMessage) ||
            !queryMixFormat(renderAudioClient.get(), renderFormatHolder, errorMessage)) {
            signalStartup(false, errorMessage);
            return;
        }

        const int requestedSampleRate = std::max(8000, measurementSettings_.sampleRate);
        DWORD negotiatedSampleRate = 0;
        if (!negotiateSharedFormats(captureAudioClient.get(),
                                    renderAudioClient.get(),
                                    requestedSampleRate,
                                    captureDeviceName,
                                    renderDeviceName,
                                    captureFormatHolder,
                                    renderFormatHolder,
                                    negotiatedSampleRate,
                                    errorMessage)) {
            signalStartup(false, errorMessage);
            return;
        }

        if (!parseAudioDataFormat(captureFormatHolder.get(), captureFormat_, errorMessage) ||
            !parseAudioDataFormat(renderFormatHolder.get(), renderFormat_, errorMessage)) {
            signalStartup(false, errorMessage);
            return;
        }

        if (!validateChannelSelection(settings_.micInputChannel,
                                      captureFormat_.channelCount,
                                      L"input",
                                      captureDeviceName,
                                      errorMessage) ||
            !validateChannelSelection(settings_.leftOutputChannel,
                                      renderFormat_.channelCount,
                                      L"left output",
                                      renderDeviceName,
                                      errorMessage) ||
            !validateChannelSelection(settings_.rightOutputChannel,
                                      renderFormat_.channelCount,
                                      L"right output",
                                      renderDeviceName,
                                      errorMessage)) {
            signalStartup(false, errorMessage);
            return;
        }
        if (settings_.leftOutputChannel == settings_.rightOutputChannel) {
            signalStartup(false, L"Left and right Windows output channels must be different.");
            return;
        }
        captureChannelIndex_ = static_cast<WORD>(settings_.micInputChannel - 1);
        leftOutputChannelIndex_ = static_cast<WORD>(settings_.leftOutputChannel - 1);
        rightOutputChannelIndex_ = static_cast<WORD>(settings_.rightOutputChannel - 1);

        sampleRate_ = static_cast<int>(negotiatedSampleRate);
        MeasurementSettings effectiveSettings = measurementSettings_;
        effectiveSettings.sampleRate = sampleRate_;
        measurement::syncDerivedMeasurementSettings(effectiveSettings);
        playbackPlan_ = measurement::buildSweepPlaybackPlan(effectiveSettings, settings_.outputVolumeDb, runMode_);

        UINT32 captureBufferFrames = 0;
        UINT32 renderBufferFrames = 0;
        if (!initializeAudioClient(captureAudioClient.get(),
                                   captureFormatHolder.get(),
                                   L"input",
                                   captureDeviceName,
                                   captureBufferFrames,
                                   errorMessage) ||
            !initializeAudioClient(renderAudioClient.get(),
                                   renderFormatHolder.get(),
                                   L"output",
                                   renderDeviceName,
                                   renderBufferFrames,
                                   errorMessage)) {
            signalStartup(false, errorMessage);
            return;
        }

        ComPtr<IAudioCaptureClient> captureClient;
        ComPtr<IAudioRenderClient> renderClient;
        const HRESULT captureServiceResult =
            captureAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(captureClient.put()));
        if (FAILED(captureServiceResult)) {
            signalStartup(false, formatWasapiError(L"Could not open the Windows audio capture stream", captureServiceResult));
            return;
        }

        const HRESULT renderServiceResult =
            renderAudioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(renderClient.put()));
        if (FAILED(renderServiceResult)) {
            signalStartup(false, formatWasapiError(L"Could not open the Windows audio render stream", renderServiceResult));
            return;
        }

        capturedSamples_.reserve(playbackPlan_.totalFrames + static_cast<size_t>(sampleRate_ / 2));
        fillRenderBuffer(renderClient.get(), renderBufferFrames);

        const HRESULT captureStartResult = captureAudioClient->Start();
        if (FAILED(captureStartResult)) {
            signalStartup(false, formatWasapiError(L"Could not start the Windows audio input stream", captureStartResult));
            return;
        }

        const HRESULT renderStartResult = renderAudioClient->Start();
        if (FAILED(renderStartResult)) {
            captureAudioClient->Stop();
            signalStartup(false, formatWasapiError(L"Could not start the Windows audio output stream", renderStartResult));
            return;
        }

        sessionDetails_.backendName = "WASAPI";
        sessionDetails_.inputDeviceName = captureDeviceName;
        sessionDetails_.outputDeviceName = renderDeviceName;
        sessionDetails_.routingSelectionHonored = true;
        sessionDetails_.routingNotes =
            L"WASAPI shared mode negotiated " + std::to_wstring(sampleRate_) + L" Hz";
        if (requestedSampleRate != sampleRate_) {
            sessionDetails_.routingNotes += L" from a requested " + std::to_wstring(requestedSampleRate) + L" Hz";
        }
        sessionDetails_.routingNotes +=
            L". Input format " + formatWaveFormatSummary(captureFormatHolder.get()) +
            L", output format " + formatWaveFormatSummary(renderFormatHolder.get()) +
            L". Input channel " + std::to_wstring(settings_.micInputChannel) +
            L" of " + std::to_wstring(captureFormat_.channelCount) +
            L", output channels " + std::to_wstring(settings_.leftOutputChannel) +
            L" / " + std::to_wstring(settings_.rightOutputChannel) +
            L" of " + std::to_wstring(renderFormat_.channelCount) + L".";
        signalStartup(true, L"");

        while (!stopRequested_.load()) {
            serviceRender(renderAudioClient.get(), renderClient.get(), renderBufferFrames);
            serviceCapture(captureClient.get());
            std::this_thread::sleep_for(std::chrono::milliseconds(kWorkerSleepMs));
        }

        renderAudioClient->Stop();
        captureAudioClient->Stop();
        serviceCapture(captureClient.get());
    }

    bool parseAudioDataFormat(const WAVEFORMATEX* format, AudioDataFormat& dataFormat, std::wstring& errorMessage) {
        if (!decodeAudioDataFormat(format, dataFormat)) {
            errorMessage = L"The selected Windows audio device reported an unsupported mix format.";
            return false;
        }
        return true;
    }

    AudioSettings settings_;
    MeasurementSettings measurementSettings_;
    MeasurementRunMode runMode_ = MeasurementRunMode::Room;
    std::thread workerThread_;
    std::mutex startupMutex_;
    std::condition_variable startupCv_;
    bool startupComplete_ = false;
    bool startupSucceeded_ = false;
    std::wstring startupError_;
    SessionDetails sessionDetails_;
    measurement::SweepPlaybackPlan playbackPlan_;
    std::vector<int16_t> capturedSamples_;
    AudioDataFormat captureFormat_{};
    AudioDataFormat renderFormat_{};
    std::atomic<double> currentAmplitudeDb_{-90.0};
    std::atomic<double> peakAmplitudeDb_{-90.0};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> playbackDone_{false};
    std::atomic<bool> closed_{false};
    int sampleRate_ = 44100;
    size_t playbackFramesQueued_ = 0;
    WORD captureChannelIndex_ = 0;
    WORD leftOutputChannelIndex_ = 0;
    WORD rightOutputChannelIndex_ = 1;
};

class WasapiAudioBackend final : public IAudioBackend {
public:
    std::unique_ptr<IAudioMeasurementSession> startSession(const AudioSettings& settings,
                                                           const MeasurementSettings& measurementSettings,
                                                           MeasurementRunMode runMode,
                                                           std::wstring& errorMessage) override {
        auto session = std::make_unique<WasapiMeasurementSession>();
        if (!session->open(settings, measurementSettings, runMode, errorMessage)) {
            return nullptr;
        }
        return session;
    }
};

}  // namespace

std::unique_ptr<IAudioBackend> createWasapiAudioBackend() {
    return std::make_unique<WasapiAudioBackend>();
}

}  // namespace wolfie::audio
