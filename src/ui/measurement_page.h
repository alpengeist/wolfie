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
#include "ui/response_graph.h"
#include "ui/waterfall_graph.h"

namespace wolfie::ui {

class MeasurementPage {
public:
    static void registerPageWindowClass(HINSTANCE instance);
    static const wchar_t* pageWindowClassName();

    void create(HWND parent, HINSTANCE instance);
    void layout();
    void setVisible(bool visible) const;
    void populate(const WorkspaceState& workspace);
    void syncToWorkspace(WorkspaceState& workspace) const;
    void setWorkspaceView(const WorkspaceState& workspace);
    void setCalibrationRefreshInProgress(bool inProgress,
                                         int currentStep = 0,
                                         int totalSteps = 0,
                                         const std::wstring& statusText = L"");
    void refreshStatus(const MeasurementStatus& status, bool hasResult);
    void invalidateGraph() const;
    bool handleDrawItem(const DRAWITEMSTRUCT* draw) const;
    bool handleCommand(WORD commandId,
                       WORD notificationCode,
                       WorkspaceState& workspace,
                       bool& roomMeasurePressed,
                       bool& referenceMeasurePressed,
                       bool& microphoneCalibrationChanged,
                       bool& sampleRateChanged,
                       bool& graphZoomChanged,
                       bool& plotSelectionChanged);
    bool handleHScroll(HWND source, WorkspaceState& workspace);

    [[nodiscard]] HWND window() const { return window_; }

private:
    struct Controls {
        HWND labelMicCalibration = nullptr;
        HWND editMicCalibrationPath = nullptr;
        HWND buttonMicCalibrationBrowse = nullptr;
        HWND buttonMicCalibrationClear = nullptr;
        HWND calibrationActivityLabel = nullptr;
        HWND calibrationActivityBar = nullptr;
        HWND labelFadeIn = nullptr;
        HWND labelFadeOut = nullptr;
        HWND labelDuration = nullptr;
        HWND labelStartFrequency = nullptr;
        HWND labelEndFrequency = nullptr;
        HWND labelTargetLength = nullptr;
        HWND labelLeadIn = nullptr;
        HWND labelSampleRate = nullptr;
        HWND unitFadeIn = nullptr;
        HWND unitFadeOut = nullptr;
        HWND unitDuration = nullptr;
        HWND unitStartFrequency = nullptr;
        HWND unitEndFrequency = nullptr;
        HWND unitTargetLength = nullptr;
        HWND unitLeadIn = nullptr;
        HWND editFadeIn = nullptr;
        HWND editFadeOut = nullptr;
        HWND editDuration = nullptr;
        HWND editStartFrequency = nullptr;
        HWND editEndFrequency = nullptr;
        HWND editTargetLength = nullptr;
        HWND editLeadIn = nullptr;
        HWND comboSampleRate = nullptr;
        HWND labelOutputVolume = nullptr;
        HWND outputVolumeValue = nullptr;
        HWND outputVolumeSlider = nullptr;
        HWND outputVolumeMuteLabel = nullptr;
        HWND outputVolumeMaxLabel = nullptr;
        HWND buttonMeasure = nullptr;
        HWND buttonMeasureReference = nullptr;
        HWND leftChannelLabel = nullptr;
        HWND leftProgressBar = nullptr;
        HWND leftProgressText = nullptr;
        HWND rightChannelLabel = nullptr;
        HWND rightProgressBar = nullptr;
        HWND rightProgressText = nullptr;
        HWND currentFrequency = nullptr;
        HWND currentAmplitude = nullptr;
        HWND peakAmplitude = nullptr;
        HWND labelPlot = nullptr;
        HWND comboPlot = nullptr;
        HWND labelWaterfallChannel = nullptr;
        HWND comboWaterfallChannel = nullptr;
        HWND infoStatusFrame = nullptr;
        HWND referenceStatus = nullptr;
        HWND micCompStatus = nullptr;
        HWND responseLegendFrame = nullptr;
        HWND checkboxShowRoomLeft = nullptr;
        HWND lineRoomLeft = nullptr;
        HWND labelRoomLeft = nullptr;
        HWND checkboxShowRoomRight = nullptr;
        HWND lineRoomRight = nullptr;
        HWND labelRoomRight = nullptr;
        HWND checkboxShowReference = nullptr;
        HWND lineReference = nullptr;
        HWND labelReference = nullptr;
        HWND metadataLabel = nullptr;
        HWND metadataToggle = nullptr;
        HWND metadataTable = nullptr;
    };

    struct MetadataRow {
        std::wstring section;
        std::wstring metric;
        std::wstring value;
    };

    static constexpr wchar_t kPageClassName[] = L"WolfiePageWindow";
    static constexpr int kEditFadeIn = 3004;
    static constexpr int kEditFadeOut = 3005;
    static constexpr int kEditDuration = 3006;
    static constexpr int kEditStartFrequency = 3007;
    static constexpr int kEditEndFrequency = 3008;
    static constexpr int kEditTargetLength = 3009;
    static constexpr int kEditLeadIn = 3010;
    static constexpr int kButtonMeasure = 3011;
    static constexpr int kComboMeasurementSampleRate = 3012;
    static constexpr int kResponseGraph = 3014;
    static constexpr int kComboPlot = 3018;
    static constexpr int kComboWaterfallChannel = 3019;
    static constexpr int kMetadataTable = 3017;
    static constexpr int kButtonMetadataToggle = 3020;
    static constexpr int kButtonMeasureReference = 3021;
    static constexpr int kCheckboxShowRoomLeft = 3022;
    static constexpr int kCheckboxShowRoomRight = 3023;
    static constexpr int kCheckboxShowReference = 3024;
    static constexpr int kButtonMicCalibrationBrowse = 3025;
    static constexpr int kButtonMicCalibrationClear = 3026;
    static constexpr int kOutputVolumeSliderMax = 61;

    static LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static int measurementSampleRateFromComboIndex(int index);
    static int comboIndexFromMeasurementSampleRate(int sampleRate);
    static void populateMeasurementSampleRateCombo(HWND combo);
    static void populatePlotCombo(HWND combo);
    static int comboIndexFromPlotMode(const std::string& plotMode);
    static std::string plotModeFromComboIndex(int index);
    static void populateWaterfallChannelCombo(HWND combo);
    static int comboIndexFromWaterfallChannel(const std::string& channel);
    static std::string waterfallChannelFromComboIndex(int index);
    static double sliderPositionToOutputVolumeDb(int position);
    static int outputVolumeDbToSliderPosition(double outputVolumeDb);
    static std::wstring formatOutputVolumeLabel(double outputVolumeDb);
    static std::wstring getWindowTextValue(HWND control);
    static void setWindowTextValue(HWND control, const std::wstring& text);
    static void setListViewText(HWND listView, int row, int column, const std::wstring& text);
    static std::wstring referenceStatusText(const AudioSettings& audio,
                                            const MeasurementSettings& measurement,
                                            const MeasurementResult& referenceResult);
    static std::wstring microphoneCompStatusText(const AudioSettings& audio);

    ResponseGraphData buildGraphData() const;
    void refreshPlots();
    void updatePlotControlVisibility() const;
    void updateMetadataVisibility() const;
    void updateLegendVisibility() const;
    void setInteractiveControlsEnabled(bool enabled) const;
    void refreshReferenceStatusLabels() const;
    void refreshActionButtons() const;
    std::vector<MetadataRow> buildMetadataRows(const MeasurementResult& result) const;
    void populateMetadataTable(const MeasurementResult& result) const;
    void createControls();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    MeasurementResult result_;
    MeasurementResult referenceResult_;
    AudioSettings audioSettings_;
    MeasurementSettings measurementSettings_;
    MeasurementStatus status_;
    bool metadataCollapsed_ = false;
    bool showRoomLeft_ = true;
    bool showRoomRight_ = true;
    bool showReference_ = true;
    bool calibrationRefreshInProgress_ = false;
    ResponseGraph responseGraph_;
    WaterfallGraph waterfallGraph_;
};

}  // namespace wolfie::ui
