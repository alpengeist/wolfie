#include "ui/filters_page.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

double interpolateLinear(double x, double x0, double y0, double x1, double y1) {
    if (std::abs(x1 - x0) < 1.0e-9) {
        return y0;
    }
    const double t = (x - x0) / (x1 - x0);
    return y0 + (t * (y1 - y0));
}

double interpolateLogFrequency(const std::vector<double>& sourceAxisHz,
                               const std::vector<double>& sourceValues,
                               double frequencyHz) {
    if (sourceAxisHz.empty() || sourceValues.size() != sourceAxisHz.size()) {
        return 0.0;
    }

    if (frequencyHz <= sourceAxisHz.front()) {
        return sourceValues.front();
    }
    if (frequencyHz >= sourceAxisHz.back()) {
        return sourceValues.back();
    }

    const auto upper = std::lower_bound(sourceAxisHz.begin(), sourceAxisHz.end(), frequencyHz);
    if (upper == sourceAxisHz.begin()) {
        return sourceValues.front();
    }
    if (upper == sourceAxisHz.end()) {
        return sourceValues.back();
    }

    const size_t upperIndex = static_cast<size_t>(upper - sourceAxisHz.begin());
    const size_t lowerIndex = upperIndex - 1;
    const double x = std::log10(std::max(frequencyHz, 1.0));
    const double x0 = std::log10(std::max(sourceAxisHz[lowerIndex], 1.0));
    const double x1 = std::log10(std::max(sourceAxisHz[upperIndex], 1.0));
    return interpolateLinear(x, x0, sourceValues[lowerIndex], x1, sourceValues[upperIndex]);
}

std::vector<double> resampleLogFrequency(const std::vector<double>& sourceAxisHz,
                                         const std::vector<double>& sourceValues,
                                         const std::vector<double>& targetAxisHz) {
    std::vector<double> resampled;
    if (sourceAxisHz.empty() || sourceValues.size() != sourceAxisHz.size() || targetAxisHz.empty()) {
        return resampled;
    }

    resampled.reserve(targetAxisHz.size());
    for (const double frequencyHz : targetAxisHz) {
        resampled.push_back(interpolateLogFrequency(sourceAxisHz, sourceValues, frequencyHz));
    }
    return resampled;
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
    controls_.labelSmoothness = CreateWindowW(L"STATIC", L"Smoothness", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editSmoothness = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditSmoothness), instance_, nullptr);
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
    controls_.inversionTitle = CreateWindowW(L"STATIC", L"Inversion", WS_CHILD | WS_VISIBLE,
                                             0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.inversionLegendFrame = CreateWindowW(L"BUTTON",
                                                   L"",
                                                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   window_,
                                                   nullptr,
                                                   instance_,
                                                   nullptr);
    controls_.checkboxShowInputRight = CreateWindowW(L"BUTTON",
                                                     L"",
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     window_,
                                                     reinterpret_cast<HMENU>(kCheckboxShowInputRight),
                                                     instance_,
                                                     nullptr);
    controls_.lineInputRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInputRight = CreateWindowW(L"STATIC", L"R", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowInputLeft = CreateWindowW(L"BUTTON",
                                                    L"",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    window_,
                                                    reinterpret_cast<HMENU>(kCheckboxShowInputLeft),
                                                    instance_,
                                                    nullptr);
    controls_.lineInputLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInputLeft = CreateWindowW(L"STATIC", L"L", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowInversionRight = CreateWindowW(L"BUTTON",
                                                         L"",
                                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                         0,
                                                         0,
                                                         0,
                                                         0,
                                                         window_,
                                                         reinterpret_cast<HMENU>(kCheckboxShowInversionRight),
                                                         instance_,
                                                         nullptr);
    controls_.lineInversionRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInversionRight = CreateWindowW(L"STATIC", L"R inv", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowInversionLeft = CreateWindowW(L"BUTTON",
                                                        L"",
                                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                        0,
                                                        0,
                                                        0,
                                                        0,
                                                        window_,
                                                        reinterpret_cast<HMENU>(kCheckboxShowInversionLeft),
                                                        instance_,
                                                        nullptr);
    controls_.lineInversionLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInversionLeft = CreateWindowW(L"STATIC", L"L inv", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.correctedTitle = CreateWindowW(L"STATIC", L"Predicted Corrected Response", WS_CHILD | WS_VISIBLE,
                                             0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.groupDelayTitle = CreateWindowW(L"STATIC", L"Filter Group Delay", WS_CHILD | WS_VISIBLE,
                                              0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.impulseTitle = CreateWindowW(L"STATIC", L"Filter Impulse", WS_CHILD | WS_VISIBLE,
                                           0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    populateTapCountCombo(controls_.comboTapCount);
    SendMessageW(controls_.checkboxShowInputRight, BM_SETCHECK, showInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInputLeft, BM_SETCHECK, showInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionRight, BM_SETCHECK, showInversionRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionLeft, BM_SETCHECK, showInversionLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
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
    const int graphHeight = 320;
    const int graphGap = 34;
    const int sectionGap = 26;
    const int legendGap = 14;
    const int legendWidth = 128;
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
    MoveWindow(controls_.labelSmoothness, contentLeft + 496, top, 72, 18, TRUE);
    MoveWindow(controls_.editSmoothness, contentLeft + 496, top + 22, 58, 26, TRUE);
    MoveWindow(controls_.labelLowCorrection, contentLeft + 574, top, 72, 18, TRUE);
    MoveWindow(controls_.editLowCorrection, contentLeft + 574, top + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitLowCorrection, contentLeft + 646, top + 26, 22, 18, TRUE);
    MoveWindow(controls_.labelHighCorrection, contentLeft + 684, top, 76, 18, TRUE);
    MoveWindow(controls_.editHighCorrection, contentLeft + 684, top + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitHighCorrection, contentLeft + 756, top + 26, 22, 18, TRUE);
    MoveWindow(controls_.buttonRecalculate, contentLeft + contentWidth - 110, top + 18, 110, 30, TRUE);
    MoveWindow(controls_.summary, contentLeft, top + 62, contentWidth, 18, TRUE);

    int y = top + 96;
    MoveWindow(controls_.inversionTitle, contentLeft, y, 120, 18, TRUE);
    const int legendLeft = contentLeft + contentWidth - legendWidth;
    const int graphRight = legendLeft - legendGap;
    const int frameTop = y + 24;
    MoveWindow(controls_.inversionLegendFrame, legendLeft, frameTop, legendWidth, graphHeight, TRUE);
    correctionGraph_.layout(RECT{contentLeft, y + 24, graphRight, y + 24 + graphHeight});

    const int checkboxLeft = legendLeft + 14;
    const int checkboxWidth = 18;
    const int rowStep = 30;
    const int firstRowTop = frameTop + 18;
    const int lineLeft = checkboxLeft + checkboxWidth + 8;
    const int lineWidth = 24;
    const int lineHeight = 3;
    const int labelLeft = lineLeft + lineWidth + 8;
    const int labelWidth = legendWidth - (labelLeft - legendLeft) - 12;
    MoveWindow(controls_.checkboxShowInputRight, checkboxLeft, firstRowTop, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInputRight, lineLeft, firstRowTop + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInputRight, labelLeft, firstRowTop + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInputLeft, checkboxLeft, firstRowTop + rowStep, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInputLeft, lineLeft, firstRowTop + rowStep + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInputLeft, labelLeft, firstRowTop + rowStep + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInversionRight, checkboxLeft, firstRowTop + (rowStep * 2), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInversionRight, lineLeft, firstRowTop + (rowStep * 2) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInversionRight, labelLeft, firstRowTop + (rowStep * 2) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInversionLeft, checkboxLeft, firstRowTop + (rowStep * 3), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInversionLeft, lineLeft, firstRowTop + (rowStep * 3) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInversionLeft, labelLeft, firstRowTop + (rowStep * 3) + 2, labelWidth, 18, TRUE);

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
    setWindowTextValue(controls_.editSmoothness, formatWideDouble(settings.smoothness, 2));
    setWindowTextValue(controls_.editLowCorrection, formatWideDouble(settings.lowCorrectionHz, 0));
    setWindowTextValue(controls_.editHighCorrection, formatWideDouble(settings.highCorrectionHz, 0));
    SendMessageW(controls_.checkboxShowInputRight, BM_SETCHECK, showInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInputLeft, BM_SETCHECK, showInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionRight, BM_SETCHECK, showInversionRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionLeft, BM_SETCHECK, showInversionLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);

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
    if (tryParseDouble(getWindowTextValue(controls_.editSmoothness), value)) {
        workspace.filters.smoothness = value;
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

    if ((commandId == kCheckboxShowInputRight ||
         commandId == kCheckboxShowInputLeft ||
         commandId == kCheckboxShowInversionRight ||
         commandId == kCheckboxShowInversionLeft) &&
        notificationCode == BN_CLICKED) {
        showInputRight_ = SendMessageW(controls_.checkboxShowInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showInputLeft_ = SendMessageW(controls_.checkboxShowInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showInversionRight_ = SendMessageW(controls_.checkboxShowInversionRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showInversionLeft_ = SendMessageW(controls_.checkboxShowInversionLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        correctionGraph_.setData(buildCorrectionGraphData(workspace));
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
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(hdc, &rect, pageBackgroundBrush);
        return 1;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        static HBRUSH lineInputRightBrush = CreateSolidBrush(ui_theme::kRed);
        static HBRUSH lineInputLeftBrush = CreateSolidBrush(ui_theme::kGreen);
        static HBRUSH lineInversionRightBrush = CreateSolidBrush(ui_theme::kBlue);
        static HBRUSH lineInversionLeftBrush = CreateSolidBrush(ui_theme::kGray);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        if (page != nullptr) {
            if (control == page->controls_.lineInputRight) {
                SetBkColor(hdc, ui_theme::kRed);
                return reinterpret_cast<INT_PTR>(lineInputRightBrush);
            }
            if (control == page->controls_.lineInputLeft) {
                SetBkColor(hdc, ui_theme::kGreen);
                return reinterpret_cast<INT_PTR>(lineInputLeftBrush);
            }
            if (control == page->controls_.lineInversionRight) {
                SetBkColor(hdc, ui_theme::kBlue);
                return reinterpret_cast<INT_PTR>(lineInversionRightBrush);
            }
            if (control == page->controls_.lineInversionLeft) {
                SetBkColor(hdc, ui_theme::kGray);
                return reinterpret_cast<INT_PTR>(lineInversionLeftBrush);
            }
        }
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

    const int deltaY = scrollOffset_ - nextScrollOffset;
    scrollOffset_ = nextScrollOffset;
    updateScrollBar();
    ScrollWindowEx(window_,
                   0,
                   deltaY,
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr,
                   SW_ERASE | SW_INVALIDATE | SW_SCROLLCHILDREN);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
        data.fixedYRange = true;
        data.minY = -workspace.filters.maxCutDb - 3.0;
        data.maxY = workspace.filters.maxBoostDb + 3.0;
        return data;
    }

    double minY = -workspace.filters.maxCutDb - 3.0;
    double maxY = workspace.filters.maxBoostDb + 3.0;
    const auto accumulateRange = [&](const std::vector<double>& values) {
        for (const double value : values) {
            minY = std::min(minY, value);
            maxY = std::max(maxY, value);
        }
    };

    if (showInputRight_) {
        accumulateRange(workspace.smoothedResponse.rightChannelDb);
    }
    if (showInputLeft_) {
        accumulateRange(workspace.smoothedResponse.leftChannelDb);
    }
    if (showInversionRight_) {
        accumulateRange(workspace.filterResult.right.correctionCurveDb);
    }
    if (showInversionLeft_) {
        accumulateRange(workspace.filterResult.left.correctionCurveDb);
    }

    data.fixedYRange = true;
    data.minY = std::floor((minY - 1.5) / 3.0) * 3.0;
    data.maxY = std::ceil((maxY + 1.5) / 3.0) * 3.0;
    if (showInputRight_) {
        data.series.push_back({L"Right input",
                               ui_theme::kRed,
                               resampleLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                                                    workspace.smoothedResponse.rightChannelDb,
                                                    workspace.filterResult.frequencyAxisHz)});
    }
    if (showInputLeft_) {
        data.series.push_back({L"Left input",
                               ui_theme::kGreen,
                               resampleLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                                                    workspace.smoothedResponse.leftChannelDb,
                                                    workspace.filterResult.frequencyAxisHz)});
    }
    if (showInversionRight_) {
        data.series.push_back({L"Right inversion", ui_theme::kBlue, workspace.filterResult.right.correctionCurveDb});
    }
    if (showInversionLeft_) {
        data.series.push_back({L"Left inversion", ui_theme::kGray, workspace.filterResult.left.correctionCurveDb});
    }
    return data;
}

PlotGraphData FiltersPage::buildCorrectedResponseGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xValues = workspace.filterResult.frequencyAxisHz;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"dB";
    data.fixedYRange = true;
    if (!workspace.filterResult.valid) {
        data.minY = -18.0;
        data.maxY = 12.0;
        return data;
    }

    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    const auto accumulateRange = [&](const std::vector<double>& values) {
        for (const double value : values) {
            minY = std::min(minY, value);
            maxY = std::max(maxY, value);
        }
    };
    accumulateRange(workspace.filterResult.targetCurveDb);
    accumulateRange(workspace.filterResult.left.correctedResponseDb);
    accumulateRange(workspace.filterResult.right.correctedResponseDb);
    if (!std::isfinite(minY) || !std::isfinite(maxY)) {
        minY = -18.0;
        maxY = 12.0;
    }
    const double paddedMin = std::floor((minY - 2.0) / 3.0) * 3.0;
    const double paddedMax = std::ceil((maxY + 2.0) / 3.0) * 3.0;
    const double minimumSpan = 18.0;
    const double center = (paddedMin + paddedMax) * 0.5;
    const double halfSpan = std::max((paddedMax - paddedMin) * 0.5, minimumSpan * 0.5);
    data.minY = center - halfSpan;
    data.maxY = center + halfSpan;

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
