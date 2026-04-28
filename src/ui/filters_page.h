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
    bool handleCommand(WORD commandId,
                       WORD notificationCode,
                       WorkspaceState& workspace,
                       bool& settingsChanged,
                       bool& recalculateRequested);

    [[nodiscard]] HWND window() const { return window_; }

private:
    struct Controls {
        HWND labelTapCount = nullptr;
        HWND comboTapCount = nullptr;
        HWND labelPhaseMode = nullptr;
        HWND comboPhaseMode = nullptr;
        HWND labelLowCorrection = nullptr;
        HWND editLowCorrection = nullptr;
        HWND unitLowCorrection = nullptr;
        HWND labelHighCorrection = nullptr;
        HWND editHighCorrection = nullptr;
        HWND unitHighCorrection = nullptr;
        HWND labelMaxBoost = nullptr;
        HWND editMaxBoost = nullptr;
        HWND unitMaxBoost = nullptr;
        HWND labelMaxCut = nullptr;
        HWND editMaxCut = nullptr;
        HWND unitMaxCut = nullptr;
        HWND labelSmoothness = nullptr;
        HWND sliderSmoothness = nullptr;
        HWND valueSmoothness = nullptr;
        HWND labelMixedPhaseMax = nullptr;
        HWND editMixedPhaseMax = nullptr;
        HWND unitMixedPhaseMax = nullptr;
        HWND labelMixedPhaseStrength = nullptr;
        HWND editMixedPhaseStrength = nullptr;
        HWND unitMixedPhaseStrength = nullptr;
        HWND labelMixedPhaseCap = nullptr;
        HWND editMixedPhaseCap = nullptr;
        HWND unitMixedPhaseCap = nullptr;
        HWND buttonRecalculate = nullptr;
        HWND checkboxSyncHoverFrequency = nullptr;
        HWND inversionTitle = nullptr;
        HWND inversionLegendFrame = nullptr;
        HWND checkboxShowInputRight = nullptr;
        HWND lineInputRight = nullptr;
        HWND labelInputRight = nullptr;
        HWND checkboxShowInputLeft = nullptr;
        HWND lineInputLeft = nullptr;
        HWND labelInputLeft = nullptr;
        HWND checkboxShowInversionRight = nullptr;
        HWND lineInversionRight = nullptr;
        HWND labelInversionRight = nullptr;
        HWND checkboxShowInversionLeft = nullptr;
        HWND lineInversionLeft = nullptr;
        HWND labelInversionLeft = nullptr;
        HWND correctedTitle = nullptr;
        HWND correctedLegendFrame = nullptr;
        HWND lineCorrectedTarget = nullptr;
        HWND labelCorrectedTarget = nullptr;
        HWND checkboxShowCorrectedInputLeft = nullptr;
        HWND lineCorrectedInputLeft = nullptr;
        HWND labelCorrectedInputLeft = nullptr;
        HWND checkboxShowCorrectedInputRight = nullptr;
        HWND lineCorrectedInputRight = nullptr;
        HWND labelCorrectedInputRight = nullptr;
        HWND checkboxShowCorrectedLeft = nullptr;
        HWND lineCorrectedLeft = nullptr;
        HWND labelCorrectedLeft = nullptr;
        HWND checkboxShowCorrectedRight = nullptr;
        HWND lineCorrectedRight = nullptr;
        HWND labelCorrectedRight = nullptr;
        HWND excessPhaseTitle = nullptr;
        HWND excessPhaseLegendFrame = nullptr;
        HWND checkboxShowExcessPhaseInputRight = nullptr;
        HWND lineExcessPhaseInputRight = nullptr;
        HWND labelExcessPhaseInputRight = nullptr;
        HWND checkboxShowExcessPhaseInputLeft = nullptr;
        HWND lineExcessPhaseInputLeft = nullptr;
        HWND labelExcessPhaseInputLeft = nullptr;
        HWND checkboxShowExcessPhasePredictedRight = nullptr;
        HWND lineExcessPhasePredictedRight = nullptr;
        HWND labelExcessPhasePredictedRight = nullptr;
        HWND checkboxShowExcessPhasePredictedLeft = nullptr;
        HWND lineExcessPhasePredictedLeft = nullptr;
        HWND labelExcessPhasePredictedLeft = nullptr;
        HWND checkboxShowPredictedGroupDelayRight = nullptr;
        HWND linePredictedGroupDelayRight = nullptr;
        HWND labelPredictedGroupDelayRight = nullptr;
        HWND checkboxShowPredictedGroupDelayLeft = nullptr;
        HWND linePredictedGroupDelayLeft = nullptr;
        HWND labelPredictedGroupDelayLeft = nullptr;
        HWND checkboxShowInputGroupDelayLeft = nullptr;
        HWND lineInputGroupDelayLeft = nullptr;
        HWND labelInputGroupDelayLeft = nullptr;
        HWND checkboxShowInputGroupDelayRight = nullptr;
        HWND lineInputGroupDelayRight = nullptr;
        HWND labelInputGroupDelayRight = nullptr;
        HWND groupDelayTitle = nullptr;
        HWND checkboxAlignGroupDelayLatency = nullptr;
        HWND groupDelayLegendFrame = nullptr;
        HWND checkboxShowFilterGroupDelayLeft = nullptr;
        HWND lineGroupDelayLeft = nullptr;
        HWND labelGroupDelayLeft = nullptr;
        HWND checkboxShowFilterGroupDelayRight = nullptr;
        HWND lineGroupDelayRight = nullptr;
        HWND labelGroupDelayRight = nullptr;
        HWND impulseTitle = nullptr;
        HWND buttonImpulseZoomOutX = nullptr;
        HWND buttonImpulseZoomInX = nullptr;
        HWND buttonImpulseResetX = nullptr;
        HWND buttonImpulseZoomOutY = nullptr;
        HWND buttonImpulseZoomInY = nullptr;
        HWND buttonImpulseResetY = nullptr;
        HWND buttonImpulseFit = nullptr;
    };

    static constexpr wchar_t kPageClassName[] = L"WolfieFiltersPage";
    static constexpr int kComboTapCount = 3401;
    static constexpr int kComboPhaseMode = 3440;
    static constexpr int kEditMaxBoost = 3402;
    static constexpr int kEditMaxCut = 3403;
    static constexpr int kEditLowCorrection = 3404;
    static constexpr int kEditHighCorrection = 3405;
    static constexpr int kButtonRecalculate = 3406;
    static constexpr int kSliderSmoothness = 3411;
    static constexpr int kCheckboxShowInputRight = 3407;
    static constexpr int kCheckboxShowInputLeft = 3408;
    static constexpr int kCheckboxShowInversionRight = 3409;
    static constexpr int kCheckboxShowInversionLeft = 3410;
    static constexpr int kCheckboxShowCorrectedLeft = 3412;
    static constexpr int kCheckboxShowCorrectedRight = 3413;
    static constexpr int kCheckboxSyncHoverFrequency = 3414;
    static constexpr int kCheckboxShowCorrectedInputLeft = 3415;
    static constexpr int kCheckboxShowCorrectedInputRight = 3416;
    static constexpr int kCorrectionGraph = 3420;
    static constexpr int kCorrectedGraph = 3421;
    static constexpr int kGroupDelayGraph = 3422;
    static constexpr int kImpulseGraph = 3423;
    static constexpr int kButtonImpulseZoomOutX = 3424;
    static constexpr int kButtonImpulseZoomInX = 3425;
    static constexpr int kButtonImpulseResetX = 3426;
    static constexpr int kButtonImpulseZoomOutY = 3427;
    static constexpr int kButtonImpulseZoomInY = 3428;
    static constexpr int kButtonImpulseResetY = 3429;
    static constexpr int kButtonImpulseFit = 3430;
    static constexpr int kCheckboxShowExcessPhaseInputRight = 3431;
    static constexpr int kCheckboxShowExcessPhaseInputLeft = 3432;
    static constexpr int kCheckboxShowExcessPhasePredictedRight = 3433;
    static constexpr int kCheckboxShowExcessPhasePredictedLeft = 3434;
    static constexpr int kCheckboxShowPredictedGroupDelayRight = 3435;
    static constexpr int kCheckboxShowPredictedGroupDelayLeft = 3436;
    static constexpr int kExcessPhaseGraph = 3437;
    static constexpr int kCheckboxShowFilterGroupDelayLeft = 3438;
    static constexpr int kCheckboxShowFilterGroupDelayRight = 3439;
    static constexpr int kEditMixedPhaseMax = 3441;
    static constexpr int kEditMixedPhaseStrength = 3442;
    static constexpr int kCheckboxShowInputGroupDelayLeft = 3443;
    static constexpr int kCheckboxShowInputGroupDelayRight = 3444;
    static constexpr int kCheckboxAlignGroupDelayLatency = 3445;
    static constexpr int kEditMixedPhaseCap = 3446;

    static LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static bool tryParseDouble(const std::wstring& text, double& value);
    static std::wstring getWindowTextValue(HWND control);
    static void setWindowTextValue(HWND control, const std::wstring& text);
    static void populateTapCountCombo(HWND combo);
    static void populatePhaseModeCombo(HWND combo);
    static int comboIndexFromTapCount(int tapCount);
    static int tapCountFromComboIndex(int index);
    static int comboIndexFromPhaseMode(const std::string& phaseMode);
    static std::string phaseModeFromComboIndex(int index);
    static bool areSettingsEqual(const FilterDesignSettings& left, const FilterDesignSettings& right);

    void createControls();
    [[nodiscard]] double selectedSmoothness() const;
    void setSelectedSmoothness(double smoothness) const;
    [[nodiscard]] bool mixedModeSelected() const;
    void refreshSmoothnessValue() const;
    void refreshPhaseModeControls() const;
    [[nodiscard]] FilterDesignSettings currentSettings() const;
    void refreshRecalculateButton();
    bool drawRecalculateButton(const DRAWITEMSTRUCT& draw) const;
    void updateScrollBar();
    void setScrollOffset(int scrollOffset);
    bool handleMouseWheel(WPARAM wParam);
    void handleVScroll(WORD code, WORD thumbPosition);
    void applySharedFrequencyHoverMarker();
    void configureImpulseGraphViewport(const WorkspaceState& workspace);
    PlotGraphData buildCorrectionGraphData(const WorkspaceState& workspace) const;
    PlotGraphData buildCorrectedResponseGraphData(const WorkspaceState& workspace) const;
    PlotGraphData buildExcessPhaseGraphData(const WorkspaceState& workspace) const;
    PlotGraphData buildGroupDelayGraphData(const WorkspaceState& workspace) const;
    PlotGraphData buildImpulseGraphData(const WorkspaceState& workspace) const;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    PlotGraph correctionGraph_;
    PlotGraph correctedGraph_;
    PlotGraph excessPhaseGraph_;
    PlotGraph groupDelayGraph_;
    PlotGraph impulseGraph_;
    bool showInputRight_ = true;
    bool showInputLeft_ = true;
    bool showInversionRight_ = true;
    bool showInversionLeft_ = true;
    bool showCorrectedInputLeft_ = true;
    bool showCorrectedInputRight_ = true;
    bool showCorrectedLeft_ = true;
    bool showCorrectedRight_ = true;
    bool showExcessPhaseInputRight_ = true;
    bool showExcessPhaseInputLeft_ = true;
    bool showExcessPhasePredictedRight_ = true;
    bool showExcessPhasePredictedLeft_ = true;
    bool showInputGroupDelayLeft_ = true;
    bool showInputGroupDelayRight_ = true;
    bool showPredictedGroupDelayRight_ = true;
    bool showPredictedGroupDelayLeft_ = true;
    bool showFilterGroupDelayLeft_ = true;
    bool showFilterGroupDelayRight_ = true;
    bool alignGroupDelayLatency_ = false;
    bool syncHoverFrequencyEnabled_ = false;
    bool sharedFrequencyHoverActive_ = false;
    double sharedFrequencyHoverHz_ = 1000.0;
    FilterDesignSettings appliedSettings_{};
    bool filterDesignValid_ = false;
    bool recalculatePending_ = true;
    int sampleRate_ = 48000;
    int scrollOffset_ = 0;
    int contentHeight_ = 0;
};

}  // namespace wolfie::ui
