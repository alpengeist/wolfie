#include "ui/analysis_page.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <commctrl.h>

#include "core/text_utils.h"
#include "measurement/stereo_diagnostics.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

const StereoDiagnosticsResult& selectFilterDiagnostics(const FilterAnalysisResult& filterAnalysis,
                                                       std::string_view window) {
    return window == "room" ? filterAnalysis.room : filterAnalysis.direct;
}

}  // namespace

void AnalysisPage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = ui_theme::backgroundBrush();
    RegisterClassW(&pageClass);
}

const wchar_t* AnalysisPage::pageWindowClassName() {
    return kPageClassName;
}

void AnalysisPage::create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    window_ = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr, instance, this);
    createControls();
}

void AnalysisPage::createControls() {
    controls_.labelWindow = CreateWindowW(L"STATIC", L"Window", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboWindow = CreateWindowW(L"COMBOBOX",
                                          nullptr,
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                          0,
                                          0,
                                          0,
                                          0,
                                          window_,
                                          reinterpret_cast<HMENU>(kComboWindow),
                                          instance_,
                                          nullptr);
    controls_.buttonRefresh = CreateWindowW(L"BUTTON",
                                            L"Refresh Filter Analysis",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                            0,
                                            0,
                                            0,
                                            0,
                                            window_,
                                            reinterpret_cast<HMENU>(kButtonRefresh),
                                            instance_,
                                            nullptr);
    controls_.progressRefresh = CreateWindowExW(0,
                                                PROGRESS_CLASSW,
                                                nullptr,
                                                WS_CHILD | PBS_MARQUEE,
                                                0,
                                                0,
                                                0,
                                                0,
                                                window_,
                                                nullptr,
                                                instance_,
                                                nullptr);
    controls_.note = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    constexpr const wchar_t* kMetricLabels[10] = {
        L"Direct L-R Delay",
        L"Direct Impulse Corr",
        L"Phase RMS 40-120",
        L"Phase RMS 120-300",
        L"Mag RMS 40-300",
        L"Phase Similarity",
        L"IACC10",
        L"IACC20",
        L"IACC80",
        L"IACC Late",
    };
    for (int index = 0; index < 10; ++index) {
        controls_.metrics[index].label =
            CreateWindowW(L"STATIC", kMetricLabels[index], WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        controls_.metrics[index].beforeValue =
            CreateWindowW(L"STATIC", L"Before: --", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        controls_.metrics[index].afterValue =
            CreateWindowW(L"STATIC", L"After: --", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    }

    controls_.phaseTitle = CreateWindowW(L"STATIC", L"L-R Phase Delta", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.magnitudeTitle =
        CreateWindowW(L"STATIC", L"L-R Magnitude Delta", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    populateWindowCombo(controls_.comboWindow);

    phaseGraph_.create(window_, instance_, kPhaseGraph);
    magnitudeGraph_.create(window_, instance_, kMagnitudeGraph);
    phaseGraph_.setDefaultXRange(true, 20.0, 20000.0);
    magnitudeGraph_.setDefaultXRange(true, 20.0, 20000.0);
}

void AnalysisPage::layout() {
    RECT pageRect{};
    GetClientRect(window_, &pageRect);
    const int contentLeft = 20;
    const int contentTop = 20;
    const int innerWidth = std::max(520L, pageRect.right - (contentLeft * 2));
    const int innerHeight = std::max(420L, pageRect.bottom - (contentTop * 2));
    const int comboWidth = 180;
    const int labelWidth = 70;
    const int refreshButtonWidth = 170;
    const int topRowTop = contentTop;

    MoveWindow(controls_.labelWindow, contentLeft, topRowTop + 4, labelWidth, 18, TRUE);
    MoveWindow(controls_.comboWindow, contentLeft + labelWidth + 8, topRowTop, comboWidth, 220, TRUE);
    MoveWindow(controls_.buttonRefresh,
               contentLeft + labelWidth + 8 + comboWidth + 12,
               topRowTop,
               refreshButtonWidth,
               24,
               TRUE);
    MoveWindow(controls_.progressRefresh,
               contentLeft + labelWidth + 8 + comboWidth + 12,
               topRowTop + 2,
               refreshButtonWidth,
               20,
               TRUE);
    MoveWindow(controls_.note, contentLeft, topRowTop + 34, innerWidth, 18, TRUE);

    const int summaryTop = topRowTop + 64;
    const int summaryGapX = 16;
    const int summaryGapY = 12;
    const int summaryColumns = 5;
    const int summaryRows = 2;
    const int metricWidth = (innerWidth - ((summaryColumns - 1) * summaryGapX)) / summaryColumns;
    const int metricHeight = 72;
    for (int index = 0; index < 10; ++index) {
        const int row = index / summaryColumns;
        const int column = index % summaryColumns;
        const int left = contentLeft + (column * (metricWidth + summaryGapX));
        const int top = summaryTop + (row * (metricHeight + summaryGapY));
        MoveWindow(controls_.metrics[index].label, left, top, metricWidth, 18, TRUE);
        MoveWindow(controls_.metrics[index].beforeValue, left, top + 22, metricWidth, 18, TRUE);
        MoveWindow(controls_.metrics[index].afterValue, left, top + 42, metricWidth, 18, TRUE);
    }

    const int titlesTop = summaryTop + (summaryRows * metricHeight) + ((summaryRows - 1) * summaryGapY) + 18;
    const int graphGap = 24;
    const int graphHeight = std::max(160, (innerHeight - (titlesTop - contentTop) - 42 - graphGap) / 2);
    MoveWindow(controls_.phaseTitle, contentLeft, titlesTop, innerWidth, 18, TRUE);
    phaseGraph_.layout(RECT{contentLeft, titlesTop + 24, contentLeft + innerWidth, titlesTop + 24 + graphHeight});

    const int magnitudeTop = titlesTop + 24 + graphHeight + graphGap;
    MoveWindow(controls_.magnitudeTitle, contentLeft, magnitudeTop, innerWidth, 18, TRUE);
    magnitudeGraph_.layout(
        RECT{contentLeft, magnitudeTop + 24, contentLeft + innerWidth, magnitudeTop + 24 + graphHeight});
}

void AnalysisPage::setVisible(bool visible) const {
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
}

void AnalysisPage::populate(const WorkspaceState& workspace) {
    updatingControls_ = true;
    SendMessageW(controls_.comboWindow, CB_SETCURSEL, comboIndexFromWindow(workspace.ui.analysisWindow), 0);
    updatingControls_ = false;

    phaseGraph_.setDefaultXRange(true, 20.0, 20000.0);
    magnitudeGraph_.setDefaultXRange(true, 20.0, 20000.0);
    if (workspace.ui.analysisGraphHasCustomFrequencyRange) {
        phaseGraph_.setVisibleXRange(workspace.ui.analysisGraphVisibleMinFrequencyHz,
                                     workspace.ui.analysisGraphVisibleMaxFrequencyHz);
        magnitudeGraph_.setVisibleXRange(workspace.ui.analysisGraphVisibleMinFrequencyHz,
                                         workspace.ui.analysisGraphVisibleMaxFrequencyHz);
    } else {
        phaseGraph_.resetXRange();
        magnitudeGraph_.resetXRange();
    }

    refreshDiagnostics(workspace);
}

void AnalysisPage::setCalculationInProgress(bool running, const std::wstring& statusText) {
    calculationInProgress_ = running;
    calculationStatusText_ = running ? statusText : std::wstring();
    refreshActionControls();
    refreshNote();
}

void AnalysisPage::syncToWorkspace(WorkspaceState& workspace) const {
    workspace.ui.analysisWindow =
        windowFromComboIndex(static_cast<int>(SendMessageW(controls_.comboWindow, CB_GETCURSEL, 0, 0)));
    workspace.ui.analysisGraphHasCustomFrequencyRange = phaseGraph_.hasCustomXRange();
    workspace.ui.analysisGraphVisibleMinFrequencyHz = phaseGraph_.visibleMinX();
    workspace.ui.analysisGraphVisibleMaxFrequencyHz = phaseGraph_.visibleMaxX();
}

bool AnalysisPage::handleCommand(WORD commandId,
                                 WORD notificationCode,
                                 WorkspaceState& workspace,
                                 bool& viewSettingsChanged,
                                 bool& refreshRequested) {
    if (commandId == kComboWindow && notificationCode == CBN_SELCHANGE) {
        if (updatingControls_) {
            return true;
        }
        syncToWorkspace(workspace);
        refreshDiagnostics(workspace);
        viewSettingsChanged = true;
        return true;
    }

    if (commandId == kButtonRefresh && notificationCode == BN_CLICKED) {
        refreshRequested = true;
        return true;
    }

    if (notificationCode == PlotGraph::kXRangeChangedNotification) {
        PlotGraph* sourceGraph = graphForCommandId(commandId);
        if (sourceGraph == nullptr) {
            return false;
        }
        applySharedXRange(*sourceGraph);
        syncToWorkspace(workspace);
        viewSettingsChanged = true;
        return true;
    }

    return false;
}

void AnalysisPage::refreshDiagnostics(const WorkspaceState& workspace) {
    const std::string window =
        windowFromComboIndex(static_cast<int>(SendMessageW(controls_.comboWindow, CB_GETCURSEL, 0, 0)));
    beforeDiagnostics_ = measurement::buildStereoDiagnostics(workspace.result, window);
    afterDiagnostics_ = selectFilterDiagnostics(workspace.filterAnalysis, window);
    hasMeasurementData_ = workspace.result.hasAnyValues();

    if (!hasMeasurementData_) {
        noteText_ = L"No measurement data available.";
    } else if (!afterDiagnostics_.available) {
        noteText_ = L"Post-filter analysis is not available for this workspace. Refresh Filter Analysis to build it.";
    } else {
        noteText_.clear();
    }
    refreshActionControls();
    refreshNote();

    refreshSummary();
    refreshGraphs();
}

void AnalysisPage::refreshActionControls() {
    if (controls_.comboWindow != nullptr) {
        EnableWindow(controls_.comboWindow, calculationInProgress_ ? FALSE : TRUE);
    }
    if (controls_.buttonRefresh != nullptr) {
        const bool showButton = !calculationInProgress_;
        ShowWindow(controls_.buttonRefresh, showButton ? SW_SHOW : SW_HIDE);
        EnableWindow(controls_.buttonRefresh, (hasMeasurementData_ && showButton) ? TRUE : FALSE);
    }
    if (controls_.progressRefresh != nullptr) {
        ShowWindow(controls_.progressRefresh, calculationInProgress_ ? SW_SHOW : SW_HIDE);
        SendMessageW(controls_.progressRefresh, PBM_SETMARQUEE, calculationInProgress_ ? TRUE : FALSE, 0);
        RedrawWindow(controls_.progressRefresh, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

void AnalysisPage::refreshNote() {
    const std::wstring displayedNote = calculationInProgress_ ? calculationStatusText_ : noteText_;
    if (controls_.note != nullptr) {
        SetWindowTextW(controls_.note, displayedNote.c_str());
        RedrawWindow(controls_.note, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

void AnalysisPage::refreshSummary() {
    auto metricLine = [&](const wchar_t* prefix,
                          const StereoDiagnosticsResult& diagnostics,
                          double StereoDiagnosticsSummary::* member,
                          const wchar_t* unit,
                          int decimals,
                          bool signedValue = false) {
        const std::wstring value = diagnostics.summary.available
                                       ? formatMetricValue(diagnostics.summary.*member, unit, decimals, signedValue)
                                       : L"--";
        return std::wstring(prefix) + value;
    };

    SetWindowTextW(controls_.metrics[0].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::delayMismatchMs, L" ms", 3, true).c_str());
    SetWindowTextW(controls_.metrics[0].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::delayMismatchMs, L" ms", 3, true).c_str());
    SetWindowTextW(controls_.metrics[1].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::directImpulseCorrelation, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[1].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::directImpulseCorrelation, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[2].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::lowBandPhaseRmsDegrees, L" deg", 1).c_str());
    SetWindowTextW(controls_.metrics[2].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::lowBandPhaseRmsDegrees, L" deg", 1).c_str());
    SetWindowTextW(controls_.metrics[3].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::midBandPhaseRmsDegrees, L" deg", 1).c_str());
    SetWindowTextW(controls_.metrics[3].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::midBandPhaseRmsDegrees, L" deg", 1).c_str());
    SetWindowTextW(controls_.metrics[4].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::lowBandMagnitudeRmsDb, L" dB", 2).c_str());
    SetWindowTextW(controls_.metrics[4].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::lowBandMagnitudeRmsDb, L" dB", 2).c_str());
    SetWindowTextW(controls_.metrics[5].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::phaseSimilarity, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[5].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::phaseSimilarity, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[6].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::iacc10, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[6].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::iacc10, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[7].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::iacc20, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[7].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::iacc20, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[8].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::iacc80, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[8].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::iacc80, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[9].beforeValue,
                   metricLine(L"Before: ", beforeDiagnostics_, &StereoDiagnosticsSummary::iaccLate, L"", 3).c_str());
    SetWindowTextW(controls_.metrics[9].afterValue,
                   metricLine(L"After: ", afterDiagnostics_, &StereoDiagnosticsSummary::iaccLate, L"", 3).c_str());
}

void AnalysisPage::refreshGraphs() {
    phaseGraph_.setData(buildPhaseGraphData());
    magnitudeGraph_.setData(buildMagnitudeGraphData());
}

void AnalysisPage::applySharedXRange(const PlotGraph& sourceGraph) {
    if (sourceGraph.hasCustomXRange()) {
        const double minX = sourceGraph.visibleMinX();
        const double maxX = sourceGraph.visibleMaxX();
        if (&sourceGraph != &phaseGraph_) {
            phaseGraph_.setVisibleXRange(minX, maxX);
        }
        if (&sourceGraph != &magnitudeGraph_) {
            magnitudeGraph_.setVisibleXRange(minX, maxX);
        }
        return;
    }

    if (&sourceGraph != &phaseGraph_) {
        phaseGraph_.resetXRange();
    }
    if (&sourceGraph != &magnitudeGraph_) {
        magnitudeGraph_.resetXRange();
    }
}

PlotGraph* AnalysisPage::graphForCommandId(WORD commandId) {
    if (commandId == kPhaseGraph) {
        return &phaseGraph_;
    }
    if (commandId == kMagnitudeGraph) {
        return &magnitudeGraph_;
    }
    return nullptr;
}

PlotGraphData AnalysisPage::buildPhaseGraphData() const {
    PlotGraphData data;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.yAxisMode = PlotGraphYAxisMode::SymmetricAroundZero;
    data.xUnit = L"Hz";
    data.yUnit = L"deg";
    if (!beforeDiagnostics_.phaseDeltaDegrees.empty()) {
        data.xValues = beforeDiagnostics_.frequencyAxisHz;
        data.series.push_back({L"Before L-R phase", ui_theme::kAccent, beforeDiagnostics_.phaseDeltaDegrees});
    }
    if (!afterDiagnostics_.phaseDeltaDegrees.empty()) {
        if (data.xValues.empty()) {
            data.xValues = afterDiagnostics_.frequencyAxisHz;
        }
        data.series.push_back({L"After L-R phase", ui_theme::kTeal, afterDiagnostics_.phaseDeltaDegrees});
    }
    return data;
}

PlotGraphData AnalysisPage::buildMagnitudeGraphData() const {
    PlotGraphData data;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.yAxisMode = PlotGraphYAxisMode::SymmetricAroundZero;
    data.xUnit = L"Hz";
    data.yUnit = L"dB";
    if (!beforeDiagnostics_.magnitudeDeltaDb.empty()) {
        data.xValues = beforeDiagnostics_.frequencyAxisHz;
        data.series.push_back({L"Before L-R magnitude", ui_theme::kAccent, beforeDiagnostics_.magnitudeDeltaDb});
    }
    if (!afterDiagnostics_.magnitudeDeltaDb.empty()) {
        if (data.xValues.empty()) {
            data.xValues = afterDiagnostics_.frequencyAxisHz;
        }
        data.series.push_back({L"After L-R magnitude", ui_theme::kTeal, afterDiagnostics_.magnitudeDeltaDb});
    }
    return data;
}

LRESULT CALLBACK AnalysisPage::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    auto* page = reinterpret_cast<AnalysisPage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (page == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_COMMAND: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void AnalysisPage::populateWindowCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Direct"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Room"));
}

int AnalysisPage::comboIndexFromWindow(const std::string& window) {
    return window == "room" ? 1 : 0;
}

std::string AnalysisPage::windowFromComboIndex(int index) {
    return index == 1 ? "room" : "direct";
}

std::wstring AnalysisPage::formatMetricValue(double value,
                                             const wchar_t* unit,
                                             int decimals,
                                             bool signedValue) {
    if (!std::isfinite(value)) {
        return L"--";
    }

    const std::wstring numeric = signedValue && value > 0.0
                                     ? (L"+" + formatWideDouble(value, decimals))
                                     : formatWideDouble(value, decimals);
    return numeric + unit;
}

}  // namespace wolfie::ui
