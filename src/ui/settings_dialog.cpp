#include "ui/settings_dialog.h"

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr wchar_t kWindowClassName[] = L"WolfieSettingsWindow";
constexpr wchar_t kNoAsioDrivers[] = L"(No ASIO drivers found)";

void registerSettingsWindowClass(HINSTANCE instance) {
    WNDCLASSW settingsClass{};
    settingsClass.lpfnWndProc = SettingsDialog::WindowProc;
    settingsClass.hInstance = instance;
    settingsClass.lpszClassName = kWindowClassName;
    settingsClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = CreateSolidBrush(ui_theme::kBackground);
    RegisterClassW(&settingsClass);
}

void selectComboBoxString(HWND combo, const std::wstring& value) {
    const LRESULT index = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value.c_str()));
    if (index != CB_ERR) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    } else if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
}

}  // namespace

void SettingsDialog::show(HINSTANCE instance,
                          HWND owner,
                          const AudioSettings& settings,
                          const audio::AsioService& asioService,
                          SaveCallback onSave) {
    registerSettingsWindowClass(instance);
    auto* dialog = new SettingsDialog(instance, owner, settings, asioService, std::move(onSave));
    dialog->window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClassName, L"Measurement Settings",
                                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 460, 246,
                                      owner, nullptr, instance, dialog);
    if (dialog->window_ == nullptr) {
        delete dialog;
    }
}

SettingsDialog::SettingsDialog(HINSTANCE instance,
                               HWND owner,
                               AudioSettings settings,
                               const audio::AsioService& asioService,
                               SaveCallback onSave)
    : instance_(instance),
      owner_(owner),
      settings_(std::move(settings)),
      asioService_(asioService),
      onSave_(std::move(onSave)) {}

LRESULT CALLBACK SettingsDialog::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    SettingsDialog* dialog = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        dialog = reinterpret_cast<SettingsDialog*>(create->lpCreateParams);
        dialog->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        return TRUE;
    }

    dialog = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (dialog == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static HBRUSH settingsBackgroundBrush = CreateSolidBrush(ui_theme::kBackground);
    switch (message) {
    case WM_CREATE:
        dialog->createControls();
        return 0;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(settingsBackgroundBrush);
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(settingsBackgroundBrush);
    }
    case WM_COMMAND: {
        const WORD commandId = LOWORD(wParam);
        const WORD notificationCode = HIWORD(wParam);
        if (commandId == 1 && notificationCode == CBN_SELCHANGE) {
            dialog->applyAndNotify();
            return 0;
        }
        if ((commandId == 2 || commandId == 3 || commandId == 4) && notificationCode == EN_KILLFOCUS) {
            dialog->applyAndNotify();
            return 0;
        }
        if (commandId == 7) {
            const std::wstring driverText = getWindowTextValue(dialog->driver_);
            if (const auto error = dialog->asioService_.openControlPanel(window, driverText == kNoAsioDrivers ? L"" : driverText)) {
                MessageBoxW(window, error->c_str(), L"ASIO Control Panel", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (commandId == 8) {
            dialog->applyAndNotify();
            DestroyWindow(window);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        dialog->applyAndNotify();
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete dialog;
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }

    return 0;
}

std::wstring SettingsDialog::getWindowTextValue(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}

bool SettingsDialog::tryParseInt(const std::wstring& text, int& value) {
    try {
        size_t cursor = 0;
        const int parsed = std::stoi(text, &cursor);
        if (cursor != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

void SettingsDialog::createControls() {
    const auto drivers = asioService_.enumerateDrivers();

    CreateWindowW(L"STATIC", L"ASIO driver", WS_CHILD | WS_VISIBLE, 20, 20, 140, 20, window_, nullptr, nullptr, nullptr);
    driver_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                            170, 16, 240, 240, window_, reinterpret_cast<HMENU>(1), nullptr, nullptr);
    for (const auto& driverName : drivers) {
        SendMessageW(driver_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(driverName.c_str()));
    }
    if (drivers.empty()) {
        SendMessageW(driver_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kNoAsioDrivers));
    }
    selectComboBoxString(driver_, toWide(settings_.driver));

    CreateWindowW(L"STATIC", L"Mic input channel", WS_CHILD | WS_VISIBLE, 20, 56, 140, 20, window_, nullptr, nullptr, nullptr);
    mic_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", formatWideDouble(settings_.micInputChannel, 0).c_str(),
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 170, 52, 80, 24, window_, reinterpret_cast<HMENU>(2), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Left output channel", WS_CHILD | WS_VISIBLE, 20, 92, 140, 20, window_, nullptr, nullptr, nullptr);
    left_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", formatWideDouble(settings_.leftOutputChannel, 0).c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 170, 88, 80, 24, window_, reinterpret_cast<HMENU>(3), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Right output channel", WS_CHILD | WS_VISIBLE, 20, 128, 140, 20, window_, nullptr, nullptr, nullptr);
    right_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", formatWideDouble(settings_.rightOutputChannel, 0).c_str(),
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 170, 124, 80, 24, window_, reinterpret_cast<HMENU>(4), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Open ASIO Control Panel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 164, 180, 28, window_,
                  reinterpret_cast<HMENU>(7), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 340, 164, 80, 28, window_,
                  reinterpret_cast<HMENU>(8), nullptr, nullptr);
}

void SettingsDialog::applyAndNotify() {
    const std::wstring driverText = getWindowTextValue(driver_);
    if (driverText != kNoAsioDrivers) {
        settings_.driver = toUtf8(driverText);
    }

    int parsedValue = 0;
    if (tryParseInt(getWindowTextValue(mic_), parsedValue)) {
        settings_.micInputChannel = parsedValue;
    }
    if (tryParseInt(getWindowTextValue(left_), parsedValue)) {
        settings_.leftOutputChannel = parsedValue;
    }
    if (tryParseInt(getWindowTextValue(right_), parsedValue)) {
        settings_.rightOutputChannel = parsedValue;
    }

    if (onSave_) {
        onSave_(settings_);
    }
}

}  // namespace wolfie::ui
