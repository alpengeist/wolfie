#pragma once

#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace wolfie::ui {

struct ResponseGraphSeries {
    std::wstring label;
    COLORREF color = RGB(0, 0, 0);
    std::vector<double> values;
};

struct ResponseGraphData {
    std::vector<double> frequencyAxisHz;
    std::vector<ResponseGraphSeries> series;
    double minDb = -50.0;
};

class ResponseGraph {
public:
    static constexpr WORD kZoomChangedNotification = 0x7F01;

    static void registerWindowClass(HINSTANCE instance);

    void create(HWND parent, HINSTANCE instance, int controlId = 0);
    void setData(ResponseGraphData data);
    void setVisibleFrequencyRange(bool hasCustomRange, double minFrequencyHz, double maxFrequencyHz);
    void resetVisibleFrequencyRange();
    void layout(const RECT& bounds) const;
    void invalidate() const;

    [[nodiscard]] HWND window() const { return window_; }
    [[nodiscard]] bool hasCustomVisibleFrequencyRange() const { return hasCustomVisibleFrequencyRange_; }
    [[nodiscard]] double visibleMinFrequencyHz() const { return visibleMinFrequencyHz_; }
    [[nodiscard]] double visibleMaxFrequencyHz() const { return visibleMaxFrequencyHz_; }

private:
    struct HoverState {
        bool active = false;
        bool tracking = false;
        POINT position{};
    };

    struct BrushState {
        bool active = false;
        POINT anchor{};
        POINT current{};
    };

    static constexpr wchar_t kWindowClassName[] = L"WolfieResponseGraph";

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    [[nodiscard]] RECT infoLineRect() const;
    void invalidateInfoLine() const;
    void notifyZoomChanged() const;
    void onLButtonDown(LPARAM lParam);
    void onLButtonUp(LPARAM lParam);
    void onMouseMove(LPARAM lParam);
    void onMouseLeave();
    void onCaptureChanged();
    void onLButtonDblClk(LPARAM lParam);
    void onPaint() const;

    HWND window_ = nullptr;
    ResponseGraphData data_;
    HoverState hover_;
    BrushState brush_;
    bool hasCustomVisibleFrequencyRange_ = false;
    double visibleMinFrequencyHz_ = 10.0;
    double visibleMaxFrequencyHz_ = 20000.0;
};

}  // namespace wolfie::ui
