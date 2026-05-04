#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wolfie {

enum class MeasurementChannel {
    None,
    Left,
    Right
};

enum class MeasurementRunMode {
    Room,
    Reference
};

struct AudioSettings {
    std::string backend = "windows";
    std::string driver = "ASIO driver";
    std::string windowsInputDeviceId;
    std::string windowsInputDeviceName;
    std::string windowsOutputDeviceId;
    std::string windowsOutputDeviceName;
    int micInputChannel = 1;
    bool loopbackEnabled = false;
    int loopbackInputChannel = 1;
    int leftOutputChannel = 1;
    int rightOutputChannel = 2;
    double outputVolumeDb = -30.0;
    std::filesystem::path microphoneCalibrationPath;
    std::vector<double> microphoneCalibrationFrequencyHz;
    std::vector<double> microphoneCalibrationCorrectionDb;
};

struct MeasurementSettings {
    int sampleRate = 44100;
    double fadeInSeconds = 0.05;
    double fadeOutSeconds = 0.1;
    double durationSeconds = 20.0;
    double startFrequencyHz = 20.0;
    double endFrequencyHz = 22050.0;
    int targetLengthSamples = 65536;
    int leadInSamples = 6000;
};

enum class ProcessLogSize {
    Compact,
    Medium,
    Expanded
};

struct UiSettings {
    int measurementSectionHeight = 320;
    int resultSectionHeight = 360;
    ProcessLogSize processLogSize = ProcessLogSize::Medium;
    double measurementGraphExtraRangeDb = 0.0;
    double measurementGraphVerticalOffsetDb = 0.0;
    bool measurementGraphHasCustomFrequencyRange = false;
    double measurementGraphVisibleMinFrequencyHz = 20.0;
    double measurementGraphVisibleMaxFrequencyHz = 20000.0;
    std::string measurementPlotMode = "magnitude";
    std::string measurementWaterfallSource = "measured";
    std::string measurementWaterfallChannel = "left";
    double measurementWaterfallLowCutoffDb = -72.0;
    bool measurementShowRoomLeft = true;
    bool measurementShowRoomRight = true;
    bool measurementShowReference = true;
    bool measurementMetadataCollapsed = true;
    std::string analysisWindow = "direct";
    bool analysisGraphHasCustomFrequencyRange = false;
    double analysisGraphVisibleMinFrequencyHz = 20.0;
    double analysisGraphVisibleMaxFrequencyHz = 20000.0;
    double smoothingGraphExtraRangeDb = 0.0;
    double smoothingGraphVerticalOffsetDb = 0.0;
    bool smoothingGraphHasCustomFrequencyRange = false;
    double smoothingGraphVisibleMinFrequencyHz = 20.0;
    double smoothingGraphVisibleMaxFrequencyHz = 20000.0;
    double targetCurveGraphExtraRangeDb = 0.0;
    double targetCurveGraphVerticalOffsetDb = 0.0;
    bool targetCurveGraphHasCustomVisibleDbRange = false;
    double targetCurveGraphVisibleMinDb = -12.0;
    double targetCurveGraphVisibleMaxDb = 12.0;
    bool filterShowInputRight = true;
    bool filterShowInputLeft = true;
    bool filterShowInversionRight = true;
    bool filterShowInversionLeft = true;
    bool filterShowCorrectedInputLeft = true;
    bool filterShowCorrectedInputRight = true;
    bool filterShowCorrectedLeft = true;
    bool filterShowCorrectedRight = true;
    bool filterShowExcessPhaseInputRight = true;
    bool filterShowExcessPhaseInputLeft = true;
    bool filterShowExcessPhasePredictedRight = true;
    bool filterShowExcessPhasePredictedLeft = true;
    bool filterShowInputGroupDelayLeft = true;
    bool filterShowInputGroupDelayRight = true;
    bool filterShowPredictedGroupDelayRight = true;
    bool filterShowPredictedGroupDelayLeft = true;
    bool filterShowFilterGroupDelayLeft = true;
    bool filterShowFilterGroupDelayRight = true;
    bool filterAlignGroupDelayLatency = false;
    int filterGroupDelayZoomPreset = 5;
    bool filterSyncHoverFrequency = false;
    bool exportSampleRatesCustomized = false;
    std::vector<int> exportSampleRatesHz;
};

struct MeasurementValueSet {
    std::string key;
    std::string xQuantity = "frequency";
    std::string xUnit = "Hz";
    std::string yQuantity = "level";
    std::string yUnit = "dB";
    std::vector<double> xValues;
    std::vector<double> leftValues;
    std::vector<double> rightValues;

    [[nodiscard]] bool valid() const {
        return !xValues.empty() &&
               leftValues.size() == xValues.size() &&
               rightValues.size() == xValues.size();
    }
};

struct MeasurementArtifact {
    std::string key;
    std::filesystem::path path;
};

struct MeasurementChannelMetrics {
    bool available = false;
    int detectedLatencySamples = 0;
    int onsetSampleIndex = 0;
    double onsetTimeSeconds = 0.0;
    int peakSampleIndex = 0;
    int impulseStartSample = 0;
    int impulseLengthSamples = 0;
    int preRollSamples = 0;
    int analysisWindowStartSample = 0;
    int analysisWindowLengthSamples = 0;
    int analysisWindowFadeSamples = 0;
    double capturePeakDb = -90.0;
    double captureRmsDb = -90.0;
    double noiseFloorDb = -90.0;
    double impulsePeakAmplitude = 0.0;
    double impulsePeakDb = -90.0;
    double impulseRmsDb = -90.0;
    double impulsePeakToNoiseDb = 0.0;
};

struct MeasurementAnalysis {
    std::string analyzerVersion = "ir-v2";
    std::string measurementKind = "room";
    std::string measurementTimestampUtc;
    std::string backendName;
    std::string backendInputDevice;
    std::string backendOutputDevice;
    std::string requestedBackend;
    std::string requestedDriver;
    std::string requestedWindowsInputDeviceId;
    std::string requestedWindowsInputDeviceName;
    std::string requestedWindowsOutputDeviceId;
    std::string requestedWindowsOutputDeviceName;
    int requestedMicInputChannel = 0;
    int requestedLeftOutputChannel = 0;
    int requestedRightOutputChannel = 0;
    bool routingSelectionHonored = false;
    std::string routingNotes;
    int sampleRate = 0;
    double sweepDurationSeconds = 0.0;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    double startFrequencyHz = 0.0;
    double endFrequencyHz = 0.0;
    int targetLengthSamples = 0;
    int leadInSamples = 0;
    double outputVolumeDb = 0.0;
    int playedSweepSamples = 0;
    int capturedSamples = 0;
    int alignmentSearchSamples = 0;
    std::string alignmentMethod;
    std::string windowType;
    int inverseFilterLengthSamples = 0;
    int inverseFilterPeakIndex = 0;
    int fftSize = 0;
    int displayPointCount = 0;
    bool captureClippingDetected = false;
    bool captureTooQuiet = false;
    double capturePeakDb = -90.0;
    double captureRmsDb = -90.0;
    double captureNoiseFloorDb = -90.0;
    MeasurementChannelMetrics left;
    MeasurementChannelMetrics right;
    std::vector<MeasurementArtifact> artifacts;

    [[nodiscard]] const MeasurementArtifact* findArtifact(std::string_view key) const {
        for (const MeasurementArtifact& artifact : artifacts) {
            if (artifact.key == key) {
                return &artifact;
            }
        }
        return nullptr;
    }
};

struct MeasurementResult {
    std::vector<MeasurementValueSet> valueSets;
    MeasurementAnalysis analysis;

    [[nodiscard]] const MeasurementValueSet* findValueSet(std::string_view key) const {
        for (const MeasurementValueSet& valueSet : valueSets) {
            if (valueSet.key == key) {
                return &valueSet;
            }
        }
        return nullptr;
    }

    [[nodiscard]] MeasurementValueSet* findValueSet(std::string_view key) {
        for (MeasurementValueSet& valueSet : valueSets) {
            if (valueSet.key == key) {
                return &valueSet;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const MeasurementValueSet* magnitudeResponse() const {
        if (analysis.measurementKind == "reference") {
            if (const MeasurementValueSet* direct = findValueSet("measurement.direct_magnitude_response")) {
                if (direct->valid()) {
                    return direct;
                }
            }
        }
        if (const MeasurementValueSet* canonical = findValueSet("measurement.magnitude_response")) {
            if (canonical->valid()) {
                return canonical;
            }
        }
        if (const MeasurementValueSet* calibrated = findValueSet("measurement.calibrated_magnitude_response")) {
            if (calibrated->valid()) {
                return calibrated;
            }
        }
        if (const MeasurementValueSet* raw = findValueSet("measurement.raw_magnitude_response")) {
            if (raw->valid()) {
                return raw;
            }
        }
        if (const MeasurementValueSet* room = findValueSet("measurement.room_magnitude_response")) {
            if (room->valid()) {
                return room;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool hasAnyValues() const {
        return magnitudeResponse() != nullptr;
    }
};

struct ResponseSmoothingSettings {
    std::string psychoacousticModel = "ERB auditory smoothing";
    int resolutionPercent = 50;
    double lowFrequencyWindowCycles = 15.0;
    double highFrequencyWindowCycles = 15.0;
    double highFrequencySlopeCutoffHz = 21000.0;
};

struct SmoothedResponse {
    ResponseSmoothingSettings smoothingSettings;
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

struct TargetCurveProfile {
    std::string name = "Default";
    std::string comment;
    TargetCurveSettings curve;
};

struct RoomSimulationSettings {
    double stereoSkewMs = 0.25;
    double spectralTiltDbPerOctave = -0.75;
    double lowShelfGainDb = 5.0;
    double lowShelfCornerHz = 140.0;
    double modalPeakFrequencyHz = 52.0;
    double modalPeakGainDb = 6.0;
    double modalPeakQ = 8.0;
    double modalNullFrequencyHz = 88.0;
    double modalNullDepthDb = 10.0;
    double modalNullQ = 10.0;
    int earlyReflectionCount = 6;
    double earlyReflectionStartMs = 7.5;
    double earlyReflectionSpacingMs = 4.2;
    double earlyReflectionDecayDbPerTap = 2.2;
    double lateDecayRt60Ms = 420.0;
    double lateDecayStartDb = -22.0;
    double lateDensityPerSecond = 180.0;
    double noiseFloorDb = -72.0;
    int seed = 1;
};

struct RoomSimulationDefinition {
    std::string name;
    RoomSimulationSettings settings;
};

struct StereoDiagnosticsSummary {
    bool available = false;
    double delayMismatchMs = 0.0;
    double directImpulseCorrelation = 0.0;
    double lowBandPhaseRmsDegrees = 0.0;
    double midBandPhaseRmsDegrees = 0.0;
    double lowBandMagnitudeRmsDb = 0.0;
    double phaseSimilarity = 0.0;
    double iacc10 = 0.0;
    double iacc20 = 0.0;
    double iacc80 = 0.0;
    double iaccLate = 0.0;
};

struct StereoDiagnosticsResult {
    bool available = false;
    std::string window = "direct";
    StereoDiagnosticsSummary summary;
    std::vector<double> frequencyAxisHz;
    std::vector<double> phaseDeltaDegrees;
    std::vector<double> magnitudeDeltaDb;
};

struct FilterAnalysisResult {
    bool available = false;
    StereoDiagnosticsResult direct;
    StereoDiagnosticsResult room;
};

struct FilterDesignSettings {
    int tapCount = 65536;
    double maxBoostDb = 0.0;
    double maxCutDb = 12.0;
    double smoothness = 1.0;
    double lowCorrectionHz = 30.0;
    double lowTaperOctaves = 2.0;
    double highCorrectionHz = 12000.0;
    double highTaperOctaves = 1.25;
    int displayPointCount = 2048;
    std::string phaseMode = "minimum";
    double mixedPhaseMaxFrequencyHz = 220.0;
    double mixedPhaseStrength = 1.0;
    double mixedPhaseMaxCorrectionDegrees = 360.0;
};

struct FilterDesignChannelResult {
    std::vector<double> correctionCurveDb;
    std::vector<double> filterResponseDb;
    std::vector<double> correctedResponseDb;
    std::vector<double> inputGroupDelayMs;
    std::vector<double> groupDelayMs;
    std::vector<double> inputExcessPhaseDegrees;
    std::vector<double> inputExcessPhaseContinuousDegrees;
    std::vector<double> predictedExcessPhaseDegrees;
    std::vector<double> predictedExcessPhaseContinuousDegrees;
    std::vector<double> predictedGroupDelayMs;
    std::vector<double> impulseTimeMs;
    std::vector<double> filterTaps;
    int impulsePeakIndex = 0;
    double peakAmplitude = 0.0;
};

struct FilterDesignResult {
    bool valid = false;
    int sampleRate = 0;
    int tapCount = 0;
    int fftSize = 0;
    int positiveBinCount = 0;
    std::string phaseMode = "minimum";
    std::string phasePreparationSourceWindow;
    std::string phasePreparationSourceKey;
    std::string phasePreparationSeriesKind;
    double phasePreparationBulkDelaySeconds = 0.0;
    std::vector<std::string> processLog;
    std::vector<double> frequencyAxisHz;
    std::vector<double> targetCurveDb;
    FilterDesignChannelResult left;
    FilterDesignChannelResult right;
};

struct WorkspaceState {
    std::filesystem::path rootPath;
    AudioSettings audio;
    MeasurementSettings measurement;
    std::string activeRoomSimulationName;
    std::vector<RoomSimulationDefinition> roomSimulations;
    ResponseSmoothingSettings smoothing;
    TargetCurveSettings targetCurve;
    std::string activeTargetCurveProfileName = "Default";
    std::string activeTargetCurveComment;
    std::vector<TargetCurveProfile> targetCurveProfiles;
    FilterDesignSettings filters;
    UiSettings ui;
    MeasurementResult result;
    MeasurementResult referenceResult;
    SmoothedResponse smoothedResponse;
    FilterDesignResult filterResult;
    FilterAnalysisResult filterAnalysis;
};

struct AppState {
    std::filesystem::path lastWorkspace;
    std::vector<std::filesystem::path> recentWorkspaces;
};

struct MeasurementStatus {
    MeasurementRunMode runMode = MeasurementRunMode::Room;
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
