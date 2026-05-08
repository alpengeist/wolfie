#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/models.h"
#include "measurement/sweep_generator.h"

namespace wolfie::audio {

struct AudioLevels {
    double currentAmplitudeDb = -90.0;
    double peakAmplitudeDb = -90.0;
};

struct SessionDetails {
    std::string backendName;
    std::wstring inputDeviceName;
    std::wstring outputDeviceName;
    bool routingSelectionHonored = false;
    std::wstring routingNotes;
};

class IAudioMeasurementSession {
public:
    virtual ~IAudioMeasurementSession() = default;

    virtual void poll(AudioLevels& levels) = 0;
    virtual bool playbackDone() const = 0;
    virtual const measurement::SweepPlaybackPlan& playbackPlan() const = 0;
    virtual const std::vector<double>& capturedSamples() const = 0;
    virtual int sampleRate() const = 0;
    virtual SessionDetails details() const = 0;
    virtual void stop(AudioLevels& levels) = 0;
    virtual bool consumeCompletedAlignmentCycle(std::vector<double>& capturedSamples) {
        (void)capturedSamples;
        return false;
    }
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual std::unique_ptr<IAudioMeasurementSession> startSession(const AudioSettings& settings,
                                                                   const MeasurementSettings& measurementSettings,
                                                                   MeasurementRunMode runMode,
                                                                   std::wstring& errorMessage) = 0;
};

}  // namespace wolfie::audio
