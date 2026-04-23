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
};

struct UiSettings {
    int measurementSectionHeight = 320;
    int resultSectionHeight = 360;
    double measurementGraphExtraRangeDb = 0.0;
    double smoothingGraphExtraRangeDb = 0.0;
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

struct WorkspaceState {
    std::filesystem::path rootPath;
    AudioSettings audio;
    MeasurementSettings measurement;
    ResponseSmoothingSettings smoothing;
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
    double progress = 0.0;
    MeasurementChannel currentChannel = MeasurementChannel::None;
    double currentFrequencyHz = 0.0;
    double currentAmplitudeDb = -90.0;
    double peakAmplitudeDb = -90.0;
    std::filesystem::path generatedSweepPath;
    std::wstring lastErrorMessage;
};

}  // namespace wolfie
