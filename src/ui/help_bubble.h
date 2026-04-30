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

inline constexpr DWORD kHelpBubbleChildClipStyle = WS_CLIPSIBLINGS;

class HelpBubbleController {
public:
    void create(HWND parent, HINSTANCE instance);
    void destroy();
    void hide();
    void registerLabel(HWND label, std::wstring text);
    [[nodiscard]] bool handleCtlColorStatic(HDC hdc, HWND control, LRESULT& result) const;

private:
    struct Entry {
        HWND label = nullptr;
        std::wstring text;
    };

    void showForLabel(HWND label);
    [[nodiscard]] const std::wstring* findText(HWND label) const;
    static LRESULT CALLBACK LabelSubclassProc(HWND window,
                                              UINT message,
                                              WPARAM wParam,
                                              LPARAM lParam,
                                              UINT_PTR subclassId,
                                              DWORD_PTR refData);

    HWND parent_ = nullptr;
    HWND bubble_ = nullptr;
    HBRUSH bubbleBrush_ = nullptr;
    HWND activeLabel_ = nullptr;
    std::wstring activeText_;
    std::vector<Entry> entries_;
};

}  // namespace wolfie::ui
