#include "wolfie_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include <commctrl.h>
#include <richedit.h>
#include <shobjidl.h>
#include <windowsx.h>

#include "audio/winmm_audio_backend.h"
#include "measurement/response_analyzer.h"
#include "measurement/response_smoother.h"
#include "measurement/target_curve_designer.h"
#include "persistence/microphone_calibration_repository.h"
#include "ui/response_graph.h"
#include "ui/settings_dialog.h"
#include "ui/target_curve_graph.h"
#include "ui/ui_theme.h"

namespace wolfie {

namespace {

constexpr int kMenuFileNew = 1001;
constexpr int kMenuFileOpen = 1002;
constexpr int kMenuFileSave = 1003;
constexpr int kMenuFileSaveAs = 1004;
constexpr int kMenuFileSettings = 1005;
constexpr int kMenuFileRecentBase = 1100;
constexpr int kTabMain = 3013;
constexpr int kProcessLog = 3015;
constexpr int kProcessLogSplitter = 3016;
constexpr wchar_t kMainClassName[] = L"WolfieMainWindow";
constexpr int kLogSplitterHeight = 6;
constexpr int kLogLabelHeight = 20;
constexpr int kMinLogHeight = 86;
constexpr int kMaxLogHeight = 420;
constexpr int kMinTabHeight = 360;
constexpr COLORREF kLogNormalColor = RGB(0, 0, 0);
constexpr COLORREF kLogErrorColor = RGB(190, 0, 0);

std::filesystem::path appStatePath() {
    const std::filesystem::path legacyPath = std::filesystem::current_path() / "wolfie-app-state.json";

    std::filesystem::path basePath;
    if (const wchar_t* home = _wgetenv(L"HOME"); home != nullptr && home[0] != L'\0') {
        basePath = std::filesystem::path(home);
    } else if (const wchar_t* profile = _wgetenv(L"USERPROFILE"); profile != nullptr && profile[0] != L'\0') {
        basePath = std::filesystem::path(profile);
    } else {
        basePath = std::filesystem::current_path();
    }

    const std::filesystem::path newPath = basePath / L".wolfie" / L"wolfie-app-state.json";
    if (std::filesystem::exists(newPath) || !std::filesystem::exists(legacyPath)) {
        return newPath;
    }

    std::error_code error;
    std::filesystem::create_directories(newPath.parent_path(), error);
    if (!error) {
        std::filesystem::rename(legacyPath, newPath, error);
    }
    return error ? legacyPath : newPath;
}

}  // namespace

WolfieApp::WolfieApp(HINSTANCE instance)
    : instance_(instance),
      measurementController_(audio::createWinMmAudioBackend()),
      appStateRepository_(appStatePath()) {}

int WolfieApp::run() {
    INITCOMMONCONTROLSEX initCommonControls{};
    initCommonControls.dwSize = sizeof(initCommonControls);
    initCommonControls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&initCommonControls);

    appState_ = appStateRepository_.load();
    createMainWindow();
    loadLastWorkspaceIfPossible();

    ACCEL accelerator{};
    accelerator.fVirt = FCONTROL | FVIRTKEY;
    accelerator.key = 'S';
    accelerator.cmd = kMenuFileSave;
    acceleratorTable_ = CreateAcceleratorTableW(&accelerator, 1);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(mainWindow_, acceleratorTable_, &message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK WolfieApp::MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    WolfieApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<WolfieApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<WolfieApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (app == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_COMMAND:
        app->onCommand(LOWORD(wParam), HIWORD(wParam));
        return 0;
    case WM_NOTIFY:
        app->onNotify(lParam);
        return 0;
    case WM_HSCROLL:
        app->onHScroll(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_SETCURSOR: {
        const POINT cursor = [] {
            POINT point{};
            GetCursorPos(&point);
            return point;
        }();
        POINT clientPoint = cursor;
        ScreenToClient(window, &clientPoint);
        if (app->resizingLog_ || app->isPointOnLogSplitter(clientPoint.y)) {
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        const int y = GET_Y_LPARAM(lParam);
        if (app->isPointOnLogSplitter(y)) {
            app->beginLogResize(y);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        if (app->resizingLog_) {
            app->endLogResize();
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (app->resizingLog_) {
            app->updateLogResize(GET_Y_LPARAM(lParam));
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        app->resizingLog_ = false;
        break;
    case WM_DRAWITEM:
        if (app->measurementPage_.handleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam),
                                                 app->measurementController_.status().running)) {
            return TRUE;
        }
        if (app->targetCurvePage_.handleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'S' || wParam == 's')) {
            app->saveWorkspace(false);
            return 0;
        }
        break;
    case WM_TIMER:
        app->onTimer(wParam);
        return 0;
    case WM_SIZE:
        app->onResize();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 1120;
        info->ptMinTrackSize.y = 760;
        return 0;
    }
    case WM_DESTROY:
        if (app->acceleratorTable_ != nullptr) {
            DestroyAcceleratorTable(app->acceleratorTable_);
            app->acceleratorTable_ = nullptr;
        }
        app->saveCurrentWorkspaceIfOpen();
        app->appStateRepository_.save(app->appState_);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void WolfieApp::createMainWindow() {
    WNDCLASSW mainClass{};
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = instance_;
    mainClass.lpszClassName = kMainClassName;
    mainClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mainClass.hbrBackground = CreateSolidBrush(ui_theme::kBackground);
    RegisterClassW(&mainClass);

    ui::ResponseGraph::registerWindowClass(instance_);
    ui::TargetCurveGraph::registerWindowClass(instance_);
    ui::MeasurementPage::registerPageWindowClass(instance_);
    ui::SmoothingPage::registerPageWindowClass(instance_);
    ui::TargetCurvePage::registerPageWindowClass(instance_);

    mainWindow_ = CreateWindowExW(0, kMainClassName, L"Wolfie", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1400, 920, nullptr, nullptr, instance_, this);

    createMenus();
    createLayout();
    populateControlsFromState();
    refreshWindowTitle();
    refreshMeasurementStatus();
}

void WolfieApp::createMenus() {
    HMENU menuBar = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();

    AppendMenuW(fileMenu, MF_STRING, kMenuFileNew, L"&New...");
    AppendMenuW(fileMenu, MF_STRING, kMenuFileOpen, L"&Open...");
    HMENU recentMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(recentMenu), L"&Recent");
    AppendMenuW(fileMenu, MF_STRING, kMenuFileSave, L"&Save\tCtrl+S");
    AppendMenuW(fileMenu, MF_STRING, kMenuFileSaveAs, L"Save &As...");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, kMenuFileSettings, L"&Settings");

    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");
    SetMenu(mainWindow_, menuBar);
    refreshRecentMenu();
}

void WolfieApp::createLayout() {
    tabControl_ = CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                                  0, 0, 0, 0, mainWindow_, reinterpret_cast<HMENU>(kTabMain), instance_, nullptr);

    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<LPWSTR>(L"Measurement");
    TabCtrl_InsertItem(tabControl_, 0, &item);
    item.pszText = const_cast<LPWSTR>(L"Smoothing");
    TabCtrl_InsertItem(tabControl_, 1, &item);
    item.pszText = const_cast<LPWSTR>(L"Target Curve");
    TabCtrl_InsertItem(tabControl_, 2, &item);
    item.pszText = const_cast<LPWSTR>(L"Filters");
    TabCtrl_InsertItem(tabControl_, 3, &item);
    item.pszText = const_cast<LPWSTR>(L"Export");
    TabCtrl_InsertItem(tabControl_, 4, &item);

    measurementPage_.create(tabControl_, instance_);
    smoothingPage_.create(tabControl_, instance_);
    targetCurvePage_.create(tabControl_, instance_);
    pageSmoothing_ = smoothingPage_.window();
    pageTargetCurve_ = targetCurvePage_.window();
    pageFilters_ = CreateWindowExW(0, ui::MeasurementPage::pageWindowClassName(), nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                   0, 0, 0, 0, tabControl_, nullptr, instance_, nullptr);
    pageExport_ = CreateWindowExW(0, ui::MeasurementPage::pageWindowClassName(), nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                  0, 0, 0, 0, tabControl_, nullptr, instance_, nullptr);

    placeholderFilters_ = CreateWindowW(L"STATIC", L"Filter design and simulation will live here.", WS_CHILD | SS_CENTER,
                                        0, 0, 0, 0, pageFilters_, nullptr, instance_, nullptr);
    placeholderExport_ = CreateWindowW(L"STATIC", L"ROON export will live here.", WS_CHILD | SS_CENTER,
                                       0, 0, 0, 0, pageExport_, nullptr, instance_, nullptr);
    logSplitter_ = CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                                 0, 0, 0, 0, mainWindow_, reinterpret_cast<HMENU>(kProcessLogSplitter), instance_, nullptr);
    logLabel_ = CreateWindowW(L"STATIC", L"Process Log", WS_CHILD | WS_VISIBLE,
                              0, 0, 0, 0, mainWindow_, nullptr, instance_, nullptr);
    LoadLibraryW(L"Msftedit.dll");
    logEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE,
                               MSFTEDIT_CLASS,
                               L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                               0,
                               0,
                               0,
                               0,
                               mainWindow_,
                               reinterpret_cast<HMENU>(kProcessLog),
                               instance_,
                               nullptr);
    SendMessageW(logEdit_, EM_SETLIMITTEXT, 0, 0);

    updateVisibleTab();
    layoutMainWindow();
    appendLog(L"Application ready.");
}

void WolfieApp::layoutMainWindow() {
    const RECT bounds = clientRect(mainWindow_);
    const int width = std::max(320L, bounds.right - (2 * kContentMargin));
    const int height = std::max(360L, bounds.bottom - (2 * kContentMargin));
    const int maxLogHeight = std::max(kMinLogHeight, height - kMinTabHeight - kLogSplitterHeight);
    workspace_.ui.processLogHeight = std::clamp(workspace_.ui.processLogHeight,
                                                kMinLogHeight,
                                                std::min(kMaxLogHeight, maxLogHeight));
    const int logHeight = workspace_.ui.processLogHeight;
    const int tabHeight = std::max(kMinTabHeight, height - logHeight - kLogSplitterHeight);
    MoveWindow(tabControl_, kContentMargin, kContentMargin, width, tabHeight, TRUE);

    const int splitterTop = kContentMargin + tabHeight;
    logSplitterRect_ = RECT{kContentMargin, splitterTop, kContentMargin + width, splitterTop + kLogSplitterHeight};
    MoveWindow(logSplitter_,
               logSplitterRect_.left,
               logSplitterRect_.top,
               logSplitterRect_.right - logSplitterRect_.left,
               logSplitterRect_.bottom - logSplitterRect_.top,
               TRUE);

    const int logTop = splitterTop + kLogSplitterHeight;
    MoveWindow(logLabel_, kContentMargin, logTop + 2, width, kLogLabelHeight, TRUE);
    MoveWindow(logEdit_,
               kContentMargin,
               logTop + kLogLabelHeight,
               width,
               std::max(40, logHeight - kLogLabelHeight),
               TRUE);

    RECT tabRect{};
    GetClientRect(tabControl_, &tabRect);
    TabCtrl_AdjustRect(tabControl_, FALSE, &tabRect);
    const int pageWidth = std::max(320L, tabRect.right - tabRect.left);
    const int pageHeight = std::max(240L, tabRect.bottom - tabRect.top);
    MoveWindow(measurementPage_.window(), tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageSmoothing_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageTargetCurve_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageFilters_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageExport_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    layoutContent();
}

void WolfieApp::layoutContent() {
    RECT filtersRect = clientRect(pageFilters_);
    MoveWindow(placeholderFilters_, 24, 32, std::max(200L, filtersRect.right - 48), 24, TRUE);
    RECT exportRect = clientRect(pageExport_);
    MoveWindow(placeholderExport_, 24, 32, std::max(200L, exportRect.right - 48), 24, TRUE);
    measurementPage_.layout();
    smoothingPage_.layout();
    targetCurvePage_.layout();
}

void WolfieApp::appendLog(const std::wstring& message, LogSeverity severity) {
    if (logEdit_ == nullptr) {
        return;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wostringstream line;
    line << L'['
         << (time.wHour < 10 ? L"0" : L"") << time.wHour << L':'
         << (time.wMinute < 10 ? L"0" : L"") << time.wMinute << L':'
         << (time.wSecond < 10 ? L"0" : L"") << time.wSecond << L"] "
         << message << L"\r\n";

    const int length = GetWindowTextLengthW(logEdit_);
    SendMessageW(logEdit_, EM_SETSEL, length, length);

    CHARFORMATW format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = severity == LogSeverity::Error ? kLogErrorColor : kLogNormalColor;
    SendMessageW(logEdit_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));

    SendMessageW(logEdit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.str().c_str()));
    SendMessageW(logEdit_, EM_SCROLLCARET, 0, 0);
}

void WolfieApp::appendMeasurementLog(const std::wstring& message, LogSeverity severity) {
    appendLog(L"Measurement: " + message, severity);
}

bool WolfieApp::isPointOnLogSplitter(int y) const {
    return y >= logSplitterRect_.top - 3 && y <= logSplitterRect_.bottom + 3;
}

void WolfieApp::beginLogResize(int) {
    resizingLog_ = true;
    SetCapture(mainWindow_);
    SetCursor(LoadCursor(nullptr, IDC_SIZENS));
}

void WolfieApp::updateLogResize(int y) {
    const RECT bounds = clientRect(mainWindow_);
    const int contentBottom = bounds.bottom - kContentMargin;
    const int height = std::max(360L, bounds.bottom - (2 * kContentMargin));
    const int maxLogHeight = std::max(kMinLogHeight, height - kMinTabHeight - kLogSplitterHeight);
    workspace_.ui.processLogHeight = std::clamp(contentBottom - y - kLogSplitterHeight,
                                                kMinLogHeight,
                                                std::min(kMaxLogHeight, maxLogHeight));
    layoutMainWindow();
}

void WolfieApp::endLogResize() {
    resizingLog_ = false;
    if (GetCapture() == mainWindow_) {
        ReleaseCapture();
    }
    if (!workspace_.rootPath.empty()) {
        try {
            syncStateFromControls();
        } catch (...) {
        }
        workspaceRepository_.save(workspace_);
    }
}

void WolfieApp::showSettingsWindow() {
    ui::SettingsDialog::show(instance_, mainWindow_, workspace_.audio, asioService_, [this](const AudioSettings& settings) {
        const std::filesystem::path previousCalibrationPath = workspace_.audio.microphoneCalibrationPath;
        workspace_.audio = settings;
        std::wstring calibrationError;
        if (!persistence::loadMicrophoneCalibration(workspace_.audio, calibrationError)) {
            appendLog(L"Microphone calibration unavailable: " + calibrationError, LogSeverity::Error);
        } else if (workspace_.audio.microphoneCalibrationPath != previousCalibrationPath) {
            if (workspace_.audio.microphoneCalibrationPath.empty()) {
                appendLog(L"Microphone calibration cleared.");
            } else {
                appendLog(L"Microphone calibration loaded: " + workspace_.audio.microphoneCalibrationPath.wstring());
            }
        }
        populateControlsFromState();
        syncStateFromControls();
        workspaceRepository_.save(workspace_);
        refreshMeasurementStatus();
    });
}

void WolfieApp::populateControlsFromState() {
    measurement::syncDerivedMeasurementSettings(workspace_.measurement);
    measurement::normalizeResponseSmoothingSettings(workspace_.smoothing);
    const auto targetPlot = measurement::buildTargetCurvePlotData(workspace_.smoothedResponse,
                                                                  workspace_.measurement,
                                                                  workspace_.targetCurve,
                                                                  std::nullopt);
    measurement::normalizeTargetCurveSettings(workspace_.targetCurve, targetPlot.minFrequencyHz, targetPlot.maxFrequencyHz);
    measurementPage_.populate(workspace_);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    layoutMainWindow();
}

void WolfieApp::syncStateFromControls() {
    measurementPage_.syncToWorkspace(workspace_);
    smoothingPage_.syncToWorkspace(workspace_);
    targetCurvePage_.syncToWorkspace(workspace_);
}

void WolfieApp::saveCurrentWorkspaceIfOpen() {
    if (workspace_.rootPath.empty()) {
        return;
    }

    syncStateFromControls();
    workspaceRepository_.save(workspace_);
}

void WolfieApp::refreshWindowTitle() {
    std::wstring title = L"Wolfie";
    if (!workspace_.rootPath.empty()) {
        title += L" - " + workspace_.rootPath.filename().wstring();
    }
    SetWindowTextW(mainWindow_, title.c_str());
}

void WolfieApp::refreshMeasurementStatus() {
    measurementPage_.refreshStatus(measurementController_.status(), workspace_.result.hasAnyValues());
}

void WolfieApp::refreshRecentMenu() {
    HMENU menuBar = GetMenu(mainWindow_);
    HMENU fileMenu = GetSubMenu(menuBar, 0);
    HMENU recentMenu = GetSubMenu(fileMenu, 2);
    while (GetMenuItemCount(recentMenu) > 0) {
        DeleteMenu(recentMenu, 0, MF_BYPOSITION);
    }

    if (appState_.recentWorkspaces.empty()) {
        AppendMenuW(recentMenu, MF_GRAYED | MF_STRING, kMenuFileRecentBase, L"(none)");
        return;
    }

    for (size_t i = 0; i < appState_.recentWorkspaces.size() && i < 8; ++i) {
        AppendMenuW(recentMenu, MF_STRING, kMenuFileRecentBase + static_cast<UINT>(i), appState_.recentWorkspaces[i].wstring().c_str());
    }
}

void WolfieApp::ensureSmoothedResponseReady() {
    if (!workspace_.smoothedResponse.frequencyAxisHz.empty() ||
        !workspace_.result.hasAnyValues()) {
        return;
    }

    measurement::normalizeResponseSmoothingSettings(workspace_.smoothing);
    workspace_.smoothedResponse = measurement::buildSmoothedResponse(workspace_.result, workspace_.smoothing);
}

void WolfieApp::onCommand(WORD commandId, WORD notificationCode) {
    bool measurePressed = false;
    bool loopbackPressed = false;
    bool sampleRateChanged = false;
    bool measurementGraphZoomChanged = false;
    if (measurementPage_.handleCommand(commandId,
                                       notificationCode,
                                       workspace_,
                                       measurePressed,
                                       loopbackPressed,
                                       sampleRateChanged,
                                       measurementGraphZoomChanged)) {
        if (measurementGraphZoomChanged) {
            return;
        }
        if (sampleRateChanged) {
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
            refreshMeasurementStatus();
        }
        if (measurePressed) {
            if (measurementController_.status().running) {
                stopMeasurement();
            } else {
                startMeasurement();
            }
        }
        if (loopbackPressed) {
            if (measurementController_.status().running) {
                stopMeasurement();
            } else {
                startLoopbackCalibration();
            }
        }
        return;
    }

    bool smoothingModelChanged = false;
    bool smoothingGraphZoomChanged = false;
    if (smoothingPage_.handleCommand(commandId,
                                     notificationCode,
                                     workspace_,
                                     smoothingModelChanged,
                                     smoothingGraphZoomChanged)) {
        if (smoothingGraphZoomChanged) {
            return;
        }
        if (smoothingModelChanged) {
            workspace_.smoothedResponse = {};
            ensureSmoothedResponseReady();
            smoothingPage_.populate(workspace_);
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
        }
        return;
    }

    bool targetCurveChanged = false;
    if (targetCurvePage_.handleCommand(commandId, notificationCode, workspace_, targetCurveChanged)) {
        if (targetCurveChanged) {
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
        }
        return;
    }

    switch (commandId) {
    case kMenuFileNew:
        newWorkspace();
        return;
    case kMenuFileOpen:
        openWorkspace();
        return;
    case kMenuFileSave:
        saveWorkspace(false);
        return;
    case kMenuFileSaveAs:
        saveWorkspace(true);
        return;
    case kMenuFileSettings:
        showSettingsWindow();
        return;
    default:
        if (commandId >= kMenuFileRecentBase && commandId < kMenuFileRecentBase + 8) {
            const size_t index = commandId - kMenuFileRecentBase;
            if (index < appState_.recentWorkspaces.size()) {
                openWorkspace(appState_.recentWorkspaces[index]);
            }
        }
        return;
    }
}

void WolfieApp::onHScroll(HWND source) {
    if (measurementPage_.handleHScroll(source, workspace_)) {
        syncStateFromControls();
        workspaceRepository_.save(workspace_);
        return;
    }

    bool smoothingResolutionChanged = false;
    if (smoothingPage_.handleHScroll(source, workspace_, smoothingResolutionChanged)) {
        if (smoothingResolutionChanged) {
            workspace_.smoothedResponse = {};
            ensureSmoothedResponseReady();
            smoothingPage_.populate(workspace_);
            targetCurvePage_.populate(workspace_);
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
        }
        return;
    }

    bool targetCurveChanged = false;
    if (targetCurvePage_.handleHScroll(source, workspace_, targetCurveChanged)) {
        if (targetCurveChanged) {
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
        }
    }
}

void WolfieApp::onNotify(LPARAM lParam) {
    const auto* header = reinterpret_cast<const NMHDR*>(lParam);
    if (header != nullptr && header->hwndFrom == tabControl_ && header->code == TCN_SELCHANGE) {
        updateVisibleTab();
        layoutContent();
    }
}

void WolfieApp::onTimer(UINT_PTR timerId) {
    if (timerId != kMeasurementTimerId) {
        return;
    }

    measurementController_.tick();
    refreshMeasurementStatus();
    if (measurementController_.status().finished && !measurementCompletionHandled_) {
        measurementCompletionHandled_ = true;
        KillTimer(mainWindow_, kMeasurementTimerId);
        finalizeMeasurement();
    }
}

void WolfieApp::onResize() {
    layoutMainWindow();
    RedrawWindow(mainWindow_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void WolfieApp::updateVisibleTab() {
    const int selected = tabControl_ == nullptr ? 0 : TabCtrl_GetCurSel(tabControl_);
    if (selected == 1 || selected == 2) {
        ensureSmoothedResponseReady();
        if (selected == 1) {
            smoothingPage_.populate(workspace_);
        } else {
            targetCurvePage_.populate(workspace_);
        }
    }

    measurementPage_.setVisible(selected == 0);
    smoothingPage_.setVisible(selected == 1);
    targetCurvePage_.setVisible(selected == 2);
    ShowWindow(pageFilters_, selected == 3 ? SW_SHOW : SW_HIDE);
    ShowWindow(pageExport_, selected == 4 ? SW_SHOW : SW_HIDE);
}

void WolfieApp::newWorkspace() {
    auto path = pickFolder(true);
    if (!path) {
        return;
    }

    saveCurrentWorkspaceIfOpen();
    workspace_ = {};
    workspace_.rootPath = *path;
    measurement::syncDerivedMeasurementSettings(workspace_.measurement);
    measurement::normalizeResponseSmoothingSettings(workspace_.smoothing);
    measurement::normalizeTargetCurveSettings(workspace_.targetCurve,
                                              std::max(workspace_.measurement.startFrequencyHz, 20.0),
                                              std::min(workspace_.measurement.endFrequencyHz, 20000.0));
    populateControlsFromState();
    refreshWindowTitle();
    measurementPage_.invalidateGraph();
    syncStateFromControls();
    workspaceRepository_.save(workspace_);
    touchRecentWorkspace(*path);
    appendLog(L"Workspace created: " + path->wstring());
}

void WolfieApp::openWorkspace() {
    auto path = pickFolder(false);
    if (path) {
        openWorkspace(*path);
    }
}

void WolfieApp::openWorkspace(const std::filesystem::path& path) {
    saveCurrentWorkspaceIfOpen();
    workspace_ = workspaceRepository_.load(path);
    touchRecentWorkspace(path);
    populateControlsFromState();
    refreshWindowTitle();
    refreshMeasurementStatus();
    measurementPage_.invalidateGraph();
    targetCurvePage_.populate(workspace_);
    if (!workspace_.audio.microphoneCalibrationPath.empty() &&
        workspace_.audio.microphoneCalibrationFrequencyHz.size() < 2) {
        appendLog(L"Workspace microphone calibration could not be loaded: " +
                  workspace_.audio.microphoneCalibrationPath.wstring(),
                  LogSeverity::Error);
    }
    appendLog(L"Workspace opened: " + path.wstring());
}

void WolfieApp::saveWorkspace(bool saveAs) {
    if (saveAs || workspace_.rootPath.empty()) {
        auto path = pickFolder(true);
        if (!path) {
            return;
        }
        workspace_.rootPath = *path;
        touchRecentWorkspace(*path);
    }

    syncStateFromControls();
    workspaceRepository_.save(workspace_);
    refreshWindowTitle();
    appendLog(L"Workspace saved: " + workspace_.rootPath.wstring());
}

void WolfieApp::loadLastWorkspaceIfPossible() {
    if (!appState_.lastWorkspace.empty() && std::filesystem::exists(appState_.lastWorkspace / "workspace.json")) {
        openWorkspace(appState_.lastWorkspace);
    }
}

void WolfieApp::touchRecentWorkspace(const std::filesystem::path& path) {
    appState_.lastWorkspace = path;
    appState_.recentWorkspaces.erase(std::remove(appState_.recentWorkspaces.begin(), appState_.recentWorkspaces.end(), path), appState_.recentWorkspaces.end());
    appState_.recentWorkspaces.insert(appState_.recentWorkspaces.begin(), path);
    if (appState_.recentWorkspaces.size() > 8) {
        appState_.recentWorkspaces.resize(8);
    }
    refreshRecentMenu();
    appStateRepository_.save(appState_);
}

void WolfieApp::startMeasurement() {
    if (workspace_.rootPath.empty()) {
        appendMeasurementLog(L"Cannot start measurement because no workspace is open.", LogSeverity::Error);
        return;
    }

    syncStateFromControls();
    appendMeasurementLog(L"Starting sweep measurement at " +
                         std::to_wstring(workspace_.measurement.sampleRate) +
                         L" Hz, stored loopback latency " +
                         std::to_wstring(measurement::configuredLoopbackLatencySamples(workspace_.measurement,
                                                                                       workspace_.measurement.sampleRate)) +
                         L" samples.");
    if (!workspace_.audio.microphoneCalibrationPath.empty()) {
        if (workspace_.audio.microphoneCalibrationFrequencyHz.size() >= 2) {
            appendMeasurementLog(L"Using microphone calibration: " + workspace_.audio.microphoneCalibrationPath.wstring());
        } else {
            appendMeasurementLog(L"Configured microphone calibration could not be loaded: " +
                                     workspace_.audio.microphoneCalibrationPath.wstring(),
                                 LogSeverity::Error);
        }
    }
    if (!measurementController_.start(workspace_)) {
        refreshMeasurementStatus();
        if (!measurementController_.status().lastErrorMessage.empty()) {
            appendMeasurementLog(L"Start failed: " + measurementController_.status().lastErrorMessage, LogSeverity::Error);
        }
        return;
    }

    measurementCompletionHandled_ = false;
    appendMeasurementLog(L"Playback file written to " + measurementController_.status().generatedSweepPath.wstring());

    workspace_.result = {};
    workspace_.smoothedResponse = {};
    measurementPage_.setMeasurementResult(workspace_.result);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::startLoopbackCalibration() {
    if (workspace_.rootPath.empty()) {
        appendMeasurementLog(L"Cannot start loopback calibration because no workspace is open.", LogSeverity::Error);
        return;
    }

    syncStateFromControls();
    appendMeasurementLog(L"Starting loopback latency calibration at " +
                         std::to_wstring(workspace_.measurement.sampleRate) +
                         L" Hz.");
    if (!measurementController_.startLoopbackCalibration(workspace_)) {
        refreshMeasurementStatus();
        if (!measurementController_.status().lastErrorMessage.empty()) {
            appendMeasurementLog(L"Loopback calibration start failed: " + measurementController_.status().lastErrorMessage,
                                 LogSeverity::Error);
        }
        return;
    }

    measurementCompletionHandled_ = false;
    appendMeasurementLog(L"Loopback pulse file written to " + measurementController_.status().generatedSweepPath.wstring());

    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::stopMeasurement() {
    if (!measurementController_.status().running) {
        return;
    }

    appendMeasurementLog(measurementController_.status().loopbackCalibration
                             ? L"Loopback calibration stopped by user."
                             : L"Measurement stopped by user.");
    measurementCompletionHandled_ = true;
    measurementController_.cancel();
    KillTimer(mainWindow_, kMeasurementTimerId);
    if (!workspace_.rootPath.empty()) {
        const WorkspaceState persistedWorkspace = workspaceRepository_.load(workspace_.rootPath);
        workspace_.result = persistedWorkspace.result;
        workspace_.smoothedResponse = {};
        measurementPage_.setMeasurementResult(workspace_.result);
        smoothingPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
    }
    refreshMeasurementStatus();
}

void WolfieApp::finalizeMeasurement() {
    const MeasurementStatus completedStatus = measurementController_.status();
    if (completedStatus.loopbackCalibration) {
        if (completedStatus.lastErrorMessage.empty()) {
            workspace_.measurement.loopbackLatencySamples = completedStatus.measuredLoopbackLatencySamples;
            workspace_.measurement.loopbackLatencySampleRate = workspace_.measurement.sampleRate;
            workspaceRepository_.save(workspace_);
            measurementPage_.populate(workspace_);
            appendMeasurementLog(L"Loopback latency stored: " +
                                 std::to_wstring(completedStatus.measuredLoopbackLatencySamples) +
                                 L" samples, peak-to-noise " +
                                 std::to_wstring(static_cast<int>(std::lround(completedStatus.loopbackPeakToNoiseDb))) +
                                 L" dB.");
            if (completedStatus.loopbackClippingDetected) {
                appendMeasurementLog(L"Warning: clipping was detected during loopback capture.");
            }
        } else {
            appendMeasurementLog(L"Loopback calibration failed: " + completedStatus.lastErrorMessage, LogSeverity::Error);
            if (completedStatus.loopbackTooQuiet) {
                appendMeasurementLog(L"Loopback input was too low. Raise the interface output/input level or check the cable path.",
                                     LogSeverity::Error);
            }
        }
        refreshMeasurementStatus();
        return;
    }

    workspace_.result = measurementController_.result();
    workspace_.smoothedResponse = {};
    const int selected = tabControl_ == nullptr ? 0 : TabCtrl_GetCurSel(tabControl_);
    if (selected == 1 || selected == 2) {
        ensureSmoothedResponseReady();
    }
    measurementPage_.setMeasurementResult(workspace_.result);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    syncStateFromControls();
    workspaceRepository_.save(workspace_);
    appendMeasurementLog(L"Measurement finished. Generated " +
                         std::to_wstring(workspace_.result.preferredMagnitudeResponse() == nullptr
                                             ? 0
                                             : workspace_.result.preferredMagnitudeResponse()->xValues.size()) +
                         L" response points. Peak capture level " +
                         std::to_wstring(static_cast<int>(std::lround(completedStatus.peakAmplitudeDb))) +
                         L" dB.");
    appendMeasurementLog(L"Detected alignment: left " +
                         std::to_wstring(workspace_.result.analysis.left.detectedLatencySamples) +
                         L" samples, right " +
                         std::to_wstring(workspace_.result.analysis.right.detectedLatencySamples) +
                         L" samples.");
    if (workspace_.result.analysis.captureClippingDetected) {
        appendMeasurementLog(L"Warning: clipping was detected during the sweep capture.", LogSeverity::Error);
    }
    if (workspace_.result.analysis.captureTooQuiet) {
        appendMeasurementLog(L"Warning: sweep capture level was very low.", LogSeverity::Error);
    }
    refreshMeasurementStatus();
}

std::optional<std::filesystem::path> WolfieApp::pickFolder(bool createIfMissing) const {
    std::optional<std::filesystem::path> selectedPath;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (dialog->Show(mainWindow_) == S_OK) {
            IShellItem* item = nullptr;
            if (dialog->GetResult(&item) == S_OK) {
                PWSTR pathBuffer = nullptr;
                if (item->GetDisplayName(SIGDN_FILESYSPATH, &pathBuffer) == S_OK) {
                    selectedPath = std::filesystem::path(pathBuffer);
                    CoTaskMemFree(pathBuffer);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }

    if (selectedPath && createIfMissing) {
        std::filesystem::create_directories(*selectedPath);
    }
    return selectedPath;
}

RECT WolfieApp::clientRect(HWND window) const {
    RECT rect{};
    GetClientRect(window, &rect);
    return rect;
}

}  // namespace wolfie
