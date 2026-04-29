#include "ui/splash_screen.h"

#include <algorithm>
#include <array>
#include <filesystem>

#include <objidl.h>
#include <gdiplus.h>

namespace wolfie::ui {

namespace {

constexpr int kFallbackWindowWidth = 520;
constexpr int kFallbackWindowHeight = 280;

std::filesystem::path executableDirectory(HINSTANCE instance) {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = GetModuleFileNameW(instance, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(buffer.data()).parent_path();
}

std::filesystem::path resolveSplashImagePath(HINSTANCE instance) {
    const std::filesystem::path relativePath = std::filesystem::path(L"src") / L"ui" / L"splash-small.png";
    const std::array<std::filesystem::path, 4> candidates{
        std::filesystem::current_path() / relativePath,
        executableDirectory(instance) / relativePath,
        executableDirectory(instance).parent_path() / relativePath,
        executableDirectory(instance).parent_path().parent_path() / relativePath,
    };

    for (const std::filesystem::path& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

}  // namespace

SplashScreen::~SplashScreen() {
    destroy();
}

void SplashScreen::registerWindowClass(HINSTANCE instance) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&windowClass);
}

void SplashScreen::create(HINSTANCE instance) {
    instance_ = instance;
    initializeImaging();
    const RECT windowRect = centeredWindowRect();
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW,
                              kWindowClassName,
                              L"Wolfie",
                              WS_POPUP,
                              windowRect.left,
                              windowRect.top,
                              windowRect.right - windowRect.left,
                              windowRect.bottom - windowRect.top,
                              nullptr,
                              nullptr,
                              instance_,
                              this);
}

void SplashScreen::show() const {
    if (window_ == nullptr) {
        return;
    }

    ShowWindow(window_, SW_SHOWNORMAL);
    UpdateWindow(window_);
}

void SplashScreen::destroy() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    releaseImaging();
}

void SplashScreen::setStatus(const std::wstring& status, int currentStep, int totalSteps) {
    (void)status;
    (void)currentStep;
    (void)totalSteps;
}

LRESULT CALLBACK SplashScreen::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    SplashScreen* splash = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        splash = reinterpret_cast<SplashScreen*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(splash));
        if (splash != nullptr) {
            splash->window_ = window;
        }
        return TRUE;
    }

    splash = reinterpret_cast<SplashScreen*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (splash == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_PAINT:
        splash->onPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void SplashScreen::onPaint() const {
    PAINTSTRUCT paint{};
    HDC hdc = BeginPaint(window_, &paint);

    RECT rect{};
    GetClientRect(window_, &rect);
    HBRUSH backgroundBrush = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(hdc, &rect, backgroundBrush);

    if (splashImage_ != nullptr) {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(splashImage_,
                           0,
                           0,
                           static_cast<INT>(rect.right - rect.left),
                           static_cast<INT>(rect.bottom - rect.top));
    }

    EndPaint(window_, &paint);
}

RECT SplashScreen::centeredWindowRect() const {
    int windowWidth = kFallbackWindowWidth;
    int windowHeight = kFallbackWindowHeight;
    if (splashImage_ != nullptr) {
        windowWidth = std::max(static_cast<int>(splashImage_->GetWidth()), 1);
        windowHeight = std::max(static_cast<int>(splashImage_->GetHeight()), 1);
    }
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    windowWidth = std::min(windowWidth, screenWidth);
    windowHeight = std::min(windowHeight, screenHeight);
    const int left = std::max(0, (screenWidth - windowWidth) / 2);
    const int top = std::max(0, (screenHeight - windowHeight) / 2);
    return RECT{left, top, left + windowWidth, top + windowHeight};
}

void SplashScreen::initializeImaging() {
    if (gdiplusToken_ == 0) {
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken_, &startupInput, nullptr) != Gdiplus::Ok) {
            gdiplusToken_ = 0;
            return;
        }
    }

    if (splashImage_ != nullptr) {
        return;
    }

    const std::filesystem::path imagePath = resolveSplashImagePath(instance_);
    if (imagePath.empty()) {
        return;
    }

    splashImage_ = Gdiplus::Image::FromFile(imagePath.c_str(), FALSE);
    if (splashImage_ == nullptr || splashImage_->GetLastStatus() != Gdiplus::Ok) {
        delete splashImage_;
        splashImage_ = nullptr;
    }
}

void SplashScreen::releaseImaging() {
    delete splashImage_;
    splashImage_ = nullptr;

    if (gdiplusToken_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }
}

}  // namespace wolfie::ui
