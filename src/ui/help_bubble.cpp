#include "ui/help_bubble.h"

#include <algorithm>

#include <commctrl.h>

#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr int kFieldHelpMaxWidth = 280;
constexpr int kFieldHelpPaddingX = 10;
constexpr int kFieldHelpPaddingY = 8;
constexpr COLORREF kHelpBubbleTint = RGB(255, 255, 225);

}  // namespace

void HelpBubbleController::create(HWND parent, HINSTANCE instance) {
    destroy();

    parent_ = parent;
    if (parent_ == nullptr) {
        return;
    }

    const COLORREF helpBubbleColor = ui_theme::blendColor(ui_theme::backgroundColor(), kHelpBubbleTint, 0.8);
    bubbleBrush_ = CreateSolidBrush(helpBubbleColor);
    bubble_ = CreateWindowExW(0,
                              L"STATIC",
                              L"",
                              WS_CHILD | WS_BORDER | SS_LEFT | SS_NOPREFIX | kHelpBubbleChildClipStyle,
                              0,
                              0,
                              0,
                              0,
                              parent_,
                              nullptr,
                              instance,
                              nullptr);
    if (bubble_ != nullptr) {
        ShowWindow(bubble_, SW_HIDE);
    }
}

void HelpBubbleController::destroy() {
    hide();

    for (const Entry& entry : entries_) {
        if (entry.label != nullptr && IsWindow(entry.label)) {
            RemoveWindowSubclass(entry.label, LabelSubclassProc, 0);
        }
    }
    entries_.clear();

    if (bubble_ != nullptr && IsWindow(bubble_)) {
        DestroyWindow(bubble_);
    }
    bubble_ = nullptr;
    if (bubbleBrush_ != nullptr) {
        DeleteObject(bubbleBrush_);
        bubbleBrush_ = nullptr;
    }
    parent_ = nullptr;
}

void HelpBubbleController::hide() {
    if (bubble_ != nullptr && IsWindow(bubble_)) {
        RECT bubbleRect{};
        GetWindowRect(bubble_, &bubbleRect);
        MapWindowPoints(nullptr, parent_, reinterpret_cast<LPPOINT>(&bubbleRect), 2);
        ShowWindow(bubble_, SW_HIDE);
        if (parent_ != nullptr && IsWindow(parent_)) {
            RedrawWindow(parent_, &bubbleRect, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
    }
    activeLabel_ = nullptr;
    activeText_.clear();
}

void HelpBubbleController::registerLabel(HWND label, std::wstring text) {
    if (label == nullptr || text.empty()) {
        return;
    }

    entries_.push_back({label, std::move(text)});
    SetWindowSubclass(label, LabelSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));
}

bool HelpBubbleController::handleCtlColorStatic(HDC hdc, HWND control, LRESULT& result) const {
    if (control != bubble_ || bubbleBrush_ == nullptr || bubble_ == nullptr || !IsWindow(bubble_)) {
        return false;
    }

    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, ui_theme::blendColor(ui_theme::backgroundColor(), kHelpBubbleTint, 0.8));
    SetTextColor(hdc, ui_theme::kText);
    result = reinterpret_cast<LRESULT>(bubbleBrush_);
    return true;
}

void HelpBubbleController::showForLabel(HWND label) {
    if (bubble_ == nullptr || parent_ == nullptr || label == nullptr) {
        return;
    }

    const std::wstring* text = findText(label);
    if (text == nullptr || text->empty()) {
        return;
    }

    activeLabel_ = label;
    activeText_ = *text;
    SetWindowTextW(bubble_, activeText_.c_str());
    SendMessageW(bubble_, WM_SETFONT, SendMessageW(label, WM_GETFONT, 0, 0), TRUE);

    HDC hdc = GetDC(parent_);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(label, WM_GETFONT, 0, 0));
    HFONT previousFont = nullptr;
    if (font != nullptr) {
        previousFont = reinterpret_cast<HFONT>(SelectObject(hdc, font));
    }

    RECT textRect{0, 0, kFieldHelpMaxWidth - (kFieldHelpPaddingX * 2), 0};
    DrawTextW(hdc, activeText_.c_str(), -1, &textRect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (previousFont != nullptr) {
        SelectObject(hdc, previousFont);
    }
    ReleaseDC(parent_, hdc);

    RECT labelRect{};
    GetWindowRect(label, &labelRect);
    MapWindowPoints(nullptr, parent_, reinterpret_cast<LPPOINT>(&labelRect), 2);

    RECT clientRect{};
    GetClientRect(parent_, &clientRect);

    const int bubbleWidth = textRect.right - textRect.left + (kFieldHelpPaddingX * 2);
    const int bubbleHeight = textRect.bottom - textRect.top + (kFieldHelpPaddingY * 2);

    int bubbleLeft = labelRect.left;
    int bubbleTop = labelRect.bottom + 6;
    if (bubbleLeft + bubbleWidth > clientRect.right - 8) {
        bubbleLeft = std::max(8, static_cast<int>(clientRect.right) - bubbleWidth - 8);
    }
    if (bubbleTop + bubbleHeight > clientRect.bottom - 8) {
        bubbleTop = std::max(8, static_cast<int>(labelRect.top) - bubbleHeight - 6);
    }

    SetWindowPos(bubble_, HWND_TOP, bubbleLeft, bubbleTop, bubbleWidth, bubbleHeight, SWP_SHOWWINDOW);
    InvalidateRect(bubble_, nullptr, TRUE);
}

const std::wstring* HelpBubbleController::findText(HWND label) const {
    for (const Entry& entry : entries_) {
        if (entry.label == label) {
            return &entry.text;
        }
    }
    return nullptr;
}

LRESULT CALLBACK HelpBubbleController::LabelSubclassProc(HWND window,
                                                         UINT message,
                                                         WPARAM wParam,
                                                         LPARAM lParam,
                                                         UINT_PTR,
                                                         DWORD_PTR refData) {
    auto* controller = reinterpret_cast<HelpBubbleController*>(refData);
    if (controller == nullptr) {
        return DefSubclassProc(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_LBUTTONDOWN:
        SetCapture(window);
        controller->showForLabel(window);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        controller->hide();
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        controller->hide();
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, LabelSubclassProc, 0);
        break;
    default:
        break;
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

}  // namespace wolfie::ui
