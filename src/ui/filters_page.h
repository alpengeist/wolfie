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

class FiltersPage {
public:
    static void registerPageWindowClass(HINSTANCE instance);
    static const wchar_t* pageWindowClassName();

    void create(HWND parent, HINSTANCE instance);
    void layout();
    void setVisible(bool visible) const;
    void populate(const WorkspaceState& workspace);
    void syncToWorkspace(WorkspaceState& workspace) const;
    bool handleCommand(WORD commandId, WORD notificationCode, WorkspaceState& workspace, bool& recalculateRequested);

    [[nodiscard]] HWND window() const { return window_; }

private:
    struct Controls {
        HWND labelTapCount = nullptr;
        HWND comboTapCount = nullptr;
        HWND labelPhaseMode = nullptr;
        HWND phaseModeValue = nullptr;
        HWND labelMaxBoost = nullptr;
        HWND editMaxBoost = nullptr;
        HWND unitMaxBoost = nullptr;
        HWND labelMaxCut = nullptr;
        HWND editMaxCut = nullptr;
        HWND unitMaxCut = nullptr;
        HWND labelLowCorrection = nullptr;
        HWND editLowCorrection = nullptr;
        HWND unitLowCorrection = nullptr;
        HWND labelHighCorrection = nullptr;
        HWND editHighCorrection = nullptr;
        HWND unitHighCorrection = nullptr;
        HWND buttonRecalculate = nullptr;
        HWND summary = nullptr;
        HWND inversionTitle = nullptr;
        HWND checkboxShowMeasuredLeft = nullptr;
        HWND checkboxShowMeasuredRight = nullptr;
        HWND correctedTitle = nullptr;
        HWND groupDelayTitle = nullptr;
        HWND impulseTitle = nullptr;
    };

    static constexpr wchar_t kPageClassName[] = L"WolfieFiltersPage";
    static constexpr int kComboTapCount = 3401;
    static constexpr int kEditMaxBoost = 3402;
    static constexpr int kEditMaxCut = 3403;
    static constexpr int kEditLowCorrection = 3404;
    static constexpr int kEditHighCorrection = 3405;
    static constexpr int kButtonRecalculate = 3406;
    static constexpr int kCheckboxShowMeasuredLeft = 3407;
    static constexpr int kCheckboxShowMeasuredRight = 3408;

    static LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static bool tryParseDouble(const std::wstring& text, double& value);
    static std::wstring getWindowTextValue(HWND control);
    static void setWindowTextValue(HWND control, const std::wstring& text);
    static void populateTapCountCombo(HWND combo);
    static int comboIndexFromTapCount(int tapCount);
    static int tapCountFromComboIndex(int index);

    void createControls();
    void updateScrollBar();
    void setScrollOffset(int scrollOffset);
    bool handleMouseWheel(WPARAM wParam);
    void handleVScroll(WORD code, WORD thumbPosition);
    PlotGraphData buildCorrectionGraphData(const WorkspaceState& workspace) const;
    PlotGraphData buildCorrectedResponseGraphData(const WorkspaceState& workspace) const;
    PlotGraphData buildGroupDelayGraphData(const WorkspaceState& workspace) const;
    PlotGraphData buildImpulseGraphData(const WorkspaceState& workspace) const;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    PlotGraph correctionGraph_;
    PlotGraph correctedGraph_;
    PlotGraph groupDelayGraph_;
    PlotGraph impulseGraph_;
    bool showMeasuredLeft_ = true;
    bool showMeasuredRight_ = true;
    int scrollOffset_ = 0;
    int contentHeight_ = 0;
};

}  // namespace wolfie::ui
