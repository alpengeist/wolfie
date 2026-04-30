#include "wolfie_app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include <commctrl.h>
#include <richedit.h>
#include <shobjidl.h>

#include "audio/asio_audio_backend.h"
#include "core/text_utils.h"
#include "measurement/response_analyzer.h"
#include "measurement/filter_designer.h"
#include "measurement/filter_wav_export.h"
#include "measurement/room_simulator.h"
#include "measurement/response_smoother.h"
#include "measurement/target_curve_designer.h"
#include "persistence/microphone_calibration_repository.h"
#include "persistence/wave_file_repository.h"
#include "wolfie_resources.h"
#include "ui/plot_graph.h"
#include "ui/response_graph.h"
#include "ui/settings_dialog.h"
#include "ui/splash_screen.h"
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

std::wstring staleReferenceReason(const AudioSettings& audio,
                                  const MeasurementSettings& measurement,
                                  const MeasurementResult& referenceResult) {
    if (!referenceResult.hasAnyValues()) {
        return L"missing";
    }
    if (!audio.loopbackEnabled) {
        return L"loopback off";
    }

    const MeasurementAnalysis& analysis = referenceResult.analysis;
    if (analysis.sampleRate > 0 && analysis.sampleRate != measurement.sampleRate) {
        return L"sample rate changed";
    }
    if (!analysis.requestedBackend.empty() && analysis.requestedBackend != audio.backend) {
        return L"backend changed";
    }
    if (audio.backend == "asio") {
        if (!analysis.requestedDriver.empty() && analysis.requestedDriver != audio.driver) {
            return L"driver changed";
        }
    } else {
        if (!analysis.requestedWindowsInputDeviceId.empty() &&
            analysis.requestedWindowsInputDeviceId != audio.windowsInputDeviceId) {
            return L"input device changed";
        }
        if (!analysis.requestedWindowsOutputDeviceId.empty() &&
            analysis.requestedWindowsOutputDeviceId != audio.windowsOutputDeviceId) {
            return L"output device changed";
        }
    }
    if (analysis.requestedMicInputChannel > 0 && analysis.requestedMicInputChannel != audio.loopbackInputChannel) {
        return L"loopback input changed";
    }
    if (analysis.requestedLeftOutputChannel > 0 && analysis.requestedLeftOutputChannel != audio.leftOutputChannel) {
        return L"left output changed";
    }
    if (analysis.requestedRightOutputChannel > 0 && analysis.requestedRightOutputChannel != audio.rightOutputChannel) {
        return L"right output changed";
    }
    return {};
}

constexpr int kMenuFileNew = 1001;
constexpr int kMenuFileOpen = 1002;
constexpr int kMenuFileSave = 1003;
constexpr int kMenuFileSaveAs = 1004;
constexpr int kMenuFileSettings = 1005;
constexpr int kMenuFileRecentBase = 1100;
constexpr int kTabMain = 3013;
constexpr int kProcessLog = 3015;
constexpr int kProcessLogSizeCompact = 3016;
constexpr int kProcessLogSizeMedium = 3017;
constexpr int kProcessLogSizeExpanded = 3018;
constexpr int kButtonExportRoon = 3019;
constexpr int kExportSampleRateCheckboxBase = 3020;
constexpr wchar_t kMainClassName[] = L"WolfieMainWindow";
constexpr int kLogDividerHeight = 2;
constexpr int kLogLabelHeight = 20;
constexpr int kLogHeaderBottomGap = 4;
constexpr int kCompactLogHeight = 86;
constexpr int kMediumLogHeight = 190;
constexpr int kExpandedLogHeight = 320;
constexpr int kMinTabHeight = 360;
constexpr int kLogSizeButtonWidth = 24;
constexpr int kLogSizeButtonHeight = 18;
constexpr int kLogSizeButtonGap = 4;
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

int processLogHeightForSize(ProcessLogSize size) {
    switch (size) {
    case ProcessLogSize::Compact:
        return kCompactLogHeight;
    case ProcessLogSize::Expanded:
        return kExpandedLogHeight;
    case ProcessLogSize::Medium:
    default:
        return kMediumLogHeight;
    }
}

void pumpPendingMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
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

    ui::SplashScreen::registerWindowClass(instance_);
    ui::SplashScreen splashScreen;
    splashScreen.create(instance_);
    splashScreen.setStatus(L"Loading application state", 1, 3);
    splashScreen.show();
    pumpPendingMessages();

    appState_ = appStateRepository_.load();
    splashScreen.setStatus(L"Creating main window", 2, 3);
    pumpPendingMessages();

    createMainWindow();

    splashScreen.setStatus(L"Loading workspace", 3, 3);
    pumpPendingMessages();
    loadLastWorkspaceIfPossible();

    splashScreen.destroy();
    ShowWindow(mainWindow_, SW_SHOW);
    UpdateWindow(mainWindow_);

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
    case WM_DRAWITEM:
        if (app->handleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        if (app->measurementPage_.handleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
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
    const HICON largeIcon = reinterpret_cast<HICON>(
        LoadImageW(instance_,
                   MAKEINTRESOURCEW(IDI_WOLFIE_APP),
                   IMAGE_ICON,
                   GetSystemMetrics(SM_CXICON),
                   GetSystemMetrics(SM_CYICON),
                   LR_DEFAULTCOLOR));
    const HICON smallIcon = reinterpret_cast<HICON>(
        LoadImageW(instance_,
                   MAKEINTRESOURCEW(IDI_WOLFIE_APP),
                   IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON),
                   LR_DEFAULTCOLOR));

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = instance_;
    mainClass.lpszClassName = kMainClassName;
    mainClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mainClass.hbrBackground = ui_theme::backgroundBrush();
    mainClass.hIcon = largeIcon;
    mainClass.hIconSm = smallIcon;
    RegisterClassExW(&mainClass);

    ui::ResponseGraph::registerWindowClass(instance_);
    ui::WaterfallGraph::registerWindowClass(instance_);
    ui::TargetCurveGraph::registerWindowClass(instance_);
    ui::PlotGraph::registerWindowClass(instance_);
    ui::MeasurementPage::registerPageWindowClass(instance_);
    ui::AnalysisPage::registerPageWindowClass(instance_);
    ui::SmoothingPage::registerPageWindowClass(instance_);
    ui::TargetCurvePage::registerPageWindowClass(instance_);
    ui::FiltersPage::registerPageWindowClass(instance_);

    mainWindow_ = CreateWindowExW(0, kMainClassName, L"Wolfie", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
    item.pszText = const_cast<LPWSTR>(L"Analysis");
    TabCtrl_InsertItem(tabControl_, 1, &item);
    item.pszText = const_cast<LPWSTR>(L"Smoothing");
    TabCtrl_InsertItem(tabControl_, 2, &item);
    item.pszText = const_cast<LPWSTR>(L"Target Curve");
    TabCtrl_InsertItem(tabControl_, 3, &item);
    item.pszText = const_cast<LPWSTR>(L"Filters");
    TabCtrl_InsertItem(tabControl_, 4, &item);
    item.pszText = const_cast<LPWSTR>(L"Export");
    TabCtrl_InsertItem(tabControl_, 5, &item);

    measurementPage_.create(tabControl_, instance_);
    analysisPage_.create(tabControl_, instance_);
    smoothingPage_.create(tabControl_, instance_);
    targetCurvePage_.create(tabControl_, instance_);
    filtersPage_.create(tabControl_, instance_);
    pageAnalysis_ = analysisPage_.window();
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
    logDivider_ = CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                                0, 0, 0, 0, mainWindow_, nullptr, instance_, nullptr);
    logLabel_ = CreateWindowW(L"STATIC", L"Process Log", WS_CHILD | WS_VISIBLE,
                              0, 0, 0, 0, mainWindow_, nullptr, instance_, nullptr);
    logSizeCompactButton_ = CreateWindowW(L"BUTTON",
                                          L"\x2581",
                                          WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON | BS_OWNERDRAW,
                                          0,
                                          0,
                                          0,
                                          0,
                                          mainWindow_,
                                          reinterpret_cast<HMENU>(kProcessLogSizeCompact),
                                          instance_,
                                          nullptr);
    logSizeMediumButton_ = CreateWindowW(L"BUTTON",
                                         L"\x2584",
                                         WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_OWNERDRAW,
                                         0,
                                         0,
                                         0,
                                         0,
                                         mainWindow_,
                                         reinterpret_cast<HMENU>(kProcessLogSizeMedium),
                                         instance_,
                                         nullptr);
    logSizeExpandedButton_ = CreateWindowW(L"BUTTON",
                                           L"\x2588",
                                           WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_OWNERDRAW,
                                           0,
                                           0,
                                           0,
                                           0,
                                           mainWindow_,
                                           reinterpret_cast<HMENU>(kProcessLogSizeExpanded),
                                           instance_,
                                           nullptr);
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
    updateProcessLogSizeButtons();

    updateVisibleTab();
    layoutMainWindow();
    appendLog(L"Application ready.");
}

void WolfieApp::layoutMainWindow() {
    const RECT bounds = clientRect(mainWindow_);
    const int width = std::max(320L, bounds.right - (2 * kContentMargin));
    const int height = std::max(360L, bounds.bottom - (2 * kContentMargin));
    const int preferredLogHeight = processLogHeightForSize(workspace_.ui.processLogSize);
    const int maxLogHeight = std::max(kCompactLogHeight, height - kMinTabHeight - kLogDividerHeight);
    const int logHeight = std::min(preferredLogHeight, maxLogHeight);
    const int tabHeight = std::max(kMinTabHeight, height - logHeight - kLogDividerHeight);
    MoveWindow(tabControl_, kContentMargin, kContentMargin, width, tabHeight, TRUE);

    const int dividerTop = kContentMargin + tabHeight;
    MoveWindow(logDivider_, kContentMargin, dividerTop, width, kLogDividerHeight, TRUE);

    const int logTop = dividerTop + kLogDividerHeight;
    const int buttonsWidth = (3 * kLogSizeButtonWidth) + (2 * kLogSizeButtonGap);
    const int buttonsLeft = kContentMargin + width - buttonsWidth;
    const int labelWidth = std::max(80, buttonsLeft - kContentMargin - 8);
    MoveWindow(logLabel_, kContentMargin, logTop + 2, labelWidth, kLogLabelHeight, TRUE);
    MoveWindow(logSizeCompactButton_,
               buttonsLeft,
               logTop + 1,
               kLogSizeButtonWidth,
               kLogSizeButtonHeight,
               TRUE);
    MoveWindow(logSizeMediumButton_,
               buttonsLeft + kLogSizeButtonWidth + kLogSizeButtonGap,
               logTop + 1,
               kLogSizeButtonWidth,
               kLogSizeButtonHeight,
               TRUE);
    MoveWindow(logSizeExpandedButton_,
               buttonsLeft + (2 * (kLogSizeButtonWidth + kLogSizeButtonGap)),
               logTop + 1,
               kLogSizeButtonWidth,
               kLogSizeButtonHeight,
               TRUE);
    MoveWindow(logEdit_,
               kContentMargin,
               logTop + kLogLabelHeight + kLogHeaderBottomGap,
               width,
               std::max(40, logHeight - kLogLabelHeight - kLogHeaderBottomGap),
               TRUE);

    RECT tabRect{};
    GetClientRect(tabControl_, &tabRect);
    TabCtrl_AdjustRect(tabControl_, FALSE, &tabRect);
    const int pageWidth = std::max(320L, tabRect.right - tabRect.left);
    const int pageHeight = std::max(240L, tabRect.bottom - tabRect.top);
    MoveWindow(measurementPage_.window(), tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageAnalysis_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageSmoothing_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageTargetCurve_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(filtersPage_.window(), tabRect.left, tabRect.top + 10, pageWidth, std::max(240, pageHeight - 10), TRUE);
    MoveWindow(pageExport_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    layoutContent();
}

bool WolfieApp::handleDrawItem(const DRAWITEMSTRUCT* drawItem) const {
    if (drawItem == nullptr) {
        return false;
    }

    if (drawItem->CtlID != kProcessLogSizeCompact &&
        drawItem->CtlID != kProcessLogSizeMedium &&
        drawItem->CtlID != kProcessLogSizeExpanded) {
        return false;
    }

    const bool checked = SendMessageW(drawItem->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
    FillRect(drawItem->hDC, &drawItem->rcItem, ui_theme::backgroundBrush());

    wchar_t glyph[8] = {};
    GetWindowTextW(drawItem->hwndItem, glyph, static_cast<int>(sizeof(glyph) / sizeof(glyph[0])));

    SetBkMode(drawItem->hDC, TRANSPARENT);
    SetTextColor(drawItem->hDC, checked ? ui_theme::kAccent : ui_theme::kMuted);

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(drawItem->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ previousFont = nullptr;
    if (font != nullptr) {
        previousFont = SelectObject(drawItem->hDC, font);
    }

    RECT glyphRect = drawItem->rcItem;
    if ((drawItem->itemState & ODS_SELECTED) != 0) {
        OffsetRect(&glyphRect, 0, 1);
    }
    DrawTextW(drawItem->hDC, glyph, -1, &glyphRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (checked) {
        RECT underline = drawItem->rcItem;
        underline.top = std::max(underline.top, underline.bottom - 2);
        HBRUSH accentBrush = CreateSolidBrush(ui_theme::kAccent);
        FillRect(drawItem->hDC, &underline, accentBrush);
        DeleteObject(accentBrush);
    }

    if (previousFont != nullptr) {
        SelectObject(drawItem->hDC, previousFont);
    }

    return true;
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
    analysisPage_.layout();
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

void WolfieApp::updateProcessLogSizeButtons() const {
    SendMessageW(logSizeCompactButton_,
                 BM_SETCHECK,
                 workspace_.ui.processLogSize == ProcessLogSize::Compact ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(logSizeMediumButton_,
                 BM_SETCHECK,
                 workspace_.ui.processLogSize == ProcessLogSize::Medium ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(logSizeExpandedButton_,
                 BM_SETCHECK,
                 workspace_.ui.processLogSize == ProcessLogSize::Expanded ? BST_CHECKED : BST_UNCHECKED,
                 0);
}

void WolfieApp::setProcessLogSize(ProcessLogSize size) {
    if (workspace_.ui.processLogSize == size) {
        return;
    }

    workspace_.ui.processLogSize = size;
    updateProcessLogSizeButtons();
    layoutMainWindow();

    if (!workspace_.rootPath.empty()) {
        workspaceRepository_.saveUiSettings(workspace_);
    }
}

void WolfieApp::showSettingsWindow() {
    ui::SettingsDialog::show(instance_, mainWindow_, workspace_.audio, wasapiService_, asioService_, [this](const AudioSettings& settings) {
        workspace_.audio = settings;
        populateControlsFromState();
        syncStateFromControls();
        workspaceRepository_.save(workspace_);
        refreshMeasurementStatus();
    });
}

void WolfieApp::showRoomSimulationWindow() {
    roomSimulationDialog_.show(instance_,
                               mainWindow_,
                               [this](const std::string& name, const RoomSimulationSettings& settings) {
                                   saveRoomSimulationDefinition(name, settings);
                               },
                               [this](const std::string& name, const RoomSimulationSettings& settings) {
                                   generateRoomSimulationMeasurement(name, settings);
                               });
    roomSimulationDialog_.populate(workspace_);
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
    if (!roomSimulationDialog_.isOpen()) {
        roomSimulationDialog_.populate(workspace_);
    }
    analysisPage_.populate(workspace_);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    populateExportSampleRateControls();
    updateExportControls();
    updateProcessLogSizeButtons();
    layoutMainWindow();
}

void WolfieApp::syncStateFromControls() {
    measurementPage_.syncToWorkspace(workspace_);
    analysisPage_.syncToWorkspace(workspace_);
    smoothingPage_.syncToWorkspace(workspace_);
    targetCurvePage_.syncToWorkspace(workspace_);
    filtersPage_.syncToWorkspace(workspace_);
    syncExportSampleRatesToWorkspace();
}

WolfieApp::CalibrationReanalysisTaskResult
WolfieApp::buildReanalyzedMeasurementWithCurrentMicCalibration(const WorkspaceState& workspace,
                                                               const std::shared_ptr<CalibrationReanalysisProgress>& progress) {
    CalibrationReanalysisTaskResult taskResult;
    if (!workspace.result.hasAnyValues()) {
        taskResult.success = true;
        return taskResult;
    }

    if (progress != nullptr) {
        progress->currentStep.store(1);
    }

    const MeasurementAnalysis& existingAnalysis = workspace.result.analysis;
    const MeasurementArtifact* rawCaptureArtifact = existingAnalysis.findArtifact("raw_capture_wav");
    if (rawCaptureArtifact == nullptr || rawCaptureArtifact->path.empty()) {
        taskResult.errorMessage = L"Could not reanalyze the measurement because the raw capture WAV is unavailable.";
        return taskResult;
    }

    int captureSampleRate = 0;
    std::vector<int16_t> capturedSamples;
    std::wstring errorMessage;
    if (!persistence::loadMonoPcm16WaveFile(rawCaptureArtifact->path, captureSampleRate, capturedSamples, errorMessage)) {
        taskResult.errorMessage = L"Could not reanalyze the measurement: " + errorMessage;
        return taskResult;
    }

    if (progress != nullptr) {
        progress->currentStep.store(2);
    }

    MeasurementSettings analysisSettings = workspace.measurement;
    analysisSettings.sampleRate = captureSampleRate;
    if (existingAnalysis.fadeInSeconds > 0.0) {
        analysisSettings.fadeInSeconds = existingAnalysis.fadeInSeconds;
    }
    if (existingAnalysis.fadeOutSeconds > 0.0) {
        analysisSettings.fadeOutSeconds = existingAnalysis.fadeOutSeconds;
    }
    if (existingAnalysis.sweepDurationSeconds > 0.0) {
        analysisSettings.durationSeconds = existingAnalysis.sweepDurationSeconds;
    }
    if (existingAnalysis.startFrequencyHz > 0.0) {
        analysisSettings.startFrequencyHz = existingAnalysis.startFrequencyHz;
    }
    if (existingAnalysis.endFrequencyHz > 0.0) {
        analysisSettings.endFrequencyHz = existingAnalysis.endFrequencyHz;
    }
    if (existingAnalysis.targetLengthSamples > 0) {
        analysisSettings.targetLengthSamples = existingAnalysis.targetLengthSamples;
    }
    if (existingAnalysis.leadInSamples >= 0) {
        analysisSettings.leadInSamples = existingAnalysis.leadInSamples;
    }

    AudioSettings analysisAudio = workspace.audio;
    if (!existingAnalysis.requestedBackend.empty()) {
        analysisAudio.backend = existingAnalysis.requestedBackend;
    }
    if (!existingAnalysis.requestedDriver.empty()) {
        analysisAudio.driver = existingAnalysis.requestedDriver;
    }
    if (!existingAnalysis.requestedWindowsInputDeviceId.empty()) {
        analysisAudio.windowsInputDeviceId = existingAnalysis.requestedWindowsInputDeviceId;
    }
    if (!existingAnalysis.requestedWindowsInputDeviceName.empty()) {
        analysisAudio.windowsInputDeviceName = existingAnalysis.requestedWindowsInputDeviceName;
    }
    if (!existingAnalysis.requestedWindowsOutputDeviceId.empty()) {
        analysisAudio.windowsOutputDeviceId = existingAnalysis.requestedWindowsOutputDeviceId;
    }
    if (!existingAnalysis.requestedWindowsOutputDeviceName.empty()) {
        analysisAudio.windowsOutputDeviceName = existingAnalysis.requestedWindowsOutputDeviceName;
    }
    if (existingAnalysis.requestedMicInputChannel > 0) {
        analysisAudio.micInputChannel = existingAnalysis.requestedMicInputChannel;
    }
    if (existingAnalysis.requestedLeftOutputChannel > 0) {
        analysisAudio.leftOutputChannel = existingAnalysis.requestedLeftOutputChannel;
    }
    if (existingAnalysis.requestedRightOutputChannel > 0) {
        analysisAudio.rightOutputChannel = existingAnalysis.requestedRightOutputChannel;
    }
    analysisAudio.outputVolumeDb = existingAnalysis.outputVolumeDb;

    if (progress != nullptr) {
        progress->currentStep.store(3);
    }

    const measurement::SweepPlaybackPlan playbackPlan =
        measurement::buildSweepPlaybackPlan(analysisSettings, analysisAudio.outputVolumeDb, MeasurementRunMode::Room);
    MeasurementResult reanalyzed = measurement::buildMeasurementResultFromCapture(capturedSamples,
                                                                                  playbackPlan,
                                                                                  captureSampleRate,
                                                                                  analysisAudio,
                                                                                  analysisSettings,
                                                                                  workspace.referenceResult.hasAnyValues()
                                                                                      ? &workspace.referenceResult
                                                                                      : nullptr);

    reanalyzed.analysis.measurementKind =
        existingAnalysis.measurementKind.empty() ? reanalyzed.analysis.measurementKind : existingAnalysis.measurementKind;
    reanalyzed.analysis.measurementTimestampUtc = existingAnalysis.measurementTimestampUtc;
    reanalyzed.analysis.backendName = existingAnalysis.backendName;
    reanalyzed.analysis.backendInputDevice = existingAnalysis.backendInputDevice;
    reanalyzed.analysis.backendOutputDevice = existingAnalysis.backendOutputDevice;
    reanalyzed.analysis.routingSelectionHonored = existingAnalysis.routingSelectionHonored;
    reanalyzed.analysis.routingNotes = existingAnalysis.routingNotes;
    reanalyzed.analysis.artifacts = existingAnalysis.artifacts;
    taskResult.success = true;
    taskResult.result = std::move(reanalyzed);
    return taskResult;
}

void WolfieApp::beginCalibrationReanalysis() {
    if (calibrationReanalysisInProgress_) {
        return;
    }

    calibrationReanalysisInProgress_ = true;
    if (tabControl_ != nullptr) {
        EnableWindow(tabControl_, FALSE);
    }
    if (HMENU menuBar = GetMenu(mainWindow_); menuBar != nullptr) {
        EnableMenuItem(menuBar, 0, MF_BYPOSITION | MF_DISABLED | MF_GRAYED);
        DrawMenuBar(mainWindow_);
    }
    calibrationReanalysisProgress_ = std::make_shared<CalibrationReanalysisProgress>();
    measurementPage_.setCalibrationRefreshInProgress(true, 1, CalibrationReanalysisProgress::kTotalSteps, L"Loading raw capture");
    appendMeasurementLog(L"Reanalyzing the saved measurement with the current microphone calibration...");
    const WorkspaceState workspaceSnapshot = workspace_;
    const auto progress = calibrationReanalysisProgress_;
    calibrationReanalysisFuture_ = std::async(std::launch::async, [workspaceSnapshot, progress]() {
        return buildReanalyzedMeasurementWithCurrentMicCalibration(workspaceSnapshot, progress);
    });
    SetTimer(mainWindow_, kCalibrationReanalysisTimerId, 50, nullptr);
}

void WolfieApp::finishCalibrationReanalysis() {
    if (!calibrationReanalysisFuture_.valid()) {
        calibrationReanalysisInProgress_ = false;
        calibrationReanalysisProgress_.reset();
        measurementPage_.setCalibrationRefreshInProgress(false);
        if (tabControl_ != nullptr) {
            EnableWindow(tabControl_, TRUE);
        }
        if (HMENU menuBar = GetMenu(mainWindow_); menuBar != nullptr) {
            EnableMenuItem(menuBar, 0, MF_BYPOSITION | MF_ENABLED);
            DrawMenuBar(mainWindow_);
        }
        return;
    }

    if (calibrationReanalysisProgress_ != nullptr) {
        calibrationReanalysisProgress_->currentStep.store(CalibrationReanalysisProgress::kTotalSteps);
    }
    measurementPage_.setCalibrationRefreshInProgress(true,
                                                     CalibrationReanalysisProgress::kTotalSteps,
                                                     CalibrationReanalysisProgress::kTotalSteps,
                                                     L"Refreshing views");
    CalibrationReanalysisTaskResult taskResult = calibrationReanalysisFuture_.get();
    calibrationReanalysisInProgress_ = false;
    calibrationReanalysisProgress_.reset();
    if (tabControl_ != nullptr) {
        EnableWindow(tabControl_, TRUE);
    }
    if (HMENU menuBar = GetMenu(mainWindow_); menuBar != nullptr) {
        EnableMenuItem(menuBar, 0, MF_BYPOSITION | MF_ENABLED);
        DrawMenuBar(mainWindow_);
    }

    if (!taskResult.success) {
        measurementPage_.setCalibrationRefreshInProgress(false);
        appendMeasurementLog(taskResult.errorMessage, LogSeverity::Error);
        measurementPage_.setWorkspaceView(workspace_);
        analysisPage_.populate(workspace_);
        smoothingPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
        filtersPage_.populate(workspace_);
        refreshMeasurementStatus();
        return;
    }

    workspace_.result = std::move(taskResult.result);
    workspace_.smoothedResponse = {};
    invalidateFilterDesign();
    ensureSmoothedResponseReady();
    measurementPage_.setWorkspaceView(workspace_);
    analysisPage_.populate(workspace_);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    workspaceRepository_.save(workspace_);
    measurementPage_.setCalibrationRefreshInProgress(false);
    appendMeasurementLog(L"Reanalyzed the saved measurement with the current microphone calibration.");
    refreshMeasurementStatus();
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

void WolfieApp::ensureFilterDesignReady(bool warnOnStaleReference) {
    if (workspace_.filterResult.valid) {
        return;
    }

    ensureSmoothedResponseReady();
    if (workspace_.smoothedResponse.frequencyAxisHz.empty()) {
        return;
    }

    const std::wstring ignoredReferenceReason =
        staleReferenceReason(workspace_.audio, workspace_.measurement, workspace_.referenceResult);
    if (warnOnStaleReference &&
        !ignoredReferenceReason.empty() &&
        ignoredReferenceReason != L"missing") {
        MessageBoxW(mainWindow_,
                    (L"The saved reference is stale (" + ignoredReferenceReason +
                     L") and will be ignored for this filter run.")
                        .c_str(),
                    L"Filter Design",
                    MB_OK | MB_ICONWARNING);
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

void WolfieApp::saveRoomSimulationDefinition(const std::string& name,
                                             const RoomSimulationSettings& settings) {
    if (name.empty()) {
        return;
    }

    RoomSimulationSettings normalized = settings;
    measurement::normalizeRoomSimulationSettings(normalized);

    auto it = std::find_if(workspace_.roomSimulations.begin(),
                           workspace_.roomSimulations.end(),
                           [&](const RoomSimulationDefinition& simulation) {
                               return simulation.name == name;
                           });
    if (it == workspace_.roomSimulations.end()) {
        workspace_.roomSimulations.push_back({name, normalized});
        std::sort(workspace_.roomSimulations.begin(),
                  workspace_.roomSimulations.end(),
                  [](const RoomSimulationDefinition& left, const RoomSimulationDefinition& right) {
                      return left.name < right.name;
                  });
    } else {
        it->settings = normalized;
    }
    workspace_.activeRoomSimulationName = name;

    if (!workspace_.rootPath.empty()) {
        roomSimulationRepository_.save(workspace_.rootPath, {name, normalized});
        workspaceRepository_.save(workspace_);
    }
}

void WolfieApp::generateRoomSimulationMeasurement(const std::string& name,
                                                  const RoomSimulationSettings& settings) {
    if (workspace_.rootPath.empty()) {
        appendMeasurementLog(L"Cannot generate a room simulation because no workspace is open.", LogSeverity::Error);
        return;
    }

    syncStateFromControls();
    saveRoomSimulationDefinition(name, settings);
    workspace_.result = measurement::buildSimulatedRoomMeasurement(workspace_.measurement, settings, name);
    workspace_.smoothedResponse = {};
    invalidateFilterDesign();

    const int selected = tabControl_ == nullptr ? 0 : TabCtrl_GetCurSel(tabControl_);
    if (selected == 2 || selected == 3 || selected == 4) {
        ensureSmoothedResponseReady();
    }

    measurementPage_.setWorkspaceView(workspace_);
    analysisPage_.populate(workspace_);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    workspaceRepository_.save(workspace_);
    refreshMeasurementStatus();
    appendMeasurementLog(L"Generated room simulation: " + toWide(name) + L".");
}

void WolfieApp::onCommand(WORD commandId, WORD notificationCode) {
    if (calibrationReanalysisInProgress_) {
        return;
    }

    bool roomMeasurePressed = false;
    bool referenceMeasurePressed = false;
    bool roomSimulationPressed = false;
    bool microphoneCalibrationChanged = false;
    bool sampleRateChanged = false;
    bool measurementGraphZoomChanged = false;
    bool measurementPlotChanged = false;
    if (measurementPage_.handleCommand(commandId,
                                       notificationCode,
                                       workspace_,
                                       roomMeasurePressed,
                                       referenceMeasurePressed,
                                       roomSimulationPressed,
                                       microphoneCalibrationChanged,
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
        if (microphoneCalibrationChanged) {
            std::wstring calibrationError;
            if (!persistence::loadMicrophoneCalibration(workspace_.audio, calibrationError)) {
                appendMeasurementLog(L"Microphone calibration unavailable: " + calibrationError, LogSeverity::Error);
            } else if (workspace_.audio.microphoneCalibrationPath.empty()) {
                appendMeasurementLog(L"Microphone calibration cleared.");
            } else {
                appendMeasurementLog(L"Microphone calibration loaded: " + workspace_.audio.microphoneCalibrationPath.wstring());
            }

            if (workspace_.result.hasAnyValues()) {
                beginCalibrationReanalysis();
                return;
            }

            measurementPage_.setWorkspaceView(workspace_);
            analysisPage_.populate(workspace_);
            smoothingPage_.populate(workspace_);
            targetCurvePage_.populate(workspace_);
            filtersPage_.populate(workspace_);
            workspaceRepository_.save(workspace_);
            refreshMeasurementStatus();
            return;
        }
        if (sampleRateChanged) {
            invalidateFilterDesign();
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
            refreshMeasurementStatus();
        }
        if (roomSimulationPressed) {
            showRoomSimulationWindow();
            return;
        }
        if (roomMeasurePressed || referenceMeasurePressed) {
            if (measurementController_.status().running) {
                stopMeasurement();
            } else {
                startMeasurement(referenceMeasurePressed ? MeasurementRunMode::Reference : MeasurementRunMode::Room);
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
            workspaceRepository_.saveSettings(workspace_);
        }
        return;
    }

    bool analysisViewSettingsChanged = false;
    if (analysisPage_.handleCommand(commandId, notificationCode, workspace_, analysisViewSettingsChanged)) {
        if (analysisViewSettingsChanged) {
            workspaceRepository_.saveUiSettings(workspace_);
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
            workspaceRepository_.saveSettings(workspace_);
        }
        if (filtersRecalculateRequested) {
            invalidateFilterDesign();
            filtersPage_.setRecalculateInProgress(true);
            ensureFilterDesignReady(true);
            filtersPage_.populate(workspace_);
            filtersPage_.setRecalculateInProgress(false);
            syncStateFromControls();
            workspaceRepository_.saveSettings(workspace_);
        }
        if (filterViewSettingsChanged) {
            workspaceRepository_.saveUiSettings(workspace_);
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
    case kProcessLogSizeCompact:
        if (notificationCode == BN_CLICKED) {
            setProcessLogSize(ProcessLogSize::Compact);
        }
        return;
    case kProcessLogSizeMedium:
        if (notificationCode == BN_CLICKED) {
            setProcessLogSize(ProcessLogSize::Medium);
        }
        return;
    case kProcessLogSizeExpanded:
        if (notificationCode == BN_CLICKED) {
            setProcessLogSize(ProcessLogSize::Expanded);
        }
        return;
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
    if (calibrationReanalysisInProgress_) {
        return;
    }

    if (measurementPage_.handleHScroll(source, workspace_)) {
        syncStateFromControls();
        workspaceRepository_.saveSettings(workspace_);
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
            workspaceRepository_.saveSettings(workspace_);
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
    if (calibrationReanalysisInProgress_) {
        return;
    }

    const auto* header = reinterpret_cast<const NMHDR*>(lParam);
    if (header != nullptr && header->hwndFrom == tabControl_ && header->code == TCN_SELCHANGE) {
        const int selected = TabCtrl_GetCurSel(tabControl_);
        if (activeTabIndex_ == 3 && selected != 3) {
            persistTargetCurveStateIfPending();
        }
        updateVisibleTab();
        layoutContent();
    }
}

void WolfieApp::onTimer(UINT_PTR timerId) {
    if (timerId == kCalibrationReanalysisTimerId) {
        if (!calibrationReanalysisInProgress_ || !calibrationReanalysisFuture_.valid()) {
            KillTimer(mainWindow_, kCalibrationReanalysisTimerId);
            return;
        }

        const int currentStep =
            calibrationReanalysisProgress_ == nullptr ? 1 : calibrationReanalysisProgress_->currentStep.load();
        std::wstring statusText = L"Loading raw capture";
        switch (currentStep) {
        case 1:
            statusText = L"Loading raw capture";
            break;
        case 2:
            statusText = L"Preparing analyzer inputs";
            break;
        case 3:
            statusText = L"Running analysis";
            break;
        case 4:
            statusText = L"Refreshing views";
            break;
        default:
            break;
        }
        measurementPage_.setCalibrationRefreshInProgress(true,
                                                         std::clamp(currentStep, 1, CalibrationReanalysisProgress::kTotalSteps),
                                                         CalibrationReanalysisProgress::kTotalSteps,
                                                         statusText);
        if (calibrationReanalysisFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            KillTimer(mainWindow_, kCalibrationReanalysisTimerId);
            finishCalibrationReanalysis();
        }
        return;
    }

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
    if (selected == 2 || selected == 3 || selected == 4 || selected == 5) {
        ensureSmoothedResponseReady();
        if (selected == 2) {
            smoothingPage_.populate(workspace_);
        } else if (selected == 3) {
            targetCurvePage_.populate(workspace_);
        } else if (selected == 4) {
            filtersPage_.populate(workspace_);
        } else {
            updateExportControls();
        }
    }
    if (selected == 1) {
        analysisPage_.populate(workspace_);
    }

    measurementPage_.setVisible(selected == 0);
    analysisPage_.setVisible(selected == 1);
    smoothingPage_.setVisible(selected == 2);
    targetCurvePage_.setVisible(selected == 3);
    filtersPage_.setVisible(selected == 4);
    ShowWindow(pageExport_, selected == 5 ? SW_SHOW : SW_HIDE);
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
    roomSimulationDialog_.close();
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
    roomSimulationDialog_.close();
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

void WolfieApp::startMeasurement(MeasurementRunMode runMode) {
    if (workspace_.rootPath.empty()) {
        appendMeasurementLog(L"Cannot start measurement because no workspace is open.", LogSeverity::Error);
        return;
    }

    syncStateFromControls();
    appendMeasurementLog(std::wstring(runMode == MeasurementRunMode::Reference ? L"Starting reference measurement at "
                                                                              : L"Starting sweep measurement at ") +
                         std::to_wstring(workspace_.measurement.sampleRate) +
                         L" Hz.");
    if (runMode == MeasurementRunMode::Room && !workspace_.audio.microphoneCalibrationPath.empty()) {
        if (workspace_.audio.microphoneCalibrationFrequencyHz.size() >= 2) {
            appendMeasurementLog(L"Using microphone calibration: " + workspace_.audio.microphoneCalibrationPath.wstring());
        } else {
            appendMeasurementLog(L"Configured microphone calibration could not be loaded: " +
                                     workspace_.audio.microphoneCalibrationPath.wstring(),
                                 LogSeverity::Error);
        }
    }
    if (!measurementController_.start(workspace_, runMode)) {
        refreshMeasurementStatus();
        if (!measurementController_.status().lastErrorMessage.empty()) {
            appendMeasurementLog(L"Start failed: " + measurementController_.status().lastErrorMessage, LogSeverity::Error);
        }
        return;
    }

    measurementCompletionHandled_ = false;
    appendMeasurementLog(L"Playback file written to " + measurementController_.status().generatedSweepPath.wstring());

    if (runMode == MeasurementRunMode::Room) {
        workspace_.result = {};
        workspace_.smoothedResponse = {};
        invalidateFilterDesign();
        analysisPage_.populate(workspace_);
        smoothingPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
        filtersPage_.populate(workspace_);
    }
    measurementPage_.setWorkspaceView(workspace_);
    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::stopMeasurement() {
    if (!measurementController_.status().running) {
        return;
    }

    appendMeasurementLog(measurementController_.status().runMode == MeasurementRunMode::Reference
                             ? L"Reference measurement stopped by user."
                             : L"Measurement stopped by user.");
    measurementCompletionHandled_ = true;
    measurementController_.cancel();
    KillTimer(mainWindow_, kMeasurementTimerId);
    if (!workspace_.rootPath.empty()) {
        const WorkspaceState persistedWorkspace = workspaceRepository_.load(workspace_.rootPath);
        workspace_.result = persistedWorkspace.result;
        workspace_.referenceResult = persistedWorkspace.referenceResult;
        workspace_.smoothedResponse = {};
        invalidateFilterDesign();
        measurementPage_.setWorkspaceView(workspace_);
        analysisPage_.populate(workspace_);
        smoothingPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
        filtersPage_.populate(workspace_);
    }
    refreshMeasurementStatus();
}

void WolfieApp::finalizeMeasurement() {
    const MeasurementStatus completedStatus = measurementController_.status();
    const bool referenceRun = completedStatus.runMode == MeasurementRunMode::Reference;
    if (referenceRun) {
        workspace_.referenceResult = measurementController_.result();
    } else {
        workspace_.result = measurementController_.result();
        workspace_.smoothedResponse = {};
        invalidateFilterDesign();
        const int selected = tabControl_ == nullptr ? 0 : TabCtrl_GetCurSel(tabControl_);
        if (selected == 2 || selected == 3 || selected == 4) {
            ensureSmoothedResponseReady();
        }
    }
    measurementPage_.setWorkspaceView(workspace_);
    analysisPage_.populate(workspace_);
    smoothingPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    syncStateFromControls();
    workspaceRepository_.save(workspace_);
    const MeasurementResult& finishedResult = referenceRun ? workspace_.referenceResult : workspace_.result;
    appendMeasurementLog(std::wstring(referenceRun ? L"Reference measurement finished. Generated "
                                                   : L"Measurement finished. Generated ") +
                         std::to_wstring(finishedResult.magnitudeResponse() == nullptr
                                              ? 0
                                              : finishedResult.magnitudeResponse()->xValues.size()) +
                         L" response points. Peak capture level " +
                         std::to_wstring(static_cast<int>(std::lround(completedStatus.peakAmplitudeDb))) +
                         L" dB.");
    appendMeasurementLog(L"Detected alignment: left " +
                         std::to_wstring(finishedResult.analysis.left.detectedLatencySamples) +
                         L" samples, right " +
                         std::to_wstring(finishedResult.analysis.right.detectedLatencySamples) +
                         L" samples.");
    if (finishedResult.analysis.captureClippingDetected) {
        appendMeasurementLog(L"Warning: clipping was detected during the sweep capture.", LogSeverity::Error);
    }
    if (finishedResult.analysis.captureTooQuiet) {
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
