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

enum class PlotGraphXAxisMode {
    LogFrequency,
    Linear
};

struct PlotGraphSeries {
    std::wstring label;
    COLORREF color = RGB(0, 0, 0);
    std::vector<double> values;
};

struct PlotGraphData {
    std::vector<double> xValues;
    std::vector<PlotGraphSeries> series;
    PlotGraphXAxisMode xAxisMode = PlotGraphXAxisMode::LogFrequency;
    std::wstring xUnit;
    std::wstring yUnit;
    bool fixedYRange = false;
    double minY = 0.0;
    double maxY = 1.0;
};

class PlotGraph {
public:
    static void registerWindowClass(HINSTANCE instance);

    void create(HWND parent, HINSTANCE instance, int controlId = 0);
    void setData(PlotGraphData data);
    void layout(const RECT& bounds) const;
    void invalidate() const;

    [[nodiscard]] HWND window() const { return window_; }

private:
    static constexpr wchar_t kWindowClassName[] = L"WolfiePlotGraph";
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void onPaint() const;

    HWND window_ = nullptr;
    PlotGraphData data_;
};

}  // namespace wolfie::ui
