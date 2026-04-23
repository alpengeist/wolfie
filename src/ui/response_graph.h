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
    void setExtraVisibleRangeDb(double extraVisibleRangeDb);
    void setVerticalOffsetDb(double verticalOffsetDb);
    void layout(const RECT& bounds) const;
    void invalidate() const;

    [[nodiscard]] HWND window() const { return window_; }
    [[nodiscard]] double extraVisibleRangeDb() const { return extraVisibleRangeDb_; }
    [[nodiscard]] double verticalOffsetDb() const { return verticalOffsetDb_; }

private:
    struct HoverState {
        bool active = false;
        bool tracking = false;
        POINT position{};
    };

    static constexpr wchar_t kWindowClassName[] = L"WolfieResponseGraph";
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] RECT infoLineRect() const;
    void invalidateInfoLine() const;
    void notifyZoomChanged() const;
    bool onMouseWheel(WPARAM wParam, LPARAM lParam);
    void onMouseMove(LPARAM lParam);
    void onMouseLeave();
    void onPaint() const;

    HWND window_ = nullptr;
    ResponseGraphData data_;
    HoverState hover_;
    double extraVisibleRangeDb_ = 0.0;
    double verticalOffsetDb_ = 0.0;
};

}  // namespace wolfie::ui
