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

enum class PlotGraphYAxisMode {
    Auto,
    SymmetricAroundZero
};

enum class PlotGraphLineStyle {
    Solid,
    Dash
};

enum class PlotGraphMeasurementDerivedValueMode {
    None,
    QuarterCycleFrequencyFromDeltaX
};

struct PlotGraphSeries {
    std::wstring label;
    COLORREF color = RGB(0, 0, 0);
    std::vector<double> values;
    PlotGraphLineStyle lineStyle = PlotGraphLineStyle::Solid;
};

struct PlotGraphXMarker {
    double xValue = 0.0;
    COLORREF color = RGB(0, 0, 0);
    PlotGraphLineStyle lineStyle = PlotGraphLineStyle::Dash;
};

struct PlotGraphXSpan {
    double startX = 0.0;
    double endX = 0.0;
    COLORREF color = RGB(0, 0, 0);
    BYTE alpha = 32;
};

struct PlotGraphData {
    std::vector<double> xValues;
    std::vector<PlotGraphSeries> series;
    std::vector<PlotGraphXMarker> xMarkers;
    std::vector<PlotGraphXSpan> xSpans;
    PlotGraphXAxisMode xAxisMode = PlotGraphXAxisMode::LogFrequency;
    PlotGraphYAxisMode yAxisMode = PlotGraphYAxisMode::Auto;
    std::wstring xUnit;
    std::wstring yUnit;
    bool fixedYRange = false;
    double minY = 0.0;
    double maxY = 1.0;
    bool zebraStripeYBands = false;
    int yTickSubdivision = 1;
    PlotGraphMeasurementDerivedValueMode measurementDerivedValueMode =
        PlotGraphMeasurementDerivedValueMode::None;
};

class PlotGraph {
public:
    static constexpr WORD kHoverChangedNotification = 0x7F11;
    static constexpr WORD kXRangeChangedNotification = 0x7F12;

    static void registerWindowClass(HINSTANCE instance);

    void create(HWND parent, HINSTANCE instance, int controlId = 0);
    void setData(PlotGraphData data);
    void setHoverCrosshairEnabled(bool enabled);
    void setSharedHoverMarker(bool enabled, bool active, double xValue);
    void setDefaultXRange(bool enabled, double minX, double maxX);
    void setDefaultYRange(bool enabled, double minY, double maxY);
    void setVisibleXRange(double minX, double maxX);
    void resetXRange();
    void resetYRange();
    void resetView();
    bool zoomX(double factor);
    bool zoomXFromMin(double factor);
    bool zoomY(double factor);
    void layout(const RECT& bounds) const;
    void invalidate() const;

    [[nodiscard]] HWND window() const { return window_; }
    [[nodiscard]] bool hasHoveredXValue() const { return hover_.active; }
    [[nodiscard]] double hoveredXValue() const { return hover_.xValue; }
    [[nodiscard]] bool hasCustomXRange() const { return hasCustomXRange_; }
    [[nodiscard]] double visibleMinX() const { return visibleMinX_; }
    [[nodiscard]] double visibleMaxX() const { return visibleMaxX_; }

private:
    struct HoverState {
        bool active = false;
        bool tracking = false;
        POINT position{};
        double xValue = 0.0;
        double yValue = 0.0;
    };

    struct BrushState {
        bool active = false;
        POINT anchor{};
        POINT current{};
    };

    struct MeasurementState {
        bool active = false;
        POINT anchor{};
        POINT current{};
    };

    struct SharedHoverMarkerState {
        bool enabled = false;
        bool active = false;
        double xValue = 0.0;
    };

    static constexpr wchar_t kWindowClassName[] = L"WolfiePlotGraph";

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void invalidateBackgroundCache() const;
    void releaseBackgroundCache() const;
    void drawStaticLayer(HDC hdc, const RECT& rect) const;
    void notifyHoverChanged() const;
    void notifyXRangeChanged() const;
    void onLButtonDown(LPARAM lParam);
    void onLButtonUp(LPARAM lParam);
    void onMouseMove(LPARAM lParam);
    void onMouseLeave();
    void onCaptureChanged();
    void onPaint() const;

    HWND window_ = nullptr;
    PlotGraphData data_;
    HoverState hover_;
    bool hasDefaultXRange_ = false;
    double defaultMinX_ = 0.0;
    double defaultMaxX_ = 1.0;
    bool hasDefaultYRange_ = false;
    double defaultMinY_ = -1.0;
    double defaultMaxY_ = 1.0;
    bool hasCustomXRange_ = false;
    double visibleMinX_ = 0.0;
    double visibleMaxX_ = 1.0;
    bool hasCustomYRange_ = false;
    double visibleMinY_ = -1.0;
    double visibleMaxY_ = 1.0;
    BrushState brush_;
    MeasurementState measurement_;
    bool hoverCrosshairEnabled_ = false;
    SharedHoverMarkerState sharedHoverMarker_;
    mutable HBITMAP backgroundCacheBitmap_ = nullptr;
    mutable SIZE backgroundCacheSize_{};
    mutable bool backgroundCacheValid_ = false;
};

}  // namespace wolfie::ui
