#include "wolfie_app.h"

#include <algorithm>

#include <commctrl.h>
#include <shobjidl.h>

#include "audio/winmm_audio_backend.h"
#include "ui/response_graph.h"
#include "ui/settings_dialog.h"
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
constexpr wchar_t kMainClassName[] = L"WolfieMainWindow";

}  // namespace

WolfieApp::WolfieApp(HINSTANCE instance)
    : instance_(instance),
      measurementController_(audio::createWinMmAudioBackend()),
      appStateRepository_(std::filesystem::current_path() / "wolfie-app-state.json") {}

int WolfieApp::run() {
    INITCOMMONCONTROLSEX initCommonControls{};
    initCommonControls.dwSize = sizeof(initCommonControls);
    initCommonControls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
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
    case WM_DRAWITEM:
        if (app->measurementPage_.handleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam),
                                                 app->measurementController_.status().running)) {
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
    ui::MeasurementPage::registerPageWindowClass(instance_);

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
    item.pszText = const_cast<LPWSTR>(L"Align Mic");
    TabCtrl_InsertItem(tabControl_, 1, &item);
    item.pszText = const_cast<LPWSTR>(L"Target Curve");
    TabCtrl_InsertItem(tabControl_, 2, &item);
    item.pszText = const_cast<LPWSTR>(L"Filters");
    TabCtrl_InsertItem(tabControl_, 3, &item);
    item.pszText = const_cast<LPWSTR>(L"Export");
    TabCtrl_InsertItem(tabControl_, 4, &item);

    measurementPage_.create(tabControl_, instance_);
    pageAlignment_ = CreateWindowExW(0, ui::MeasurementPage::pageWindowClassName(), nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                     0, 0, 0, 0, tabControl_, nullptr, instance_, nullptr);
    pageTargetCurve_ = CreateWindowExW(0, ui::MeasurementPage::pageWindowClassName(), nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                       0, 0, 0, 0, tabControl_, nullptr, instance_, nullptr);
    pageFilters_ = CreateWindowExW(0, ui::MeasurementPage::pageWindowClassName(), nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                   0, 0, 0, 0, tabControl_, nullptr, instance_, nullptr);
    pageExport_ = CreateWindowExW(0, ui::MeasurementPage::pageWindowClassName(), nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                  0, 0, 0, 0, tabControl_, nullptr, instance_, nullptr);

    placeholderAlignment_ = CreateWindowW(L"STATIC", L"Microphone alignment will live here.", WS_CHILD | SS_CENTER,
                                          0, 0, 0, 0, pageAlignment_, nullptr, instance_, nullptr);
    placeholderTargetCurve_ = CreateWindowW(L"STATIC", L"Target curve design is not implemented yet.", WS_CHILD | SS_CENTER,
                                            0, 0, 0, 0, pageTargetCurve_, nullptr, instance_, nullptr);
    placeholderFilters_ = CreateWindowW(L"STATIC", L"Filter design and simulation will live here.", WS_CHILD | SS_CENTER,
                                        0, 0, 0, 0, pageFilters_, nullptr, instance_, nullptr);
    placeholderExport_ = CreateWindowW(L"STATIC", L"ROON export will live here.", WS_CHILD | SS_CENTER,
                                       0, 0, 0, 0, pageExport_, nullptr, instance_, nullptr);

    updateVisibleTab();
    layoutMainWindow();
}

void WolfieApp::layoutMainWindow() {
    const RECT bounds = clientRect(mainWindow_);
    const int width = std::max(320L, bounds.right - (2 * kContentMargin));
    const int height = std::max(360L, bounds.bottom - (2 * kContentMargin));
    MoveWindow(tabControl_, kContentMargin, kContentMargin, width, height, TRUE);

    RECT tabRect{};
    GetClientRect(tabControl_, &tabRect);
    TabCtrl_AdjustRect(tabControl_, FALSE, &tabRect);
    const int pageWidth = std::max(320L, tabRect.right - tabRect.left);
    const int pageHeight = std::max(240L, tabRect.bottom - tabRect.top);
    MoveWindow(measurementPage_.window(), tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageAlignment_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageTargetCurve_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageFilters_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(pageExport_, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    layoutContent();
}

void WolfieApp::layoutContent() {
    RECT alignmentRect = clientRect(pageAlignment_);
    MoveWindow(placeholderAlignment_, 24, 32, std::max(200L, alignmentRect.right - 48), 24, TRUE);
    RECT targetRect = clientRect(pageTargetCurve_);
    MoveWindow(placeholderTargetCurve_, 24, 32, std::max(200L, targetRect.right - 48), 24, TRUE);
    RECT filtersRect = clientRect(pageFilters_);
    MoveWindow(placeholderFilters_, 24, 32, std::max(200L, filtersRect.right - 48), 24, TRUE);
    RECT exportRect = clientRect(pageExport_);
    MoveWindow(placeholderExport_, 24, 32, std::max(200L, exportRect.right - 48), 24, TRUE);
    measurementPage_.layout();
}

void WolfieApp::showSettingsWindow() {
    ui::SettingsDialog::show(instance_, mainWindow_, workspace_.audio, asioService_, [this](const AudioSettings& settings) {
        workspace_.audio = settings;
        populateControlsFromState();
        workspaceRepository_.save(workspace_);
        refreshMeasurementStatus();
    });
}

void WolfieApp::populateControlsFromState() {
    measurement::syncDerivedMeasurementSettings(workspace_.measurement);
    measurementPage_.populate(workspace_);
}

void WolfieApp::syncStateFromControls() {
    measurementPage_.syncToWorkspace(workspace_);
}

void WolfieApp::refreshWindowTitle() {
    std::wstring title = L"Wolfie";
    if (!workspace_.rootPath.empty()) {
        title += L" - " + workspace_.rootPath.filename().wstring();
    }
    SetWindowTextW(mainWindow_, title.c_str());
}

void WolfieApp::refreshMeasurementStatus() {
    measurementPage_.refreshStatus(measurementController_.status(), !workspace_.result.frequencyAxisHz.empty());
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

void WolfieApp::onCommand(WORD commandId, WORD notificationCode) {
    bool measurePressed = false;
    bool sampleRateChanged = false;
    if (measurementPage_.handleCommand(commandId, notificationCode, workspace_, measurePressed, sampleRateChanged)) {
        if (sampleRateChanged) {
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
        workspaceRepository_.save(workspace_);
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
    if (measurementController_.status().finished) {
        finalizeMeasurement();
        KillTimer(mainWindow_, kMeasurementTimerId);
    }
}

void WolfieApp::onResize() {
    layoutMainWindow();
    RedrawWindow(mainWindow_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void WolfieApp::updateVisibleTab() {
    const int selected = tabControl_ == nullptr ? 0 : TabCtrl_GetCurSel(tabControl_);
    measurementPage_.setVisible(selected == 0);
    ShowWindow(pageAlignment_, selected == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(pageTargetCurve_, selected == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(pageFilters_, selected == 3 ? SW_SHOW : SW_HIDE);
    ShowWindow(pageExport_, selected == 4 ? SW_SHOW : SW_HIDE);
}

void WolfieApp::newWorkspace() {
    auto path = pickFolder(true);
    if (!path) {
        return;
    }

    workspace_ = {};
    workspace_.rootPath = *path;
    measurement::syncDerivedMeasurementSettings(workspace_.measurement);
    populateControlsFromState();
    refreshWindowTitle();
    measurementPage_.invalidateGraph();
    workspaceRepository_.save(workspace_);
    touchRecentWorkspace(*path);
}

void WolfieApp::openWorkspace() {
    auto path = pickFolder(false);
    if (path) {
        openWorkspace(*path);
    }
}

void WolfieApp::openWorkspace(const std::filesystem::path& path) {
    workspace_ = workspaceRepository_.load(path);
    touchRecentWorkspace(path);
    populateControlsFromState();
    refreshWindowTitle();
    refreshMeasurementStatus();
    measurementPage_.invalidateGraph();
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
        MessageBoxW(mainWindow_, L"Create or open a workspace first.", L"Wolfie", MB_OK | MB_ICONWARNING);
        return;
    }

    syncStateFromControls();
    if (!measurementController_.start(workspace_)) {
        refreshMeasurementStatus();
        if (!measurementController_.status().lastErrorMessage.empty()) {
            MessageBoxW(mainWindow_, measurementController_.status().lastErrorMessage.c_str(), L"Wolfie", MB_OK | MB_ICONERROR);
        }
        return;
    }

    workspace_.result = {};
    measurementPage_.setMeasurementResult(workspace_.result);
    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::stopMeasurement() {
    if (!measurementController_.status().running) {
        return;
    }

    measurementController_.cancel();
    KillTimer(mainWindow_, kMeasurementTimerId);
    if (!workspace_.rootPath.empty()) {
        workspace_.result = workspaceRepository_.load(workspace_.rootPath).result;
        measurementPage_.setMeasurementResult(workspace_.result);
    }
    refreshMeasurementStatus();
}

void WolfieApp::finalizeMeasurement() {
    workspace_.result = measurementController_.result();
    measurementPage_.setMeasurementResult(workspace_.result);
    workspaceRepository_.save(workspace_);
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
