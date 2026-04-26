#pragma once

#include <optional>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/models.h"
#include "measurement/target_curve_designer.h"

namespace wolfie::ui {

class TargetCurveGraph {
public:
    static constexpr WORD kZoomChangedNotification = 0x7F11;
    static constexpr WORD kModelPreviewNotification = 0x7F12;
    static constexpr WORD kModelChangedNotification = 0x7F13;
    static constexpr WORD kSelectionChangedNotification = 0x7F15;

    static void registerWindowClass(HINSTANCE instance);

    void create(HWND parent, HINSTANCE instance, int controlId = 0);
    void setModel(const SmoothedResponse& response,
                  const MeasurementSettings& measurement,
                  const TargetCurveSettings& settings,
                  int selectedBandIndex);
    void setVisibleDbRange(bool hasCustomRange, double minDb, double maxDb);
    void resetVisibleDbRange();
    void layout(const RECT& bounds) const;
    void invalidate() const;

    [[nodiscard]] HWND window() const { return window_; }
    [[nodiscard]] bool hasCustomVisibleDbRange() const { return hasCustomVisibleDbRange_; }
    [[nodiscard]] double visibleMinDb() const { return visibleMinDb_; }
    [[nodiscard]] double visibleMaxDb() const { return visibleMaxDb_; }
    [[nodiscard]] const SmoothedResponse& response() const { return response_; }
    [[nodiscard]] const TargetCurveSettings& settings() const { return settings_; }
    [[nodiscard]] const measurement::TargetCurvePlotData& plot() const { return plot_; }
    [[nodiscard]] int selectedBandIndex() const { return selectedBandIndex_; }

private:
    enum class DragHandleType {
        None,
        BasicLow,
        BasicMid,
        BasicHigh,
        EqBand
    };

    struct DragState {
        bool active = false;
        DragHandleType type = DragHandleType::None;
        int bandIndex = -1;
        POINT origin{};
        bool changed = false;
        TargetCurveSettings originalSettings;
    };

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

    static constexpr wchar_t kWindowClassName[] = L"WolfieTargetCurveGraph";

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void invalidateBackgroundCache() const;
    void releaseBackgroundCache() const;
    void drawStaticLayer(HDC hdc, const RECT& rect, const RECT& paintRect) const;
    [[nodiscard]] RECT infoLineRect() const;
    void invalidateInfoLine() const;
    void notifyParent(WORD code) const;
    void rebuildPlot();
    void onPaint() const;
    void onMouseMove(LPARAM lParam);
    void onMouseLeave();
    void onLButtonDown(LPARAM lParam);
    void onLButtonUp(LPARAM lParam);
    void onCaptureChanged();
    void onLButtonDblClk(LPARAM lParam);
    void updateDrag(const POINT& position);
    void finishDrag(bool notifyCommit);

    [[nodiscard]] int hitTestHandle(const POINT& position, DragHandleType& type) const;

    HWND window_ = nullptr;
    MeasurementSettings measurement_;
    SmoothedResponse response_;
    TargetCurveSettings settings_;
    measurement::TargetCurvePlotData plot_;
    DragState drag_;
    HoverState hover_;
    BrushState brush_;
    int selectedBandIndex_ = -1;
    bool hasCustomVisibleDbRange_ = false;
    double visibleMinDb_ = -12.0;
    double visibleMaxDb_ = 12.0;
    mutable HBITMAP backgroundCacheBitmap_ = nullptr;
    mutable SIZE backgroundCacheSize_{};
    mutable bool backgroundCacheValid_ = false;
};

COLORREF targetCurveBandColor(int colorIndex);

}  // namespace wolfie::ui
