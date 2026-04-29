#include "measurement/measurement_controller.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include <windows.h>

#include "core/text_utils.h"
#include "measurement/response_analyzer.h"

namespace wolfie {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

uint64_t tickMillis() {
    return GetTickCount64();
}

std::string currentUtcTimestamp() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(4) << time.wYear << '-'
        << std::setw(2) << time.wMonth << '-'
        << std::setw(2) << time.wDay << 'T'
        << std::setw(2) << time.wHour << ':'
        << std::setw(2) << time.wMinute << ':'
        << std::setw(2) << time.wSecond << 'Z';
    return out.str();
}

}  // namespace

MeasurementController::MeasurementController(std::unique_ptr<audio::IAudioBackend> backend)
    : backend_(std::move(backend)) {}

MeasurementController::~MeasurementController() {
    cancel();
}

void MeasurementController::resetState() {
    result_ = {};
    status_ = {};
    status_.runMode = MeasurementRunMode::Room;
    startTickMs_ = 0;
    durationMs_ = 0;
    runMode_ = MeasurementRunMode::Room;
    activeMeasurementSettings_ = {};
    playbackPlan_ = {};
    measurementTimestampUtc_.clear();
}

bool MeasurementController::start(const WorkspaceState& workspace, MeasurementRunMode runMode) {
    cancel();
    resetState();

    snapshot_ = workspace;
    runMode_ = runMode;
    status_.runMode = runMode_;
    measurement::syncDerivedMeasurementSettings(snapshot_.measurement);
    if (runMode_ == MeasurementRunMode::Reference) {
        if (!snapshot_.audio.loopbackEnabled) {
            status_.lastErrorMessage = L"Loopback reference routing is not enabled in Measurement Settings.";
            return false;
        }
        snapshot_.audio.micInputChannel = snapshot_.audio.loopbackInputChannel;
        snapshot_.audio.microphoneCalibrationPath.clear();
        snapshot_.audio.microphoneCalibrationFrequencyHz.clear();
        snapshot_.audio.microphoneCalibrationCorrectionDb.clear();
    }

    measurementTimestampUtc_ = currentUtcTimestamp();

    std::wstring errorMessage;
    session_ = backend_->startSession(snapshot_.audio, snapshot_.measurement, runMode_, errorMessage);
    if (!session_) {
        status_.lastErrorMessage = errorMessage;
        return false;
    }

    playbackPlan_ = session_->playbackPlan();
    activeMeasurementSettings_ = snapshot_.measurement;
    activeMeasurementSettings_.sampleRate = session_->sampleRate();
    measurement::syncDerivedMeasurementSettings(activeMeasurementSettings_);

    const std::filesystem::path measurementDir = snapshot_.rootPath / "measurement";
    std::filesystem::create_directories(measurementDir);
    status_.generatedSweepPath = measurementDir /
                                 (runMode_ == MeasurementRunMode::Reference ? "reference-logsweep.wav" : "logsweep.wav");
    measurement::writeStereoWaveFile(status_.generatedSweepPath, playbackPlan_.playbackPcm, session_->sampleRate());

    durationMs_ = static_cast<uint64_t>(
        std::ceil((static_cast<double>(playbackPlan_.totalFrames) * 1000.0) / static_cast<double>(session_->sampleRate())));
    startTickMs_ = tickMillis();
    status_.running = true;
    status_.currentChannel = MeasurementChannel::Left;
    return true;
}

void MeasurementController::cancel() {
    if (session_) {
        audio::AudioLevels levels{status_.currentAmplitudeDb, status_.peakAmplitudeDb};
        session_->stop(levels);
        session_.reset();
    }
    resetState();
}

void MeasurementController::tick() {
    if (!status_.running || !session_) {
        return;
    }

    audio::AudioLevels levels{status_.currentAmplitudeDb, status_.peakAmplitudeDb};
    session_->poll(levels);
    status_.currentAmplitudeDb = levels.currentAmplitudeDb;
    status_.peakAmplitudeDb = levels.peakAmplitudeDb;

    const uint64_t elapsedMs = tickMillis() - startTickMs_;
    const size_t elapsedFrames = std::min(
        playbackPlan_.totalFrames,
        static_cast<size_t>((elapsedMs * static_cast<uint64_t>(session_->sampleRate())) / 1000ULL));
    const size_t leftSweepStart = playbackPlan_.leadInFrames;
    const double sweepFrames = std::max(1.0, static_cast<double>(playbackPlan_.sweepFrames));

    if (playbackPlan_.channelSweepCount < 2 || elapsedFrames < playbackPlan_.segmentFrames) {
        status_.currentChannel = MeasurementChannel::Left;
        if (elapsedFrames <= leftSweepStart) {
            status_.progress = 0.0;
        } else {
            status_.progress = clampValue(static_cast<double>(elapsedFrames - leftSweepStart) / sweepFrames, 0.0, 1.0);
        }
    } else {
        const size_t rightSegmentStart = playbackPlan_.segmentFrames;
        const size_t rightSweepStart = rightSegmentStart + playbackPlan_.leadInFrames;
        status_.currentChannel = MeasurementChannel::Right;
        if (elapsedFrames <= rightSweepStart) {
            status_.progress = 0.0;
        } else {
            status_.progress = clampValue(static_cast<double>(elapsedFrames - rightSweepStart) / sweepFrames, 0.0, 1.0);
        }
    }

    if (elapsedFrames < playbackPlan_.leadInFrames) {
        status_.currentFrequencyHz = activeMeasurementSettings_.startFrequencyHz;
    } else if (elapsedFrames < playbackPlan_.segmentFrames) {
        status_.currentFrequencyHz = measurement::sweepFrequencyAtSample(activeMeasurementSettings_,
                                                                         session_->sampleRate(),
                                                                         elapsedFrames - playbackPlan_.leadInFrames,
                                                                         playbackPlan_.sweepFrames);
    } else if (playbackPlan_.channelSweepCount < 2) {
        status_.currentFrequencyHz = activeMeasurementSettings_.endFrequencyHz;
    } else if (elapsedFrames < playbackPlan_.segmentFrames + playbackPlan_.leadInFrames) {
        status_.currentFrequencyHz = activeMeasurementSettings_.startFrequencyHz;
    } else {
        const size_t rightSegmentStart = playbackPlan_.segmentFrames;
        const size_t rightSweepFrame = std::min(playbackPlan_.sweepFrames, elapsedFrames - rightSegmentStart - playbackPlan_.leadInFrames);
        status_.currentFrequencyHz = measurement::sweepFrequencyAtSample(activeMeasurementSettings_,
                                                                         session_->sampleRate(),
                                                                         rightSweepFrame,
                                                                         playbackPlan_.sweepFrames);
    }

    if (!session_->playbackDone() && elapsedMs < durationMs_) {
        return;
    }

    const audio::SessionDetails sessionDetails = session_->details();
    session_->stop(levels);
    status_.currentAmplitudeDb = levels.currentAmplitudeDb;
    status_.peakAmplitudeDb = levels.peakAmplitudeDb;

    const std::filesystem::path measurementDir = snapshot_.rootPath / "measurement";
    std::filesystem::create_directories(measurementDir);
    const std::filesystem::path rawCapturePath =
        measurementDir / (runMode_ == MeasurementRunMode::Reference ? "reference-raw-capture.wav" : "raw-capture.wav");
    measurement::writeMonoWaveFile(rawCapturePath, session_->capturedSamples(), session_->sampleRate());

    result_ = measurement::buildMeasurementResultFromCapture(session_->capturedSamples(),
                                                             playbackPlan_,
                                                             session_->sampleRate(),
                                                             snapshot_.audio,
                                                             activeMeasurementSettings_);
    result_.analysis.measurementKind = runMode_ == MeasurementRunMode::Reference ? "reference" : "room";
    result_.analysis.measurementTimestampUtc = measurementTimestampUtc_;
    result_.analysis.backendName = sessionDetails.backendName;
    result_.analysis.backendInputDevice = toUtf8(sessionDetails.inputDeviceName);
    result_.analysis.backendOutputDevice = toUtf8(sessionDetails.outputDeviceName);
    result_.analysis.routingSelectionHonored = sessionDetails.routingSelectionHonored;
    result_.analysis.routingNotes = toUtf8(sessionDetails.routingNotes);
    result_.analysis.artifacts.push_back({"generated_sweep_wav", status_.generatedSweepPath});
    result_.analysis.artifacts.push_back({"raw_capture_wav", rawCapturePath});
    result_.analysis.artifacts.push_back(
        {"result_values_txt",
         measurementDir / (runMode_ == MeasurementRunMode::Reference ? "reference-result-values.txt" : "result-values.txt")});
    result_.analysis.artifacts.push_back(
        {"analysis_json",
         measurementDir / (runMode_ == MeasurementRunMode::Reference ? "reference-analysis.json" : "analysis.json")});

    session_.reset();
    status_.running = false;
    status_.finished = true;
    status_.progress = 1.0;
    status_.currentChannel = MeasurementChannel::None;
    status_.currentFrequencyHz = activeMeasurementSettings_.endFrequencyHz;
}

}  // namespace wolfie
