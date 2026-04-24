#pragma once

#include <memory>

#include "audio/audio_backend.h"
#include "core/models.h"
#include "measurement/sweep_generator.h"

namespace wolfie {

class MeasurementController {
public:
    explicit MeasurementController(std::unique_ptr<audio::IAudioBackend> backend);
    ~MeasurementController();

    bool start(const WorkspaceState& workspace);
    bool startLoopbackCalibration(const WorkspaceState& workspace);
    void cancel();
    void tick();

    [[nodiscard]] const MeasurementStatus& status() const { return status_; }
    [[nodiscard]] const MeasurementResult& result() const { return result_; }

private:
    void resetState();
    bool startInternal(const WorkspaceState& workspace, bool loopbackCalibration);

    std::unique_ptr<audio::IAudioBackend> backend_;
    std::unique_ptr<audio::IAudioMeasurementSession> session_;
    WorkspaceState snapshot_;
    measurement::SweepPlaybackPlan playbackPlan_;
    MeasurementResult result_;
    MeasurementStatus status_;
    uint64_t startTickMs_ = 0;
    uint64_t durationMs_ = 0;
    bool loopbackCalibration_ = false;
};

}  // namespace wolfie
