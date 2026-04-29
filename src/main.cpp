#include "wolfie_app.h"

#include <windows.h>
#include <shellapi.h>

#include "audio/asio_service.h"

namespace {

constexpr wchar_t kAsioHelperWindowClass[] = L"WolfieAsioHelperWindow";

struct AsioHelperState {
    HWND window = nullptr;
    bool sawExternalWindow = false;
    int idleTicks = 0;
};

bool hasHelperOwnedTopLevelWindow(DWORD processId, HWND helperWindow) {
    struct EnumState {
        DWORD processId = 0;
        HWND helperWindow = nullptr;
        bool found = false;
    } state{processId, helperWindow, false};

    EnumWindows([](HWND window, LPARAM lParam) -> BOOL {
        auto& state = *reinterpret_cast<EnumState*>(lParam);
        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(window, &windowProcessId);
        if (windowProcessId != state.processId || window == state.helperWindow || !IsWindowVisible(window)) {
            return TRUE;
        }
        state.found = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&state));

    return state.found;
}

LRESULT CALLBACK AsioHelperWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AsioHelperState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* helperState = reinterpret_cast<AsioHelperState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(helperState));
        helperState->window = window;
        return TRUE;
    }
    case WM_TIMER:
        if (state == nullptr) {
            return 0;
        }
        if (hasHelperOwnedTopLevelWindow(GetCurrentProcessId(), window)) {
            state->sawExternalWindow = true;
            state->idleTicks = 0;
            return 0;
        }
        ++state->idleTicks;
        if ((state->sawExternalWindow && state->idleTicks >= 4) ||
            (!state->sawExternalWindow && state->idleTicks >= 8)) {
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int runAsioControlPanelHelper(HINSTANCE instance, std::wstring driverName) {
    WNDCLASSW helperClass{};
    helperClass.lpfnWndProc = AsioHelperWindowProc;
    helperClass.hInstance = instance;
    helperClass.lpszClassName = kAsioHelperWindowClass;
    RegisterClassW(&helperClass);

    AsioHelperState state{};
    HWND helperWindow = CreateWindowExW(0,
                                        kAsioHelperWindowClass,
                                        L"Wolfie ASIO Helper",
                                        WS_OVERLAPPED,
                                        CW_USEDEFAULT,
                                        CW_USEDEFAULT,
                                        0,
                                        0,
                                        nullptr,
                                        nullptr,
                                        instance,
                                        &state);
    if (helperWindow == nullptr) {
        MessageBoxW(nullptr, L"Failed to create the ASIO helper window.", L"ASIO Control Panel", MB_OK | MB_ICONERROR);
        return 1;
    }

    wolfie::audio::AsioService asioService;
    wolfie::audio::AsioControlPanelSession controlPanelSession;
    if (const auto error = asioService.startControlPanelSession(helperWindow, driverName, controlPanelSession)) {
        MessageBoxW(nullptr, error->c_str(), L"ASIO Control Panel", MB_OK | MB_ICONERROR);
        DestroyWindow(helperWindow);
        return 1;
    }

    SetTimer(helperWindow, 1, 250, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        const bool isAsioHelper = argc >= 3 && std::wstring_view(argv[1]) == L"--asio-control-panel";
        if (isAsioHelper) {
            const std::wstring driverName = argv[2];
            LocalFree(argv);
            return runAsioControlPanelHelper(instance, driverName);
        }
        LocalFree(argv);
    }

    wolfie::WolfieApp app(instance);
    return app.run();
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR, int showCommand) {
    return wWinMain(instance, previousInstance, GetCommandLineW(), showCommand);
}
