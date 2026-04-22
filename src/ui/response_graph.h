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
    double minDb = -30.0;
};

class ResponseGraph {
public:
    static void registerWindowClass(HINSTANCE instance);

    void create(HWND parent, HINSTANCE instance, int controlId = 0);
    void setData(ResponseGraphData data);
    void layout(const RECT& bounds) const;
    void invalidate() const;

    [[nodiscard]] HWND window() const { return window_; }

private:
    static constexpr wchar_t kWindowClassName[] = L"WolfieResponseGraph";
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void onPaint() const;

    HWND window_ = nullptr;
    ResponseGraphData data_;
};

}  // namespace wolfie::ui
