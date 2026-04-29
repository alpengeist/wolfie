#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace Gdiplus {
class Image;
}

namespace wolfie::ui {

class SplashScreen {
public:
    ~SplashScreen();

    static void registerWindowClass(HINSTANCE instance);

    void create(HINSTANCE instance);
    void show() const;
    void destroy();
    void setStatus(const std::wstring& status, int currentStep, int totalSteps);

    [[nodiscard]] HWND window() const { return window_; }

private:
    static constexpr wchar_t kWindowClassName[] = L"WolfieSplashScreen";

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void onPaint() const;
    [[nodiscard]] RECT centeredWindowRect() const;
    void initializeImaging();
    void releaseImaging();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    std::wstring status_ = L"Starting";
    int currentStep_ = 0;
    int totalSteps_ = 0;
    ULONG_PTR gdiplusToken_ = 0;
    Gdiplus::Image* splashImage_ = nullptr;
};

}  // namespace wolfie::ui
