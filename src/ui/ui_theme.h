#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <algorithm>
#include <windows.h>

namespace wolfie::ui_theme {

inline COLORREF blendColor(COLORREF first, COLORREF second, double ratio) {
    const double clampedRatio = std::clamp(ratio, 0.0, 1.0);
    const auto blendChannel = [clampedRatio](BYTE from, BYTE to) -> BYTE {
        return static_cast<BYTE>((static_cast<double>(from) * (1.0 - clampedRatio)) +
                                 (static_cast<double>(to) * clampedRatio));
    };
    return RGB(blendChannel(GetRValue(first), GetRValue(second)),
               blendChannel(GetGValue(first), GetGValue(second)),
               blendChannel(GetBValue(first), GetBValue(second)));
}

inline COLORREF backgroundColor() {
    return GetSysColor(COLOR_BTNFACE);
}

inline HBRUSH backgroundBrush() {
    return GetSysColorBrush(COLOR_BTNFACE);
}

inline COLORREF panelBackgroundColor() {
    return backgroundColor();
}

inline HBRUSH panelBackgroundBrush() {
    return backgroundBrush();
}

inline COLORREF inputBackgroundColor() {
    return GetSysColor(COLOR_WINDOW);
}

inline HBRUSH inputBackgroundBrush() {
    return GetSysColorBrush(COLOR_WINDOW);
}

inline COLORREF graphBackgroundColor() {
    return RGB(255, 255, 255);
}

inline HBRUSH graphBackgroundBrush() {
    return reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
}

inline COLORREF graphStripeColor() {
    return blendColor(graphBackgroundColor(), RGB(255, 255, 255), 0.08);
}

inline constexpr COLORREF kBorder = RGB(218, 224, 231);
inline constexpr COLORREF kAccent = RGB(44, 110, 182);
inline constexpr COLORREF kBlue = RGB(44, 110, 182);
inline constexpr COLORREF kGreen = RGB(46, 143, 82);
inline constexpr COLORREF kGray = RGB(118, 126, 136);
inline constexpr COLORREF kRed = RGB(190, 69, 69);
inline constexpr COLORREF kOrange = RGB(204, 112, 37);
inline constexpr COLORREF kTeal = RGB(33, 140, 144);
inline constexpr COLORREF kGold = RGB(181, 140, 24);
inline constexpr COLORREF kMagenta = RGB(170, 66, 120);
inline constexpr COLORREF kText = RGB(45, 52, 61);
inline constexpr COLORREF kMuted = RGB(109, 118, 130);

inline COLORREF graphOverlayColor() {
    return blendColor(kAccent, graphBackgroundColor(), 0.78);
}

}  // namespace wolfie::ui_theme
