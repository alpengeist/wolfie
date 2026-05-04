#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

#include "core/models.h"
#include "measurement/sweet_spot_alignment.h"
#include "ui/plot_graph.h"

namespace wolfie::ui {

class AlignmentPage {
public:
    static void registerPageWindowClass(HINSTANCE instance);
    static const wchar_t* pageWindowClassName();

    void create(HWND parent, HINSTANCE instance);
    void layout();
    void setVisible(bool visible) const;
    void populate(const measurement::SweetSpotAlignmentView& view);
    void refreshStatus(const MeasurementStatus& status, bool alignmentRunActive, bool hasWorkspace);
    bool handleCommand(WORD commandId, WORD notificationCode, bool& startStopPressed);

    [[nodiscard]] HWND window() const { return window_; }

private:
    static constexpr int kMetricCount = 3;

    struct MetricControls {
        HWND label = nullptr;
        HWND value = nullptr;
    };

    struct Controls {
        HWND note = nullptr;
        HWND buttonRun = nullptr;
        HWND status = nullptr;
        MetricControls metrics[kMetricCount];
        HWND graphTitle = nullptr;
    };

    static constexpr wchar_t kPageClassName[] = L"WolfieAlignmentPage";
    static constexpr int kButtonRun = 3601;
    static constexpr int kPulseGraph = 3602;

    static LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    PlotGraphData buildPulseGraphData() const;
    void createControls();
    void paint(HDC hdc) const;
    void paintDirectionArrow(HDC hdc, const RECT& bounds, bool pointLeft, COLORREF fillColor) const;
    void refreshPresentation() const;
    static std::wstring formatMetricValue(double value, const wchar_t* unit, int decimals, bool signedValue = false);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    measurement::SweetSpotAlignmentView view_;
    MeasurementStatus status_;
    bool alignmentRunActive_ = false;
    bool hasWorkspace_ = false;
    RECT leftArrowBounds_{};
    RECT rightArrowBounds_{};
    PlotGraph pulseGraph_;
};

}  // namespace wolfie::ui
