#include "ui/settings_dialog.h"

#include <algorithm>
#include <optional>

#include <commdlg.h>

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr wchar_t kWindowClassName[] = L"WolfieSettingsWindow";
constexpr wchar_t kNoAsioDrivers[] = L"(No ASIO drivers found)";
constexpr int kDriverControlId = 1;
constexpr int kMicControlId = 2;
constexpr int kLeftControlId = 3;
constexpr int kRightControlId = 4;
constexpr int kMicCalibrationBrowseId = 5;
constexpr int kMicCalibrationClearId = 6;
constexpr int kOpenControlPanelId = 7;
constexpr int kCloseControlId = 8;

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

std::wstring formatSavedChannelLabel(int channelNumber) {
    return L"Channel " + std::to_wstring(channelNumber);
}

void addChannelComboItem(HWND combo, int channelNumber, const std::wstring& label) {
    const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (index != CB_ERR) {
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(channelNumber));
    }
}

void selectComboBoxItemData(HWND combo, int channelNumber) {
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        const LRESULT itemData = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
        if (itemData == channelNumber) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            return;
        }
    }

    if (count > 0) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
}

int selectedChannelNumber(HWND combo, int fallback) {
    const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR) {
        return fallback;
    }

    const LRESULT itemData = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
    return itemData > 0 ? static_cast<int>(itemData) : fallback;
}

std::optional<std::filesystem::path> pickCalibrationFile(HWND owner, const std::filesystem::path& initialPath) {
    std::wstring buffer(32768, L'\0');
    const std::wstring initialText = initialPath.wstring();
    if (!initialText.empty()) {
        initialText.copy(buffer.data(), std::min(initialText.size(), buffer.size() - 1));
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter =
        L"Calibration Files (*.txt;*.cal;*.csv)\0*.txt;*.cal;*.csv\0"
        L"All Files (*.*)\0*.*\0\0";
    dialog.lpstrTitle = L"Select Microphone Calibration File";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }

    buffer.resize(wcslen(buffer.c_str()));
    return std::filesystem::path(buffer);
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
                                      CW_USEDEFAULT, CW_USEDEFAULT, 640, 292,
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
        if (commandId == kDriverControlId && notificationCode == CBN_SELCHANGE) {
            dialog->populateChannelCombos();
            dialog->applyAndNotify();
            return 0;
        }
        if ((commandId == kMicControlId || commandId == kLeftControlId || commandId == kRightControlId) &&
            notificationCode == CBN_SELCHANGE) {
            dialog->applyAndNotify();
            return 0;
        }
        if (commandId == kMicCalibrationBrowseId) {
            const std::wstring currentPath = getWindowTextValue(dialog->micCalibrationPath_);
            if (const auto selected = pickCalibrationFile(window, std::filesystem::path(currentPath))) {
                SetWindowTextW(dialog->micCalibrationPath_, selected->wstring().c_str());
                dialog->applyAndNotify();
            }
            return 0;
        }
        if (commandId == kMicCalibrationClearId) {
            SetWindowTextW(dialog->micCalibrationPath_, L"");
            dialog->applyAndNotify();
            return 0;
        }
        if (commandId == kOpenControlPanelId) {
            const std::wstring driverText = getWindowTextValue(dialog->driver_);
            if (const auto error = dialog->asioService_.openControlPanel(window, driverText == kNoAsioDrivers ? L"" : driverText)) {
                MessageBoxW(window, error->c_str(), L"ASIO Control Panel", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (commandId == kCloseControlId) {
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

void SettingsDialog::createControls() {
    const auto drivers = asioService_.enumerateDrivers();

    CreateWindowW(L"STATIC", L"ASIO device", WS_CHILD | WS_VISIBLE, 20, 20, 140, 20, window_, nullptr, nullptr, nullptr);
    driver_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                            170, 16, 240, 240, window_, reinterpret_cast<HMENU>(kDriverControlId), nullptr, nullptr);
    for (const auto& driverName : drivers) {
        SendMessageW(driver_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(driverName.c_str()));
    }
    if (drivers.empty()) {
        SendMessageW(driver_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kNoAsioDrivers));
    }
    selectComboBoxString(driver_, toWide(settings_.driver));
    SendMessageW(driver_, CB_SETDROPPEDWIDTH, 320, 0);

    CreateWindowW(L"STATIC", L"Mic input channel", WS_CHILD | WS_VISIBLE, 20, 56, 140, 20, window_, nullptr, nullptr, nullptr);
    mic_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                         170, 52, 240, 220, window_, reinterpret_cast<HMENU>(kMicControlId), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Left output channel", WS_CHILD | WS_VISIBLE, 20, 92, 140, 20, window_, nullptr, nullptr, nullptr);
    left_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                          170, 88, 240, 220, window_, reinterpret_cast<HMENU>(kLeftControlId), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Right output channel", WS_CHILD | WS_VISIBLE, 20, 128, 140, 20, window_, nullptr, nullptr, nullptr);
    right_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                           170, 124, 240, 220, window_, reinterpret_cast<HMENU>(kRightControlId), nullptr, nullptr);
    SendMessageW(mic_, CB_SETDROPPEDWIDTH, 320, 0);
    SendMessageW(left_, CB_SETDROPPEDWIDTH, 320, 0);
    SendMessageW(right_, CB_SETDROPPEDWIDTH, 320, 0);
    populateChannelCombos();

    CreateWindowW(L"STATIC", L"Mic calibration file", WS_CHILD | WS_VISIBLE, 20, 164, 140, 20, window_, nullptr, nullptr, nullptr);
    micCalibrationPath_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                          170, 160, 280, 24, window_, nullptr, nullptr, nullptr);
    SetWindowTextW(micCalibrationPath_, settings_.microphoneCalibrationPath.wstring().c_str());
    CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 460, 158, 74, 28, window_,
                  reinterpret_cast<HMENU>(kMicCalibrationBrowseId), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 542, 158, 58, 28, window_,
                  reinterpret_cast<HMENU>(kMicCalibrationClearId), nullptr, nullptr);

    CreateWindowW(L"BUTTON", L"Open ASIO Control Panel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 212, 180, 28, window_,
                  reinterpret_cast<HMENU>(kOpenControlPanelId), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 520, 212, 80, 28, window_,
                  reinterpret_cast<HMENU>(kCloseControlId), nullptr, nullptr);
}

void SettingsDialog::populateChannelCombos() {
    const std::wstring driverText = getWindowTextValue(driver_);
    SendMessageW(mic_, CB_RESETCONTENT, 0, 0);
    SendMessageW(left_, CB_RESETCONTENT, 0, 0);
    SendMessageW(right_, CB_RESETCONTENT, 0, 0);

    audio::AsioChannelListing channels;
    if (!driverText.empty() && driverText != kNoAsioDrivers) {
        channels = asioService_.enumerateChannels(window_, driverText);
    }

    auto populateCombo = [](HWND combo, const std::vector<audio::AsioChannel>& options, int currentChannel) {
        for (const auto& channel : options) {
            addChannelComboItem(combo, channel.number, channel.name);
        }
        if (SendMessageW(combo, CB_GETCOUNT, 0, 0) == 0) {
            addChannelComboItem(combo, currentChannel, formatSavedChannelLabel(currentChannel));
        }
        selectComboBoxItemData(combo, currentChannel);
    };

    populateCombo(mic_, channels.inputs, settings_.micInputChannel);
    populateCombo(left_, channels.outputs, settings_.leftOutputChannel);
    populateCombo(right_, channels.outputs, settings_.rightOutputChannel);
}

void SettingsDialog::applyAndNotify() {
    const std::wstring driverText = getWindowTextValue(driver_);
    if (driverText != kNoAsioDrivers) {
        settings_.driver = toUtf8(driverText);
    }

    settings_.micInputChannel = selectedChannelNumber(mic_, settings_.micInputChannel);
    settings_.leftOutputChannel = selectedChannelNumber(left_, settings_.leftOutputChannel);
    settings_.rightOutputChannel = selectedChannelNumber(right_, settings_.rightOutputChannel);
    settings_.microphoneCalibrationPath = std::filesystem::path(getWindowTextValue(micCalibrationPath_));
    settings_.microphoneCalibrationFrequencyHz.clear();
    settings_.microphoneCalibrationCorrectionDb.clear();

    if (onSave_) {
        onSave_(settings_);
    }
}

}  // namespace wolfie::ui
