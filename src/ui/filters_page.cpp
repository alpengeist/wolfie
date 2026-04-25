#include "ui/filters_page.h"

#include <algorithm>
#include <cmath>

#include <commctrl.h>

#include "core/text_utils.h"
#include "measurement/filter_designer.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

}  // namespace

void FiltersPage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = CreateSolidBrush(ui_theme::kBackground);
    RegisterClassW(&pageClass);
}

const wchar_t* FiltersPage::pageWindowClassName() {
    return kPageClassName;
}

void FiltersPage::create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    window_ = CreateWindowExW(0,
                              kPageClassName,
                              nullptr,
                              WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
                              0,
                              0,
                              0,
                              0,
                              parent,
                              nullptr,
                              instance,
                              this);
    createControls();
}

void FiltersPage::createControls() {
    controls_.labelTapCount = CreateWindowW(L"STATIC", L"Tap Count", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboTapCount = CreateWindowW(L"COMBOBOX",
                                            nullptr,
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            0,
                                            0,
                                            0,
                                            0,
                                            window_,
                                            reinterpret_cast<HMENU>(kComboTapCount),
                                            instance_,
                                            nullptr);
    controls_.labelPhaseMode = CreateWindowW(L"STATIC", L"Phase Mode", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.phaseModeValue = CreateWindowW(L"STATIC", L"Minimum phase", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMaxBoost = CreateWindowW(L"STATIC", L"Max Boost", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMaxBoost = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                             0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditMaxBoost), instance_, nullptr);
    controls_.unitMaxBoost = CreateWindowW(L"STATIC", L"dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMaxCut = CreateWindowW(L"STATIC", L"Max Cut", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMaxCut = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                           0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditMaxCut), instance_, nullptr);
    controls_.unitMaxCut = CreateWindowW(L"STATIC", L"dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelLowCorrection = CreateWindowW(L"STATIC", L"Low Bound", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editLowCorrection = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditLowCorrection), instance_, nullptr);
    controls_.unitLowCorrection = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelHighCorrection = CreateWindowW(L"STATIC", L"High Bound", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editHighCorrection = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                                   0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditHighCorrection), instance_, nullptr);
    controls_.unitHighCorrection = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonRecalculate = CreateWindowW(L"BUTTON", L"Recalculate", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonRecalculate), instance_, nullptr);
    controls_.summary = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.correctionTitle = CreateWindowW(L"STATIC", L"Correction And Filter Response", WS_CHILD | WS_VISIBLE,
                                              0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.correctedTitle = CreateWindowW(L"STATIC", L"Predicted Corrected Response", WS_CHILD | WS_VISIBLE,
                                             0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.groupDelayTitle = CreateWindowW(L"STATIC", L"Filter Group Delay", WS_CHILD | WS_VISIBLE,
                                              0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.impulseTitle = CreateWindowW(L"STATIC", L"Filter Impulse", WS_CHILD | WS_VISIBLE,
                                           0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    populateTapCountCombo(controls_.comboTapCount);
    correctionGraph_.create(window_, instance_);
    correctedGraph_.create(window_, instance_);
    groupDelayGraph_.create(window_, instance_);
    impulseGraph_.create(window_, instance_);
}

void FiltersPage::layout() {
    RECT pageRect{};
    GetClientRect(window_, &pageRect);
    const int viewportWidth = std::max(480L, pageRect.right);
    const int viewportHeight = std::max(360L, pageRect.bottom);
    const int contentLeft = 20;
    const int contentWidth = std::max(420, viewportWidth - (contentLeft * 2) - GetSystemMetrics(SM_CXVSCROLL));
    const int graphHeight = 260;
    const int graphGap = 34;
    const int sectionGap = 26;
    const int top = 20 - scrollOffset_;
    const int comboDropHeight = 220;

    MoveWindow(controls_.labelTapCount, contentLeft, top, 84, 18, TRUE);
    MoveWindow(controls_.comboTapCount, contentLeft, top + 22, 120, comboDropHeight, TRUE);
    MoveWindow(controls_.labelPhaseMode, contentLeft + 148, top, 84, 18, TRUE);
    MoveWindow(controls_.phaseModeValue, contentLeft + 148, top + 24, 120, 18, TRUE);
    MoveWindow(controls_.labelMaxBoost, contentLeft + 292, top, 70, 18, TRUE);
    MoveWindow(controls_.editMaxBoost, contentLeft + 292, top + 22, 58, 26, TRUE);
    MoveWindow(controls_.unitMaxBoost, contentLeft + 354, top + 26, 24, 18, TRUE);
    MoveWindow(controls_.labelMaxCut, contentLeft + 394, top, 70, 18, TRUE);
    MoveWindow(controls_.editMaxCut, contentLeft + 394, top + 22, 58, 26, TRUE);
    MoveWindow(controls_.unitMaxCut, contentLeft + 456, top + 26, 24, 18, TRUE);
    MoveWindow(controls_.labelLowCorrection, contentLeft + 496, top, 72, 18, TRUE);
    MoveWindow(controls_.editLowCorrection, contentLeft + 496, top + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitLowCorrection, contentLeft + 568, top + 26, 22, 18, TRUE);
    MoveWindow(controls_.labelHighCorrection, contentLeft + 606, top, 76, 18, TRUE);
    MoveWindow(controls_.editHighCorrection, contentLeft + 606, top + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitHighCorrection, contentLeft + 678, top + 26, 22, 18, TRUE);
    MoveWindow(controls_.buttonRecalculate, contentLeft + contentWidth - 110, top + 18, 110, 30, TRUE);
    MoveWindow(controls_.summary, contentLeft, top + 62, contentWidth, 18, TRUE);

    int y = top + 96;
    MoveWindow(controls_.correctionTitle, contentLeft, y, contentWidth, 18, TRUE);
    correctionGraph_.layout(RECT{contentLeft, y + 24, contentLeft + contentWidth, y + 24 + graphHeight});

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.correctedTitle, contentLeft, y, contentWidth, 18, TRUE);
    correctedGraph_.layout(RECT{contentLeft, y + 24, contentLeft + contentWidth, y + 24 + graphHeight});

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.groupDelayTitle, contentLeft, y, contentWidth, 18, TRUE);
    groupDelayGraph_.layout(RECT{contentLeft, y + 24, contentLeft + contentWidth, y + 24 + graphHeight});

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.impulseTitle, contentLeft, y, contentWidth, 18, TRUE);
    impulseGraph_.layout(RECT{contentLeft, y + 24, contentLeft + contentWidth, y + 24 + graphHeight});

    contentHeight_ = y + 24 + graphHeight + sectionGap + 20;
    updateScrollBar();

    if (contentHeight_ - scrollOffset_ < viewportHeight) {
        setScrollOffset(std::max(0, contentHeight_ - viewportHeight));
    }
}

void FiltersPage::setVisible(bool visible) const {
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
}

void FiltersPage::populate(const WorkspaceState& workspace) {
    FilterDesignSettings settings = workspace.filters;
    measurement::normalizeFilterDesignSettings(settings, workspace.measurement.sampleRate);
    SendMessageW(controls_.comboTapCount, CB_SETCURSEL, comboIndexFromTapCount(settings.tapCount), 0);
    SetWindowTextW(controls_.phaseModeValue, L"Minimum phase");
    setWindowTextValue(controls_.editMaxBoost, formatWideDouble(settings.maxBoostDb, 1));
    setWindowTextValue(controls_.editMaxCut, formatWideDouble(settings.maxCutDb, 1));
    setWindowTextValue(controls_.editLowCorrection, formatWideDouble(settings.lowCorrectionHz, 0));
    setWindowTextValue(controls_.editHighCorrection, formatWideDouble(settings.highCorrectionHz, 0));

    if (workspace.filterResult.valid) {
        SetWindowTextW(controls_.summary,
                       (L"Designed " + std::to_wstring(workspace.filterResult.tapCount) +
                        L" taps, FFT " + std::to_wstring(workspace.filterResult.fftSize) +
                        L", bins " + std::to_wstring(workspace.filterResult.positiveBinCount) + L".")
                           .c_str());
    } else {
        SetWindowTextW(controls_.summary, L"No filter calculated yet. Recalculate after smoothing and target-curve edits.");
    }

    correctionGraph_.setData(buildCorrectionGraphData(workspace));
    correctedGraph_.setData(buildCorrectedResponseGraphData(workspace));
    groupDelayGraph_.setData(buildGroupDelayGraphData(workspace));
    impulseGraph_.setData(buildImpulseGraphData(workspace));
}

void FiltersPage::syncToWorkspace(WorkspaceState& workspace) const {
    workspace.filters.tapCount = tapCountFromComboIndex(static_cast<int>(SendMessageW(controls_.comboTapCount, CB_GETCURSEL, 0, 0)));
    double value = 0.0;
    if (tryParseDouble(getWindowTextValue(controls_.editMaxBoost), value)) {
        workspace.filters.maxBoostDb = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMaxCut), value)) {
        workspace.filters.maxCutDb = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editLowCorrection), value)) {
        workspace.filters.lowCorrectionHz = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editHighCorrection), value)) {
        workspace.filters.highCorrectionHz = value;
    }
    measurement::normalizeFilterDesignSettings(workspace.filters, workspace.measurement.sampleRate);
}

bool FiltersPage::handleCommand(WORD commandId, WORD notificationCode, WorkspaceState& workspace, bool& recalculateRequested) {
    if (commandId == kComboTapCount && notificationCode == CBN_SELCHANGE) {
        workspace.filters.tapCount =
            tapCountFromComboIndex(static_cast<int>(SendMessageW(controls_.comboTapCount, CB_GETCURSEL, 0, 0)));
        measurement::normalizeFilterDesignSettings(workspace.filters, workspace.measurement.sampleRate);
        recalculateRequested = true;
        return true;
    }

    if (commandId == kButtonRecalculate && notificationCode == BN_CLICKED) {
        syncToWorkspace(workspace);
        recalculateRequested = true;
        return true;
    }

    return false;
}

LRESULT CALLBACK FiltersPage::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* page = reinterpret_cast<FiltersPage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    static HBRUSH pageBackgroundBrush = CreateSolidBrush(ui_theme::kBackground);

    switch (message) {
    case WM_SIZE:
        if (page != nullptr) {
            page->layout();
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (page != nullptr && page->handleMouseWheel(wParam)) {
            return 0;
        }
        break;
    case WM_VSCROLL:
        if (page != nullptr) {
            page->handleVScroll(LOWORD(wParam), HIWORD(wParam));
            return 0;
        }
        break;
    case WM_COMMAND: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    }
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool FiltersPage::tryParseDouble(const std::wstring& text, double& value) {
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

std::wstring FiltersPage::getWindowTextValue(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}

void FiltersPage::setWindowTextValue(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

void FiltersPage::populateTapCountCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"16384"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"32768"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"65536"));
}

int FiltersPage::comboIndexFromTapCount(int tapCount) {
    switch (tapCount) {
    case 16384:
        return 0;
    case 32768:
        return 1;
    case 65536:
    default:
        return 2;
    }
}

int FiltersPage::tapCountFromComboIndex(int index) {
    switch (index) {
    case 0:
        return 16384;
    case 1:
        return 32768;
    case 2:
    default:
        return 65536;
    }
}

void FiltersPage::updateScrollBar() {
    if (window_ == nullptr) {
        return;
    }

    RECT rect{};
    GetClientRect(window_, &rect);
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_PAGE | SIF_RANGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(contentHeight_ - 1, 0);
    info.nPage = static_cast<UINT>(std::max(rect.bottom, 1L));
    info.nPos = scrollOffset_;
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
}

void FiltersPage::setScrollOffset(int scrollOffset) {
    RECT rect{};
    GetClientRect(window_, &rect);
    const int maxScrollOffset = std::max(contentHeight_ - rect.bottom, 0L);
    const int nextScrollOffset = clampValue(scrollOffset, 0, maxScrollOffset);
    if (nextScrollOffset == scrollOffset_) {
        updateScrollBar();
        return;
    }

    scrollOffset_ = nextScrollOffset;
    updateScrollBar();
    layout();
    InvalidateRect(window_, nullptr, TRUE);
}

bool FiltersPage::handleMouseWheel(WPARAM wParam) {
    const int wheelSteps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
    if (wheelSteps == 0) {
        return false;
    }
    setScrollOffset(scrollOffset_ - (wheelSteps * 60));
    return true;
}

void FiltersPage::handleVScroll(WORD code, WORD thumbPosition) {
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    GetScrollInfo(window_, SB_VERT, &info);

    int nextScrollOffset = scrollOffset_;
    switch (code) {
    case SB_LINEUP:
        nextScrollOffset -= 40;
        break;
    case SB_LINEDOWN:
        nextScrollOffset += 40;
        break;
    case SB_PAGEUP:
        nextScrollOffset -= static_cast<int>(info.nPage);
        break;
    case SB_PAGEDOWN:
        nextScrollOffset += static_cast<int>(info.nPage);
        break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK:
        nextScrollOffset = thumbPosition;
        break;
    case SB_TOP:
        nextScrollOffset = 0;
        break;
    case SB_BOTTOM:
        nextScrollOffset = info.nMax;
        break;
    default:
        return;
    }

    setScrollOffset(nextScrollOffset);
}

PlotGraphData FiltersPage::buildCorrectionGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xValues = workspace.filterResult.frequencyAxisHz;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"dB";
    if (!workspace.filterResult.valid) {
        return data;
    }

    data.series.push_back({L"Left correction", ui_theme::kGreen, workspace.filterResult.left.correctionCurveDb});
    data.series.push_back({L"Right correction", ui_theme::kRed, workspace.filterResult.right.correctionCurveDb});
    data.series.push_back({L"Left filter", ui_theme::kTeal, workspace.filterResult.left.filterResponseDb});
    data.series.push_back({L"Right filter", ui_theme::kOrange, workspace.filterResult.right.filterResponseDb});
    return data;
}

PlotGraphData FiltersPage::buildCorrectedResponseGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xValues = workspace.filterResult.frequencyAxisHz;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"dB";
    if (!workspace.filterResult.valid) {
        return data;
    }

    data.series.push_back({L"Target", ui_theme::kAccent, workspace.filterResult.targetCurveDb});
    data.series.push_back({L"Left", ui_theme::kGreen, workspace.filterResult.left.correctedResponseDb});
    data.series.push_back({L"Right", ui_theme::kRed, workspace.filterResult.right.correctedResponseDb});
    return data;
}

PlotGraphData FiltersPage::buildGroupDelayGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xValues = workspace.filterResult.frequencyAxisHz;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"ms";
    if (!workspace.filterResult.valid) {
        return data;
    }

    data.series.push_back({L"Left", ui_theme::kGreen, workspace.filterResult.left.groupDelayMs});
    data.series.push_back({L"Right", ui_theme::kRed, workspace.filterResult.right.groupDelayMs});
    return data;
}

PlotGraphData FiltersPage::buildImpulseGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xAxisMode = PlotGraphXAxisMode::Linear;
    data.xUnit = L"ms";
    data.yUnit = L"amp";
    if (!workspace.filterResult.valid ||
        workspace.filterResult.left.filterTaps.empty() ||
        workspace.filterResult.right.filterTaps.empty()) {
        return data;
    }

    const size_t leftPeak = static_cast<size_t>(std::max(workspace.filterResult.left.impulsePeakIndex, 0));
    const size_t rightPeak = static_cast<size_t>(std::max(workspace.filterResult.right.impulsePeakIndex, 0));
    const size_t preSamples = std::min<size_t>(1024, std::min(leftPeak, rightPeak));
    const size_t leftPost = workspace.filterResult.left.filterTaps.size() > leftPeak
                                ? workspace.filterResult.left.filterTaps.size() - leftPeak - 1
                                : 0;
    const size_t rightPost = workspace.filterResult.right.filterTaps.size() > rightPeak
                                 ? workspace.filterResult.right.filterTaps.size() - rightPeak - 1
                                 : 0;
    const size_t postSamples = std::min<size_t>(4096, std::min(leftPost, rightPost));
    const double sampleRate = static_cast<double>(std::max(workspace.filterResult.sampleRate, 1));

    std::vector<double> xValues;
    std::vector<double> leftValues;
    std::vector<double> rightValues;
    for (int offset = -static_cast<int>(preSamples); offset <= static_cast<int>(postSamples); ++offset) {
        const int leftIndex = static_cast<int>(leftPeak) + offset;
        const int rightIndex = static_cast<int>(rightPeak) + offset;
        xValues.push_back(static_cast<double>(offset) * 1000.0 / sampleRate);
        leftValues.push_back(leftIndex >= 0 && leftIndex < static_cast<int>(workspace.filterResult.left.filterTaps.size())
                                 ? workspace.filterResult.left.filterTaps[static_cast<size_t>(leftIndex)]
                                 : 0.0);
        rightValues.push_back(rightIndex >= 0 && rightIndex < static_cast<int>(workspace.filterResult.right.filterTaps.size())
                                  ? workspace.filterResult.right.filterTaps[static_cast<size_t>(rightIndex)]
                                  : 0.0);
    }

    data.xValues = std::move(xValues);
    data.series.push_back({L"Left", ui_theme::kGreen, std::move(leftValues)});
    data.series.push_back({L"Right", ui_theme::kRed, std::move(rightValues)});
    return data;
}

}  // namespace wolfie::ui
