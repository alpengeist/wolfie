#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/models.h"
#include "ui/plot_graph.h"

namespace wolfie::ui {

class AnalysisPage {
public:
    static void registerPageWindowClass(HINSTANCE instance);
    static const wchar_t* pageWindowClassName();

    void create(HWND parent, HINSTANCE instance);
    void layout();
    void setVisible(bool visible) const;
    void populate(const WorkspaceState& workspace);
    void setCalculationInProgress(bool running, const std::wstring& statusText = L"");
    void syncToWorkspace(WorkspaceState& workspace) const;
    bool handleCommand(WORD commandId,
                       WORD notificationCode,
                       WorkspaceState& workspace,
                       bool& viewSettingsChanged,
                       bool& refreshRequested);

    [[nodiscard]] HWND window() const { return window_; }

private:
    struct MetricControls {
        HWND label = nullptr;
        HWND beforeValue = nullptr;
        HWND afterValue = nullptr;
    };

    struct Controls {
        HWND labelWindow = nullptr;
        HWND comboWindow = nullptr;
        HWND buttonRefresh = nullptr;
        HWND progressRefresh = nullptr;
        HWND note = nullptr;
        MetricControls metrics[10];
        HWND phaseTitle = nullptr;
        HWND magnitudeTitle = nullptr;
    };

    static constexpr wchar_t kPageClassName[] = L"WolfieAnalysisPage";
    static constexpr int kComboWindow = 3501;
    static constexpr int kButtonRefresh = 3502;
    static constexpr int kPhaseGraph = 3503;
    static constexpr int kMagnitudeGraph = 3504;

    static LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static void populateWindowCombo(HWND combo);
    static int comboIndexFromWindow(const std::string& window);
    static std::string windowFromComboIndex(int index);
    static std::wstring formatMetricValue(double value, const wchar_t* unit, int decimals, bool signedValue = false);

    void createControls();
    void refreshDiagnostics(const WorkspaceState& workspace);
    void refreshActionControls();
    void refreshNote();
    void refreshSummary();
    void refreshGraphs();
    void applySharedXRange(const PlotGraph& sourceGraph);
    PlotGraph* graphForCommandId(WORD commandId);
    PlotGraphData buildPhaseGraphData() const;
    PlotGraphData buildMagnitudeGraphData() const;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    PlotGraph phaseGraph_;
    PlotGraph magnitudeGraph_;
    StereoDiagnosticsResult beforeDiagnostics_;
    StereoDiagnosticsResult afterDiagnostics_;
    std::wstring noteText_;
    std::wstring calculationStatusText_;
    bool hasMeasurementData_ = false;
    bool calculationInProgress_ = false;
    bool updatingControls_ = false;
};

}  // namespace wolfie::ui
