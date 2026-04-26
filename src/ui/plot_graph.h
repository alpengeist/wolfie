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
    struct BrushState {
        bool active = false;
        POINT anchor{};
        POINT current{};
    };

    static constexpr wchar_t kWindowClassName[] = L"WolfiePlotGraph";

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void invalidateBackgroundCache() const;
    void releaseBackgroundCache() const;
    void drawStaticLayer(HDC hdc, const RECT& rect) const;
    void onLButtonDown(LPARAM lParam);
    void onLButtonUp(LPARAM lParam);
    void onMouseMove(LPARAM lParam);
    void onCaptureChanged();
    void onLButtonDblClk(LPARAM lParam);
    void resetView();
    void onPaint() const;

    HWND window_ = nullptr;
    PlotGraphData data_;
    bool hasCustomXRange_ = false;
    double visibleMinX_ = 0.0;
    double visibleMaxX_ = 1.0;
    BrushState brush_;
    mutable HBITMAP backgroundCacheBitmap_ = nullptr;
    mutable SIZE backgroundCacheSize_{};
    mutable bool backgroundCacheValid_ = false;
};

}  // namespace wolfie::ui
