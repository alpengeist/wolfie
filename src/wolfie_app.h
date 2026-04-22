#pragma once

#include <filesystem>
#include <optional>

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
#include "ui/measurement_page.h"

namespace wolfie {

class WolfieApp {
public:
    explicit WolfieApp(HINSTANCE instance);
    int run();

private:
    static constexpr UINT_PTR kMeasurementTimerId = 101;
    static constexpr int kContentMargin = 20;

    static LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void createMainWindow();
    void createMenus();
    void createLayout();
    void layoutMainWindow();
    void layoutContent();
    void showSettingsWindow();
    void populateControlsFromState();
    void syncStateFromControls();
    void refreshWindowTitle();
    void refreshMeasurementStatus();
    void refreshRecentMenu();
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
    HWND pageAlignment_ = nullptr;
    HWND pageTargetCurve_ = nullptr;
    HWND pageFilters_ = nullptr;
    HWND pageExport_ = nullptr;
    HWND placeholderAlignment_ = nullptr;
    HWND placeholderTargetCurve_ = nullptr;
    HWND placeholderFilters_ = nullptr;
    HWND placeholderExport_ = nullptr;
    WorkspaceState workspace_;
    AppState appState_;
    ui::MeasurementPage measurementPage_;
    MeasurementController measurementController_;
    persistence::WorkspaceRepository workspaceRepository_;
    persistence::AppStateRepository appStateRepository_;
    audio::AsioService asioService_;
};

}  // namespace wolfie
