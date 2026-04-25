#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace wolfie::ui_theme {

inline constexpr COLORREF kBackground = RGB(241, 244, 248);
inline constexpr COLORREF kPanelBackground = RGB(255, 255, 255);
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

}  // namespace wolfie::ui_theme
