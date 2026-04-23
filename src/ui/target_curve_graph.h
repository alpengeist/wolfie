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
    static constexpr WORD kModelChangedNotification = 0x7F12;
    static constexpr WORD kSelectionChangedNotification = 0x7F13;

    static void registerWindowClass(HINSTANCE instance);

    void create(HWND parent, HINSTANCE instance, int controlId = 0);
    void setModel(const SmoothedResponse& response,
                  const MeasurementSettings& measurement,
                  const TargetCurveSettings& settings,
                  int selectedBandIndex);
    void setExtraVisibleRangeDb(double extraVisibleRangeDb);
    void setVerticalOffsetDb(double verticalOffsetDb);
    void layout(const RECT& bounds) const;
    void invalidate() const;

    [[nodiscard]] HWND window() const { return window_; }
    [[nodiscard]] double extraVisibleRangeDb() const { return extraVisibleRangeDb_; }
    [[nodiscard]] double verticalOffsetDb() const { return verticalOffsetDb_; }
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
        TargetCurveSettings originalSettings;
    };

    struct HoverState {
        bool active = false;
        bool tracking = false;
        POINT position{};
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
    bool onMouseWheel(WPARAM wParam, LPARAM lParam);
    void onMouseMove(LPARAM lParam);
    void onMouseLeave();
    void onLButtonDown(LPARAM lParam);
    void onLButtonUp();
    void updateDrag(const POINT& position);
    void finishDrag();

    [[nodiscard]] int hitTestHandle(const POINT& position, DragHandleType& type) const;

    HWND window_ = nullptr;
    MeasurementSettings measurement_;
    SmoothedResponse response_;
    TargetCurveSettings settings_;
    measurement::TargetCurvePlotData plot_;
    DragState drag_;
    HoverState hover_;
    int selectedBandIndex_ = -1;
    double extraVisibleRangeDb_ = 0.0;
    double verticalOffsetDb_ = 0.0;
    mutable HBITMAP backgroundCacheBitmap_ = nullptr;
    mutable SIZE backgroundCacheSize_{};
    mutable bool backgroundCacheValid_ = false;
};

COLORREF targetCurveBandColor(int colorIndex);

}  // namespace wolfie::ui
