#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wolfie {

enum class MeasurementChannel {
    None,
    Left,
    Right
};

struct AudioSettings {
    std::string driver = "ASIO driver";
    int micInputChannel = 1;
    int leftOutputChannel = 1;
    int rightOutputChannel = 2;
    double outputVolumeDb = -30.0;
};

struct MeasurementSettings {
    int sampleRate = 44100;
    double fadeInSeconds = 0.5;
    double fadeOutSeconds = 0.1;
    double durationSeconds = 60.0;
    double startFrequencyHz = 20.0;
    double endFrequencyHz = 22050.0;
    int targetLengthSamples = 65536;
    int leadInSamples = 6000;
    int loopbackLatencySamples = 0;
    int loopbackLatencySampleRate = 44100;
};

struct UiSettings {
    int measurementSectionHeight = 320;
    int resultSectionHeight = 360;
    int processLogHeight = 190;
    double measurementGraphExtraRangeDb = 0.0;
    double measurementGraphVerticalOffsetDb = 0.0;
    double smoothingGraphExtraRangeDb = 0.0;
    double smoothingGraphVerticalOffsetDb = 0.0;
    double targetCurveGraphExtraRangeDb = 0.0;
    double targetCurveGraphVerticalOffsetDb = 0.0;
};

struct MeasurementResult {
    std::vector<double> frequencyAxisHz;
    std::vector<double> leftChannelDb;
    std::vector<double> rightChannelDb;
};

struct ResponseSmoothingSettings {
    std::string psychoacousticModel = "ERB auditory smoothing";
    int resolutionPercent = 50;
    double lowFrequencyWindowCycles = 15.0;
    double highFrequencyWindowCycles = 15.0;
    double highFrequencySlopeCutoffHz = 21000.0;
};

struct SmoothedResponse {
    std::vector<double> frequencyAxisHz;
    std::vector<double> leftChannelDb;
    std::vector<double> rightChannelDb;
};

struct TargetEqBand {
    bool enabled = true;
    int colorIndex = 0;
    double frequencyHz = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
};

struct TargetCurveSettings {
    double lowGainDb = 0.0;
    double midFrequencyHz = 1000.0;
    double midGainDb = 0.0;
    double highGainDb = 0.0;
    bool bypassEqBands = false;
    std::vector<TargetEqBand> eqBands;
};

struct WorkspaceState {
    std::filesystem::path rootPath;
    AudioSettings audio;
    MeasurementSettings measurement;
    ResponseSmoothingSettings smoothing;
    TargetCurveSettings targetCurve;
    UiSettings ui;
    MeasurementResult result;
    SmoothedResponse smoothedResponse;
};

struct AppState {
    std::filesystem::path lastWorkspace;
    std::vector<std::filesystem::path> recentWorkspaces;
};

struct MeasurementStatus {
    bool running = false;
    bool finished = false;
    bool loopbackCalibration = false;
    double progress = 0.0;
    MeasurementChannel currentChannel = MeasurementChannel::None;
    double currentFrequencyHz = 0.0;
    double currentAmplitudeDb = -90.0;
    double peakAmplitudeDb = -90.0;
    int measuredLoopbackLatencySamples = 0;
    bool loopbackClippingDetected = false;
    bool loopbackTooQuiet = false;
    double loopbackPeakToNoiseDb = 0.0;
    std::filesystem::path generatedSweepPath;
    std::wstring lastErrorMessage;
};

}  // namespace wolfie
