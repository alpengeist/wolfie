#pragma once

#include <functional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "audio/asio_service.h"
#include "core/models.h"

namespace wolfie::ui {

class SettingsDialog {
public:
    using SaveCallback = std::function<void(const AudioSettings&)>;

    static void show(HINSTANCE instance,
                     HWND owner,
                     const AudioSettings& settings,
                     const audio::AsioService& asioService,
                     SaveCallback onSave);

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    SettingsDialog(HINSTANCE instance,
                   HWND owner,
                   AudioSettings settings,
                   const audio::AsioService& asioService,
                   SaveCallback onSave);

    static std::wstring getWindowTextValue(HWND control);
    void createControls();
    void populateChannelCombos();
    void applyAndNotify();

    HINSTANCE instance_;
    HWND owner_;
    HWND window_ = nullptr;
    AudioSettings settings_;
    const audio::AsioService& asioService_;
    SaveCallback onSave_;
    HWND driver_ = nullptr;
    HWND mic_ = nullptr;
    HWND left_ = nullptr;
    HWND right_ = nullptr;
    HWND micCalibrationPath_ = nullptr;
};

}  // namespace wolfie::ui
