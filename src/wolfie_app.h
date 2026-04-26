#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "audio/asio_service.h"
#include "core/models.h"
#include "measurement/measurement_controller.h"
#include "persistence/app_state_repository.h"
#include "persistence/workspace_repository.h"
#include "ui/filters_page.h"
#include "ui/measurement_page.h"
#include "ui/smoothing_page.h"
#include "ui/target_curve_page.h"

namespace wolfie {

class WolfieApp {
public:
    explicit WolfieApp(HINSTANCE instance);
    int run();

private:
    enum class LogSeverity {
        Normal,
        Error
    };

    static constexpr UINT_PTR kMeasurementTimerId = 101;
    static constexpr int kContentMargin = 20;

    static LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void createMainWindow();
    void createMenus();
    void createLayout();
    void layoutMainWindow();
    void layoutContent();
    void appendLog(const std::wstring& message, LogSeverity severity = LogSeverity::Normal);
    void appendMeasurementLog(const std::wstring& message, LogSeverity severity = LogSeverity::Normal);
    void beginLogResize(int y);
    void updateLogResize(int y);
    void endLogResize();
    [[nodiscard]] bool isPointOnLogSplitter(int y) const;
    void showSettingsWindow();
    void populateControlsFromState();
    void syncStateFromControls();
    void saveCurrentWorkspaceIfOpen();
    void refreshWindowTitle();
    void refreshMeasurementStatus();
    void refreshRecentMenu();
    void ensureSmoothedResponseReady();
    void invalidateFilterDesign();
    void ensureFilterDesignReady();
    void setExportInProgress(bool running);
    void showExportProgress(const std::wstring& message) const;
    void updateExportControls();
    [[nodiscard]] std::vector<int> selectedExportSampleRates() const;
    void exportRoonFilters();
    void onCommand(WORD commandId, WORD notificationCode);
    void onHScroll(HWND source);
    void onNotify(LPARAM lParam);
    void onTimer(UINT_PTR timerId);
    void onResize();
    void updateVisibleTab();

    void newWorkspace();
    void openWorkspace();
    void openWorkspace(const std::filesystem::path& path);
    void saveWorkspace(bool saveAs);
    void loadLastWorkspaceIfPossible();
    void touchRecentWorkspace(const std::filesystem::path& path);
    void startMeasurement();
    void stopMeasurement();
    void finalizeMeasurement();

    [[nodiscard]] std::optional<std::filesystem::path> pickFolder(bool createIfMissing) const;
    [[nodiscard]] RECT clientRect(HWND window) const;

    HINSTANCE instance_;
    HWND mainWindow_ = nullptr;
    HACCEL acceleratorTable_ = nullptr;
    HWND tabControl_ = nullptr;
    HWND pageSmoothing_ = nullptr;
    HWND pageTargetCurve_ = nullptr;
    HWND pageExport_ = nullptr;
    HWND exportButton_ = nullptr;
    HWND exportProgress_ = nullptr;
    HWND exportStatus_ = nullptr;
    std::vector<HWND> exportSampleRateChecks_;
    HWND logLabel_ = nullptr;
    HWND logEdit_ = nullptr;
    HWND logSplitter_ = nullptr;
    bool exportRunning_ = false;
    bool resizingLog_ = false;
    bool measurementCompletionHandled_ = true;
    RECT logSplitterRect_{};
    WorkspaceState workspace_;
    AppState appState_;
    ui::MeasurementPage measurementPage_;
    ui::SmoothingPage smoothingPage_;
    ui::TargetCurvePage targetCurvePage_;
    ui::FiltersPage filtersPage_;
    MeasurementController measurementController_;
    persistence::WorkspaceRepository workspaceRepository_;
    persistence::AppStateRepository appStateRepository_;
    audio::AsioService asioService_;
};

}  // namespace wolfie
