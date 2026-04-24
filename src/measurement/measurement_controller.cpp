#include "measurement/measurement_controller.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

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

}  // namespace

MeasurementController::MeasurementController(std::unique_ptr<audio::IAudioBackend> backend)
    : backend_(std::move(backend)) {}

MeasurementController::~MeasurementController() {
    cancel();
}

void MeasurementController::resetState() {
    result_ = {};
    status_ = {};
    startTickMs_ = 0;
    durationMs_ = 0;
    playbackPlan_ = {};
    loopbackCalibration_ = false;
}

bool MeasurementController::start(const WorkspaceState& workspace) {
    return startInternal(workspace, false);
}

bool MeasurementController::startLoopbackCalibration(const WorkspaceState& workspace) {
    return startInternal(workspace, true);
}

bool MeasurementController::startInternal(const WorkspaceState& workspace, bool loopbackCalibration) {
    cancel();
    resetState();

    loopbackCalibration_ = loopbackCalibration;
    snapshot_ = workspace;
    measurement::syncDerivedMeasurementSettings(snapshot_.measurement);

    const int sampleRate = std::max(8000, snapshot_.measurement.sampleRate);
    playbackPlan_ = loopbackCalibration_
                        ? measurement::buildLoopbackCalibrationPlaybackPlan(snapshot_.measurement,
                                                                            snapshot_.audio.outputVolumeDb)
                        : measurement::buildSweepPlaybackPlan(snapshot_.measurement,
                                                              snapshot_.audio.outputVolumeDb);

    const std::filesystem::path measurementDir = snapshot_.rootPath / "measurement";
    std::filesystem::create_directories(measurementDir);
    status_.generatedSweepPath = measurementDir / (loopbackCalibration_ ? "loopback-pulses.wav" : "logsweep.wav");
    measurement::writeStereoWaveFile(status_.generatedSweepPath, playbackPlan_.playbackPcm, sampleRate);

    std::wstring errorMessage;
    session_ = backend_->startSession(snapshot_.audio, playbackPlan_, sampleRate, errorMessage);
    if (!session_) {
        status_.lastErrorMessage = errorMessage;
        return false;
    }

    durationMs_ = static_cast<uint64_t>(
        std::ceil((static_cast<double>(playbackPlan_.totalFrames) * 1000.0) / static_cast<double>(sampleRate)));
    startTickMs_ = tickMillis();
    status_.running = true;
    status_.loopbackCalibration = loopbackCalibration_;
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
    const size_t rightSegmentStart = playbackPlan_.segmentFrames;
    const size_t rightSweepStart = rightSegmentStart + playbackPlan_.leadInFrames;
    const double sweepFrames = std::max(1.0, static_cast<double>(playbackPlan_.sweepFrames));

    if (elapsedFrames < rightSegmentStart) {
        status_.currentChannel = MeasurementChannel::Left;
        if (elapsedFrames <= leftSweepStart) {
            status_.progress = 0.0;
        } else {
            status_.progress = clampValue(static_cast<double>(elapsedFrames - leftSweepStart) / sweepFrames, 0.0, 1.0);
        }
    } else {
        status_.currentChannel = MeasurementChannel::Right;
        if (elapsedFrames <= rightSweepStart) {
            status_.progress = 0.0;
        } else {
            status_.progress = clampValue(static_cast<double>(elapsedFrames - rightSweepStart) / sweepFrames, 0.0, 1.0);
        }
    }

    if (elapsedFrames < playbackPlan_.leadInFrames) {
        status_.currentFrequencyHz = snapshot_.measurement.startFrequencyHz;
    } else if (elapsedFrames < playbackPlan_.segmentFrames) {
        status_.currentFrequencyHz = measurement::sweepFrequencyAtSample(snapshot_.measurement,
                                                                         session_->sampleRate(),
                                                                         elapsedFrames - playbackPlan_.leadInFrames,
                                                                         playbackPlan_.sweepFrames);
    } else if (elapsedFrames < rightSegmentStart + playbackPlan_.leadInFrames) {
        status_.currentFrequencyHz = snapshot_.measurement.startFrequencyHz;
    } else {
        const size_t rightSweepFrame = std::min(playbackPlan_.sweepFrames, elapsedFrames - rightSegmentStart - playbackPlan_.leadInFrames);
        status_.currentFrequencyHz = measurement::sweepFrequencyAtSample(snapshot_.measurement,
                                                                         session_->sampleRate(),
                                                                         rightSweepFrame,
                                                                         playbackPlan_.sweepFrames);
    }

    if (!session_->playbackDone() && elapsedMs < durationMs_) {
        return;
    }

    session_->stop(levels);
    status_.currentAmplitudeDb = levels.currentAmplitudeDb;
    status_.peakAmplitudeDb = levels.peakAmplitudeDb;

    if (loopbackCalibration_) {
        const measurement::LoopbackDelayEstimate estimate =
            measurement::estimateLoopbackDelayFromCapture(session_->capturedSamples(),
                                                          playbackPlan_.playedSweep,
                                                          playbackPlan_.leadInFrames,
                                                          session_->sampleRate(),
                                                          snapshot_.measurement);
        status_.measuredLoopbackLatencySamples = estimate.latencySamples;
        status_.loopbackClippingDetected = estimate.clippingDetected;
        status_.loopbackTooQuiet = estimate.tooQuiet;
        status_.loopbackPeakToNoiseDb = estimate.peakToNoiseDb;
        if (!estimate.success) {
            if (estimate.clippingDetected) {
                status_.lastErrorMessage = L"Loopback calibration failed because the input clipped. Lower the output level and run loopback again.";
            } else if (estimate.tooQuiet) {
                status_.lastErrorMessage = L"Loopback calibration failed because the input level was too low.";
            } else {
                status_.lastErrorMessage = L"Loopback calibration did not find a clean pulse-train timing peak.";
            }
        }
    } else {
        result_ = measurement::buildMeasurementResultFromCapture(session_->capturedSamples(),
                                                                 playbackPlan_.playedSweep,
                                                                 playbackPlan_.leadInFrames,
                                                                 session_->sampleRate(),
                                                                 snapshot_.measurement);
    }

    session_.reset();
    status_.running = false;
    status_.finished = true;
    status_.loopbackCalibration = loopbackCalibration_;
    status_.progress = 1.0;
    status_.currentChannel = MeasurementChannel::None;
    status_.currentFrequencyHz = snapshot_.measurement.endFrequencyHz;
}

}  // namespace wolfie
