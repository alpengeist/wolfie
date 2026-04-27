#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "core/models.h"
#include "ui/target_curve_graph.h"

namespace wolfie::ui {

class TargetCurvePage {
public:
    static void registerPageWindowClass(HINSTANCE instance);
    static const wchar_t* pageWindowClassName();

    void create(HWND parent, HINSTANCE instance);
    void layout();
    void setVisible(bool visible) const;
    void populate(const WorkspaceState& workspace);
    void syncToWorkspace(WorkspaceState& workspace) const;
    bool handleCommand(WORD commandId, WORD notificationCode, WorkspaceState& workspace, bool& workspaceChanged);
    bool handleHScroll(HWND source, WorkspaceState& workspace, bool& workspaceChanged);
    bool handleDrawItem(const DRAWITEMSTRUCT* draw) const;

    [[nodiscard]] HWND window() const { return window_; }

private:
    struct Controls {
        HWND profileLabel = nullptr;
        HWND comboProfiles = nullptr;
        HWND buttonNewProfile = nullptr;
        HWND graphLabel = nullptr;
        HWND graphHint = nullptr;
        HWND bandsLabel = nullptr;
        HWND buttonNew = nullptr;
        HWND buttonDelete = nullptr;
        HWND buttonReset = nullptr;
        HWND checkboxBypassAll = nullptr;
        HWND listBands = nullptr;
        HWND detailLabel = nullptr;
        HWND checkboxEnabled = nullptr;
        HWND typeValue = nullptr;
        HWND frequencyLabel = nullptr;
        HWND frequencySlider = nullptr;
        HWND frequencyValue = nullptr;
        HWND frequencyUnit = nullptr;
        HWND gainLabel = nullptr;
        HWND gainSlider = nullptr;
        HWND gainValue = nullptr;
        HWND gainUnit = nullptr;
        HWND qLabel = nullptr;
        HWND qSlider = nullptr;
        HWND qValue = nullptr;
        HWND commentLabel = nullptr;
        HWND commentValue = nullptr;
    };

    static constexpr wchar_t kPageClassName[] = L"WolfieTargetCurvePage";
    static constexpr int kGraph = 3301;
    static constexpr int kButtonNew = 3302;
    static constexpr int kButtonDelete = 3303;
    static constexpr int kButtonReset = 3304;
    static constexpr int kCheckboxBypassAll = 3305;
    static constexpr int kListBands = 3306;
    static constexpr int kCheckboxBandEnabled = 3307;
    static constexpr int kFrequencySlider = 3308;
    static constexpr int kFrequencyEdit = 3309;
    static constexpr int kGainSlider = 3310;
    static constexpr int kGainEdit = 3311;
    static constexpr int kQSlider = 3312;
    static constexpr int kQEdit = 3313;
    static constexpr int kComboProfiles = 3314;
    static constexpr int kCommentEdit = 3315;
    static constexpr int kButtonNewProfile = 3316;
    static constexpr WORD kBandToggleNotification = 0x7F14;
    static constexpr int kFrequencySliderMax = 1000;
    static constexpr int kGainSliderMax = 240;
    static constexpr int kQSliderMax = 1000;

    static LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK BandListProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ValueEditProc(HWND window,
                                          UINT message,
                                          WPARAM wParam,
                                          LPARAM lParam,
                                          UINT_PTR subclassId,
                                          DWORD_PTR refData);
    static bool tryParseDouble(const std::wstring& text, double& value);
    static std::wstring getWindowTextValue(HWND control);
    static void setWindowTextValue(HWND control, const std::wstring& text);
    static int frequencyToSliderPosition(double frequencyHz, double minFrequencyHz, double maxFrequencyHz);
    static double sliderPositionToFrequency(int position, double minFrequencyHz, double maxFrequencyHz);
    static int gainToSliderPosition(double gainDb);
    static double sliderPositionToGain(int position);
    static int qToSliderPosition(double q);
    static double sliderPositionToQ(int position);
    static std::wstring requestProfileName(HWND owner, HINSTANCE instance);

    void createControls();
    bool handleMouseWheel(WPARAM wParam, LPARAM lParam);
    bool adjustValueField(HWND control, int wheelSteps);
    void syncAllOffState(TargetCurveSettings& settings) const;
    void syncActiveProfileFromWorkspaceState(WorkspaceState& workspace) const;
    void selectActiveProfile(WorkspaceState& workspace, const std::string& profileName);
    void refreshList(const WorkspaceState& workspace);
    void refreshProfileControls(const WorkspaceState& workspace);
    void refreshDetailControls(const WorkspaceState& workspace);
    void refreshGraph(const WorkspaceState& workspace);
    void selectBand(int index, WorkspaceState& workspace);
    void addBand(WorkspaceState& workspace);
    void createProfile(WorkspaceState& workspace);
    void deleteSelectedBand(WorkspaceState& workspace);
    void resetTarget(WorkspaceState& workspace);
    void toggleBandEnabled(int index, WorkspaceState& workspace);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    TargetCurveGraph graph_;
    int selectedBandIndex_ = 0;
    bool updatingControls_ = false;
    WNDPROC bandListProc_ = nullptr;
    std::vector<TargetEqBand> displayedBands_;
    int pendingBandToggleIndex_ = -1;
};

}  // namespace wolfie::ui
