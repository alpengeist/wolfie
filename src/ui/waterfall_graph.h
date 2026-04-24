#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "measurement/waterfall_builder.h"

namespace wolfie::ui {

class WaterfallGraph {
public:
    static void registerWindowClass(HINSTANCE instance);

    void create(HWND parent, HINSTANCE instance, int controlId = 0);
    void setData(measurement::WaterfallPlotData data);
    void layout(const RECT& bounds) const;
    void invalidate() const;

    [[nodiscard]] HWND window() const { return window_; }

private:
    static constexpr wchar_t kWindowClassName[] = L"WolfieWaterfallGraph";
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void onPaint() const;

    HWND window_ = nullptr;
    measurement::WaterfallPlotData data_;
};

}  // namespace wolfie::ui
