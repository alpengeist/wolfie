#include "audio/asio_audio_backend.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include <RtAudio.h>

#include "audio/wasapi_audio_backend.h"
#include "core/text_utils.h"
#include "measurement/response_analyzer.h"

namespace wolfie::audio {

namespace {

double pcm16ToFloat(int16_t sample) {
    return static_cast<double>(sample) / 32768.0;
}

int16_t floatToPcm16(float sample) {
    const double clamped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
    return static_cast<int16_t>(std::lround(clamped * 32767.0));
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::wstring formatRtAudioError(std::wstring_view operation, const std::string& errorText) {
    std::wstring message(operation);
    if (!errorText.empty()) {
        message += L": ";
        message += toWide(errorText);
    }
    return message;
}

class RtAudioAsioMeasurementSession final : public IAudioMeasurementSession {
public:
    ~RtAudioAsioMeasurementSession() override {
        AudioLevels levels{};
        stop(levels);
    }

    bool open(const AudioSettings& settings,
              const MeasurementSettings& measurementSettings,
              MeasurementRunMode runMode,
              std::wstring& errorMessage) {
        continuousAlignment_ = runMode == MeasurementRunMode::Alignment;
        driverName_ = settings.driver;
        if (driverName_.empty() || driverName_ == "ASIO driver") {
            errorMessage = L"No concrete ASIO driver is selected.";
            return false;
        }
        if (settings.leftOutputChannel == settings.rightOutputChannel) {
            errorMessage = L"Left and right output channels must be different for ASIO measurement.";
            return false;
        }

        try {
            audio_ = std::make_unique<RtAudio>(
                RtAudio::WINDOWS_ASIO,
                [this](RtAudioErrorType, const std::string& text) {
                    lastRtAudioError_ = text;
                });
        } catch (...) {
            errorMessage = L"Could not initialize the RtAudio ASIO host.";
            return false;
        }

        const unsigned int deviceId = resolveDeviceId(errorMessage);
        if (deviceId == 0) {
            return false;
        }

        const RtAudio::DeviceInfo deviceInfo = audio_->getDeviceInfo(deviceId);
        if (deviceInfo.ID == 0 || deviceInfo.name.empty()) {
            errorMessage = L"Could not read RtAudio ASIO device information.";
            return false;
        }
        if (settings.micInputChannel <= 0 ||
            static_cast<unsigned int>(settings.micInputChannel) > deviceInfo.inputChannels) {
            errorMessage = L"Selected ASIO input channel is out of range.";
            return false;
        }
        if (settings.leftOutputChannel <= 0 ||
            settings.rightOutputChannel <= 0 ||
            static_cast<unsigned int>(settings.leftOutputChannel) > deviceInfo.outputChannels ||
            static_cast<unsigned int>(settings.rightOutputChannel) > deviceInfo.outputChannels) {
            errorMessage = L"Selected ASIO output channel is out of range.";
            return false;
        }

        sampleRate_ = std::max(8000, measurementSettings.sampleRate);
        MeasurementSettings effectiveSettings = measurementSettings;
        effectiveSettings.sampleRate = sampleRate_;
        if (runMode != MeasurementRunMode::Alignment) {
            measurement::syncDerivedMeasurementSettings(effectiveSettings);
        }
        playbackPlan_ = measurement::buildSweepPlaybackPlan(effectiveSettings, settings.outputVolumeDb, runMode);
        playbackPcm_ = playbackPlan_.playbackPcm;
        totalFrames_ = playbackPlan_.totalFrames;
        capturedSamples_.reserve(totalFrames_ + static_cast<size_t>(sampleRate_ / 2));

        outputFirstChannel_ = static_cast<unsigned int>(std::min(settings.leftOutputChannel, settings.rightOutputChannel) - 1);
        outputChannelCount_ =
            static_cast<unsigned int>(std::max(settings.leftOutputChannel, settings.rightOutputChannel)) - outputFirstChannel_;
        leftOutputOffset_ = static_cast<unsigned int>(settings.leftOutputChannel - 1) - outputFirstChannel_;
        rightOutputOffset_ = static_cast<unsigned int>(settings.rightOutputChannel - 1) - outputFirstChannel_;

        RtAudio::StreamParameters outputParams{};
        outputParams.deviceId = deviceId;
        outputParams.nChannels = outputChannelCount_;
        outputParams.firstChannel = outputFirstChannel_;

        RtAudio::StreamParameters inputParams{};
        inputParams.deviceId = deviceId;
        inputParams.nChannels = 1;
        inputParams.firstChannel = static_cast<unsigned int>(settings.micInputChannel - 1);

        RtAudio::StreamOptions options{};
        options.flags = 0;
        options.streamName = "wolfie ASIO measurement";

        unsigned int bufferFrames = 0;
        const RtAudioErrorType openResult =
            audio_->openStream(&outputParams,
                               &inputParams,
                               RTAUDIO_FLOAT32,
                               static_cast<unsigned int>(sampleRate_),
                               &bufferFrames,
                               &RtAudioAsioMeasurementSession::audioCallback,
                               this,
                               &options);
        if (openResult != RTAUDIO_NO_ERROR) {
            errorMessage = formatRtAudioError(L"Could not open the RtAudio ASIO stream", audio_->getErrorText());
            return false;
        }

        if (const unsigned int actualRate = audio_->getStreamSampleRate(); actualRate > 0) {
            sampleRate_ = static_cast<int>(actualRate);
        }
        bufferSize_ = bufferFrames;

        const RtAudioErrorType startResult = audio_->startStream();
        if (startResult != RTAUDIO_NO_ERROR) {
            errorMessage = formatRtAudioError(L"Could not start the RtAudio ASIO stream", audio_->getErrorText());
            return false;
        }

        sessionDetails_.backendName = "ASIO";
        sessionDetails_.inputDeviceName =
            toWide(deviceInfo.name) + L" - input " + std::to_wstring(settings.micInputChannel);
        sessionDetails_.outputDeviceName =
            toWide(deviceInfo.name) + L" - outputs " +
            std::to_wstring(settings.leftOutputChannel) + L" / " +
            std::to_wstring(settings.rightOutputChannel);
        sessionDetails_.routingSelectionHonored = true;
        sessionDetails_.routingNotes =
            L"ASIO measurement is hosted by RtAudio. Buffer size " +
            std::to_wstring(bufferSize_) +
            L" frames, input channel " + std::to_wstring(settings.micInputChannel) +
            L" of " + std::to_wstring(deviceInfo.inputChannels) +
            L", output channels " + std::to_wstring(settings.leftOutputChannel) +
            L" / " + std::to_wstring(settings.rightOutputChannel) +
            L" of " + std::to_wstring(deviceInfo.outputChannels) + L".";
        return true;
    }

    void poll(AudioLevels& levels) override {
        levels.currentAmplitudeDb = currentAmplitudeDb_.load();
        levels.peakAmplitudeDb = peakAmplitudeDb_.load();
    }

    bool playbackDone() const override {
        return continuousAlignment_ ? false : playbackDone_.load();
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
        SessionDetails details = sessionDetails_;
        details.routingNotes += L" Callbacks " + std::to_wstring(callbackCount_.load()) + L".";
        if (!lastRtAudioError_.empty()) {
            details.routingNotes += L" Last RtAudio message: " + toWide(lastRtAudioError_) + L".";
        }
        return details;
    }

    void stop(AudioLevels& levels) override {
        if (closed_.exchange(true)) {
            levels.currentAmplitudeDb = currentAmplitudeDb_.load();
            levels.peakAmplitudeDb = peakAmplitudeDb_.load();
            return;
        }

        if (audio_) {
            if (audio_->isStreamRunning()) {
                audio_->stopStream();
            }
            if (audio_->isStreamOpen()) {
                audio_->closeStream();
            }
            audio_.reset();
        }
        playbackDone_.store(true);
        levels.currentAmplitudeDb = currentAmplitudeDb_.load();
        levels.peakAmplitudeDb = peakAmplitudeDb_.load();
    }

    bool consumeCompletedAlignmentCycle(std::vector<int16_t>& capturedSamples) override {
        if (!continuousAlignment_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(cycleMutex_);
        if (!completedCycleReady_) {
            return false;
        }
        capturedSamples = completedCycleSamples_;
        completedCycleReady_ = false;
        return true;
    }

private:
    unsigned int resolveDeviceId(std::wstring& errorMessage) {
        const std::vector<unsigned int> deviceIds = audio_->getDeviceIds();
        if (deviceIds.empty()) {
            errorMessage = L"RtAudio did not find any ASIO devices.";
            return 0;
        }

        const std::string requested = lowerAscii(driverName_);
        unsigned int fallbackId = 0;
        for (const unsigned int id : deviceIds) {
            const RtAudio::DeviceInfo info = audio_->getDeviceInfo(id);
            if (info.ID == 0 || info.name.empty()) {
                continue;
            }
            if (fallbackId == 0) {
                fallbackId = id;
            }
            const std::string candidate = lowerAscii(info.name);
            if (candidate == requested ||
                candidate.find(requested) != std::string::npos ||
                requested.find(candidate) != std::string::npos) {
                return id;
            }
        }

        if (deviceIds.size() == 1 && fallbackId != 0) {
            return fallbackId;
        }

        std::wstring available;
        for (const unsigned int id : deviceIds) {
            const RtAudio::DeviceInfo info = audio_->getDeviceInfo(id);
            if (!info.name.empty()) {
                if (!available.empty()) {
                    available += L", ";
                }
                available += toWide(info.name);
            }
        }
        errorMessage = L"RtAudio could not match selected ASIO driver '" +
                       toWide(driverName_) +
                       L"'. Available RtAudio ASIO devices: " + available;
        return 0;
    }

    static int audioCallback(void* outputBuffer,
                             void* inputBuffer,
                             unsigned int frameCount,
                             double,
                             RtAudioStreamStatus,
                             void* userData) {
        auto* session = static_cast<RtAudioAsioMeasurementSession*>(userData);
        if (session == nullptr) {
            return 0;
        }
        return session->processAudio(outputBuffer, inputBuffer, frameCount);
    }

    int processAudio(void* outputBuffer, void* inputBuffer, unsigned int frameCount) {
        if (closed_.load()) {
            return 0;
        }

        callbackCount_.fetch_add(1);

        auto* output = static_cast<float*>(outputBuffer);
        if (output != nullptr) {
            std::fill(output, output + (static_cast<size_t>(frameCount) * outputChannelCount_), 0.0f);
            for (unsigned int frame = 0; frame < frameCount; ++frame) {
                int16_t leftSample = 0;
                int16_t rightSample = 0;
                const size_t playbackFrame = renderedFrames_.fetch_add(1);
                if (continuousAlignment_ && totalFrames_ > 0) {
                    const size_t cycleFrame = playbackFrame % totalFrames_;
                    const size_t baseIndex = cycleFrame * 2;
                    leftSample = playbackPcm_[baseIndex];
                    rightSample = playbackPcm_[baseIndex + 1];
                } else if (playbackFrame < totalFrames_) {
                    const size_t baseIndex = playbackFrame * 2;
                    leftSample = playbackPcm_[baseIndex];
                    rightSample = playbackPcm_[baseIndex + 1];
                } else {
                    playbackDone_.store(true);
                }

                const size_t base = static_cast<size_t>(frame) * outputChannelCount_;
                output[base + leftOutputOffset_] = static_cast<float>(pcm16ToFloat(leftSample));
                output[base + rightOutputOffset_] = static_cast<float>(pcm16ToFloat(rightSample));
            }
        }

        auto* input = static_cast<float*>(inputBuffer);
        if (input != nullptr) {
            std::vector<int16_t> frameSamples(frameCount, 0);
            for (unsigned int frame = 0; frame < frameCount; ++frame) {
                frameSamples[frame] = floatToPcm16(input[frame]);
            }

            if (continuousAlignment_) {
                std::lock_guard<std::mutex> lock(cycleMutex_);
                currentCycleSamples_.insert(currentCycleSamples_.end(), frameSamples.begin(), frameSamples.end());
                while (currentCycleSamples_.size() >= totalFrames_ && totalFrames_ > 0) {
                    completedCycleSamples_.assign(currentCycleSamples_.begin(),
                                                  currentCycleSamples_.begin() + static_cast<std::ptrdiff_t>(totalFrames_));
                    currentCycleSamples_.erase(currentCycleSamples_.begin(),
                                               currentCycleSamples_.begin() + static_cast<std::ptrdiff_t>(totalFrames_));
                    completedCycleReady_ = true;
                }
            } else {
                capturedSamples_.insert(capturedSamples_.end(), frameSamples.begin(), frameSamples.end());
            }

            const double currentDb = measurement::amplitudeDbFromPcm16(frameSamples.data(), frameCount);
            currentAmplitudeDb_.store(currentDb);
            double peakDb = peakAmplitudeDb_.load();
            while (currentDb > peakDb && !peakAmplitudeDb_.compare_exchange_weak(peakDb, currentDb)) {
            }
        }

        return 0;
    }

    std::unique_ptr<RtAudio> audio_;
    std::string driverName_;
    std::string lastRtAudioError_;
    measurement::SweepPlaybackPlan playbackPlan_;
    std::vector<int16_t> playbackPcm_;
    std::vector<int16_t> capturedSamples_;
    SessionDetails sessionDetails_;
    std::atomic<double> currentAmplitudeDb_{-90.0};
    std::atomic<double> peakAmplitudeDb_{-90.0};
    std::atomic<size_t> callbackCount_{0};
    std::atomic<bool> playbackDone_{false};
    std::atomic<bool> closed_{false};
    std::atomic<size_t> renderedFrames_{0};
    std::mutex cycleMutex_;
    std::vector<int16_t> currentCycleSamples_;
    std::vector<int16_t> completedCycleSamples_;
    bool completedCycleReady_ = false;
    bool continuousAlignment_ = false;
    int sampleRate_ = 44100;
    unsigned int bufferSize_ = 0;
    unsigned int outputFirstChannel_ = 0;
    unsigned int outputChannelCount_ = 2;
    unsigned int leftOutputOffset_ = 0;
    unsigned int rightOutputOffset_ = 1;
    size_t totalFrames_ = 0;
};

class AsioAudioBackend final : public IAudioBackend {
public:
    std::unique_ptr<IAudioMeasurementSession> startSession(const AudioSettings& settings,
                                                           const MeasurementSettings& measurementSettings,
                                                           MeasurementRunMode runMode,
                                                           std::wstring& errorMessage) override {
        auto session = std::make_unique<RtAudioAsioMeasurementSession>();
        if (!session->open(settings, measurementSettings, runMode, errorMessage)) {
            return nullptr;
        }
        return session;
    }
};

class DefaultAudioBackend final : public IAudioBackend {
public:
    DefaultAudioBackend()
        : asioBackend_(createAsioAudioBackend()),
          wasapiBackend_(createWasapiAudioBackend()) {}

    std::unique_ptr<IAudioMeasurementSession> startSession(const AudioSettings& settings,
                                                           const MeasurementSettings& measurementSettings,
                                                           MeasurementRunMode runMode,
                                                           std::wstring& errorMessage) override {
        if (settings.backend == "asio") {
            return asioBackend_->startSession(settings, measurementSettings, runMode, errorMessage);
        }
        return wasapiBackend_->startSession(settings, measurementSettings, runMode, errorMessage);
    }

private:
    std::unique_ptr<IAudioBackend> asioBackend_;
    std::unique_ptr<IAudioBackend> wasapiBackend_;
};

}  // namespace

std::unique_ptr<IAudioBackend> createAsioAudioBackend() {
    return std::make_unique<AsioAudioBackend>();
}

std::unique_ptr<IAudioBackend> createDefaultAudioBackend() {
    return std::make_unique<DefaultAudioBackend>();
}

}  // namespace wolfie::audio
