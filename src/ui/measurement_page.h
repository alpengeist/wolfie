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
    void setMeasurementResult(const MeasurementResult& result);
    void refreshStatus(const MeasurementStatus& status, bool hasResult);
    void invalidateGraph() const;
    bool handleDrawItem(const DRAWITEMSTRUCT* draw, bool measurementRunning) const;
    bool handleCommand(WORD commandId,
                       WORD notificationCode,
                       WorkspaceState& workspace,
                       bool& measurePressed,
                       bool& loopbackPressed,
                       bool& sampleRateChanged,
                       bool& graphZoomChanged);
    bool handleHScroll(HWND source, WorkspaceState& workspace);

    [[nodiscard]] HWND window() const { return window_; }

private:
    struct Controls {
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
        HWND buttonLoopback = nullptr;
        HWND leftChannelLabel = nullptr;
        HWND leftProgressBar = nullptr;
        HWND leftProgressText = nullptr;
        HWND rightChannelLabel = nullptr;
        HWND rightProgressBar = nullptr;
        HWND rightProgressText = nullptr;
        HWND currentFrequency = nullptr;
        HWND currentAmplitude = nullptr;
        HWND peakAmplitude = nullptr;
        HWND metadataLabel = nullptr;
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
    static constexpr int kButtonLoopback = 3013;
    static constexpr int kResponseGraph = 3014;
    static constexpr int kMetadataTable = 3017;
    static constexpr int kOutputVolumeSliderMax = 61;

    static LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static int measurementSampleRateFromComboIndex(int index);
    static int comboIndexFromMeasurementSampleRate(int sampleRate);
    static void populateMeasurementSampleRateCombo(HWND combo);
    static double sliderPositionToOutputVolumeDb(int position);
    static int outputVolumeDbToSliderPosition(double outputVolumeDb);
    static std::wstring formatOutputVolumeLabel(double outputVolumeDb);
    static std::wstring getWindowTextValue(HWND control);
    static void setWindowTextValue(HWND control, const std::wstring& text);
    static void setListViewText(HWND listView, int row, int column, const std::wstring& text);

    ResponseGraphData buildGraphData(const MeasurementResult& result) const;
    std::vector<MetadataRow> buildMetadataRows(const MeasurementResult& result) const;
    void populateMetadataTable(const MeasurementResult& result) const;
    void createControls();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    ResponseGraph responseGraph_;
};

}  // namespace wolfie::ui
