#include "audio/winmm_audio_backend.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include "measurement/response_analyzer.h"

namespace wolfie::audio {

namespace {

std::wstring formatMmError(MMRESULT result, std::wstring_view operation) {
    std::array<wchar_t, MAXERRORLENGTH> buffer{};
    const MMRESULT textResult = waveOutGetErrorTextW(result, buffer.data(), static_cast<UINT>(buffer.size()));

    std::wstring message = operation.empty() ? L"Audio operation failed" : std::wstring(operation);
    message += L": ";
    if (textResult == MMSYSERR_NOERROR) {
        message += buffer.data();
    } else {
        message += L"Unknown multimedia error";
    }
    return message;
}

std::wstring outputDeviceName(HWAVEOUT waveOut) {
    if (waveOut == nullptr) {
        return {};
    }

    UINT deviceId = 0;
    if (waveOutGetID(waveOut, &deviceId) != MMSYSERR_NOERROR) {
        return {};
    }

    WAVEOUTCAPSW caps{};
    if (waveOutGetDevCapsW(deviceId, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
        return {};
    }
    return caps.szPname;
}

std::wstring inputDeviceName(HWAVEIN waveIn) {
    if (waveIn == nullptr) {
        return {};
    }

    UINT deviceId = 0;
    if (waveInGetID(waveIn, &deviceId) != MMSYSERR_NOERROR) {
        return {};
    }

    WAVEINCAPSW caps{};
    if (waveInGetDevCapsW(deviceId, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
        return {};
    }
    return caps.szPname;
}

class WinMmMeasurementSession final : public IAudioMeasurementSession {
public:
    struct CaptureBuffer {
        WAVEHDR header{};
        std::vector<int16_t> samples;
    };

    ~WinMmMeasurementSession() override {
        AudioLevels levels{};
        stop(levels);
    }

    void poll(AudioLevels& levels) override {
        if (waveIn_ == nullptr) {
            return;
        }

        for (auto& buffer : captureBuffers_) {
            if ((buffer.header.dwFlags & WHDR_DONE) == 0) {
                continue;
            }

            const size_t sampleCount = buffer.header.dwBytesRecorded / sizeof(int16_t);
            if (sampleCount > 0) {
                capturedSamples_.insert(capturedSamples_.end(), buffer.samples.begin(), buffer.samples.begin() + sampleCount);
                levels.currentAmplitudeDb = measurement::amplitudeDbFromPcm16(buffer.samples.data(), sampleCount);
                levels.peakAmplitudeDb = std::max(levels.peakAmplitudeDb, levels.currentAmplitudeDb);
            }

            buffer.header.dwBytesRecorded = 0;
            buffer.header.dwFlags &= ~WHDR_DONE;
            if (inputActive_) {
                waveInAddBuffer(waveIn_, &buffer.header, sizeof(buffer.header));
            }
        }
    }

    bool playbackDone() const override {
        return (playbackHeader_.dwFlags & WHDR_DONE) != 0;
    }

    const std::vector<int16_t>& capturedSamples() const override {
        return capturedSamples_;
    }

    int sampleRate() const override {
        return sampleRate_;
    }

    SessionDetails details() const override {
        SessionDetails sessionDetails;
        sessionDetails.backendName = "WinMM";
        sessionDetails.inputDeviceName = inputDeviceName(waveIn_);
        sessionDetails.outputDeviceName = outputDeviceName(waveOut_);
        sessionDetails.routingSelectionHonored = false;
        sessionDetails.routingNotes = L"WinMM measurement uses the current default stereo output and mono input. "
                                      L"The selected ASIO driver and channel numbers are recorded as requested routing only.";
        return sessionDetails;
    }

    void stop(AudioLevels& levels) override {
        if (closed_) {
            return;
        }

        inputActive_ = false;
        if (waveIn_ != nullptr) {
            waveInStop(waveIn_);
            waveInReset(waveIn_);
            poll(levels);
        }
        close();
        closed_ = true;
    }

    bool open(const measurement::SweepPlaybackPlan& playbackPlan, int sampleRate, std::wstring& errorMessage) {
        sampleRate_ = sampleRate;
        playbackPcm_ = playbackPlan.playbackPcm;

        WAVEFORMATEX outputFormat{};
        outputFormat.wFormatTag = WAVE_FORMAT_PCM;
        outputFormat.nChannels = 2;
        outputFormat.nSamplesPerSec = sampleRate;
        outputFormat.wBitsPerSample = 16;
        outputFormat.nBlockAlign = outputFormat.nChannels * outputFormat.wBitsPerSample / 8;
        outputFormat.nAvgBytesPerSec = outputFormat.nSamplesPerSec * outputFormat.nBlockAlign;

        MMRESULT mmResult = waveOutOpen(&waveOut_, WAVE_MAPPER, &outputFormat, 0, 0, CALLBACK_NULL);
        if (mmResult != MMSYSERR_NOERROR) {
            errorMessage = formatMmError(mmResult, L"Could not open the speaker output");
            close();
            return false;
        }

        playbackHeader_.lpData = reinterpret_cast<LPSTR>(playbackPcm_.data());
        playbackHeader_.dwBufferLength = static_cast<DWORD>(playbackPcm_.size() * sizeof(int16_t));
        mmResult = waveOutPrepareHeader(waveOut_, &playbackHeader_, sizeof(playbackHeader_));
        if (mmResult != MMSYSERR_NOERROR) {
            errorMessage = formatMmError(mmResult, L"Could not prepare the speaker buffer");
            close();
            return false;
        }

        WAVEFORMATEX inputFormat{};
        inputFormat.wFormatTag = WAVE_FORMAT_PCM;
        inputFormat.nChannels = 1;
        inputFormat.nSamplesPerSec = sampleRate;
        inputFormat.wBitsPerSample = 16;
        inputFormat.nBlockAlign = inputFormat.nChannels * inputFormat.wBitsPerSample / 8;
        inputFormat.nAvgBytesPerSec = inputFormat.nSamplesPerSec * inputFormat.nBlockAlign;

        mmResult = waveInOpen(&waveIn_, WAVE_MAPPER, &inputFormat, 0, 0, CALLBACK_NULL);
        if (mmResult != MMSYSERR_NOERROR) {
            errorMessage = formatMmError(mmResult, L"Could not open the microphone input");
            close();
            return false;
        }

        const size_t captureBufferFrames = std::clamp(static_cast<size_t>(sampleRate / 20), size_t{1024}, size_t{8192});
        captureBuffers_.resize(6);
        capturedSamples_.reserve(playbackPlan.totalFrames + (captureBufferFrames * captureBuffers_.size()));
        for (auto& buffer : captureBuffers_) {
            buffer.samples.assign(captureBufferFrames, 0);
            buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
            buffer.header.dwBufferLength = static_cast<DWORD>(buffer.samples.size() * sizeof(int16_t));

            mmResult = waveInPrepareHeader(waveIn_, &buffer.header, sizeof(buffer.header));
            if (mmResult != MMSYSERR_NOERROR) {
                errorMessage = formatMmError(mmResult, L"Could not prepare the microphone buffer");
                close();
                return false;
            }

            mmResult = waveInAddBuffer(waveIn_, &buffer.header, sizeof(buffer.header));
            if (mmResult != MMSYSERR_NOERROR) {
                errorMessage = formatMmError(mmResult, L"Could not queue the microphone buffer");
                close();
                return false;
            }
        }

        mmResult = waveInStart(waveIn_);
        if (mmResult != MMSYSERR_NOERROR) {
            errorMessage = formatMmError(mmResult, L"Could not start the microphone capture");
            close();
            return false;
        }
        inputActive_ = true;

        mmResult = waveOutWrite(waveOut_, &playbackHeader_, sizeof(playbackHeader_));
        if (mmResult != MMSYSERR_NOERROR) {
            errorMessage = formatMmError(mmResult, L"Could not start the sweep playback");
            close();
            return false;
        }

        return true;
    }

private:
    void close() {
        inputActive_ = false;

        if (waveIn_ != nullptr) {
            waveInStop(waveIn_);
            waveInReset(waveIn_);
            for (auto& buffer : captureBuffers_) {
                if ((buffer.header.dwFlags & WHDR_PREPARED) != 0) {
                    waveInUnprepareHeader(waveIn_, &buffer.header, sizeof(buffer.header));
                }
            }
            waveInClose(waveIn_);
            waveIn_ = nullptr;
        }

        if (waveOut_ != nullptr) {
            waveOutReset(waveOut_);
            if ((playbackHeader_.dwFlags & WHDR_PREPARED) != 0) {
                waveOutUnprepareHeader(waveOut_, &playbackHeader_, sizeof(playbackHeader_));
            }
            waveOutClose(waveOut_);
            waveOut_ = nullptr;
        }
    }

    HWAVEOUT waveOut_ = nullptr;
    HWAVEIN waveIn_ = nullptr;
    std::vector<int16_t> playbackPcm_;
    WAVEHDR playbackHeader_{};
    std::vector<CaptureBuffer> captureBuffers_;
    std::vector<int16_t> capturedSamples_;
    int sampleRate_ = 44100;
    bool inputActive_ = false;
    bool closed_ = false;
};

class WinMmAudioBackend final : public IAudioBackend {
public:
    std::unique_ptr<IAudioMeasurementSession> startSession(const AudioSettings&,
                                                           const measurement::SweepPlaybackPlan& playbackPlan,
                                                           int sampleRate,
                                                           std::wstring& errorMessage) override {
        auto session = std::make_unique<WinMmMeasurementSession>();
        if (!session->open(playbackPlan, sampleRate, errorMessage)) {
            return nullptr;
        }
        return session;
    }
};

}  // namespace

std::unique_ptr<IAudioBackend> createWinMmAudioBackend() {
    return std::make_unique<WinMmAudioBackend>();
}

}  // namespace wolfie::audio
