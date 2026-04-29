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
#include "audio/wasapi_service.h"
#include "core/models.h"

namespace wolfie::ui {

class SettingsDialog {
public:
    using SaveCallback = std::function<void(const AudioSettings&)>;

    static void show(HINSTANCE instance,
                     HWND owner,
                     const AudioSettings& settings,
                     const audio::WasapiService& wasapiService,
                     const audio::AsioService& asioService,
                     SaveCallback onSave);

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    SettingsDialog(HINSTANCE instance,
                   HWND owner,
                   AudioSettings settings,
                   const audio::WasapiService& wasapiService,
                   const audio::AsioService& asioService,
                   SaveCallback onSave);

    static std::wstring getWindowTextValue(HWND control);
    void createControls();
    void populateWindowsDeviceCombos();
    void populateChannelCombos();
    void populateWasapiChannelCombos();
    void refreshControlStates();
    void applyAndNotify();
    [[nodiscard]] bool usingAsioBackend() const;

    HINSTANCE instance_;
    HWND owner_;
    HWND window_ = nullptr;
    AudioSettings settings_;
    const audio::WasapiService& wasapiService_;
    const audio::AsioService& asioService_;
    SaveCallback onSave_;
    HWND backend_ = nullptr;
    HWND windowsInput_ = nullptr;
    HWND windowsOutput_ = nullptr;
    HWND driver_ = nullptr;
    HWND mic_ = nullptr;
    HWND left_ = nullptr;
    HWND right_ = nullptr;
    HWND micCalibrationPath_ = nullptr;
    std::vector<audio::WasapiDevice> windowsInputs_;
    std::vector<audio::WasapiDevice> windowsOutputs_;
};

}  // namespace wolfie::ui
