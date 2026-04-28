#include "wolfie_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include <commctrl.h>
#include <richedit.h>
#include <shobjidl.h>
#include <windowsx.h>

#include "audio/asio_audio_backend.h"
#include "core/text_utils.h"
#include "measurement/response_analyzer.h"
#include "measurement/filter_designer.h"
#include "measurement/filter_wav_export.h"
#include "measurement/response_smoother.h"
#include "measurement/target_curve_designer.h"
#include "persistence/microphone_calibration_repository.h"
#include "ui/plot_graph.h"
#include "ui/response_graph.h"
#include "ui/settings_dialog.h"
#include "ui/target_curve_graph.h"
#include "ui/ui_theme.h"
#include "ui/waterfall_graph.h"

namespace wolfie {

namespace {

std::string sanitizeFileComponent(std::string_view value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    for (const char ch : value) {
        const bool invalid = ch < 32 || ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
                             ch == '"' || ch == '<' || ch == '>' || ch == '|';
        sanitized.push_back(invalid ? '_' : ch);
    }
    while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.')) {
        sanitized.pop_back();
    }
    if (sanitized.empty()) {
        sanitized = "Default";
    }
    return sanitized;
}

constexpr int kMenuFileNew = 1001;
constexpr int kMenuFileOpen = 1002;
constexpr int kMenuFileSave = 1003;
constexpr int kMenuFileSaveAs = 1004;
constexpr int kMenuFileSettings = 1005;
constexpr int kMenuFileRecentBase = 1100;
constexpr int kTabMain = 3013;
constexpr int kProcessLog = 3015;
constexpr int kProcessLogSplitter = 3016;
constexpr int kButtonExportRoon = 3017;
constexpr int kExportSampleRateCheckboxBase = 3020;
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

std::wstring formatSampleRateLabel(int sampleRate) {
    const int decimals = sampleRate % 1000 == 0 ? 0 : 1;
    return formatWideDouble(static_cast<double>(sampleRate) / 1000.0, decimals) + L" kHz";
}

}  // namespace

WolfieApp::WolfieApp(HINSTANCE instance)
    : instance_(instance),
      measurementController_(audio::createDefaultAudioBackend()),
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
    ui::WaterfallGraph::registerWindowClass(instance_);
    ui::TargetCurveGraph::registerWindowClass(instance_);
    ui::PlotGraph::registerWindowClass(instance_);
    ui::MeasurementPage::registerPageWindowClass(instance_);
    ui::SmoothingPage::registerPageWindowClass(instance_);
    ui::TargetCurvePage::registerPageWindowClass(instance_);
    ui::FiltersPage::registerPageWindowClass(instance_);

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
    filtersPage_.create(tabControl_, instance_);
    pageSmoothing_ = smoothingPage_.window();
    pageTargetCurve_ = targetCurvePage_.window();
    pageExport_ = CreateWindowExW(0, ui::MeasurementPage::pageWindowClassName(), nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                  0, 0, 0, 0, tabControl_, nullptr, instance_, nullptr);
    const std::vector<int>& exportSampleRates = measurement::roonCommonSampleRates();
    exportSampleRateChecks_.reserve(exportSampleRates.size());
    for (std::size_t index = 0; index < exportSampleRates.size(); ++index) {
        HWND checkbox = CreateWindowW(L"BUTTON",
                                      formatSampleRateLabel(exportSampleRates[index]).c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      0,
                                      0,
                                      0,
                                      0,
                                      pageExport_,
                                      reinterpret_cast<HMENU>(kExportSampleRateCheckboxBase + static_cast<int>(index)),
                                      instance_,
                                      nullptr);
        SendMessageW(checkbox, BM_SETCHECK, BST_CHECKED, 0);
        exportSampleRateChecks_.push_back(checkbox);
    }
    exportButton_ = CreateWindowW(L"BUTTON",
                                  L"Generate Roon ZIP",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0,
                                  0,
                                  0,
                                  0,
                                  pageExport_,
                                  reinterpret_cast<HMENU>(kButtonExportRoon),
                                  instance_,
                                  nullptr);
    exportProgress_ = CreateWindowW(L"STATIC",
                                    L"",
                                    WS_CHILD | SS_CENTERIMAGE,
                                    0,
                                    0,
                                    0,
                                    0,
                                    pageExport_,
                                    nullptr,
                                    instance_,
                                    nullptr);
    exportStatus_ = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
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
    MoveWindow(filtersPage_.window(), tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageExport_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    layoutContent();
}

void WolfieApp::layoutContent() {
    RECT exportRect = clientRect(pageExport_);
    const int exportWidth = std::max(320L, exportRect.right - 48);
    const int checkboxLeft = 24;
    const int checkboxTop = 28;
    const int checkboxHeight = 22;
    const int checkboxGapX = 12;
    const int checkboxGapY = 8;
    const int minCheckboxWidth = 112;
    const int checkboxColumns = std::max(
        1,
        std::min(static_cast<int>(exportSampleRateChecks_.size()),
                 (exportWidth + checkboxGapX) / (minCheckboxWidth + checkboxGapX)));
    const int checkboxWidth = std::max(
        minCheckboxWidth,
        (exportWidth - ((checkboxColumns - 1) * checkboxGapX)) / checkboxColumns);
    for (std::size_t index = 0; index < exportSampleRateChecks_.size(); ++index) {
        const int row = static_cast<int>(index) / checkboxColumns;
        const int column = static_cast<int>(index) % checkboxColumns;
        const int x = checkboxLeft + (column * (checkboxWidth + checkboxGapX));
        const int y = checkboxTop + (row * (checkboxHeight + checkboxGapY));
        MoveWindow(exportSampleRateChecks_[index], x, y, checkboxWidth, checkboxHeight, TRUE);
    }

    const int checkboxRowCount =
        exportSampleRateChecks_.empty() ? 0 : ((static_cast<int>(exportSampleRateChecks_.size()) + checkboxColumns - 1) /
                                               checkboxColumns);
    const int buttonTop = checkboxTop +
                          (checkboxRowCount * checkboxHeight) +
                          (std::max(0, checkboxRowCount - 1) * checkboxGapY) +
                          18;
    MoveWindow(exportButton_, 24, buttonTop, 180, 28, TRUE);
    MoveWindow(exportProgress_, 24, buttonTop, exportWidth, 28, TRUE);
    MoveWindow(exportStatus_, 24, buttonTop + 44, exportWidth, 48, TRUE);
    measurementPage_.layout();
    smoothingPage_.layout();
    targetCurvePage_.layout();
    filtersPage_.layout();
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
    measurement::normalizeFilterDesignSettings(workspace_.filters, workspace_.measurement.sampleRate);
    const auto targetPlot = measurement::buildTargetCurvePlotData(workspace_.smoothedResponse,
                                                                  workspace_.measurement,
                                                                  workspace_.targetCurve,
                                                                  std::nullopt);
    measurement::normalizeTargetCurveSettings(workspace_.targetCurve, targetPlot.minFrequencyHz, targetPlot.maxFrequencyHz);
    measurementPage_.populate(workspace_);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    populateExportSampleRateControls();
    updateExportControls();
    layoutMainWindow();
}

void WolfieApp::syncStateFromControls() {
    measurementPage_.syncToWorkspace(workspace_);
    smoothingPage_.syncToWorkspace(workspace_);
    targetCurvePage_.syncToWorkspace(workspace_);
    filtersPage_.syncToWorkspace(workspace_);
    syncExportSampleRatesToWorkspace();
}

void WolfieApp::populateExportSampleRateControls() {
    const std::vector<int>& allSampleRates = measurement::roonCommonSampleRates();
    const bool usePersistedSelection = workspace_.ui.exportSampleRatesCustomized;
    const std::vector<int>& persistedSampleRates = workspace_.ui.exportSampleRatesHz;
    const std::size_t checkboxCount = std::min(exportSampleRateChecks_.size(), allSampleRates.size());
    for (std::size_t index = 0; index < checkboxCount; ++index) {
        bool checked = true;
        if (usePersistedSelection) {
            checked = std::find(persistedSampleRates.begin(), persistedSampleRates.end(), allSampleRates[index]) !=
                      persistedSampleRates.end();
        }
        SendMessageW(exportSampleRateChecks_[index], BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void WolfieApp::syncExportSampleRatesToWorkspace() {
    workspace_.ui.exportSampleRatesCustomized = true;
    workspace_.ui.exportSampleRatesHz = selectedExportSampleRates();
}

void WolfieApp::saveCurrentWorkspaceIfOpen() {
    if (workspace_.rootPath.empty()) {
        return;
    }

    syncStateFromControls();
    workspaceRepository_.save(workspace_);
    targetCurvePersistencePending_ = false;
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

void WolfieApp::invalidateFilterDesign() {
    workspace_.filterResult = {};
}

void WolfieApp::ensureFilterDesignReady() {
    if (workspace_.filterResult.valid) {
        return;
    }

    ensureSmoothedResponseReady();
    if (workspace_.smoothedResponse.frequencyAxisHz.empty()) {
        return;
    }

    measurement::normalizeFilterDesignSettings(workspace_.filters, workspace_.measurement.sampleRate);
    workspace_.filterResult = measurement::designFilters(workspace_.smoothedResponse,
                                                         workspace_.measurement,
                                                         workspace_.targetCurve,
                                                         workspace_.filters,
                                                         &workspace_.result);
}

void WolfieApp::setExportInProgress(bool running) {
    exportRunning_ = running;
    if (exportButton_ == nullptr || exportProgress_ == nullptr) {
        return;
    }

    for (HWND checkbox : exportSampleRateChecks_) {
        EnableWindow(checkbox, running ? FALSE : TRUE);
    }

    ShowWindow(exportButton_, running ? SW_HIDE : SW_SHOW);
    ShowWindow(exportProgress_, running ? SW_SHOW : SW_HIDE);
    if (!running) {
        SetWindowTextW(exportProgress_, L"");
    }

    RedrawWindow(pageExport_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

void WolfieApp::showExportProgress(const std::wstring& message) const {
    if (exportProgress_ == nullptr) {
        return;
    }

    SetWindowTextW(exportProgress_, message.c_str());
    RedrawWindow(exportProgress_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    if (exportStatus_ != nullptr) {
        RedrawWindow(exportStatus_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

std::vector<int> WolfieApp::selectedExportSampleRates() const {
    std::vector<int> selectedSampleRates;
    const std::vector<int>& allSampleRates = measurement::roonCommonSampleRates();
    const std::size_t checkboxCount = std::min(exportSampleRateChecks_.size(), allSampleRates.size());
    selectedSampleRates.reserve(checkboxCount);
    for (std::size_t index = 0; index < checkboxCount; ++index) {
        if (SendMessageW(exportSampleRateChecks_[index], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            selectedSampleRates.push_back(allSampleRates[index]);
        }
    }
    return selectedSampleRates;
}

void WolfieApp::updateExportControls() {
    if (exportButton_ == nullptr || exportProgress_ == nullptr || exportStatus_ == nullptr) {
        return;
    }

    for (HWND checkbox : exportSampleRateChecks_) {
        EnableWindow(checkbox, exportRunning_ ? FALSE : TRUE);
    }

    ShowWindow(exportButton_, exportRunning_ ? SW_HIDE : SW_SHOW);
    ShowWindow(exportProgress_, exportRunning_ ? SW_SHOW : SW_HIDE);
    if (exportRunning_) {
        return;
    }

    const std::vector<int> selectedSampleRates = selectedExportSampleRates();
    const bool hasSelection = !selectedSampleRates.empty();
    const bool canExport = !workspace_.rootPath.empty() && workspace_.result.hasAnyValues() && hasSelection;
    EnableWindow(exportButton_, canExport ? TRUE : FALSE);
    if (workspace_.rootPath.empty() || !workspace_.result.hasAnyValues()) {
        SetWindowTextW(exportStatus_, L"Open a workspace and complete a measurement before exporting.");
        return;
    }
    if (!hasSelection) {
        SetWindowTextW(exportStatus_, L"Select at least one sample rate to export.");
        return;
    }

    SetWindowTextW(exportStatus_, L"");
}

void WolfieApp::exportRoonFilters() {
    if (workspace_.rootPath.empty()) {
        appendLog(L"Roon export failed: no workspace is open.", LogSeverity::Error);
        updateExportControls();
        return;
    }

    syncStateFromControls();
    ensureSmoothedResponseReady();
    if (workspace_.smoothedResponse.frequencyAxisHz.empty()) {
        SetWindowTextW(exportStatus_, L"No smoothed response is available. Run a measurement first.");
        appendLog(L"Roon export failed: no smoothed response is available.", LogSeverity::Error);
        return;
    }

    const std::vector<int> selectedSampleRates = selectedExportSampleRates();
    if (selectedSampleRates.empty()) {
        SetWindowTextW(exportStatus_, L"Select at least one sample rate to export.");
        appendLog(L"Roon export failed: no sample rates are selected.", LogSeverity::Error);
        updateExportControls();
        return;
    }

    const std::string activeProfileName = workspace_.activeTargetCurveProfileName.empty()
                                              ? std::string("Default")
                                              : workspace_.activeTargetCurveProfileName;
    const std::filesystem::path exportDirectory = workspace_.rootPath / "export" / sanitizeFileComponent(activeProfileName);
    appendLog(L"Roon export started: writing filter set to " + exportDirectory.wstring());
    setExportInProgress(true);
    SetWindowTextW(exportStatus_,
                   (L"Writing the Roon filter set to:\r\n" + exportDirectory.wstring()).c_str());
    showExportProgress(L"Preparing sample-rate export...");

    std::vector<std::filesystem::path> generatedFiles;
    std::wstring errorMessage;
    if (!measurement::exportRoonFilterWavSet(exportDirectory,
                                             workspace_.smoothedResponse,
                                             workspace_.measurement,
                                             workspace_.targetCurve,
                                             workspace_.filters,
                                             &workspace_.result,
                                             selectedSampleRates,
                                             generatedFiles,
                                             errorMessage,
                                             [this](int sampleRate, std::size_t sampleRateIndex, std::size_t totalSampleRates) {
                                                 const std::wstring sampleRateLabel = formatSampleRateLabel(sampleRate);
                                                 appendLog(L"Roon export: processing " +
                                                           sampleRateLabel +
                                                           L" (" + std::to_wstring(sampleRateIndex) +
                                                           L"/" + std::to_wstring(totalSampleRates) + L")");
                                                 showExportProgress(L"Processing " +
                                                                    sampleRateLabel +
                                                                    L" (" + std::to_wstring(sampleRateIndex) +
                                                                    L"/" + std::to_wstring(totalSampleRates) + L")");
                                             })) {
        setExportInProgress(false);
        SetWindowTextW(exportStatus_, errorMessage.c_str());
        appendLog(L"Roon export failed: " + errorMessage, LogSeverity::Error);
        return;
    }

    setExportInProgress(false);
    const std::size_t exportedSampleRateCount = selectedSampleRates.size();
    const std::filesystem::path zipPath = measurement::roonFilterArchivePath(exportDirectory);
    const std::wstring status =
        L"Generated " + std::to_wstring(exportedSampleRateCount) +
        L" WAV/config pairs and packed them into:\r\n" + zipPath.wstring();
    SetWindowTextW(exportStatus_, status.c_str());
    appendLog(L"Roon export completed: wrote " +
              std::to_wstring(exportedSampleRateCount) +
              L" WAV/config pairs and " + zipPath.filename().wstring() +
              L" to " + exportDirectory.wstring());
}

void WolfieApp::onCommand(WORD commandId, WORD notificationCode) {
    bool measurePressed = false;
    bool sampleRateChanged = false;
    bool measurementGraphZoomChanged = false;
    bool measurementPlotChanged = false;
    if (measurementPage_.handleCommand(commandId,
                                       notificationCode,
                                       workspace_,
                                       measurePressed,
                                       sampleRateChanged,
                                       measurementGraphZoomChanged,
                                       measurementPlotChanged)) {
        if (measurementGraphZoomChanged) {
            return;
        }
        if (measurementPlotChanged) {
            workspaceRepository_.save(workspace_);
            return;
        }
        if (sampleRateChanged) {
            invalidateFilterDesign();
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
            invalidateFilterDesign();
            ensureSmoothedResponseReady();
            smoothingPage_.populate(workspace_);
            filtersPage_.populate(workspace_);
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
        }
        return;
    }

    bool targetCurveChanged = false;
    bool targetCurvePersistencePending = false;
    bool targetCurvePersistNowRequested = false;
    if (targetCurvePage_.handleCommand(commandId,
                                       notificationCode,
                                       workspace_,
                                       targetCurveChanged,
                                       targetCurvePersistencePending,
                                       targetCurvePersistNowRequested)) {
        if (targetCurveChanged) {
            invalidateFilterDesign();
        }
        if (targetCurvePersistencePending) {
            targetCurvePersistencePending_ = true;
        }
        if (targetCurvePersistNowRequested) {
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
            targetCurvePersistencePending_ = false;
        }
        return;
    }

    bool filterSettingsChanged = false;
    bool filtersRecalculateRequested = false;
    bool filterViewSettingsChanged = false;
    if (filtersPage_.handleCommand(commandId,
                                   notificationCode,
                                   workspace_,
                                   filterSettingsChanged,
                                   filtersRecalculateRequested,
                                   filterViewSettingsChanged)) {
        if (filterSettingsChanged) {
            invalidateFilterDesign();
            filtersPage_.populate(workspace_);
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
        }
        if (filtersRecalculateRequested) {
            invalidateFilterDesign();
            ensureFilterDesignReady();
            filtersPage_.populate(workspace_);
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
        }
        if (filterViewSettingsChanged) {
            workspaceRepository_.save(workspace_);
        }
        return;
    }

    if (commandId >= kExportSampleRateCheckboxBase &&
        commandId < kExportSampleRateCheckboxBase + static_cast<WORD>(exportSampleRateChecks_.size())) {
        if (notificationCode == BN_CLICKED) {
            syncExportSampleRatesToWorkspace();
            workspaceRepository_.save(workspace_);
            updateExportControls();
        }
        return;
    }

    switch (commandId) {
    case kButtonExportRoon:
        if (notificationCode == BN_CLICKED) {
            exportRoonFilters();
        }
        return;
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
            invalidateFilterDesign();
            ensureSmoothedResponseReady();
            smoothingPage_.populate(workspace_);
            targetCurvePage_.populate(workspace_);
            filtersPage_.populate(workspace_);
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
        }
        return;
    }

    bool targetCurveChanged = false;
    bool targetCurvePersistencePending = false;
    if (targetCurvePage_.handleHScroll(source, workspace_, targetCurveChanged, targetCurvePersistencePending)) {
        if (targetCurveChanged) {
            invalidateFilterDesign();
        }
        if (targetCurvePersistencePending) {
            targetCurvePersistencePending_ = true;
        }
    }
}

void WolfieApp::onNotify(LPARAM lParam) {
    const auto* header = reinterpret_cast<const NMHDR*>(lParam);
    if (header != nullptr && header->hwndFrom == tabControl_ && header->code == TCN_SELCHANGE) {
        const int selected = TabCtrl_GetCurSel(tabControl_);
        if (activeTabIndex_ == 2 && selected != 2) {
            persistTargetCurveStateIfPending();
        }
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
    if (selected == 1 || selected == 2 || selected == 3 || selected == 4) {
        ensureSmoothedResponseReady();
        if (selected == 1) {
            smoothingPage_.populate(workspace_);
        } else if (selected == 2) {
            targetCurvePage_.populate(workspace_);
        } else if (selected == 3) {
            filtersPage_.populate(workspace_);
        } else {
            updateExportControls();
        }
    }

    measurementPage_.setVisible(selected == 0);
    smoothingPage_.setVisible(selected == 1);
    targetCurvePage_.setVisible(selected == 2);
    filtersPage_.setVisible(selected == 3);
    ShowWindow(pageExport_, selected == 4 ? SW_SHOW : SW_HIDE);
    activeTabIndex_ = selected;
}

void WolfieApp::persistTargetCurveStateIfPending() {
    if (!targetCurvePersistencePending_) {
        return;
    }
    if (workspace_.rootPath.empty()) {
        return;
    }

    syncStateFromControls();
    workspaceRepository_.save(workspace_);
    targetCurvePersistencePending_ = false;
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
    measurement::normalizeFilterDesignSettings(workspace_.filters, workspace_.measurement.sampleRate);
    measurement::normalizeTargetCurveSettings(workspace_.targetCurve,
                                              std::max(workspace_.measurement.startFrequencyHz, 20.0),
                                              std::min(workspace_.measurement.endFrequencyHz, 20000.0));
    workspace_.activeTargetCurveProfileName = "Default";
    workspace_.activeTargetCurveComment.clear();
    workspace_.targetCurveProfiles.push_back({workspace_.activeTargetCurveProfileName,
                                              workspace_.activeTargetCurveComment,
                                              workspace_.targetCurve});
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
    invalidateFilterDesign();
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
    targetCurvePersistencePending_ = false;
    refreshWindowTitle();
    updateExportControls();
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
                         L" Hz.");
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
    invalidateFilterDesign();
    measurementPage_.setMeasurementResult(workspace_.result);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::stopMeasurement() {
    if (!measurementController_.status().running) {
        return;
    }

    appendMeasurementLog(L"Measurement stopped by user.");
    measurementCompletionHandled_ = true;
    measurementController_.cancel();
    KillTimer(mainWindow_, kMeasurementTimerId);
    if (!workspace_.rootPath.empty()) {
        const WorkspaceState persistedWorkspace = workspaceRepository_.load(workspace_.rootPath);
        workspace_.result = persistedWorkspace.result;
        workspace_.smoothedResponse = {};
        invalidateFilterDesign();
        measurementPage_.setMeasurementResult(workspace_.result);
        smoothingPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
        filtersPage_.populate(workspace_);
    }
    refreshMeasurementStatus();
}

void WolfieApp::finalizeMeasurement() {
    const MeasurementStatus completedStatus = measurementController_.status();
    workspace_.result = measurementController_.result();
    workspace_.smoothedResponse = {};
    invalidateFilterDesign();
    const int selected = tabControl_ == nullptr ? 0 : TabCtrl_GetCurSel(tabControl_);
    if (selected == 1 || selected == 2 || selected == 3) {
        ensureSmoothedResponseReady();
    }
    measurementPage_.setMeasurementResult(workspace_.result);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
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
