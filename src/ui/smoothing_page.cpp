#include "ui/smoothing_page.h"

#include <algorithm>

#include "core/text_utils.h"
#include "measurement/response_smoother.h"
#include "ui/ui_theme.h"
#include <commctrl.h>

namespace wolfie::ui {

void SmoothingPage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = CreateSolidBrush(ui_theme::kBackground);
    RegisterClassW(&pageClass);
}

const wchar_t* SmoothingPage::pageWindowClassName() {
    return kPageClassName;
}

void SmoothingPage::create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    window_ = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr, instance, nullptr);
    createControls();
}

void SmoothingPage::createControls() {
    controls_.labelModel = CreateWindowW(L"STATIC", L"Model", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboModel = CreateWindowW(L"COMBOBOX",
                                         nullptr,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                         0,
                                         0,
                                         0,
                                         0,
                                         window_,
                                         reinterpret_cast<HMENU>(kComboModel),
                                         instance_,
                                         nullptr);
    controls_.labelResolution = CreateWindowW(L"STATIC", L"Resolution", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.resolutionSlider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                                                 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.resolutionCoarseLabel = CreateWindowW(L"STATIC", L"Smooth", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.resolutionFineLabel = CreateWindowW(L"STATIC", L"Fine", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.effectiveParameter = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelHighFrequencyCutoff = CreateWindowW(L"STATIC", L"HF Slope Cutoff", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editHighFrequencyCutoff = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitHighFrequencyCutoff = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    const DWORD centeredStaticStyle = SS_CENTER | WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(controls_.labelModel, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelResolution, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelHighFrequencyCutoff, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.unitHighFrequencyCutoff, GWL_STYLE, centeredStaticStyle);

    SendMessageW(controls_.editHighFrequencyCutoff, EM_SETREADONLY, TRUE, 0);
    populateModelCombo(controls_.comboModel);
    SendMessageW(controls_.resolutionSlider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.resolutionSlider, TBM_SETRANGEMAX, FALSE, kResolutionSliderMax);
    SendMessageW(controls_.resolutionSlider, TBM_SETTICFREQ, 10, 0);

    responseGraph_.create(window_, instance_, kResponseGraph);
}

void SmoothingPage::layout() {
    RECT pageRect{};
    GetClientRect(window_, &pageRect);
    const int contentLeft = 20;
    const int contentTop = 20;
    const int innerWidth = std::max(480L, pageRect.right - (contentLeft * 2));
    const int innerHeight = std::max(360L, pageRect.bottom - (contentTop * 2));
    constexpr int kFieldGap = 40;
    constexpr int kLabelTopOffset = 2;
    constexpr int kFieldTopOffset = 22;
    constexpr int kUnitTopOffset = 52;
    constexpr int kUnitHeight = 16;
    constexpr int kSliderWidth = 240;

    auto placeFieldWithUnit = [&](HWND label, HWND edit, HWND unit, int left, int top, int labelWidth, int editWidth, int unitWidth) {
        const int labelLeft = left + ((editWidth - labelWidth) / 2);
        const int unitLeft = left + ((editWidth - unitWidth) / 2);
        MoveWindow(label, labelLeft, top + kLabelTopOffset, labelWidth, 18, TRUE);
        MoveWindow(edit, left, top + kFieldTopOffset, editWidth, 26, TRUE);
        MoveWindow(unit, unitLeft, top + kUnitTopOffset, unitWidth, kUnitHeight, TRUE);
    };

    const int parameterTop = contentTop;
    MoveWindow(controls_.labelModel, contentLeft, parameterTop + kLabelTopOffset, 140, 18, TRUE);
    MoveWindow(controls_.comboModel, contentLeft, parameterTop + kFieldTopOffset, 220, 220, TRUE);
    const int resolutionLeft = contentLeft + 260;
    MoveWindow(controls_.labelResolution, resolutionLeft, parameterTop + kLabelTopOffset, 120, 18, TRUE);
    MoveWindow(controls_.resolutionSlider, resolutionLeft, parameterTop + kFieldTopOffset - 2, kSliderWidth, 32, TRUE);
    MoveWindow(controls_.resolutionCoarseLabel, resolutionLeft, parameterTop + 50, 48, 18, TRUE);
    MoveWindow(controls_.resolutionFineLabel, resolutionLeft + kSliderWidth - 36, parameterTop + 50, 36, 18, TRUE);
    MoveWindow(controls_.effectiveParameter, resolutionLeft, parameterTop + 72, std::max(260, innerWidth - resolutionLeft - 24), 18, TRUE);

    const int detailTop = parameterTop + 98;
    placeFieldWithUnit(controls_.labelHighFrequencyCutoff,
                       controls_.editHighFrequencyCutoff,
                       controls_.unitHighFrequencyCutoff,
                       contentLeft,
                       detailTop,
                       130,
                       96,
                       24);

    const int graphTop = detailTop + 92;
    const RECT graphBounds{contentLeft, graphTop, contentLeft + innerWidth, graphTop + std::max(200, innerHeight - graphTop - 12)};
    responseGraph_.layout(graphBounds);
}

void SmoothingPage::setVisible(bool visible) const {
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
}

void SmoothingPage::populate(const WorkspaceState& workspace) {
    SendMessageW(controls_.comboModel, CB_SETCURSEL, comboIndexFromModel(workspace.smoothing.psychoacousticModel), 0);
    SendMessageW(controls_.resolutionSlider, TBM_SETPOS, TRUE, workspace.smoothing.resolutionPercent);
    setWindowTextValue(controls_.effectiveParameter, formatEffectiveParameter(workspace.smoothing));
    setWindowTextValue(controls_.editHighFrequencyCutoff, formatWideDouble(workspace.smoothing.highFrequencySlopeCutoffHz, 0));
    responseGraph_.setVisibleFrequencyRange(workspace.ui.smoothingGraphHasCustomFrequencyRange,
                                            workspace.ui.smoothingGraphVisibleMinFrequencyHz,
                                            workspace.ui.smoothingGraphVisibleMaxFrequencyHz);
    responseGraph_.setData(buildGraphData(workspace.smoothedResponse));
}

void SmoothingPage::syncToWorkspace(WorkspaceState& workspace) const {
    workspace.ui.smoothingGraphExtraRangeDb = 0.0;
    workspace.ui.smoothingGraphVerticalOffsetDb = 0.0;
    workspace.ui.smoothingGraphHasCustomFrequencyRange = responseGraph_.hasCustomVisibleFrequencyRange();
    workspace.ui.smoothingGraphVisibleMinFrequencyHz = responseGraph_.visibleMinFrequencyHz();
    workspace.ui.smoothingGraphVisibleMaxFrequencyHz = responseGraph_.visibleMaxFrequencyHz();
}

void SmoothingPage::invalidateGraph() const {
    responseGraph_.invalidate();
}

bool SmoothingPage::handleCommand(WORD commandId,
                                  WORD notificationCode,
                                  WorkspaceState& workspace,
                                  bool& smoothingModelChanged,
                                  bool& graphZoomChanged) {
    if (commandId == kResponseGraph && notificationCode == ResponseGraph::kZoomChangedNotification) {
        workspace.ui.smoothingGraphExtraRangeDb = 0.0;
        workspace.ui.smoothingGraphVerticalOffsetDb = 0.0;
        workspace.ui.smoothingGraphHasCustomFrequencyRange = responseGraph_.hasCustomVisibleFrequencyRange();
        workspace.ui.smoothingGraphVisibleMinFrequencyHz = responseGraph_.visibleMinFrequencyHz();
        workspace.ui.smoothingGraphVisibleMaxFrequencyHz = responseGraph_.visibleMaxFrequencyHz();
        graphZoomChanged = true;
        return true;
    }

    if (commandId != kComboModel || notificationCode != CBN_SELCHANGE) {
        return false;
    }

    workspace.smoothing.psychoacousticModel =
        modelFromComboIndex(static_cast<int>(SendMessageW(controls_.comboModel, CB_GETCURSEL, 0, 0)));
    smoothingModelChanged = true;
    return true;
}

bool SmoothingPage::handleHScroll(HWND source, WorkspaceState& workspace, bool& smoothingResolutionChanged) {
    if (source != controls_.resolutionSlider) {
        return false;
    }

    workspace.smoothing.resolutionPercent =
        static_cast<int>(SendMessageW(controls_.resolutionSlider, TBM_GETPOS, 0, 0));
    setWindowTextValue(controls_.effectiveParameter, formatEffectiveParameter(workspace.smoothing));
    smoothingResolutionChanged = true;
    return true;
}

LRESULT CALLBACK SmoothingPage::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static HBRUSH pageBackgroundBrush = CreateSolidBrush(ui_theme::kBackground);
    switch (message) {
    case WM_COMMAND: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
    case WM_HSCROLL: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void SmoothingPage::setWindowTextValue(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

void SmoothingPage::populateModelCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ERB auditory smoothing"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"octave sliding window"));
}

int SmoothingPage::comboIndexFromModel(const std::string& model) {
    if (model == "octave sliding window" ||
        model == "1/12 octave sliding window") {
        return 1;
    }
    return 0;
}

std::string SmoothingPage::modelFromComboIndex(int index) {
    if (index == 1) {
        return "octave sliding window";
    }
    return "ERB auditory smoothing";
}

std::wstring SmoothingPage::formatEffectiveParameter(const ResponseSmoothingSettings& settings) {
    ResponseSmoothingSettings normalizedSettings = settings;
    measurement::normalizeResponseSmoothingSettings(normalizedSettings);
    if (normalizedSettings.psychoacousticModel == "octave sliding window") {
        return L"Effective band: 1/" +
               std::to_wstring(measurement::effectiveSlidingOctaveDenominator(normalizedSettings)) +
               L" octave";
    }
    return L"Effective windows: low " +
           std::to_wstring(measurement::effectiveLowWindowCycles(normalizedSettings)) +
           L" cycles, high " +
           std::to_wstring(measurement::effectiveHighWindowCycles(normalizedSettings)) +
           L" cycles";
}

ResponseGraphData SmoothingPage::buildGraphData(const SmoothedResponse& response) const {
    ResponseGraphData data;
    data.frequencyAxisHz = response.frequencyAxisHz;
    if (!response.frequencyAxisHz.empty()) {
        data.series.push_back({L"Left", ui_theme::kGreen, response.leftChannelDb});
        data.series.push_back({L"Right", ui_theme::kRed, response.rightChannelDb});
    }
    return data;
}

}  // namespace wolfie::ui
