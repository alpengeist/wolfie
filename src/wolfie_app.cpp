#include "wolfie_app.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string_view>

#include <commctrl.h>
#include <richedit.h>
#include <shobjidl.h>

#include "audio/asio_audio_backend.h"
#include "core/text_utils.h"
#include "measurement/filter_analysis.h"
#include "measurement/response_analyzer.h"
#include "measurement/filter_designer.h"
#include "measurement/filter_wav_export.h"
#include "measurement/room_simulator.h"
#include "measurement/response_smoother.h"
#include "measurement/sweet_spot_alignment.h"
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

HFONT createMenuFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) == FALSE) {
        return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    return CreateFontIndirectW(&metrics.lfMenuFont);
}

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

std::wstring getWindowTextValue(HWND control) {
    if (control == nullptr) {
        return {};
    }

    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }

    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int written = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(written, 0)));
    return text;
}

std::string boolToken(bool value) {
    return value ? "true" : "false";
}

std::string normalizeFilterViewMode(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "mixed" || value == "difference") {
        return value;
    }
    return "minimum";
}

void copyMixedPhaseSettings(FilterDesignSettings& target, const FilterDesignSettings& source) {
    target.mixedPhaseMaxFrequencyHz = source.mixedPhaseMaxFrequencyHz;
    target.excessPhaseWindowMs = source.excessPhaseWindowMs;
    target.mixedPhaseStrength = source.mixedPhaseStrength;
    target.mixedPhaseMaxCorrectionDegrees = source.mixedPhaseMaxCorrectionDegrees;
}

std::string buildExportTimestampText(const SYSTEMTIME& time) {
    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(4) << time.wYear
        << '-'
        << std::setw(2) << time.wMonth
        << '-'
        << std::setw(2) << time.wDay
        << 'T'
        << std::setw(2) << time.wHour
        << ':'
        << std::setw(2) << time.wMinute;
    return out.str();
}

std::string buildExportTimestampFileToken(const SYSTEMTIME& time) {
    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(4) << time.wYear
        << '-'
        << std::setw(2) << time.wMonth
        << '-'
        << std::setw(2) << time.wDay
        << 'T'
        << std::setw(2) << time.wHour
        << '-'
        << std::setw(2) << time.wMinute;
    return out.str();
}

void appendParameterLine(std::ostringstream& out, std::string_view key, std::string_view value) {
    out << key << '=';
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '\r':
            break;
        case '\n':
            out << "\\n";
            break;
        default:
            out << ch;
            break;
        }
    }
    out << '\n';
}

void appendParameterLine(std::ostringstream& out, std::string_view key, const std::wstring& value) {
    appendParameterLine(out, key, toUtf8(value));
}

void appendParameterLine(std::ostringstream& out, std::string_view key, double value, int decimals) {
    appendParameterLine(out, key, formatDouble(value, decimals));
}

void appendParameterLine(std::ostringstream& out, std::string_view key, int value) {
    appendParameterLine(out, key, std::to_string(value));
}

void appendTargetCurveParameters(std::ostringstream& out, const TargetCurveSettings& targetCurve) {
    appendParameterLine(out, "target_curve.lowGainDb", targetCurve.lowGainDb, 2);
    appendParameterLine(out, "target_curve.midFrequencyHz", targetCurve.midFrequencyHz, 2);
    appendParameterLine(out, "target_curve.midGainDb", targetCurve.midGainDb, 2);
    appendParameterLine(out, "target_curve.highGainDb", targetCurve.highGainDb, 2);
    appendParameterLine(out, "target_curve.levelOffsetDb", targetCurve.levelOffsetDb, 2);
    appendParameterLine(out, "target_curve.bypassEqBands", boolToken(targetCurve.bypassEqBands));
    appendParameterLine(out, "target_curve.eqBandCount", static_cast<int>(targetCurve.eqBands.size()));
    for (std::size_t index = 0; index < targetCurve.eqBands.size(); ++index) {
        const TargetEqBand& band = targetCurve.eqBands[index];
        const std::string prefix = "target_curve.eqBand[" + std::to_string(index) + "].";
        appendParameterLine(out, prefix + "enabled", boolToken(band.enabled));
        appendParameterLine(out, prefix + "colorIndex", band.colorIndex);
        appendParameterLine(out, prefix + "frequencyHz", band.frequencyHz, 2);
        appendParameterLine(out, prefix + "gainDb", band.gainDb, 2);
        appendParameterLine(out, prefix + "q", band.q, 3);
    }
}

void appendSmoothingParameters(std::ostringstream& out, const ResponseSmoothingSettings& smoothing) {
    appendParameterLine(out, "smoothing.psychoacousticModel", smoothing.psychoacousticModel);
    appendParameterLine(out, "smoothing.resolutionPercent", smoothing.resolutionPercent);
    appendParameterLine(out, "smoothing.lowFrequencyWindowCycles", smoothing.lowFrequencyWindowCycles, 2);
    appendParameterLine(out, "smoothing.highFrequencyWindowCycles", smoothing.highFrequencyWindowCycles, 2);
    appendParameterLine(out, "smoothing.highFrequencySlopeCutoffHz", smoothing.highFrequencySlopeCutoffHz, 2);
}

void appendFilterParameters(std::ostringstream& out, const FilterDesignSettings& filters) {
    appendParameterLine(out, "filters.tapCount", filters.tapCount);
    appendParameterLine(out, "filters.phaseMode", filters.phaseMode);
    appendParameterLine(out, "filters.maxBoostDb", filters.maxBoostDb, 2);
    appendParameterLine(out, "filters.maxCutDb", filters.maxCutDb, 2);
    appendParameterLine(out, "filters.smoothness", filters.smoothness, 2);
    appendParameterLine(out, "filters.lowCorrectionHz", filters.lowCorrectionHz, 2);
    appendParameterLine(out, "filters.lowTaperOctaves", filters.lowTaperOctaves, 2);
    appendParameterLine(out, "filters.highCorrectionHz", filters.highCorrectionHz, 2);
    appendParameterLine(out, "filters.highTaperOctaves", filters.highTaperOctaves, 2);
    appendParameterLine(out, "filters.displayPointCount", filters.displayPointCount);
    appendParameterLine(out, "filters.mixedPhaseMaxFrequencyHz", filters.mixedPhaseMaxFrequencyHz, 2);
    appendParameterLine(out, "filters.excessPhaseWindowMs", filters.excessPhaseWindowMs, 2);
    appendParameterLine(out, "filters.mixedPhaseStrength", filters.mixedPhaseStrength, 2);
    appendParameterLine(out, "filters.mixedPhaseMaxCorrectionDegrees", filters.mixedPhaseMaxCorrectionDegrees, 2);
}

std::string buildExportParametersText(const WorkspaceState& workspace,
                                      const std::vector<int>& sampleRates,
                                      std::wstring_view exportComment,
                                      std::string_view exportTimestamp) {
    std::ostringstream out;
    appendParameterLine(out, "export.timestamp", exportTimestamp);
    appendParameterLine(out,
                        "export.sampleRatesHz",
                        [&sampleRates]() {
                            std::ostringstream values;
                            for (std::size_t index = 0; index < sampleRates.size(); ++index) {
                                if (index > 0) {
                                    values << ',';
                                }
                                values << sampleRates[index];
                            }
                            return values.str();
                        }());
    appendParameterLine(out, "export.comment", toUtf8(exportComment));
    appendParameterLine(out, "target_curve.profile", workspace.activeTargetCurveProfileName);
    appendParameterLine(out, "target_curve.comment", workspace.activeTargetCurveComment);
    out << '\n';
    appendSmoothingParameters(out, workspace.smoothing);
    out << '\n';
    appendTargetCurveParameters(out, workspace.targetCurve);
    out << '\n';
    appendFilterParameters(out, workspace.filters);
    return out.str();
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
constexpr int kButtonExportRoon = 3701;
constexpr int kExportSampleRateCheckboxBase = 3710;
constexpr int kEditExportComment = 3730;
constexpr int kTabIndexAlignment = 0;
constexpr int kTabIndexMeasurement = 1;
constexpr int kTabIndexAnalysis = 2;
constexpr int kTabIndexTargetCurve = 3;
constexpr int kTabIndexFilters = 4;
constexpr int kTabIndexExport = 5;
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

MeasurementSettings buildAlignmentMeasurementSettings(const MeasurementSettings& baseSettings) {
    MeasurementSettings settings = baseSettings;
    settings.durationSeconds = 0.0018;
    settings.fadeInSeconds = 0.00022;
    settings.fadeOutSeconds = 0.00022;
    settings.startFrequencyHz = 4400.0;
    settings.endFrequencyHz = 6800.0;
    settings.targetLengthSamples = settings.sampleRate >= 96000 ? 1024 : 512;
    settings.leadInSamples = std::max(settings.sampleRate / 80, 768);
    return settings;
}

int clampWorkspaceTabIndex(HWND tabControl, int index) {
    if (tabControl == nullptr) {
        return 0;
    }

    const int itemCount = TabCtrl_GetItemCount(tabControl);
    if (itemCount <= 0) {
        return 0;
    }

    return std::clamp(index, 0, itemCount - 1);
}

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
        if (app->alignmentPage_.handleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        if (app->measurementPage_.handleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        if (app->targetCurvePage_.handleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        break;
    case WM_MEASUREITEM:
        if (app->handleMeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) {
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
    ui::AlignmentPage::registerPageWindowClass(instance_);
    ui::MeasurementPage::registerPageWindowClass(instance_);
    ui::AnalysisPage::registerPageWindowClass(instance_);
    ui::TargetCurvePage::registerPageWindowClass(instance_);
    ui::FiltersPage::registerPageWindowClass(instance_);

    mainWindow_ = CreateWindowExW(0, kMainClassName, L"Wolfie", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1400, 920, nullptr, nullptr, instance_, this);

    createMenus();
    createLayout();
    populateControlsFromState();
    targetCurvePage_.captureLoadingState(workspace_);
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
                                  0, 0, 0, 0, mainWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabMain)), instance_, nullptr);

    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<LPWSTR>(L"Alignment");
    TabCtrl_InsertItem(tabControl_, 0, &item);
    item.pszText = const_cast<LPWSTR>(L"Measurement");
    TabCtrl_InsertItem(tabControl_, 1, &item);
    item.pszText = const_cast<LPWSTR>(L"Analysis");
    TabCtrl_InsertItem(tabControl_, 2, &item);
    item.pszText = const_cast<LPWSTR>(L"Target Curve");
    TabCtrl_InsertItem(tabControl_, 3, &item);
    item.pszText = const_cast<LPWSTR>(L"Filters");
    TabCtrl_InsertItem(tabControl_, 4, &item);
    item.pszText = const_cast<LPWSTR>(L"Export");
    TabCtrl_InsertItem(tabControl_, 5, &item);

    alignmentPage_.create(tabControl_, instance_);
    measurementPage_.create(tabControl_, instance_);
    analysisPage_.create(tabControl_, instance_);
    targetCurvePage_.create(tabControl_, instance_);
    filtersPage_.create(tabControl_, instance_);
    pageAnalysis_ = analysisPage_.window();
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
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExportSampleRateCheckboxBase + static_cast<int>(index))),
                                      instance_,
                                      nullptr);
        SendMessageW(checkbox, BM_SETCHECK, BST_CHECKED, 0);
        exportSampleRateChecks_.push_back(checkbox);
    }
    exportCommentLabel_ = CreateWindowW(L"STATIC",
                                        L"Comment",
                                        WS_CHILD | WS_VISIBLE,
                                        0,
                                        0,
                                        0,
                                        0,
                                        pageExport_,
                                        nullptr,
                                        instance_,
                                        nullptr);
    exportCommentEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE,
                                         L"EDIT",
                                         L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                                             ES_AUTOVSCROLL | ES_WANTRETURN,
                                         0,
                                         0,
                                         0,
                                         0,
                                         pageExport_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditExportComment)),
                                         instance_,
                                         nullptr);
    exportButton_ = CreateWindowW(L"BUTTON",
                                  L"Generate Roon ZIP",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0,
                                  0,
                                  0,
                                  0,
                                  pageExport_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonExportRoon)),
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
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessLogSizeCompact)),
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
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessLogSizeMedium)),
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
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessLogSizeExpanded)),
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
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProcessLog)),
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
    MoveWindow(alignmentPage_.window(), tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(measurementPage_.window(), tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageAnalysis_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageTargetCurve_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(filtersPage_.window(), tabRect.left, tabRect.top + 10, pageWidth, std::max(240, pageHeight - 10), TRUE);
    MoveWindow(pageExport_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    layoutContent();
}

bool WolfieApp::handleDrawItem(const DRAWITEMSTRUCT* drawItem) const {
    if (drawItem == nullptr) {
        return false;
    }

    if (drawItem->CtlType == ODT_MENU &&
        drawItem->itemID >= kMenuFileRecentBase &&
        drawItem->itemID < kMenuFileRecentBase + 8) {
        const wchar_t* label = reinterpret_cast<const wchar_t*>(drawItem->itemData);
        if (label == nullptr) {
            return false;
        }

        const bool selected = (drawItem->itemState & ODS_SELECTED) != 0;
        const bool disabled = (drawItem->itemState & ODS_DISABLED) != 0;
        const COLORREF background = selected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_MENU);
        const COLORREF textColor = disabled ? GetSysColor(COLOR_GRAYTEXT)
                                            : (selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_MENUTEXT));

        HBRUSH backgroundBrush = CreateSolidBrush(background);
        FillRect(drawItem->hDC, &drawItem->rcItem, backgroundBrush);
        DeleteObject(backgroundBrush);

        HFONT font = createMenuFont();
        HGDIOBJ previousFont = nullptr;
        if (font != nullptr) {
            previousFont = SelectObject(drawItem->hDC, font);
        }

        RECT textRect = drawItem->rcItem;
        textRect.left += 12;
        textRect.right -= 12;

        SetBkMode(drawItem->hDC, TRANSPARENT);
        SetTextColor(drawItem->hDC, textColor);
        DrawTextW(drawItem->hDC, label, -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (previousFont != nullptr) {
            SelectObject(drawItem->hDC, previousFont);
        }
        if (font != nullptr && font != GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(font);
        }
        return true;
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
    const int commentLabelTop = checkboxTop +
                                (checkboxRowCount * checkboxHeight) +
                                (std::max(0, checkboxRowCount - 1) * checkboxGapY) +
                                18;
    const int commentEditTop = commentLabelTop + 22;
    const int commentHeight = 76;
    const int buttonTop = commentEditTop + commentHeight + 14;
    const int statusHeight = 56;
    const int statusTop = buttonTop + 44;
    MoveWindow(exportCommentLabel_, 24, commentLabelTop, exportWidth, 18, TRUE);
    MoveWindow(exportCommentEdit_, 24, commentEditTop, exportWidth, commentHeight, TRUE);
    MoveWindow(exportButton_, 24, buttonTop, 180, 28, TRUE);
    MoveWindow(exportProgress_, 24, buttonTop, exportWidth, 28, TRUE);
    MoveWindow(exportStatus_, 24, statusTop, exportWidth, statusHeight, TRUE);
    alignmentPage_.layout();
    measurementPage_.layout();
    analysisPage_.layout();
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
    refreshAlignmentPageView();
    measurementPage_.populate(workspace_);
    if (!roomSimulationDialog_.isOpen()) {
        roomSimulationDialog_.populate(workspace_);
    }
    analysisPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    measurementPage_.setWorkspaceView(workspace_);
    populateExportSampleRateControls();
    updateExportControls();
    updateProcessLogSizeButtons();
    if (tabControl_ != nullptr) {
        const int selected = clampWorkspaceTabIndex(tabControl_, workspace_.ui.lastOpenTabIndex);
        TabCtrl_SetCurSel(tabControl_, selected);
        updateVisibleTab();
    }
    layoutMainWindow();
}

void WolfieApp::syncStateFromControls() {
    measurementPage_.syncToWorkspace(workspace_);
    analysisPage_.syncToWorkspace(workspace_);
    targetCurvePage_.syncToWorkspace(workspace_);
    filtersPage_.syncToWorkspace(workspace_);
    syncExportSampleRatesToWorkspace();
    workspace_.ui.lastOpenTabIndex = clampWorkspaceTabIndex(tabControl_, activeTabIndex_);
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
    std::vector<double> capturedSamples;
    std::wstring errorMessage;
    if (!persistence::loadMonoWaveFileNormalized(rawCaptureArtifact->path, captureSampleRate, capturedSamples, errorMessage)) {
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
                                                                                  workspace.audio.loopbackEnabled &&
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

WolfieApp::FilterAnalysisTaskResult
WolfieApp::buildFilterAnalysisWithCurrentSettings(const WorkspaceState& workspace,
                                                  const std::shared_ptr<FilterAnalysisProgress>& progress) {
    FilterAnalysisTaskResult taskResult;
    taskResult.success = true;

    auto setStatus = [&](const std::wstring& statusText) {
        if (progress == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(progress->mutex);
        progress->statusText = statusText;
    };

    try {
        WorkspaceState working = workspace;

        if (!working.filterResult.valid) {
            if (working.smoothedResponse.frequencyAxisHz.empty() && working.result.hasAnyValues()) {
                setStatus(L"Calculating filter analysis: Preparing smoothed response");
                measurement::normalizeResponseSmoothingSettings(working.smoothing);
                working.smoothedResponse = measurement::buildSmoothedResponse(working.result, working.smoothing);
            }

            if (!working.smoothedResponse.frequencyAxisHz.empty()) {
                setStatus(L"Calculating filter analysis: Designing filters");
                measurement::normalizeFilterDesignSettings(working.filters, working.measurement.sampleRate);
                working.filterResult = measurement::designFilters(working.smoothedResponse,
                                                                 working.measurement,
                                                                 working.targetCurve,
                                                                 working.filters,
                                                                 &working.result);
            }
        }

        if (working.filterResult.valid) {
            working.filterAnalysis = measurement::buildFilterAnalysis(
                working.result,
                working.filterResult,
                [&](std::string_view window, std::string_view statusLabel) {
                    std::wstring status = L"Calculating filter analysis: ";
                    status += (window == "room") ? L"Room / " : L"Direct / ";
                    status += toWide(std::string(statusLabel));
                    setStatus(status);
                });
        }

        taskResult.smoothedResponse = std::move(working.smoothedResponse);
        taskResult.filterResult = std::move(working.filterResult);
        taskResult.filterAnalysis = std::move(working.filterAnalysis);
    } catch (const std::exception& exception) {
        taskResult.success = false;
        taskResult.errorMessage = L"Filter analysis refresh failed: " + toWide(exception.what());
    } catch (...) {
        taskResult.success = false;
        taskResult.errorMessage = L"Filter analysis refresh failed.";
    }

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
        refreshMeasurementPageView();
        analysisPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
        filtersPage_.populate(workspace_);
        refreshMeasurementStatus();
        return;
    }

    alignmentResult_ = {};
    workspace_.result = std::move(taskResult.result);
    workspace_.smoothedResponse = {};
    invalidateFilterDesign();
    invalidateStoredFilters();
    invalidateFilterAnalysis();
    ensureSmoothedResponseReady();
    refreshAlignmentPageView();
    refreshMeasurementPageView();
    analysisPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    workspaceRepository_.save(workspace_);
    measurementPage_.setCalibrationRefreshInProgress(false);
    appendMeasurementLog(L"Reanalyzed the saved measurement with the current microphone calibration.");
    refreshMeasurementStatus();
}

void WolfieApp::beginFilterAnalysisRefresh() {
    if (filterAnalysisRefreshInProgress_) {
        return;
    }

    syncStateFromControls();
    invalidateFilterDesign();
    invalidateFilterAnalysis();

    filterAnalysisRefreshInProgress_ = true;
    if (tabControl_ != nullptr) {
        EnableWindow(tabControl_, FALSE);
    }
    if (HMENU menuBar = GetMenu(mainWindow_); menuBar != nullptr) {
        EnableMenuItem(menuBar, 0, MF_BYPOSITION | MF_DISABLED | MF_GRAYED);
        DrawMenuBar(mainWindow_);
    }

    const std::wstring initialStatus = L"Calculating filter analysis: Preparing inputs";
    filterAnalysisProgress_ = std::make_shared<FilterAnalysisProgress>();
    {
        std::lock_guard<std::mutex> lock(filterAnalysisProgress_->mutex);
        filterAnalysisProgress_->statusText = initialStatus;
    }

    analysisPage_.setCalculationInProgress(true, initialStatus);
    appendLog(L"Refreshing filter analysis from the current filter settings...");

    const WorkspaceState workspaceSnapshot = workspace_;
    const auto progress = filterAnalysisProgress_;
    filterAnalysisFuture_ = std::async(std::launch::async, [workspaceSnapshot, progress]() {
        return buildFilterAnalysisWithCurrentSettings(workspaceSnapshot, progress);
    });
    SetTimer(mainWindow_, kFilterAnalysisTimerId, 50, nullptr);
}

void WolfieApp::finishFilterAnalysisRefresh() {
    if (!filterAnalysisFuture_.valid()) {
        filterAnalysisRefreshInProgress_ = false;
        filterAnalysisProgress_.reset();
        analysisPage_.setCalculationInProgress(false);
        if (tabControl_ != nullptr) {
            EnableWindow(tabControl_, TRUE);
        }
        if (HMENU menuBar = GetMenu(mainWindow_); menuBar != nullptr) {
            EnableMenuItem(menuBar, 0, MF_BYPOSITION | MF_ENABLED);
            DrawMenuBar(mainWindow_);
        }
        return;
    }

    analysisPage_.setCalculationInProgress(true, L"Calculating filter analysis: Refreshing views");
    FilterAnalysisTaskResult taskResult = filterAnalysisFuture_.get();
    filterAnalysisRefreshInProgress_ = false;
    filterAnalysisProgress_.reset();
    if (tabControl_ != nullptr) {
        EnableWindow(tabControl_, TRUE);
    }
    if (HMENU menuBar = GetMenu(mainWindow_); menuBar != nullptr) {
        EnableMenuItem(menuBar, 0, MF_BYPOSITION | MF_ENABLED);
        DrawMenuBar(mainWindow_);
    }

    analysisPage_.setCalculationInProgress(false);

    if (!taskResult.success) {
        analysisPage_.populate(workspace_);
        appendLog(taskResult.errorMessage, LogSeverity::Error);
        return;
    }

    workspace_.smoothedResponse = std::move(taskResult.smoothedResponse);
    workspace_.filterResult = std::move(taskResult.filterResult);
    workspace_.filterAnalysis = std::move(taskResult.filterAnalysis);
    storeCurrentFilterVariant();

    refreshMeasurementPageView();
    analysisPage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    syncStateFromControls();
    workspaceRepository_.save(workspace_);

    if (!workspace_.filterResult.valid) {
        appendLog(L"Filter design failed.", LogSeverity::Error);
        for (const std::string& entry : workspace_.filterResult.processLog) {
            appendLog(L"Filter design: " + toWide(entry), LogSeverity::Error);
        }
        return;
    }

    for (const std::string& entry : workspace_.filterResult.processLog) {
        appendLog(L"Filter design: " + toWide(entry));
    }

    if (workspace_.filterAnalysis.available) {
        appendLog(L"Refreshed filter analysis from the current filter settings.");
    } else {
        appendLog(L"Filter analysis refresh completed, but this workspace still lacks the phase or impulse data needed for post-filter diagnostics.",
                  LogSeverity::Error);
    }
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
    alignmentPage_.refreshStatus(measurementController_.status(),
                                 activeMeasurementTarget_ == MeasurementTarget::Alignment,
                                 !workspace_.rootPath.empty());
}

void WolfieApp::refreshAlignmentPageView() {
    const MeasurementResult& sourceResult = alignmentResult_;
    alignmentPage_.populate(measurement::buildSweetSpotAlignmentView(sourceResult));
}

bool WolfieApp::handleMeasureItem(MEASUREITEMSTRUCT* measureItem) const {
    if (measureItem == nullptr || measureItem->CtlType != ODT_MENU) {
        return false;
    }

    if (measureItem->itemID < kMenuFileRecentBase || measureItem->itemID >= kMenuFileRecentBase + 8) {
        return false;
    }

    const wchar_t* label = reinterpret_cast<const wchar_t*>(measureItem->itemData);
    if (label == nullptr) {
        return false;
    }

    HDC screen = GetDC(mainWindow_);
    if (screen == nullptr) {
        return false;
    }

    HFONT font = createMenuFont();
    HGDIOBJ previousFont = nullptr;
    if (font != nullptr) {
        previousFont = SelectObject(screen, font);
    }

    SIZE textSize{};
    GetTextExtentPoint32W(screen, label, static_cast<int>(wcslen(label)), &textSize);
    measureItem->itemWidth = static_cast<UINT>(textSize.cx + 24);
    measureItem->itemHeight = static_cast<UINT>(std::max<LONG>(GetSystemMetrics(SM_CYMENU), textSize.cy + 8));

    if (previousFont != nullptr) {
        SelectObject(screen, previousFont);
    }
    if (font != nullptr && font != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(font);
    }
    ReleaseDC(mainWindow_, screen);
    return true;
}

void WolfieApp::refreshRecentMenu() {
    HMENU menuBar = GetMenu(mainWindow_);
    HMENU fileMenu = GetSubMenu(menuBar, 0);
    HMENU recentMenu = GetSubMenu(fileMenu, 2);
    while (GetMenuItemCount(recentMenu) > 0) {
        DeleteMenu(recentMenu, 0, MF_BYPOSITION);
    }

    recentMenuLabels_.clear();

    if (appState_.recentWorkspaces.empty()) {
        recentMenuLabels_.push_back(L"(none)");
        AppendMenuW(recentMenu,
                    MF_GRAYED | MF_OWNERDRAW,
                    kMenuFileRecentBase,
                    reinterpret_cast<LPCWSTR>(recentMenuLabels_.back().c_str()));
        return;
    }

    for (size_t i = 0; i < appState_.recentWorkspaces.size() && i < 8; ++i) {
        recentMenuLabels_.push_back(appState_.recentWorkspaces[i].wstring());
    }

    for (size_t i = 0; i < recentMenuLabels_.size(); ++i) {
        AppendMenuW(recentMenu,
                    MF_OWNERDRAW,
                    kMenuFileRecentBase + static_cast<UINT>(i),
                    reinterpret_cast<LPCWSTR>(recentMenuLabels_[i].c_str()));
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

void WolfieApp::invalidateFilterAnalysis() {
    workspace_.filterAnalysis = {};
}

void WolfieApp::invalidateFilterDesign() {
    workspace_.filterResult = {};
}

void WolfieApp::invalidateStoredFilters() {
    workspace_.minimumFilter = {};
    workspace_.mixedFilter = {};
}

void WolfieApp::storeCurrentFilterVariant() {
    if (!workspace_.filterResult.valid) {
        return;
    }

    measurement::normalizeFilterDesignSettings(workspace_.filters, workspace_.measurement.sampleRate);
    if (workspace_.filters.phaseMode == "mixed") {
        workspace_.mixedFilter.settings = workspace_.filters;
        workspace_.mixedFilter.result = workspace_.filterResult;
    } else if (workspace_.filters.phaseMode == "minimum") {
        workspace_.minimumFilter.settings = workspace_.filters;
        workspace_.minimumFilter.result = workspace_.filterResult;
    }
}

void WolfieApp::applySelectedFilterView() {
    workspace_.ui.filterViewMode = normalizeFilterViewMode(workspace_.ui.filterViewMode);
    workspace_.filterResult = {};

    const StoredFilterDesign* selected = nullptr;
    if (workspace_.ui.filterViewMode == "mixed") {
        selected = workspace_.mixedFilter.available() ? &workspace_.mixedFilter : nullptr;
    } else if (workspace_.ui.filterViewMode == "difference") {
        if (workspace_.mixedFilter.available()) {
            selected = &workspace_.mixedFilter;
        } else if (workspace_.minimumFilter.available()) {
            selected = &workspace_.minimumFilter;
        }
    } else {
        selected = workspace_.minimumFilter.available() ? &workspace_.minimumFilter : nullptr;
    }

    if (selected != nullptr) {
        if (workspace_.ui.filterViewMode == "mixed") {
            copyMixedPhaseSettings(workspace_.filters, selected->settings);
        }
        workspace_.filterResult = selected->result;
    } else {
        workspace_.filters.phaseMode = workspace_.ui.filterViewMode == "mixed" ? "mixed" : "minimum";
    }

    if (workspace_.ui.filterViewMode != "difference") {
        workspace_.filters.phaseMode = workspace_.ui.filterViewMode == "mixed" ? "mixed" : "minimum";
    }
    measurement::normalizeFilterDesignSettings(workspace_.filters, workspace_.measurement.sampleRate);
}

void WolfieApp::ensureFilterResultReady() {
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
    if (!workspace_.filterResult.valid) {
        appendLog(L"Filter design failed.", LogSeverity::Error);
        for (const std::string& entry : workspace_.filterResult.processLog) {
            appendLog(L"Filter design: " + toWide(entry), LogSeverity::Error);
        }
        return;
    }

    for (const std::string& entry : workspace_.filterResult.processLog) {
        appendLog(L"Filter design: " + toWide(entry));
    }
    storeCurrentFilterVariant();
}

void WolfieApp::ensureFilterDesignReady() {
    if (workspace_.filterResult.valid && workspace_.filterAnalysis.available) {
        return;
    }

    ensureFilterResultReady();
    if (!workspace_.filterResult.valid) {
        return;
    }

    workspace_.filterAnalysis = measurement::buildFilterAnalysis(workspace_.result, workspace_.filterResult);
}

bool WolfieApp::shouldBuildExpectedMeasurementWaterfall() const {
    return workspace_.ui.measurementPlotMode == "waterfall" &&
           workspace_.ui.measurementWaterfallSource == "expected";
}

void WolfieApp::refreshMeasurementPageView() {
    const bool hadSmoothedResponse = !workspace_.smoothedResponse.frequencyAxisHz.empty();
    const bool hadFilterResult = workspace_.filterResult.valid;
    if (shouldBuildExpectedMeasurementWaterfall()) {
        ensureFilterResultReady();
    }

    measurementPage_.setWorkspaceView(workspace_);

    const bool smoothedResponseBecameAvailable =
        !hadSmoothedResponse && !workspace_.smoothedResponse.frequencyAxisHz.empty();
    const bool filterResultBecameAvailable = !hadFilterResult && workspace_.filterResult.valid;
    if (smoothedResponseBecameAvailable) {
        targetCurvePage_.populate(workspace_);
    }
    if (smoothedResponseBecameAvailable || filterResultBecameAvailable) {
        filtersPage_.populate(workspace_);
    }
}

void WolfieApp::setExportInProgress(bool running) {
    exportRunning_ = running;
    if (exportButton_ == nullptr || exportProgress_ == nullptr) {
        return;
    }

    for (HWND checkbox : exportSampleRateChecks_) {
        EnableWindow(checkbox, running ? FALSE : TRUE);
    }
    if (exportCommentEdit_ != nullptr) {
        EnableWindow(exportCommentEdit_, running ? FALSE : TRUE);
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
    if (exportCommentEdit_ != nullptr) {
        EnableWindow(exportCommentEdit_, exportRunning_ ? FALSE : TRUE);
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

    WorkspaceState exportWorkspace = workspace_;
    measurement::syncDerivedMeasurementSettings(exportWorkspace.measurement);
    measurement::normalizeResponseSmoothingSettings(exportWorkspace.smoothing);
    measurement::normalizeFilterDesignSettings(exportWorkspace.filters, exportWorkspace.measurement.sampleRate);
    const auto targetPlot = measurement::buildTargetCurvePlotData(exportWorkspace.smoothedResponse,
                                                                  exportWorkspace.measurement,
                                                                  exportWorkspace.targetCurve,
                                                                  std::nullopt);
    measurement::normalizeTargetCurveSettings(exportWorkspace.targetCurve, targetPlot.minFrequencyHz, targetPlot.maxFrequencyHz);

    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    const std::string exportTimestamp = buildExportTimestampText(localTime);
    const std::string archiveBaseName = "_roon_" + buildExportTimestampFileToken(localTime);
    const std::wstring exportComment = getWindowTextValue(exportCommentEdit_);
    if (exportCommentEdit_ != nullptr) {
        SetWindowTextW(exportCommentEdit_, L"");
    }
    const std::string parametersText =
        buildExportParametersText(exportWorkspace, selectedSampleRates, exportComment, exportTimestamp);

    const std::string activeProfileName = workspace_.activeTargetCurveProfileName.empty()
                                              ? std::string("Default")
                                              : workspace_.activeTargetCurveProfileName;
    const std::filesystem::path exportDirectory = workspace_.rootPath / "export" / sanitizeFileComponent(activeProfileName);
    const std::filesystem::path zipPath = measurement::roonFilterArchivePath(exportDirectory, archiveBaseName);
    appendLog(L"Roon export started: writing filter set to " + zipPath.wstring());
    setExportInProgress(true);
    SetWindowTextW(exportStatus_,
                   (L"Writing the Roon filter set to:\r\n" + zipPath.wstring()).c_str());
    showExportProgress(L"Preparing sample-rate export...");

    std::vector<std::filesystem::path> generatedFiles;
    std::wstring errorMessage;
    if (!measurement::exportRoonFilterWavSet(exportDirectory,
                                             exportWorkspace.smoothedResponse,
                                             exportWorkspace.measurement,
                                             exportWorkspace.targetCurve,
                                             exportWorkspace.filters,
                                             &exportWorkspace.result,
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
                                             },
                                             archiveBaseName,
                                             parametersText)) {
        setExportInProgress(false);
        SetWindowTextW(exportStatus_, errorMessage.c_str());
        appendLog(L"Roon export failed: " + errorMessage, LogSeverity::Error);
        return;
    }

    setExportInProgress(false);
    const std::size_t exportedSampleRateCount = selectedSampleRates.size();
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
    alignmentResult_ = {};
    workspace_.result = measurement::buildSimulatedRoomMeasurement(workspace_.measurement, settings, name);
    workspace_.smoothedResponse = {};
    invalidateFilterDesign();
    invalidateStoredFilters();
    invalidateFilterAnalysis();

    const int selected = tabControl_ == nullptr ? 0 : TabCtrl_GetCurSel(tabControl_);
    if (selected == kTabIndexTargetCurve || selected == kTabIndexFilters || selected == kTabIndexExport) {
        ensureSmoothedResponseReady();
    }

    refreshAlignmentPageView();
    refreshMeasurementPageView();
    analysisPage_.populate(workspace_);
    targetCurvePage_.populate(workspace_);
    filtersPage_.populate(workspace_);
    workspaceRepository_.save(workspace_);
    refreshMeasurementStatus();
    appendMeasurementLog(L"Generated room simulation: " + toWide(name) + L".");
}

void WolfieApp::onCommand(WORD commandId, WORD notificationCode) {
    if (calibrationReanalysisInProgress_ || filterAnalysisRefreshInProgress_) {
        return;
    }

    bool alignmentStartStopPressed = false;
    if (alignmentPage_.handleCommand(commandId, notificationCode, alignmentStartStopPressed)) {
        if (alignmentStartStopPressed) {
            if (measurementController_.status().running && activeMeasurementTarget_ == MeasurementTarget::Alignment) {
                stopMeasurement();
            } else if (!measurementController_.status().running) {
                startAlignmentMeasurement();
            }
        }
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
            refreshMeasurementPageView();
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

            refreshMeasurementPageView();
            analysisPage_.populate(workspace_);
            targetCurvePage_.populate(workspace_);
            filtersPage_.populate(workspace_);
            workspaceRepository_.save(workspace_);
            refreshMeasurementStatus();
            return;
        }
        if (sampleRateChanged) {
            invalidateFilterDesign();
            invalidateStoredFilters();
            invalidateFilterAnalysis();
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

    bool analysisViewSettingsChanged = false;
    bool analysisRefreshRequested = false;
    if (analysisPage_.handleCommand(commandId,
                                    notificationCode,
                                    workspace_,
                                    analysisViewSettingsChanged,
                                    analysisRefreshRequested)) {
        if (analysisRefreshRequested) {
            beginFilterAnalysisRefresh();
        }
        if (analysisViewSettingsChanged) {
            workspaceRepository_.saveUiSettings(workspace_);
        }
        return;
    }

    bool smoothingChanged = false;
    bool targetCurveChanged = false;
    bool targetCurvePersistencePending = false;
    bool targetCurvePersistNowRequested = false;
    if (targetCurvePage_.handleCommand(commandId,
                                       notificationCode,
                                       workspace_,
                                       smoothingChanged,
                                       targetCurveChanged,
                                       targetCurvePersistencePending,
                                       targetCurvePersistNowRequested)) {
        if (smoothingChanged) {
            workspace_.smoothedResponse = {};
            invalidateFilterDesign();
            invalidateStoredFilters();
            invalidateFilterAnalysis();
            ensureSmoothedResponseReady();
            targetCurvePage_.populate(workspace_);
            filtersPage_.populate(workspace_);
            if (targetCurvePersistencePending_) {
                workspaceRepository_.save(workspace_);
                targetCurvePersistencePending_ = false;
            } else {
                workspaceRepository_.saveSettings(workspace_);
            }
        }
        if (targetCurveChanged) {
            invalidateFilterDesign();
            invalidateStoredFilters();
            invalidateFilterAnalysis();
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
    bool filterSelectionChanged = false;
    std::vector<std::wstring> filterLogMessages;
    if (filtersPage_.handleCommand(commandId,
                                   notificationCode,
                                   workspace_,
                                   filterSettingsChanged,
                                   filtersRecalculateRequested,
                                   filterViewSettingsChanged,
                                   filterSelectionChanged,
                                   filterLogMessages)) {
        for (const std::wstring& message : filterLogMessages) {
            appendLog(message);
        }
        if (filterSelectionChanged) {
            applySelectedFilterView();
            invalidateFilterAnalysis();
            refreshMeasurementPageView();
            analysisPage_.populate(workspace_);
            filtersPage_.populate(workspace_);
            syncStateFromControls();
            workspaceRepository_.saveSettings(workspace_);
        }
        if (filterSettingsChanged) {
            invalidateFilterDesign();
            invalidateFilterAnalysis();
            filtersPage_.populate(workspace_);
            syncStateFromControls();
            workspaceRepository_.saveSettings(workspace_);
        }
        if (filtersRecalculateRequested) {
            invalidateFilterDesign();
            invalidateFilterAnalysis();
            filtersPage_.setRecalculateInProgress(true);
            ensureFilterDesignReady();
            analysisPage_.populate(workspace_);
            filtersPage_.populate(workspace_);
            filtersPage_.setRecalculateInProgress(false);
            syncStateFromControls();
            workspaceRepository_.save(workspace_);
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
            workspaceRepository_.saveUiSettings(workspace_);
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
    if (calibrationReanalysisInProgress_ || filterAnalysisRefreshInProgress_) {
        return;
    }

    if (measurementPage_.handleHScroll(source, workspace_)) {
        syncStateFromControls();
        workspaceRepository_.saveSettings(workspace_);
        return;
    }

    bool smoothingChanged = false;
    bool targetCurveChanged = false;
    bool targetCurvePersistencePending = false;
    if (targetCurvePage_.handleHScroll(source,
                                       workspace_,
                                       smoothingChanged,
                                       targetCurveChanged,
                                       targetCurvePersistencePending)) {
        if (smoothingChanged) {
            workspace_.smoothedResponse = {};
            invalidateFilterDesign();
            invalidateStoredFilters();
            invalidateFilterAnalysis();
            ensureSmoothedResponseReady();
            targetCurvePage_.populate(workspace_);
            filtersPage_.populate(workspace_);
            if (targetCurvePersistencePending_) {
                workspaceRepository_.save(workspace_);
                targetCurvePersistencePending_ = false;
            } else {
                workspaceRepository_.saveSettings(workspace_);
            }
        }
        if (targetCurveChanged) {
            invalidateFilterDesign();
            invalidateStoredFilters();
            invalidateFilterAnalysis();
        }
        if (targetCurvePersistencePending) {
            targetCurvePersistencePending_ = true;
        }
        return;
    }

    if (filtersPage_.handleHScroll(source, workspace_)) {
        syncStateFromControls();
        workspaceRepository_.saveSettings(workspace_);
        return;
    }
}

void WolfieApp::onNotify(LPARAM lParam) {
    if (calibrationReanalysisInProgress_ || filterAnalysisRefreshInProgress_) {
        return;
    }

    const auto* header = reinterpret_cast<const NMHDR*>(lParam);
    if (header != nullptr && header->hwndFrom == tabControl_ && header->code == TCN_SELCHANGE) {
        const int selected = TabCtrl_GetCurSel(tabControl_);
        if (activeTabIndex_ == kTabIndexTargetCurve && selected != kTabIndexTargetCurve) {
            persistTargetCurveStateIfPending();
        }
        updateVisibleTab();
        layoutContent();
        if (!workspace_.rootPath.empty()) {
            workspace_.ui.lastOpenTabIndex = clampWorkspaceTabIndex(tabControl_, selected);
            workspaceRepository_.saveUiSettings(workspace_);
        }
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

    if (timerId == kFilterAnalysisTimerId) {
        if (!filterAnalysisRefreshInProgress_ || !filterAnalysisFuture_.valid()) {
            KillTimer(mainWindow_, kFilterAnalysisTimerId);
            return;
        }

        std::wstring statusText = L"Calculating filter analysis: Preparing inputs";
        if (filterAnalysisProgress_ != nullptr) {
            std::lock_guard<std::mutex> lock(filterAnalysisProgress_->mutex);
            if (!filterAnalysisProgress_->statusText.empty()) {
                statusText = filterAnalysisProgress_->statusText;
            }
        }
        analysisPage_.setCalculationInProgress(true, statusText);
        if (filterAnalysisFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            KillTimer(mainWindow_, kFilterAnalysisTimerId);
            finishFilterAnalysisRefresh();
        }
        return;
    }

    if (timerId != kMeasurementTimerId) {
        return;
    }

    measurementController_.tick();
    if (measurementController_.consumeAlignmentUpdate()) {
        alignmentResult_ = measurementController_.result();
        refreshAlignmentPageView();
    }
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
    if (selected == kTabIndexTargetCurve || selected == kTabIndexFilters || selected == kTabIndexExport) {
        ensureSmoothedResponseReady();
        if (selected == kTabIndexTargetCurve) {
            targetCurvePage_.populate(workspace_);
        } else if (selected == kTabIndexFilters) {
            filtersPage_.populate(workspace_);
        } else {
            updateExportControls();
        }
    }
    if (selected == kTabIndexAnalysis) {
        analysisPage_.populate(workspace_);
    }
    if (selected == kTabIndexAlignment) {
        refreshAlignmentPageView();
    }
    if (selected == kTabIndexMeasurement) {
        refreshMeasurementPageView();
    }

    alignmentPage_.setVisible(selected == kTabIndexAlignment);
    measurementPage_.setVisible(selected == kTabIndexMeasurement);
    analysisPage_.setVisible(selected == kTabIndexAnalysis);
    targetCurvePage_.setVisible(selected == kTabIndexTargetCurve);
    filtersPage_.setVisible(selected == kTabIndexFilters);
    ShowWindow(pageExport_, selected == kTabIndexExport ? SW_SHOW : SW_HIDE);
    activeTabIndex_ = selected;
    workspace_.ui.lastOpenTabIndex = clampWorkspaceTabIndex(tabControl_, selected);
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
    alignmentResult_ = {};
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
    if (exportCommentEdit_ != nullptr) {
        SetWindowTextW(exportCommentEdit_, L"");
    }
    populateControlsFromState();
    targetCurvePage_.captureLoadingState(workspace_);
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
    alignmentResult_ = {};
    if (exportCommentEdit_ != nullptr) {
        SetWindowTextW(exportCommentEdit_, L"");
    }
    touchRecentWorkspace(path);
    populateControlsFromState();
    targetCurvePage_.captureLoadingState(workspace_);
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
    activeMeasurementTarget_ = runMode == MeasurementRunMode::Reference
                                   ? MeasurementTarget::WorkspaceReference
                                   : MeasurementTarget::WorkspaceRoom;
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
        alignmentResult_ = {};
        workspace_.result = {};
        workspace_.smoothedResponse = {};
        invalidateFilterDesign();
        invalidateStoredFilters();
        invalidateFilterAnalysis();
        analysisPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
        filtersPage_.populate(workspace_);
        refreshAlignmentPageView();
    }
    refreshMeasurementPageView();
    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::startAlignmentMeasurement() {
    if (workspace_.rootPath.empty()) {
        appendMeasurementLog(L"Cannot start alignment because no workspace is open.", LogSeverity::Error);
        refreshMeasurementStatus();
        return;
    }

    syncStateFromControls();
    WorkspaceState alignmentWorkspace = workspace_;
    alignmentWorkspace.measurement = buildAlignmentMeasurementSettings(workspace_.measurement);
    activeMeasurementTarget_ = MeasurementTarget::Alignment;
    appendMeasurementLog(L"Starting alignment loop at " +
                         std::to_wstring(alignmentWorkspace.measurement.sampleRate) +
                         L" Hz using a short burst centered in the " +
                         formatWideDouble(alignmentWorkspace.measurement.startFrequencyHz, 0) +
                         L" to " +
                         formatWideDouble(alignmentWorkspace.measurement.endFrequencyHz, 0) +
                         L" Hz.");
    if (!measurementController_.start(alignmentWorkspace, MeasurementRunMode::Alignment)) {
        refreshMeasurementStatus();
        if (!measurementController_.status().lastErrorMessage.empty()) {
            appendMeasurementLog(L"Alignment start failed: " + measurementController_.status().lastErrorMessage,
                                 LogSeverity::Error);
        }
        activeMeasurementTarget_ = MeasurementTarget::WorkspaceRoom;
        return;
    }

    measurementCompletionHandled_ = false;
    alignmentResult_ = {};
    refreshAlignmentPageView();
    appendMeasurementLog(L"Alignment playback file written to " +
                         measurementController_.status().generatedSweepPath.wstring());
    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::stopMeasurement() {
    if (!measurementController_.status().running) {
        return;
    }

    if (activeMeasurementTarget_ == MeasurementTarget::Alignment) {
        appendMeasurementLog(L"Alignment loop stopped by user.");
    } else {
        appendMeasurementLog(measurementController_.status().runMode == MeasurementRunMode::Reference
                                 ? L"Reference measurement stopped by user."
                                 : L"Measurement stopped by user.");
    }
    measurementCompletionHandled_ = true;
    measurementController_.cancel();
    KillTimer(mainWindow_, kMeasurementTimerId);
    if (activeMeasurementTarget_ != MeasurementTarget::Alignment && !workspace_.rootPath.empty()) {
        const WorkspaceState persistedWorkspace = workspaceRepository_.load(workspace_.rootPath);
        workspace_.result = persistedWorkspace.result;
        workspace_.referenceResult = persistedWorkspace.referenceResult;
        workspace_.filterAnalysis = persistedWorkspace.filterAnalysis;
        workspace_.minimumFilter = persistedWorkspace.minimumFilter;
        workspace_.mixedFilter = persistedWorkspace.mixedFilter;
        workspace_.ui.filterViewMode = persistedWorkspace.ui.filterViewMode;
        workspace_.smoothedResponse = {};
        applySelectedFilterView();
        refreshMeasurementPageView();
        analysisPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
        filtersPage_.populate(workspace_);
    }
    activeMeasurementTarget_ = MeasurementTarget::WorkspaceRoom;
    refreshAlignmentPageView();
    refreshMeasurementStatus();
}

void WolfieApp::finalizeMeasurement() {
    const MeasurementStatus completedStatus = measurementController_.status();
    const bool alignmentRun = activeMeasurementTarget_ == MeasurementTarget::Alignment;
    const bool referenceRun = completedStatus.runMode == MeasurementRunMode::Reference;
    if (alignmentRun) {
        alignmentResult_ = measurementController_.result();
    } else if (referenceRun) {
        workspace_.referenceResult = measurementController_.result();
    } else {
        alignmentResult_ = {};
        workspace_.result = measurementController_.result();
        workspace_.smoothedResponse = {};
        invalidateFilterDesign();
        invalidateStoredFilters();
        invalidateFilterAnalysis();
        const int selected = tabControl_ == nullptr ? 0 : TabCtrl_GetCurSel(tabControl_);
        if (selected == kTabIndexTargetCurve || selected == kTabIndexFilters || selected == kTabIndexExport) {
            ensureSmoothedResponseReady();
        }
    }
    refreshAlignmentPageView();
    if (!alignmentRun) {
        refreshMeasurementPageView();
        analysisPage_.populate(workspace_);
        targetCurvePage_.populate(workspace_);
        filtersPage_.populate(workspace_);
        syncStateFromControls();
        workspaceRepository_.save(workspace_);
    }
    const MeasurementResult& finishedResult = alignmentRun
                                                  ? alignmentResult_
                                                  : (referenceRun ? workspace_.referenceResult : workspace_.result);
    if (!alignmentRun) {
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
    }
    activeMeasurementTarget_ = MeasurementTarget::WorkspaceRoom;
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

