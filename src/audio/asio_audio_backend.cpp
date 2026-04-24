#include "audio/asio_audio_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/asio_sdk.h"
#include "audio/winmm_audio_backend.h"
#include "core/text_utils.h"
#include "measurement/response_analyzer.h"

namespace wolfie::audio {

namespace {

constexpr ASIOError kAsioOk = 0;
constexpr long kAsioSelectorSupported = 1;
constexpr long kAsioEngineVersion = 2;
constexpr long kAsioResetRequest = 3;
constexpr long kAsioResyncRequest = 5;
constexpr long kAsioLatenciesChanged = 6;
constexpr long kAsioSupportsTimeInfo = 7;
constexpr long kAsioOverload = 15;

class AsioMeasurementSession;

std::mutex gActiveSessionMutex;
AsioMeasurementSession* gActiveSession = nullptr;

double pcm16ToNormalized(int16_t sample) {
    return static_cast<double>(sample) / 32768.0;
}

int16_t normalizedToPcm16(double sample) {
    const double clamped = std::clamp(sample, -1.0, 1.0);
    return static_cast<int16_t>(std::lround(clamped * 32767.0));
}

int32_t readSigned24Lsb(const uint8_t* bytes) {
    const int32_t value = static_cast<int32_t>(bytes[0]) |
                          (static_cast<int32_t>(bytes[1]) << 8) |
                          (static_cast<int32_t>(bytes[2]) << 16);
    return (value & 0x00800000) != 0 ? (value | ~0x00FFFFFF) : value;
}

void writeSigned24Lsb(uint8_t* bytes, int32_t value) {
    bytes[0] = static_cast<uint8_t>(value & 0xFF);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
}

double readAsioSample(const void* buffer, ASIOSampleType sampleType, size_t frameIndex) {
    switch (sampleType) {
    case kAsioSampleInt16Lsb:
        return static_cast<double>(reinterpret_cast<const int16_t*>(buffer)[frameIndex]) / 32768.0;
    case kAsioSampleInt24Lsb:
        return static_cast<double>(readSigned24Lsb(reinterpret_cast<const uint8_t*>(buffer) + (frameIndex * 3))) / 8388608.0;
    case kAsioSampleInt32Lsb:
        return static_cast<double>(reinterpret_cast<const int32_t*>(buffer)[frameIndex]) / 2147483648.0;
    case kAsioSampleFloat32Lsb:
        return static_cast<double>(reinterpret_cast<const float*>(buffer)[frameIndex]);
    case kAsioSampleFloat64Lsb:
        return reinterpret_cast<const double*>(buffer)[frameIndex];
    case kAsioSampleInt32Lsb16:
        return static_cast<double>(reinterpret_cast<const int32_t*>(buffer)[frameIndex] >> 16) / 32768.0;
    case kAsioSampleInt32Lsb18:
        return static_cast<double>(reinterpret_cast<const int32_t*>(buffer)[frameIndex] >> 14) / 131072.0;
    case kAsioSampleInt32Lsb20:
        return static_cast<double>(reinterpret_cast<const int32_t*>(buffer)[frameIndex] >> 12) / 524288.0;
    case kAsioSampleInt32Lsb24:
        return static_cast<double>(reinterpret_cast<const int32_t*>(buffer)[frameIndex] >> 8) / 8388608.0;
    default:
        return 0.0;
    }
}

void writeAsioSample(void* buffer, ASIOSampleType sampleType, size_t frameIndex, int16_t pcmSample) {
    const double normalized = pcm16ToNormalized(pcmSample);
    switch (sampleType) {
    case kAsioSampleInt16Lsb:
        reinterpret_cast<int16_t*>(buffer)[frameIndex] = pcmSample;
        break;
    case kAsioSampleInt24Lsb: {
        const int32_t value = static_cast<int32_t>(pcmSample) << 8;
        writeSigned24Lsb(reinterpret_cast<uint8_t*>(buffer) + (frameIndex * 3), value);
        break;
    }
    case kAsioSampleInt32Lsb:
        reinterpret_cast<int32_t*>(buffer)[frameIndex] = static_cast<int32_t>(pcmSample) << 16;
        break;
    case kAsioSampleFloat32Lsb:
        reinterpret_cast<float*>(buffer)[frameIndex] = static_cast<float>(normalized);
        break;
    case kAsioSampleFloat64Lsb:
        reinterpret_cast<double*>(buffer)[frameIndex] = normalized;
        break;
    case kAsioSampleInt32Lsb16:
        reinterpret_cast<int32_t*>(buffer)[frameIndex] = static_cast<int32_t>(pcmSample) << 16;
        break;
    case kAsioSampleInt32Lsb18:
        reinterpret_cast<int32_t*>(buffer)[frameIndex] = static_cast<int32_t>(pcmSample) << 14;
        break;
    case kAsioSampleInt32Lsb20:
        reinterpret_cast<int32_t*>(buffer)[frameIndex] = static_cast<int32_t>(pcmSample) << 12;
        break;
    case kAsioSampleInt32Lsb24:
        reinterpret_cast<int32_t*>(buffer)[frameIndex] = static_cast<int32_t>(pcmSample) << 8;
        break;
    default:
        break;
    }
}

std::wstring formatAsioError(IASIO* driver, std::wstring_view operation, ASIOError error) {
    std::wstring message = operation.empty() ? L"ASIO operation failed" : std::wstring(operation);
    if (const std::wstring driverMessage = asioDriverMessage(driver); !driverMessage.empty()) {
        message += L": ";
        message += driverMessage;
        return message;
    }

    message += L" (error ";
    message += std::to_wstring(error);
    message += L')';
    return message;
}

struct ChannelSelection {
    bool isInput = false;
    int channelNumber = 0;
    ASIOChannelInfo info{};
    std::wstring label;
};

class AsioMeasurementSession final : public IAudioMeasurementSession {
public:
    ~AsioMeasurementSession() override {
        AudioLevels levels{};
        stop(levels);
    }

    bool open(const AudioSettings& settings,
              const measurement::SweepPlaybackPlan& playbackPlan,
              int sampleRate,
              std::wstring& errorMessage) {
        playbackPcm_ = playbackPlan.playbackPcm;
        totalFrames_ = playbackPlan.totalFrames;
        sampleRate_ = sampleRate;
        driverName_ = toWide(settings.driver);

        if (driverName_.empty() || driverName_ == L"ASIO driver") {
            errorMessage = L"No concrete ASIO driver is selected.";
            return false;
        }

        if (settings.leftOutputChannel == settings.rightOutputChannel) {
            errorMessage = L"Left and right output channels must be different for ASIO measurement.";
            return false;
        }

        if (const auto openError = openDriver(GetDesktopWindow(), driverName_, driverHandle_)) {
            errorMessage = *openError;
            return false;
        }

        long inputCount = 0;
        long outputCount = 0;
        const ASIOError channelCountResult = driverHandle_.driver->getChannels(&inputCount, &outputCount);
        if (channelCountResult != kAsioOk) {
            errorMessage = formatAsioError(driverHandle_.driver, L"Could not read ASIO channel counts", channelCountResult);
            return false;
        }

        if (!resolveChannel(true, settings.micInputChannel, inputCount, inputChannel_, errorMessage) ||
            !resolveChannel(false, settings.leftOutputChannel, outputCount, leftOutputChannel_, errorMessage) ||
            !resolveChannel(false, settings.rightOutputChannel, outputCount, rightOutputChannel_, errorMessage)) {
            return false;
        }

        ASIOSampleRate currentSampleRate = 0.0;
        const ASIOError sampleRateResult = driverHandle_.driver->getSampleRate(&currentSampleRate);
        if (sampleRateResult != kAsioOk) {
            errorMessage = formatAsioError(driverHandle_.driver, L"Could not read the ASIO sample rate", sampleRateResult);
            return false;
        }

        if (std::abs(currentSampleRate - static_cast<double>(sampleRate)) > 0.5) {
            const ASIOError canRateResult = driverHandle_.driver->canSampleRate(static_cast<ASIOSampleRate>(sampleRate));
            if (canRateResult != kAsioOk) {
                errorMessage = L"The selected ASIO driver does not support " + std::to_wstring(sampleRate) + L" Hz.";
                return false;
            }

            const ASIOError setRateResult = driverHandle_.driver->setSampleRate(static_cast<ASIOSampleRate>(sampleRate));
            if (setRateResult != kAsioOk) {
                errorMessage = formatAsioError(driverHandle_.driver, L"Could not switch the ASIO sample rate", setRateResult);
                return false;
            }
        }

        long minBufferSize = 0;
        long maxBufferSize = 0;
        long preferredBufferSize = 0;
        long granularity = 0;
        const ASIOError bufferSizeResult =
            driverHandle_.driver->getBufferSize(&minBufferSize, &maxBufferSize, &preferredBufferSize, &granularity);
        if (bufferSizeResult != kAsioOk) {
            errorMessage = formatAsioError(driverHandle_.driver, L"Could not query ASIO buffer sizes", bufferSizeResult);
            return false;
        }

        bufferSize_ = std::clamp(preferredBufferSize > 0 ? preferredBufferSize : minBufferSize, minBufferSize, maxBufferSize);
        if (bufferSize_ <= 0) {
            errorMessage = L"The ASIO driver reported an invalid buffer size.";
            return false;
        }

        inputLatency_ = 0;
        outputLatency_ = 0;
        driverHandle_.driver->getLatencies(&inputLatency_, &outputLatency_);

        bufferInfos_[0].isInput = 1;
        bufferInfos_[0].channelNum = inputChannel_.channelNumber - 1;
        bufferInfos_[1].isInput = 0;
        bufferInfos_[1].channelNum = leftOutputChannel_.channelNumber - 1;
        bufferInfos_[2].isInput = 0;
        bufferInfos_[2].channelNum = rightOutputChannel_.channelNumber - 1;

        callbacks_.bufferSwitch = &AsioMeasurementSession::bufferSwitch;
        callbacks_.bufferSwitchTimeInfo = &AsioMeasurementSession::bufferSwitchTimeInfo;
        callbacks_.sampleRateDidChange = &AsioMeasurementSession::sampleRateDidChange;
        callbacks_.asioMessage = &AsioMeasurementSession::asioMessage;

        {
            std::lock_guard<std::mutex> lock(gActiveSessionMutex);
            if (gActiveSession != nullptr) {
                errorMessage = L"Only one ASIO measurement session can run at a time.";
                return false;
            }
            gActiveSession = this;
        }

        const ASIOError createBufferResult =
            driverHandle_.driver->createBuffers(bufferInfos_.data(),
                                                static_cast<long>(bufferInfos_.size()),
                                                bufferSize_,
                                                &callbacks_);
        if (createBufferResult != kAsioOk) {
            {
                std::lock_guard<std::mutex> lock(gActiveSessionMutex);
                if (gActiveSession == this) {
                    gActiveSession = nullptr;
                }
            }
            errorMessage = formatAsioError(driverHandle_.driver, L"Could not create ASIO buffers", createBufferResult);
            return false;
        }
        buffersCreated_ = true;

        const ASIOError startResult = driverHandle_.driver->start();
        if (startResult != kAsioOk) {
            errorMessage = formatAsioError(driverHandle_.driver, L"Could not start the ASIO driver", startResult);
            return false;
        }
        started_ = true;

        sessionDetails_.backendName = "ASIO";
        sessionDetails_.inputDeviceName = driverName_ + L" - input " + inputChannel_.label;
        sessionDetails_.outputDeviceName = driverName_ + L" - outputs " + leftOutputChannel_.label + L" / " + rightOutputChannel_.label;
        sessionDetails_.routingSelectionHonored = true;
        sessionDetails_.routingNotes =
            L"ASIO measurement uses the selected driver and channels directly. Buffer size " +
            std::to_wstring(bufferSize_) +
            L", input latency " + std::to_wstring(inputLatency_) +
            L" samples, output latency " + std::to_wstring(outputLatency_) + L" samples.";
        return true;
    }

    void poll(AudioLevels& levels) override {
        levels.currentAmplitudeDb = currentAmplitudeDb_.load();
        levels.peakAmplitudeDb = peakAmplitudeDb_.load();
    }

    bool playbackDone() const override {
        return playbackDone_.load();
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

        levels.currentAmplitudeDb = currentAmplitudeDb_.load();
        levels.peakAmplitudeDb = peakAmplitudeDb_.load();

        if (driverHandle_.driver != nullptr && started_) {
            driverHandle_.driver->stop();
            started_ = false;
        }
        if (driverHandle_.driver != nullptr && buffersCreated_) {
            driverHandle_.driver->disposeBuffers();
            buffersCreated_ = false;
        }

        std::lock_guard<std::mutex> lock(gActiveSessionMutex);
        if (gActiveSession == this) {
            gActiveSession = nullptr;
        }
    }

private:
    static void bufferSwitch(long index, ASIOBool) {
        std::lock_guard<std::mutex> lock(gActiveSessionMutex);
        if (gActiveSession != nullptr) {
            gActiveSession->processBuffer(index);
        }
    }

    static ASIOTime* bufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool) {
        std::lock_guard<std::mutex> lock(gActiveSessionMutex);
        if (gActiveSession != nullptr) {
            gActiveSession->processBuffer(index);
        }
        return params;
    }

    static void sampleRateDidChange(ASIOSampleRate) {
    }

    static long asioMessage(long selector, long value, void*, double*) {
        switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion ||
                   value == kAsioResetRequest ||
                   value == kAsioResyncRequest ||
                   value == kAsioLatenciesChanged ||
                   value == kAsioSupportsTimeInfo ||
                   value == kAsioOverload;
        case kAsioEngineVersion:
            return 2;
        case kAsioSupportsTimeInfo:
            return 1;
        default:
            return 0;
        }
    }

    bool resolveChannel(bool isInput,
                        int channelNumber,
                        long availableCount,
                        ChannelSelection& channel,
                        std::wstring& errorMessage) {
        if (channelNumber <= 0 || channelNumber > availableCount) {
            errorMessage = std::wstring(isInput ? L"Selected ASIO input channel is out of range."
                                                : L"Selected ASIO output channel is out of range.");
            return false;
        }

        channel = {};
        channel.isInput = isInput;
        channel.channelNumber = channelNumber;
        channel.info.channel = channelNumber - 1;
        channel.info.isInput = isInput ? 1 : 0;
        const ASIOError infoResult = driverHandle_.driver->getChannelInfo(&channel.info);
        if (infoResult != kAsioOk) {
            errorMessage = formatAsioError(driverHandle_.driver,
                                           isInput ? L"Could not read ASIO input channel info"
                                                   : L"Could not read ASIO output channel info",
                                           infoResult);
            return false;
        }

        if (!isSupportedAsioSampleType(channel.info.type)) {
            errorMessage = std::wstring(isInput ? L"Unsupported ASIO input sample type: "
                                                : L"Unsupported ASIO output sample type: ") +
                           asioSampleTypeName(channel.info.type);
            return false;
        }

        channel.label = std::to_wstring(channelNumber) + L" - " + asioChannelName(channel.info, channelNumber);
        return true;
    }

    void processBuffer(long bufferIndex) {
        if (closed_.load()) {
            return;
        }

        const size_t frameCount = static_cast<size_t>(std::max<long>(bufferSize_, 0));
        std::vector<int16_t> captureBlock(frameCount, 0);
        for (size_t frame = 0; frame < frameCount; ++frame) {
            captureBlock[frame] = normalizedToPcm16(
                readAsioSample(bufferInfos_[0].buffers[bufferIndex], inputChannel_.info.type, frame));

            int16_t leftSample = 0;
            int16_t rightSample = 0;
            const size_t playbackFrame = renderedFrames_.fetch_add(1);
            if (playbackFrame < totalFrames_) {
                const size_t baseIndex = playbackFrame * 2;
                leftSample = playbackPcm_[baseIndex];
                rightSample = playbackPcm_[baseIndex + 1];
            } else {
                playbackDone_.store(true);
            }

            writeAsioSample(bufferInfos_[1].buffers[bufferIndex], leftOutputChannel_.info.type, frame, leftSample);
            writeAsioSample(bufferInfos_[2].buffers[bufferIndex], rightOutputChannel_.info.type, frame, rightSample);
        }

        if (renderedFrames_.load() >= totalFrames_) {
            playbackDone_.store(true);
        }

        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            capturedSamples_.insert(capturedSamples_.end(), captureBlock.begin(), captureBlock.end());
        }

        const double currentDb = measurement::amplitudeDbFromPcm16(captureBlock.data(), captureBlock.size());
        currentAmplitudeDb_.store(currentDb);
        double peakDb = peakAmplitudeDb_.load();
        while (currentDb > peakDb && !peakAmplitudeDb_.compare_exchange_weak(peakDb, currentDb)) {
        }
    }

    DriverHandle driverHandle_;
    std::wstring driverName_;
    measurement::SweepPlaybackPlan playbackPlan_;
    std::vector<int16_t> playbackPcm_;
    std::vector<int16_t> capturedSamples_;
    std::mutex dataMutex_;
    std::array<ASIOBufferInfo, 3> bufferInfos_{};
    ASIOCallbacks callbacks_{};
    ChannelSelection inputChannel_;
    ChannelSelection leftOutputChannel_;
    ChannelSelection rightOutputChannel_;
    SessionDetails sessionDetails_;
    std::atomic<double> currentAmplitudeDb_{-90.0};
    std::atomic<double> peakAmplitudeDb_{-90.0};
    std::atomic<bool> playbackDone_{false};
    std::atomic<bool> closed_{false};
    std::atomic<size_t> renderedFrames_{0};
    int sampleRate_ = 44100;
    long bufferSize_ = 0;
    long inputLatency_ = 0;
    long outputLatency_ = 0;
    size_t totalFrames_ = 0;
    bool buffersCreated_ = false;
    bool started_ = false;
};

class AsioAudioBackend final : public IAudioBackend {
public:
    std::unique_ptr<IAudioMeasurementSession> startSession(const AudioSettings& settings,
                                                           const measurement::SweepPlaybackPlan& playbackPlan,
                                                           int sampleRate,
                                                           std::wstring& errorMessage) override {
        auto session = std::make_unique<AsioMeasurementSession>();
        if (!session->open(settings, playbackPlan, sampleRate, errorMessage)) {
            return nullptr;
        }
        return session;
    }
};

class DefaultAudioBackend final : public IAudioBackend {
public:
    DefaultAudioBackend()
        : asioBackend_(createAsioAudioBackend()),
          winMmBackend_(createWinMmAudioBackend()) {}

    std::unique_ptr<IAudioMeasurementSession> startSession(const AudioSettings& settings,
                                                           const measurement::SweepPlaybackPlan& playbackPlan,
                                                           int sampleRate,
                                                           std::wstring& errorMessage) override {
        const bool hasExplicitAsioDriver = !settings.driver.empty() && settings.driver != "ASIO driver";
        if (hasExplicitAsioDriver) {
            return asioBackend_->startSession(settings, playbackPlan, sampleRate, errorMessage);
        }
        return winMmBackend_->startSession(settings, playbackPlan, sampleRate, errorMessage);
    }

private:
    std::unique_ptr<IAudioBackend> asioBackend_;
    std::unique_ptr<IAudioBackend> winMmBackend_;
};

}  // namespace

std::unique_ptr<IAudioBackend> createAsioAudioBackend() {
    return std::make_unique<AsioAudioBackend>();
}

std::unique_ptr<IAudioBackend> createDefaultAudioBackend() {
    return std::make_unique<DefaultAudioBackend>();
}

}  // namespace wolfie::audio
