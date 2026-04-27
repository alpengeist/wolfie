#include "ui/target_curve_page.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <commctrl.h>
#include <windowsx.h>

#include "core/text_utils.h"
#include "measurement/target_curve_designer.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr double kFrequencyWheelScalePerStep = 1.0166666667;
constexpr double kGainWheelStepDb = 0.1;
constexpr double kQWheelStep = 0.1;
constexpr wchar_t kProfilePromptClassName[] = L"WolfieTargetCurveProfilePrompt";

struct ProfilePromptDialogState {
    std::wstring value;
    bool accepted = false;
    bool finished = false;
    HWND edit = nullptr;
};

LRESULT CALLBACK ProfilePromptWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ProfilePromptDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_COMMAND:
        if (state == nullptr) {
            break;
        }
        switch (LOWORD(wParam)) {
        case IDOK: {
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring value(static_cast<size_t>(length), L'\0');
            if (length > 0) {
                GetWindowTextW(state->edit, value.data(), length + 1);
            }
            state->value = value;
            state->accepted = true;
            state->finished = true;
            DestroyWindow(window);
            return 0;
        }
        case IDCANCEL:
            state->finished = true;
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        if (state != nullptr) {
            state->finished = true;
        }
        DestroyWindow(window);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ensureProfilePromptWindowClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ProfilePromptWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kProfilePromptClassName;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassW(&windowClass);
    registered = true;
}

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

COLORREF blendColor(COLORREF first, COLORREF second, double t) {
    const double blend = clampValue(t, 0.0, 1.0);
    const auto channel = [blend](BYTE a, BYTE b) -> BYTE {
        return static_cast<BYTE>(std::lround((static_cast<double>(a) * (1.0 - blend)) +
                                             (static_cast<double>(b) * blend)));
    };
    return RGB(channel(GetRValue(first), GetRValue(second)),
               channel(GetGValue(first), GetGValue(second)),
               channel(GetBValue(first), GetBValue(second)));
}

}  // namespace

void TargetCurvePage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = CreateSolidBrush(ui_theme::kBackground);
    RegisterClassW(&pageClass);
}

const wchar_t* TargetCurvePage::pageWindowClassName() {
    return kPageClassName;
}

void TargetCurvePage::create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    window_ = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr, instance, this);
    createControls();
}

void TargetCurvePage::createControls() {
    controls_.profileLabel = CreateWindowW(L"STATIC", L"Profile", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboProfiles = CreateWindowW(L"COMBOBOX",
                                            nullptr,
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            0,
                                            0,
                                            0,
                                            0,
                                            window_,
                                            reinterpret_cast<HMENU>(kComboProfiles),
                                            instance_,
                                            nullptr);
    controls_.buttonNewProfile = CreateWindowW(L"BUTTON", L"New", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonNewProfile), instance_, nullptr);
    controls_.graphLabel = CreateWindowW(L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.graphHint = CreateWindowW(L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.bandsLabel = CreateWindowW(L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonNew = CreateWindowW(L"BUTTON", L"New", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonNew), instance_, nullptr);
    controls_.buttonDelete = CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonDelete), instance_, nullptr);
    controls_.buttonReset = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonReset), instance_, nullptr);
    controls_.checkboxBypassAll = CreateWindowW(L"BUTTON", L"Bypass", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kCheckboxBypassAll), instance_, nullptr);
    controls_.listBands = CreateWindowExW(WS_EX_CLIENTEDGE,
                                          L"LISTBOX",
                                          nullptr,
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                          0,
                                          0,
                                          0,
                                          0,
                                          window_,
                                          reinterpret_cast<HMENU>(kListBands),
                                          instance_,
                                          nullptr);
    controls_.detailLabel = CreateWindowW(L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxEnabled = CreateWindowW(L"BUTTON", L"Enabled", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                              0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kCheckboxBandEnabled), instance_, nullptr);
    controls_.typeValue = CreateWindowW(L"STATIC", L"Bell", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.frequencyLabel = CreateWindowW(L"STATIC", L"Frequency", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.frequencySlider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kFrequencySlider), instance_, nullptr);
    controls_.frequencyValue = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kFrequencyEdit), instance_, nullptr);
    controls_.frequencyUnit = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.gainLabel = CreateWindowW(L"STATIC", L"Gain", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.gainSlider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                           0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kGainSlider), instance_, nullptr);
    controls_.gainValue = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                          0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kGainEdit), instance_, nullptr);
    controls_.gainUnit = CreateWindowW(L"STATIC", L"dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.qLabel = CreateWindowW(L"STATIC", L"Q", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.qSlider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kQSlider), instance_, nullptr);
    controls_.qValue = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kQEdit), instance_, nullptr);
    controls_.commentLabel = CreateWindowW(L"STATIC", L"Comment", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.commentValue = CreateWindowExW(WS_EX_CLIENTEDGE,
                                             L"EDIT",
                                             L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOVSCROLL | ES_MULTILINE | WS_VSCROLL,
                                             0,
                                             0,
                                             0,
                                             0,
                                             window_,
                                             reinterpret_cast<HMENU>(kCommentEdit),
                                             instance_,
                                             nullptr);

    SendMessageW(controls_.frequencySlider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.frequencySlider, TBM_SETRANGEMAX, FALSE, kFrequencySliderMax);
    SendMessageW(controls_.gainSlider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.gainSlider, TBM_SETRANGEMAX, FALSE, kGainSliderMax);
    SendMessageW(controls_.qSlider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.qSlider, TBM_SETRANGEMAX, FALSE, kQSliderMax);
    SendMessageW(controls_.listBands, LB_SETITEMHEIGHT, 0, 26);
    bandListProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(controls_.listBands,
                                                                GWLP_WNDPROC,
                                                                reinterpret_cast<LONG_PTR>(BandListProc)));
    SetWindowLongPtrW(controls_.listBands, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowSubclass(controls_.frequencyValue, ValueEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(controls_.gainValue, ValueEditProc, 2, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(controls_.qValue, ValueEditProc, 3, reinterpret_cast<DWORD_PTR>(this));

    graph_.create(window_, instance_, kGraph);
}

void TargetCurvePage::layout() {
    RECT pageRect{};
    GetClientRect(window_, &pageRect);
    const int contentLeft = 20;
    const int contentTop = 20;
    const int innerWidth = std::max(480L, pageRect.right - (contentLeft * 2));
    const int innerHeight = std::max(360L, pageRect.bottom - (contentTop * 2));
    const int baseSidebarWidth = clampValue(innerWidth / 3, 300, 360);
    const int sidebarWidth = std::max(200, (baseSidebarWidth * 2) / 3);
    const int gap = 24;
    const int graphWidth = std::max(320, innerWidth - sidebarWidth - gap);
    const int sidebarLeft = contentLeft + graphWidth + gap;

    const RECT graphBounds{contentLeft, contentTop, contentLeft + graphWidth, contentTop + innerHeight};
    graph_.layout(graphBounds);

    MoveWindow(controls_.profileLabel, sidebarLeft, contentTop, 56, 20, TRUE);
    MoveWindow(controls_.comboProfiles, sidebarLeft, contentTop + 22, sidebarWidth - 80, 240, TRUE);
    MoveWindow(controls_.buttonNewProfile, sidebarLeft + sidebarWidth - 72, contentTop + 21, 72, 26, TRUE);
    MoveWindow(controls_.buttonNew, sidebarLeft, contentTop + 58, 72, 28, TRUE);
    MoveWindow(controls_.buttonDelete, sidebarLeft + 80, contentTop + 58, 72, 28, TRUE);
    MoveWindow(controls_.buttonReset, sidebarLeft + 160, contentTop + 58, 72, 28, TRUE);
    MoveWindow(controls_.checkboxBypassAll, sidebarLeft, contentTop + 92, sidebarWidth, 20, TRUE);
    const int listTop = contentTop + 120;
    const int listHeight = 180;
    MoveWindow(controls_.listBands, sidebarLeft, listTop, sidebarWidth, listHeight, TRUE);

    const int detailTop = listTop + listHeight + 16;
    MoveWindow(controls_.checkboxEnabled, sidebarLeft, detailTop, 90, 20, TRUE);
    MoveWindow(controls_.typeValue, sidebarLeft + 116, detailTop, 80, 20, TRUE);

    const int sliderWidth = sidebarWidth - 98;
    const int editLeft = sidebarLeft + sidebarWidth - 70;
    MoveWindow(controls_.frequencyLabel, sidebarLeft, detailTop + 24, 80, 18, TRUE);
    MoveWindow(controls_.frequencySlider, sidebarLeft, detailTop + 42, sliderWidth, 24, TRUE);
    MoveWindow(controls_.frequencyValue, editLeft, detailTop + 38, 54, 24, TRUE);
    MoveWindow(controls_.frequencyUnit, editLeft + 56, detailTop + 42, 20, 18, TRUE);

    MoveWindow(controls_.gainLabel, sidebarLeft, detailTop + 70, 80, 18, TRUE);
    MoveWindow(controls_.gainSlider, sidebarLeft, detailTop + 88, sliderWidth, 24, TRUE);
    MoveWindow(controls_.gainValue, editLeft, detailTop + 84, 54, 24, TRUE);
    MoveWindow(controls_.gainUnit, editLeft + 56, detailTop + 88, 24, 18, TRUE);

    MoveWindow(controls_.qLabel, sidebarLeft, detailTop + 116, 80, 18, TRUE);
    MoveWindow(controls_.qSlider, sidebarLeft, detailTop + 134, sliderWidth, 24, TRUE);
    MoveWindow(controls_.qValue, editLeft, detailTop + 130, 54, 24, TRUE);

    const int commentTop = detailTop + 168;
    MoveWindow(controls_.commentLabel, sidebarLeft, commentTop, 80, 18, TRUE);
    const int availableCommentHeight = static_cast<int>(pageRect.bottom) - (commentTop + 44);
    const int commentHeight = std::max(80, availableCommentHeight);
    MoveWindow(controls_.commentValue, sidebarLeft, commentTop + 22, sidebarWidth, commentHeight, TRUE);
}

void TargetCurvePage::setVisible(bool visible) const {
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
}

void TargetCurvePage::populate(const WorkspaceState& workspace) {
    updatingControls_ = true;
    refreshProfileControls(workspace);
    SendMessageW(controls_.checkboxBypassAll,
                 BM_SETCHECK,
                 workspace.targetCurve.bypassEqBands ? BST_CHECKED : BST_UNCHECKED,
                 0);
    if (selectedBandIndex_ >= static_cast<int>(workspace.targetCurve.eqBands.size())) {
        selectedBandIndex_ = workspace.targetCurve.eqBands.empty() ? -1 : static_cast<int>(workspace.targetCurve.eqBands.size() - 1);
    }
    refreshList(workspace);
    refreshDetailControls(workspace);
    graph_.setVisibleDbRange(workspace.ui.targetCurveGraphHasCustomVisibleDbRange,
                             workspace.ui.targetCurveGraphVisibleMinDb,
                             workspace.ui.targetCurveGraphVisibleMaxDb);
    refreshGraph(workspace);
    updatingControls_ = false;
}

void TargetCurvePage::syncToWorkspace(WorkspaceState& workspace) const {
    workspace.ui.targetCurveGraphExtraRangeDb = 0.0;
    workspace.ui.targetCurveGraphVerticalOffsetDb = 0.0;
    workspace.ui.targetCurveGraphHasCustomVisibleDbRange = graph_.hasCustomVisibleDbRange();
    workspace.ui.targetCurveGraphVisibleMinDb = graph_.visibleMinDb();
    workspace.ui.targetCurveGraphVisibleMaxDb = graph_.visibleMaxDb();
    workspace.targetCurve = graph_.settings();
    workspace.activeTargetCurveComment = toUtf8(getWindowTextValue(controls_.commentValue));
    syncActiveProfileFromWorkspaceState(workspace);
}

bool TargetCurvePage::handleCommand(WORD commandId,
                                    WORD notificationCode,
                                    WorkspaceState& workspace,
                                    bool& curveChanged,
                                    bool& persistencePending,
                                    bool& persistNowRequested) {
    if (commandId == kGraph && notificationCode == TargetCurveGraph::kZoomChangedNotification) {
        workspace.ui.targetCurveGraphExtraRangeDb = 0.0;
        workspace.ui.targetCurveGraphVerticalOffsetDb = 0.0;
        workspace.ui.targetCurveGraphHasCustomVisibleDbRange = graph_.hasCustomVisibleDbRange();
        workspace.ui.targetCurveGraphVisibleMinDb = graph_.visibleMinDb();
        workspace.ui.targetCurveGraphVisibleMaxDb = graph_.visibleMaxDb();
        return true;
    }
    if (commandId == kGraph && notificationCode == TargetCurveGraph::kSelectionChangedNotification) {
        selectedBandIndex_ = graph_.selectedBandIndex();
        workspace.targetCurve = graph_.settings();
        refreshList(workspace);
        refreshDetailControls(workspace);
        return true;
    }
    if (commandId == kGraph && notificationCode == TargetCurveGraph::kModelPreviewNotification) {
        workspace.targetCurve = graph_.settings();
        selectedBandIndex_ = graph_.selectedBandIndex();
        refreshDetailControls(workspace);
        return true;
    }
    if (commandId == kGraph && notificationCode == TargetCurveGraph::kModelChangedNotification) {
        workspace.targetCurve = graph_.settings();
        selectedBandIndex_ = graph_.selectedBandIndex();
        refreshList(workspace);
        refreshDetailControls(workspace);
        curveChanged = true;
        persistencePending = true;
        return true;
    }

    switch (commandId) {
    case kButtonNewProfile:
        createProfile(workspace);
        curveChanged = true;
        persistNowRequested = true;
        return true;
    case kButtonNew:
        addBand(workspace);
        curveChanged = true;
        persistencePending = true;
        return true;
    case kButtonDelete:
        if (workspace.targetCurve.bypassEqBands) {
            return true;
        }
        deleteSelectedBand(workspace);
        curveChanged = true;
        persistencePending = true;
        return true;
    case kButtonReset:
        resetTarget(workspace);
        curveChanged = true;
        persistencePending = true;
        return true;
    case kCheckboxBypassAll:
        workspace.targetCurve.bypassEqBands =
            SendMessageW(controls_.checkboxBypassAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
        refreshList(workspace);
        refreshDetailControls(workspace);
        refreshGraph(workspace);
        curveChanged = true;
        persistencePending = true;
        return true;
    case kListBands:
        if (workspace.targetCurve.bypassEqBands) {
            return true;
        }
        if (notificationCode == kBandToggleNotification && pendingBandToggleIndex_ != -1) {
            toggleBandEnabled(pendingBandToggleIndex_, workspace);
            pendingBandToggleIndex_ = -1;
            curveChanged = true;
            persistencePending = true;
            return true;
        }
        if (notificationCode == LBN_SELCHANGE) {
            selectBand(static_cast<int>(SendMessageW(controls_.listBands, LB_GETCURSEL, 0, 0)), workspace);
            return true;
        }
        if (notificationCode == LBN_DBLCLK) {
            const int listIndex = static_cast<int>(SendMessageW(controls_.listBands, LB_GETCURSEL, 0, 0));
            if (listIndex >= 0) {
                const int bandIndex = static_cast<int>(SendMessageW(controls_.listBands, LB_GETITEMDATA, listIndex, 0));
                toggleBandEnabled(bandIndex, workspace);
                curveChanged = true;
                persistencePending = true;
                return true;
            }
        }
        return false;
    case kCheckboxBandEnabled:
        if (!workspace.targetCurve.bypassEqBands &&
            selectedBandIndex_ >= 0 &&
            selectedBandIndex_ < static_cast<int>(workspace.targetCurve.eqBands.size())) {
            workspace.targetCurve.eqBands[static_cast<size_t>(selectedBandIndex_)].enabled =
                SendMessageW(controls_.checkboxEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
            syncAllOffState(workspace.targetCurve);
            refreshList(workspace);
            refreshDetailControls(workspace);
            refreshGraph(workspace);
            curveChanged = true;
            persistencePending = true;
            return true;
        }
        return false;
    case kComboProfiles:
        if (notificationCode == CBN_SELCHANGE) {
            syncActiveProfileFromWorkspaceState(workspace);
            const int selection = static_cast<int>(SendMessageW(controls_.comboProfiles, CB_GETCURSEL, 0, 0));
            if (selection >= 0) {
                wchar_t buffer[256] = {};
                SendMessageW(controls_.comboProfiles, CB_GETLBTEXT, selection, reinterpret_cast<LPARAM>(buffer));
                selectActiveProfile(workspace, toUtf8(buffer));
                populate(workspace);
            }
            curveChanged = true;
            persistNowRequested = true;
            return true;
        }
        return false;
    case kCommentEdit:
        if (updatingControls_) {
            return false;
        }
        if (notificationCode == EN_CHANGE) {
            workspace.activeTargetCurveComment = toUtf8(getWindowTextValue(controls_.commentValue));
            syncActiveProfileFromWorkspaceState(workspace);
            persistencePending = true;
            return true;
        }
        return false;
    case kFrequencyEdit:
    case kGainEdit:
    case kQEdit:
        if (updatingControls_ || notificationCode != EN_CHANGE) {
            return false;
        }
        if (selectedBandIndex_ < 0 || selectedBandIndex_ >= static_cast<int>(workspace.targetCurve.eqBands.size())) {
            return false;
        }
        break;
    default:
        break;
    }

    double parsedValue = 0.0;
    if (workspace.targetCurve.bypassEqBands) {
        return false;
    }

    if (selectedBandIndex_ < 0 || selectedBandIndex_ >= static_cast<int>(workspace.targetCurve.eqBands.size())) {
        return false;
    }
    TargetEqBand& band = workspace.targetCurve.eqBands[static_cast<size_t>(selectedBandIndex_)];
    if (commandId == kFrequencyEdit && tryParseDouble(getWindowTextValue(controls_.frequencyValue), parsedValue)) {
        band.frequencyHz = parsedValue;
    } else if (commandId == kGainEdit && tryParseDouble(getWindowTextValue(controls_.gainValue), parsedValue)) {
        band.gainDb = parsedValue;
    } else if (commandId == kQEdit && tryParseDouble(getWindowTextValue(controls_.qValue), parsedValue)) {
        band.q = parsedValue;
    } else {
        return false;
    }

    measurement::normalizeTargetCurveSettings(workspace.targetCurve,
                                              std::max(workspace.measurement.startFrequencyHz, 20.0),
                                              std::min(workspace.measurement.endFrequencyHz, 20000.0));
    refreshList(workspace);
    refreshDetailControls(workspace);
    refreshGraph(workspace);
    curveChanged = true;
    persistencePending = true;
    return true;
}

bool TargetCurvePage::handleHScroll(HWND source, WorkspaceState& workspace, bool& curveChanged, bool& persistencePending) {
    if (selectedBandIndex_ < 0 || selectedBandIndex_ >= static_cast<int>(workspace.targetCurve.eqBands.size())) {
        return false;
    }
    if (workspace.targetCurve.bypassEqBands) {
        return false;
    }

    const measurement::TargetCurvePlotData plot = measurement::buildTargetCurvePlotData(workspace.smoothedResponse,
                                                                                         workspace.measurement,
                                                                                         workspace.targetCurve,
                                                                                         std::nullopt);
    TargetEqBand& band = workspace.targetCurve.eqBands[static_cast<size_t>(selectedBandIndex_)];
    if (source == controls_.frequencySlider) {
        band.frequencyHz = sliderPositionToFrequency(static_cast<int>(SendMessageW(controls_.frequencySlider, TBM_GETPOS, 0, 0)),
                                                     plot.minFrequencyHz,
                                                     plot.maxFrequencyHz);
    } else if (source == controls_.gainSlider) {
        band.gainDb = sliderPositionToGain(static_cast<int>(SendMessageW(controls_.gainSlider, TBM_GETPOS, 0, 0)));
    } else if (source == controls_.qSlider) {
        band.q = sliderPositionToQ(static_cast<int>(SendMessageW(controls_.qSlider, TBM_GETPOS, 0, 0)));
    } else {
        return false;
    }

    measurement::normalizeTargetCurveSettings(workspace.targetCurve, plot.minFrequencyHz, plot.maxFrequencyHz);
    refreshList(workspace);
    refreshDetailControls(workspace);
    refreshGraph(workspace);
    curveChanged = true;
    persistencePending = true;
    return true;
}

bool TargetCurvePage::handleDrawItem(const DRAWITEMSTRUCT* draw) const {
    if (draw == nullptr || draw->CtlID != kListBands) {
        return false;
    }

    const bool selected = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool bypassed = SendMessageW(controls_.checkboxBypassAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const COLORREF fillColor = !bypassed && selected ? RGB(225, 234, 246) : ui_theme::kPanelBackground;
    HBRUSH fillBrush = CreateSolidBrush(fillColor);
    FillRect(draw->hDC, &draw->rcItem, fillBrush);
    DeleteObject(fillBrush);

    if (draw->itemID != static_cast<UINT>(-1)) {
        const LPARAM itemData = SendMessageW(draw->hwndItem, LB_GETITEMDATA, draw->itemID, 0);
        const size_t bandIndex = itemData >= 0 ? static_cast<size_t>(itemData) : 0;
        const TargetEqBand* band = bandIndex < displayedBands_.size() ? &displayedBands_[bandIndex] : nullptr;
        const COLORREF textColor = bypassed ? ui_theme::kMuted : ui_theme::kText;
        const COLORREF checkColor = bypassed ? ui_theme::kMuted : ui_theme::kText;
        RECT checkboxRect{draw->rcItem.left + 8, draw->rcItem.top + 6, draw->rcItem.left + 22, draw->rcItem.top + 20};
        HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(draw->hDC, borderPen));
        HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(draw->hDC, GetStockObject(HOLLOW_BRUSH)));
        Rectangle(draw->hDC, checkboxRect.left, checkboxRect.top, checkboxRect.right, checkboxRect.bottom);
        SelectObject(draw->hDC, oldBrush);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(borderPen);

        if (band != nullptr && band->enabled) {
            HPEN checkPen = CreatePen(PS_SOLID, 2, checkColor);
            oldPen = reinterpret_cast<HPEN>(SelectObject(draw->hDC, checkPen));
            MoveToEx(draw->hDC, checkboxRect.left + 3, checkboxRect.top + 7, nullptr);
            LineTo(draw->hDC, checkboxRect.left + 6, checkboxRect.bottom - 4);
            LineTo(draw->hDC, checkboxRect.right - 3, checkboxRect.top + 4);
            SelectObject(draw->hDC, oldPen);
            DeleteObject(checkPen);
        }
        const COLORREF baseDotColor = band != nullptr ? targetCurveBandColor(band->colorIndex)
                                                      : targetCurveBandColor(static_cast<int>(bandIndex));
        const COLORREF dotColor = bypassed ? blendColor(baseDotColor, ui_theme::kPanelBackground, 0.55) : baseDotColor;
        HBRUSH dotBrush = CreateSolidBrush(dotColor);
        HBRUSH previousBrush = reinterpret_cast<HBRUSH>(SelectObject(draw->hDC, dotBrush));
        HPEN dotPen = CreatePen(PS_SOLID, 1, dotColor);
        oldPen = reinterpret_cast<HPEN>(SelectObject(draw->hDC, dotPen));
        Ellipse(draw->hDC, draw->rcItem.left + 30, draw->rcItem.top + 7, draw->rcItem.left + 42, draw->rcItem.top + 19);
        SelectObject(draw->hDC, previousBrush);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(dotPen);
        DeleteObject(dotBrush);
        RECT typeRect{draw->rcItem.left + 50, draw->rcItem.top + 4, draw->rcItem.left + 120, draw->rcItem.bottom - 2};
        RECT freqRect{draw->rcItem.left + 124, draw->rcItem.top + 4, draw->rcItem.right - 8, draw->rcItem.bottom - 2};
        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, textColor);
        const std::wstring typeText = L"Bell";
        const std::wstring freqText = band != nullptr ? formatWideDouble(band->frequencyHz, 0) + L" Hz" : L"";
        DrawTextW(draw->hDC, typeText.c_str(), -1, &typeRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(draw->hDC, freqText.c_str(), -1, &freqRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (!bypassed && focused) {
        DrawFocusRect(draw->hDC, &draw->rcItem);
    }
    return true;
}

LRESULT CALLBACK TargetCurvePage::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    TargetCurvePage* page = reinterpret_cast<TargetCurvePage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    static HBRUSH pageBackgroundBrush = CreateSolidBrush(ui_theme::kBackground);
    switch (message) {
    case WM_MOUSEWHEEL:
        if (page != nullptr && page->handleMouseWheel(wParam, lParam)) {
            return 0;
        }
        break;
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_DRAWITEM: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    }
    case WM_CTLCOLOREDIT: {
        static HBRUSH editBackgroundBrush = CreateSolidBrush(ui_theme::kPanelBackground);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, ui_theme::kPanelBackground);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(editBackgroundBrush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        static HBRUSH listBrush = CreateSolidBrush(ui_theme::kPanelBackground);
        return reinterpret_cast<INT_PTR>(listBrush);
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK TargetCurvePage::BandListProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    TargetCurvePage* page = reinterpret_cast<TargetCurvePage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (page == nullptr || page->bandListProc_ == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    if (message == WM_LBUTTONDOWN) {
        if (SendMessageW(page->controls_.checkboxBypassAll, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const DWORD itemData = static_cast<DWORD>(SendMessageW(window, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y)));
        const int index = LOWORD(itemData);
        const bool outside = HIWORD(itemData) != 0;
        if (!outside && index >= 0) {
            RECT itemRect{};
            SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&itemRect));
            const RECT checkboxRect{itemRect.left + 8, itemRect.top + 6, itemRect.left + 22, itemRect.top + 20};
            if (PtInRect(&checkboxRect, point) != FALSE) {
                page->pendingBandToggleIndex_ = static_cast<int>(SendMessageW(window, LB_GETITEMDATA, index, 0));
                SendMessageW(window, LB_SETCURSEL, index, 0);
                HWND parent = GetParent(window);
                if (parent != nullptr) {
                    SendMessageW(parent,
                                 WM_COMMAND,
                                 MAKEWPARAM(GetDlgCtrlID(window), kBandToggleNotification),
                                 reinterpret_cast<LPARAM>(window));
                }
                return 0;
            }
        }
    }

    return CallWindowProcW(page->bandListProc_, window, message, wParam, lParam);
}

LRESULT CALLBACK TargetCurvePage::ValueEditProc(HWND window,
                                                UINT message,
                                                WPARAM wParam,
                                                LPARAM lParam,
                                                UINT_PTR subclassId,
                                                DWORD_PTR refData) {
    auto* page = reinterpret_cast<TargetCurvePage*>(refData);
    if (message == WM_MOUSEWHEEL && page != nullptr) {
        const int wheelSteps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        if (page->adjustValueField(window, wheelSteps)) {
            return 0;
        }
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

bool TargetCurvePage::tryParseDouble(const std::wstring& text, double& value) {
    if (text.empty()) {
        return false;
    }
    try {
        size_t cursor = 0;
        value = std::stod(text, &cursor);
        return cursor == text.size();
    } catch (...) {
        return false;
    }
}

std::wstring TargetCurvePage::getWindowTextValue(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}

std::wstring TargetCurvePage::requestProfileName(HWND owner, HINSTANCE instance) {
    ensureProfilePromptWindowClass(instance);

    ProfilePromptDialogState state;
    const int dialogWidth = 360;
    const int dialogHeight = 144;
    RECT ownerRect{0, 0, dialogWidth, dialogHeight};
    if (owner != nullptr) {
        GetWindowRect(owner, &ownerRect);
    }
    const int x = ownerRect.left + std::max(0L, ((ownerRect.right - ownerRect.left) - dialogWidth) / 2);
    const int y = ownerRect.top + std::max(0L, ((ownerRect.bottom - ownerRect.top) - dialogHeight) / 2);

    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kProfilePromptClassName,
                                  L"New Target Curve",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  dialogWidth,
                                  dialogHeight,
                                  owner,
                                  nullptr,
                                  instance,
                                  &state);
    if (dialog == nullptr) {
        return {};
    }

    CreateWindowW(L"STATIC", L"Profile name", WS_CHILD | WS_VISIBLE, 16, 16, 100, 18, dialog, nullptr, instance, nullptr);
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE,
                                 L"EDIT",
                                 L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 16,
                                 38,
                                 312,
                                 24,
                                 dialog,
                                 nullptr,
                                 instance,
                                 nullptr);
    CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 168, 78, 72, 26, dialog, reinterpret_cast<HMENU>(IDOK), instance, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 256, 78, 72, 26, dialog, reinterpret_cast<HMENU>(IDCANCEL), instance, nullptr);

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(state.edit);

    MSG message{};
    while (!state.finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return state.accepted ? state.value : std::wstring();
}

void TargetCurvePage::setWindowTextValue(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

bool TargetCurvePage::handleMouseWheel(WPARAM wParam, LPARAM lParam) {
    POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const HWND fields[] = {controls_.frequencyValue, controls_.gainValue, controls_.qValue};
    for (HWND field : fields) {
        if (field == nullptr) {
            continue;
        }

        RECT rect{};
        GetWindowRect(field, &rect);
        if (PtInRect(&rect, cursor) == FALSE) {
            continue;
        }

        return adjustValueField(field, GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA);
    }

    return false;
}

bool TargetCurvePage::adjustValueField(HWND control, int wheelSteps) {
    if (control == nullptr || wheelSteps == 0 || updatingControls_ || IsWindowEnabled(control) == FALSE) {
        return wheelSteps == 0;
    }

    double currentValue = 0.0;
    int decimals = 0;
    double nextValue = 0.0;

    if (SendMessageW(controls_.checkboxBypassAll, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        return false;
    }

    if (selectedBandIndex_ < 0 || selectedBandIndex_ >= static_cast<int>(displayedBands_.size())) {
        return false;
    }

    const TargetEqBand& band = displayedBands_[static_cast<size_t>(selectedBandIndex_)];
    if (control == controls_.frequencyValue) {
        if (!tryParseDouble(getWindowTextValue(control), currentValue) || currentValue <= 0.0) {
            currentValue = std::max(band.frequencyHz, 1.0);
        }
        nextValue = currentValue * std::pow(kFrequencyWheelScalePerStep, static_cast<double>(wheelSteps));
        decimals = 0;
    } else if (control == controls_.gainValue) {
        if (!tryParseDouble(getWindowTextValue(control), currentValue)) {
            currentValue = band.gainDb;
        }
        nextValue = currentValue + (kGainWheelStepDb * static_cast<double>(wheelSteps));
        decimals = 1;
    } else if (control == controls_.qValue) {
        if (!tryParseDouble(getWindowTextValue(control), currentValue) || currentValue <= 0.0) {
            currentValue = std::max(band.q, 0.1);
        }
        nextValue = std::max(0.1, currentValue + (kQWheelStep * static_cast<double>(wheelSteps)));
        decimals = 2;
    } else {
        return false;
    }

    setWindowTextValue(control, formatWideDouble(nextValue, decimals));
    return true;
}

void TargetCurvePage::syncAllOffState(TargetCurveSettings& settings) const {
    (void)settings;
}

void TargetCurvePage::syncActiveProfileFromWorkspaceState(WorkspaceState& workspace) const {
    for (TargetCurveProfile& profile : workspace.targetCurveProfiles) {
        if (profile.name == workspace.activeTargetCurveProfileName) {
            profile.curve = workspace.targetCurve;
            profile.comment = workspace.activeTargetCurveComment;
            return;
        }
    }

    TargetCurveProfile profile;
    profile.name = workspace.activeTargetCurveProfileName.empty() ? "Default" : workspace.activeTargetCurveProfileName;
    profile.comment = workspace.activeTargetCurveComment;
    profile.curve = workspace.targetCurve;
    workspace.targetCurveProfiles.push_back(profile);
}

void TargetCurvePage::selectActiveProfile(WorkspaceState& workspace, const std::string& profileName) {
    for (const TargetCurveProfile& profile : workspace.targetCurveProfiles) {
        if (profile.name == profileName) {
            workspace.activeTargetCurveProfileName = profile.name;
            workspace.activeTargetCurveComment = profile.comment;
            workspace.targetCurve = profile.curve;
            if (selectedBandIndex_ >= static_cast<int>(workspace.targetCurve.eqBands.size())) {
                selectedBandIndex_ = workspace.targetCurve.eqBands.empty() ? -1 : static_cast<int>(workspace.targetCurve.eqBands.size() - 1);
            }
            return;
        }
    }
}

int TargetCurvePage::frequencyToSliderPosition(double frequencyHz, double minFrequencyHz, double maxFrequencyHz) {
    const double minLog = std::log10(std::max(minFrequencyHz, 1e-6));
    const double maxLog = std::log10(std::max(maxFrequencyHz, minFrequencyHz + 1.0));
    const double currentLog = std::log10(clampValue(frequencyHz, minFrequencyHz, maxFrequencyHz));
    const double t = clampValue((currentLog - minLog) / std::max(maxLog - minLog, 1e-9), 0.0, 1.0);
    return static_cast<int>(std::lround(t * kFrequencySliderMax));
}

double TargetCurvePage::sliderPositionToFrequency(int position, double minFrequencyHz, double maxFrequencyHz) {
    const double t = static_cast<double>(clampValue(position, 0, kFrequencySliderMax)) / static_cast<double>(kFrequencySliderMax);
    const double minLog = std::log10(std::max(minFrequencyHz, 1e-6));
    const double maxLog = std::log10(std::max(maxFrequencyHz, minFrequencyHz + 1.0));
    return std::pow(10.0, minLog + ((maxLog - minLog) * t));
}

int TargetCurvePage::gainToSliderPosition(double gainDb) {
    return clampValue(static_cast<int>(std::lround((gainDb + 12.0) * 10.0)), 0, kGainSliderMax);
}

double TargetCurvePage::sliderPositionToGain(int position) {
    return (static_cast<double>(clampValue(position, 0, kGainSliderMax)) / 10.0) - 12.0;
}

int TargetCurvePage::qToSliderPosition(double q) {
    const double minLog = std::log10(0.3);
    const double maxLog = std::log10(6.0);
    const double valueLog = std::log10(clampValue(q, 0.3, 6.0));
    const double t = clampValue((valueLog - minLog) / (maxLog - minLog), 0.0, 1.0);
    return static_cast<int>(std::lround(t * kQSliderMax));
}

double TargetCurvePage::sliderPositionToQ(int position) {
    const double minLog = std::log10(0.3);
    const double maxLog = std::log10(6.0);
    const double t = static_cast<double>(clampValue(position, 0, kQSliderMax)) / static_cast<double>(kQSliderMax);
    return std::pow(10.0, minLog + ((maxLog - minLog) * t));
}

void TargetCurvePage::refreshList(const WorkspaceState& workspace) {
    updatingControls_ = true;
    displayedBands_ = workspace.targetCurve.eqBands;
    SendMessageW(controls_.checkboxBypassAll,
                 BM_SETCHECK,
                 workspace.targetCurve.bypassEqBands ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.listBands, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < workspace.targetCurve.eqBands.size(); ++i) {
        const int bandIndex = static_cast<int>(i);
        const std::wstring label = L"Bell";
        const LRESULT index = SendMessageW(controls_.listBands, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(controls_.listBands, LB_SETITEMDATA, index, static_cast<LPARAM>(bandIndex));
    }
    if (!workspace.targetCurve.bypassEqBands &&
        selectedBandIndex_ >= 0 &&
        selectedBandIndex_ < static_cast<int>(workspace.targetCurve.eqBands.size())) {
        SendMessageW(controls_.listBands, LB_SETCURSEL, selectedBandIndex_, 0);
    } else {
        SendMessageW(controls_.listBands, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    EnableWindow(controls_.listBands, workspace.targetCurve.bypassEqBands ? FALSE : TRUE);
    EnableWindow(controls_.buttonDelete,
                 !workspace.targetCurve.bypassEqBands && !workspace.targetCurve.eqBands.empty());
    updatingControls_ = false;
}

void TargetCurvePage::refreshProfileControls(const WorkspaceState& workspace) {
    SendMessageW(controls_.comboProfiles, CB_RESETCONTENT, 0, 0);
    int selectedIndex = -1;
    for (size_t index = 0; index < workspace.targetCurveProfiles.size(); ++index) {
        const std::wstring name = toWide(workspace.targetCurveProfiles[index].name);
        const LRESULT itemIndex = SendMessageW(controls_.comboProfiles, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        if (workspace.targetCurveProfiles[index].name == workspace.activeTargetCurveProfileName) {
            selectedIndex = static_cast<int>(itemIndex);
        }
    }
    if (selectedIndex >= 0) {
        SendMessageW(controls_.comboProfiles, CB_SETCURSEL, selectedIndex, 0);
    }
    setWindowTextValue(controls_.commentValue, toWide(workspace.activeTargetCurveComment));
}

void TargetCurvePage::refreshDetailControls(const WorkspaceState& workspace) {
    updatingControls_ = true;
    const bool hasSelection = !workspace.targetCurve.bypassEqBands &&
                              selectedBandIndex_ >= 0 &&
                              selectedBandIndex_ < static_cast<int>(workspace.targetCurve.eqBands.size());
    EnableWindow(controls_.checkboxEnabled, hasSelection);
    EnableWindow(controls_.frequencySlider, hasSelection);
    EnableWindow(controls_.frequencyValue, hasSelection);
    EnableWindow(controls_.gainSlider, hasSelection);
    EnableWindow(controls_.gainValue, hasSelection);
    EnableWindow(controls_.qSlider, hasSelection);
    EnableWindow(controls_.qValue, hasSelection);

    if (!hasSelection) {
        SendMessageW(controls_.checkboxEnabled, BM_SETCHECK, BST_UNCHECKED, 0);
        setWindowTextValue(controls_.frequencyValue, L"");
        setWindowTextValue(controls_.gainValue, L"");
        setWindowTextValue(controls_.qValue, L"");
        updatingControls_ = false;
        return;
    }

    const measurement::TargetCurvePlotData plot = measurement::buildTargetCurvePlotData(workspace.smoothedResponse,
                                                                                         workspace.measurement,
                                                                                         workspace.targetCurve,
                                                                                         std::nullopt);
    const TargetEqBand& band = workspace.targetCurve.eqBands[static_cast<size_t>(selectedBandIndex_)];
    SendMessageW(controls_.checkboxEnabled, BM_SETCHECK, band.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.frequencySlider, TBM_SETPOS, TRUE, frequencyToSliderPosition(band.frequencyHz, plot.minFrequencyHz, plot.maxFrequencyHz));
    SendMessageW(controls_.gainSlider, TBM_SETPOS, TRUE, gainToSliderPosition(band.gainDb));
    SendMessageW(controls_.qSlider, TBM_SETPOS, TRUE, qToSliderPosition(band.q));
    setWindowTextValue(controls_.frequencyValue, formatWideDouble(band.frequencyHz, 0));
    setWindowTextValue(controls_.gainValue, formatWideDouble(band.gainDb, 1));
    setWindowTextValue(controls_.qValue, formatWideDouble(band.q, 2));
    updatingControls_ = false;
}

void TargetCurvePage::refreshGraph(const WorkspaceState& workspace) {
    graph_.setModel(workspace.smoothedResponse, workspace.measurement, workspace.targetCurve, selectedBandIndex_);
}

void TargetCurvePage::selectBand(int index, WorkspaceState& workspace) {
    if (index < 0) {
        selectedBandIndex_ = workspace.targetCurve.eqBands.empty() ? -1 : 0;
    } else {
        const int bandIndex = static_cast<int>(SendMessageW(controls_.listBands, LB_GETITEMDATA, index, 0));
        selectedBandIndex_ = bandIndex >= 0 && bandIndex < static_cast<int>(workspace.targetCurve.eqBands.size())
                                 ? bandIndex
                                 : (workspace.targetCurve.eqBands.empty() ? -1 : 0);
    }
    refreshList(workspace);
    refreshDetailControls(workspace);
    refreshGraph(workspace);
}

void TargetCurvePage::addBand(WorkspaceState& workspace) {
    const measurement::TargetCurvePlotData plot = measurement::buildTargetCurvePlotData(workspace.smoothedResponse,
                                                                                         workspace.measurement,
                                                                                         workspace.targetCurve,
                                                                                         std::nullopt);
    const double defaultFrequencyHz = std::sqrt(plot.minFrequencyHz * plot.maxFrequencyHz);
    workspace.targetCurve.eqBands.push_back(measurement::makeDefaultTargetEqBand(defaultFrequencyHz,
                                                                                 static_cast<int>(workspace.targetCurve.eqBands.size())));
    measurement::normalizeTargetCurveSettings(workspace.targetCurve, plot.minFrequencyHz, plot.maxFrequencyHz);

    double closestDistance = std::numeric_limits<double>::max();
    selectedBandIndex_ = 0;
    for (size_t i = 0; i < workspace.targetCurve.eqBands.size(); ++i) {
        const double distance = std::abs(workspace.targetCurve.eqBands[i].frequencyHz - defaultFrequencyHz);
        if (distance < closestDistance) {
            closestDistance = distance;
            selectedBandIndex_ = static_cast<int>(i);
        }
    }
    syncAllOffState(workspace.targetCurve);
    refreshList(workspace);
    refreshDetailControls(workspace);
    refreshGraph(workspace);
    syncActiveProfileFromWorkspaceState(workspace);
}

void TargetCurvePage::createProfile(WorkspaceState& workspace) {
    const std::wstring requestedName = requestProfileName(window_, instance_);
    if (requestedName.empty()) {
        return;
    }

    const std::string name = toUtf8(requestedName);
    if (name.empty()) {
        return;
    }
    for (const TargetCurveProfile& profile : workspace.targetCurveProfiles) {
        if (profile.name == name) {
            MessageBoxW(window_, L"A target-curve profile with that name already exists.", L"Target Curve", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    syncActiveProfileFromWorkspaceState(workspace);

    TargetCurveProfile profile;
    profile.name = name;
    workspace.targetCurve = {};
    measurement::normalizeTargetCurveSettings(workspace.targetCurve,
                                              std::max(workspace.measurement.startFrequencyHz, 20.0),
                                              std::min(workspace.measurement.endFrequencyHz, 20000.0));
    profile.curve = workspace.targetCurve;
    workspace.activeTargetCurveProfileName = profile.name;
    workspace.activeTargetCurveComment.clear();
    workspace.targetCurveProfiles.push_back(profile);
    selectedBandIndex_ = workspace.targetCurve.eqBands.empty() ? -1 : 0;
    populate(workspace);
}

void TargetCurvePage::deleteSelectedBand(WorkspaceState& workspace) {
    if (selectedBandIndex_ < 0 || selectedBandIndex_ >= static_cast<int>(workspace.targetCurve.eqBands.size())) {
        return;
    }
    workspace.targetCurve.eqBands.erase(workspace.targetCurve.eqBands.begin() + selectedBandIndex_);
    if (workspace.targetCurve.eqBands.empty()) {
        selectedBandIndex_ = -1;
    } else {
        selectedBandIndex_ = clampValue(selectedBandIndex_, 0, static_cast<int>(workspace.targetCurve.eqBands.size()) - 1);
    }
    syncAllOffState(workspace.targetCurve);
    refreshList(workspace);
    refreshDetailControls(workspace);
    refreshGraph(workspace);
    syncActiveProfileFromWorkspaceState(workspace);
}

void TargetCurvePage::resetTarget(WorkspaceState& workspace) {
    workspace.targetCurve = {};
    measurement::normalizeTargetCurveSettings(workspace.targetCurve,
                                              std::max(workspace.measurement.startFrequencyHz, 20.0),
                                              std::min(workspace.measurement.endFrequencyHz, 20000.0));
    syncAllOffState(workspace.targetCurve);
    selectedBandIndex_ = workspace.targetCurve.eqBands.empty() ? -1 : 0;
    syncActiveProfileFromWorkspaceState(workspace);
    populate(workspace);
}

void TargetCurvePage::toggleBandEnabled(int index, WorkspaceState& workspace) {
    if (index < 0 || index >= static_cast<int>(workspace.targetCurve.eqBands.size())) {
        return;
    }
    workspace.targetCurve.eqBands[static_cast<size_t>(index)].enabled =
        !workspace.targetCurve.eqBands[static_cast<size_t>(index)].enabled;
    if (selectedBandIndex_ != index) {
        selectedBandIndex_ = index;
    }
    syncAllOffState(workspace.targetCurve);
    refreshList(workspace);
    refreshDetailControls(workspace);
    refreshGraph(workspace);
    syncActiveProfileFromWorkspaceState(workspace);
}

}  // namespace wolfie::ui
