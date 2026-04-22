#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace wolfie {

struct AudioSettings {
    std::string driver = "ASIO driver";
    int micInputChannel = 1;
    int leftOutputChannel = 1;
    int rightOutputChannel = 2;
    int sampleRate = 44100;
    double outputVolumeDb = -30.0;
};

struct MeasurementSettings {
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
};

struct MeasurementResult {
    std::vector<double> frequencyAxisHz;
    std::vector<double> leftChannelDb;
    std::vector<double> rightChannelDb;
};

struct WorkspaceState {
    std::filesystem::path rootPath;
    AudioSettings audio;
    MeasurementSettings measurement;
    UiSettings ui;
    MeasurementResult result;
};

struct AppState {
    std::filesystem::path lastWorkspace;
    std::vector<std::filesystem::path> recentWorkspaces;
};

class MeasurementEngine {
public:
    MeasurementEngine();
    ~MeasurementEngine();

    bool start(const WorkspaceState& workspace);
    void cancel();
    void tick();

    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] bool finished() const { return finished_; }
    [[nodiscard]] double progress() const { return progress_; }
    [[nodiscard]] double currentFrequencyHz() const { return currentFrequencyHz_; }
    [[nodiscard]] double currentAmplitudeDb() const { return currentAmplitudeDb_; }
    [[nodiscard]] double peakAmplitudeDb() const { return peakAmplitudeDb_; }
    [[nodiscard]] const std::filesystem::path& generatedSweepPath() const { return generatedSweepPath_; }
    [[nodiscard]] const MeasurementResult& result() const { return result_; }
    [[nodiscard]] std::wstring_view lastError() const { return lastErrorMessage_; }

private:
    struct Runtime;

    void cleanupRuntime();

    WorkspaceState snapshot_;
    MeasurementResult result_;
    bool running_ = false;
    bool finished_ = false;
    uint64_t startTickMs_ = 0;
    uint64_t durationMs_ = 0;
    double progress_ = 0.0;
    double currentFrequencyHz_ = 0.0;
    double currentAmplitudeDb_ = -90.0;
    double peakAmplitudeDb_ = -90.0;
    std::filesystem::path generatedSweepPath_;
    std::wstring lastErrorMessage_;
    std::unique_ptr<Runtime> runtime_;
};

class WolfieApp {
public:
    explicit WolfieApp(HINSTANCE instance);
    int run();

private:
    struct Controls {
        HWND sectionWorkspace = nullptr;
        HWND sectionMeasurement = nullptr;
        HWND workspacePath = nullptr;
        HWND labelFadeIn = nullptr;
        HWND labelFadeOut = nullptr;
        HWND labelDuration = nullptr;
        HWND labelStartFrequency = nullptr;
        HWND labelEndFrequency = nullptr;
        HWND labelTargetLength = nullptr;
        HWND labelLeadIn = nullptr;
        HWND editFadeIn = nullptr;
        HWND editFadeOut = nullptr;
        HWND editDuration = nullptr;
        HWND editStartFrequency = nullptr;
        HWND editEndFrequency = nullptr;
        HWND editTargetLength = nullptr;
        HWND editLeadIn = nullptr;
        HWND buttonMeasure = nullptr;
        HWND buttonStopMeasurement = nullptr;
        HWND statusText = nullptr;
        HWND progressText = nullptr;
        HWND currentFrequency = nullptr;
        HWND currentAmplitude = nullptr;
        HWND peakAmplitude = nullptr;
        HWND progressBar = nullptr;
        HWND resultGraph = nullptr;
    };

    enum class GraphKind {
        Response
    };

    static constexpr UINT_PTR kMeasurementTimerId = 101;
    static constexpr int kSectionSpacing = 16;
    static constexpr int kContentMargin = 20;
    static constexpr int kControlHeight = 24;
    static constexpr int kLabelWidth = 160;

    HINSTANCE instance_;
    HWND mainWindow_ = nullptr;
    HACCEL acceleratorTable_ = nullptr;
    Controls controls_;
    WorkspaceState workspace_;
    AppState appState_;
    MeasurementEngine measurementEngine_;
    bool measurementDirty_ = false;

    static LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK GraphWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void createMainWindow();
    void createMenus();
    void createLayout();
    void createSectionChrome(HWND parent, HWND& section, const wchar_t* title);
    void layoutMainWindow();
    void layoutContent();
    void showSettingsWindow();
    void populateControlsFromState();
    void syncStateFromControls();
    void refreshWorkspaceLabels();
    void refreshMeasurementStatus();
    void refreshRecentMenu();
    void onCommand(WORD commandId);
    void onTimer(UINT_PTR timerId);
    void onResize();
    void invalidateGraphs();

    void newWorkspace();
    void openWorkspace();
    void openWorkspace(const std::filesystem::path& path);
    void saveWorkspace(bool saveAs);
    void loadLastWorkspaceIfPossible();
    void touchRecentWorkspace(const std::filesystem::path& path);

    [[nodiscard]] std::optional<std::filesystem::path> pickFolder(bool createIfMissing) const;
    [[nodiscard]] std::filesystem::path appStatePath() const;
    void loadAppState();
    void saveAppState() const;
    void loadWorkspace(const std::filesystem::path& path);
    void saveWorkspaceFiles() const;

    void startMeasurement();
    void stopMeasurement();
    void finalizeMeasurement();

    [[nodiscard]] RECT clientRect(HWND window) const;
    [[nodiscard]] static std::wstring toWide(std::string_view value);
    [[nodiscard]] static std::string toUtf8(std::wstring_view value);
    [[nodiscard]] static std::string formatDouble(double value, int decimals = 2);
    [[nodiscard]] static std::wstring formatWideDouble(double value, int decimals = 2);
};

}  // namespace wolfie
