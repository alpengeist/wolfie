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
constexpr wchar_t kNoWindowsInputs[] = L"(No Windows input devices found)";
constexpr wchar_t kNoWindowsOutputs[] = L"(No Windows output devices found)";
constexpr int kBackendControlId = 1;
constexpr int kWindowsInputControlId = 2;
constexpr int kWindowsOutputControlId = 3;
constexpr int kDriverControlId = 4;
constexpr int kMicControlId = 5;
constexpr int kLoopbackEnabledControlId = 6;
constexpr int kLoopbackControlId = 7;
constexpr int kLeftControlId = 8;
constexpr int kRightControlId = 9;
constexpr int kMicCalibrationBrowseId = 10;
constexpr int kMicCalibrationClearId = 11;
constexpr int kOpenControlPanelId = 12;
constexpr int kCloseControlId = 13;

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

void addWasapiDeviceItem(HWND combo, const audio::WasapiDevice& device, size_t deviceIndex) {
    const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
    if (index != CB_ERR) {
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(deviceIndex));
    }
}

const audio::WasapiDevice* selectedWasapiDevice(HWND combo, const std::vector<audio::WasapiDevice>& devices) {
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        return nullptr;
    }

    const LRESULT itemData = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
    if (itemData < 0 || static_cast<size_t>(itemData) >= devices.size()) {
        return nullptr;
    }
    return &devices[static_cast<size_t>(itemData)];
}

void selectWasapiDevice(HWND combo, const std::vector<audio::WasapiDevice>& devices, const std::string& deviceId) {
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        const LRESULT itemData = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
        if (itemData < 0 || static_cast<size_t>(itemData) >= devices.size()) {
            continue;
        }
        if (devices[static_cast<size_t>(itemData)].id == deviceId) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            return;
        }
    }

    if (count > 0) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
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
                          const audio::WasapiService& wasapiService,
                          const audio::AsioService& asioService,
                          SaveCallback onSave) {
    registerSettingsWindowClass(instance);
    auto* dialog = new SettingsDialog(instance, owner, settings, wasapiService, asioService, std::move(onSave));
    dialog->window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClassName, L"Measurement Settings",
                                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 700, 452,
                                      owner, nullptr, instance, dialog);
    if (dialog->window_ == nullptr) {
        delete dialog;
    }
}

SettingsDialog::SettingsDialog(HINSTANCE instance,
                               HWND owner,
                               AudioSettings settings,
                               const audio::WasapiService& wasapiService,
                               const audio::AsioService& asioService,
                               SaveCallback onSave)
    : instance_(instance),
      owner_(owner),
      settings_(std::move(settings)),
      wasapiService_(wasapiService),
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
        if (commandId == kBackendControlId && notificationCode == CBN_SELCHANGE) {
            dialog->populateChannelCombos();
            dialog->refreshControlStates();
            dialog->applyAndNotify();
            return 0;
        }
        if ((commandId == kWindowsInputControlId || commandId == kWindowsOutputControlId) && notificationCode == CBN_SELCHANGE) {
            dialog->populateChannelCombos();
            dialog->applyAndNotify();
            return 0;
        }
        if (commandId == kDriverControlId && notificationCode == CBN_SELCHANGE) {
            dialog->populateChannelCombos();
            dialog->refreshControlStates();
            dialog->applyAndNotify();
            return 0;
        }
        if (commandId == kLoopbackEnabledControlId && notificationCode == BN_CLICKED) {
            dialog->refreshControlStates();
            dialog->applyAndNotify();
            return 0;
        }
        if ((commandId == kMicControlId || commandId == kLoopbackControlId ||
             commandId == kLeftControlId || commandId == kRightControlId) &&
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
    constexpr int kLabelLeft = 20;
    constexpr int kLabelWidth = 190;
    constexpr int kControlLeft = 220;
    constexpr int kComboSmallWidth = 240;
    constexpr int kComboMediumWidth = 320;
    constexpr int kComboWideWidth = 420;
    constexpr int kBrowseLeft = 520;
    constexpr int kClearLeft = 602;

    CreateWindowW(L"STATIC", L"Audio backend", WS_CHILD | WS_VISIBLE, kLabelLeft, 20, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    backend_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                             kControlLeft, 16, kComboSmallWidth, 120, window_, reinterpret_cast<HMENU>(kBackendControlId), nullptr, nullptr);
    SendMessageW(backend_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Windows Audio (WASAPI)"));
    SendMessageW(backend_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ASIO"));
    selectComboBoxString(backend_, settings_.backend == "asio" ? L"ASIO" : L"Windows Audio (WASAPI)");

    CreateWindowW(L"STATIC", L"Windows input device", WS_CHILD | WS_VISIBLE, kLabelLeft, 56, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    windowsInput_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  kControlLeft, 52, kComboWideWidth, 240, window_, reinterpret_cast<HMENU>(kWindowsInputControlId), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Windows output device", WS_CHILD | WS_VISIBLE, kLabelLeft, 92, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    windowsOutput_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   kControlLeft, 88, kComboWideWidth, 240, window_, reinterpret_cast<HMENU>(kWindowsOutputControlId), nullptr, nullptr);
    SendMessageW(windowsInput_, CB_SETDROPPEDWIDTH, 520, 0);
    SendMessageW(windowsOutput_, CB_SETDROPPEDWIDTH, 520, 0);
    populateWindowsDeviceCombos();

    const auto drivers = asioService_.enumerateDrivers();
    CreateWindowW(L"STATIC", L"ASIO device", WS_CHILD | WS_VISIBLE, kLabelLeft, 136, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    driver_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                            kControlLeft, 132, kComboMediumWidth, 240, window_, reinterpret_cast<HMENU>(kDriverControlId), nullptr, nullptr);
    for (const auto& driverName : drivers) {
        SendMessageW(driver_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(driverName.c_str()));
    }
    if (drivers.empty()) {
        SendMessageW(driver_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kNoAsioDrivers));
    }
    selectComboBoxString(driver_, toWide(settings_.driver));
    SendMessageW(driver_, CB_SETDROPPEDWIDTH, 420, 0);

    CreateWindowW(L"STATIC", L"Mic input channel", WS_CHILD | WS_VISIBLE, kLabelLeft, 172, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    mic_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                         kControlLeft, 168, kComboSmallWidth, 220, window_, reinterpret_cast<HMENU>(kMicControlId), nullptr, nullptr);
    loopbackEnabled_ = CreateWindowW(L"BUTTON",
                                     L"Enable reference loopback",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     kLabelLeft,
                                     204,
                                     kLabelWidth,
                                     20,
                                     window_,
                                     reinterpret_cast<HMENU>(kLoopbackEnabledControlId),
                                     nullptr,
                                     nullptr);
    CreateWindowW(L"STATIC", L"Reference input channel", WS_CHILD | WS_VISIBLE, kLabelLeft, 232, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    loopback_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                              kControlLeft, 228, kComboSmallWidth, 220, window_, reinterpret_cast<HMENU>(kLoopbackControlId), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Left output channel", WS_CHILD | WS_VISIBLE, kLabelLeft, 268, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    left_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                          kControlLeft, 264, kComboSmallWidth, 220, window_, reinterpret_cast<HMENU>(kLeftControlId), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Right output channel", WS_CHILD | WS_VISIBLE, kLabelLeft, 304, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    right_ = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                           kControlLeft, 300, kComboSmallWidth, 220, window_, reinterpret_cast<HMENU>(kRightControlId), nullptr, nullptr);
    SendMessageW(mic_, CB_SETDROPPEDWIDTH, 320, 0);
    SendMessageW(loopback_, CB_SETDROPPEDWIDTH, 320, 0);
    SendMessageW(left_, CB_SETDROPPEDWIDTH, 320, 0);
    SendMessageW(right_, CB_SETDROPPEDWIDTH, 320, 0);
    populateChannelCombos();
    SendMessageW(loopbackEnabled_, BM_SETCHECK, settings_.loopbackEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindowW(L"STATIC", L"Mic calibration file", WS_CHILD | WS_VISIBLE, kLabelLeft, 344, kLabelWidth, 20, window_, nullptr, nullptr, nullptr);
    micCalibrationPath_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                          kControlLeft, 340, 340, 24, window_, nullptr, nullptr, nullptr);
    SetWindowTextW(micCalibrationPath_, settings_.microphoneCalibrationPath.wstring().c_str());
    CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, kBrowseLeft, 338, 74, 28, window_,
                  reinterpret_cast<HMENU>(kMicCalibrationBrowseId), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, kClearLeft, 338, 58, 28, window_,
                  reinterpret_cast<HMENU>(kMicCalibrationClearId), nullptr, nullptr);

    CreateWindowW(L"BUTTON", L"Open ASIO Control Panel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 390, 180, 28, window_,
                  reinterpret_cast<HMENU>(kOpenControlPanelId), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 580, 390, 80, 28, window_,
                  reinterpret_cast<HMENU>(kCloseControlId), nullptr, nullptr);

    refreshControlStates();
}

void SettingsDialog::populateWindowsDeviceCombos() {
    windowsInputs_ = wasapiService_.enumerateInputDevices();
    windowsOutputs_ = wasapiService_.enumerateOutputDevices();

    SendMessageW(windowsInput_, CB_RESETCONTENT, 0, 0);
    SendMessageW(windowsOutput_, CB_RESETCONTENT, 0, 0);

    for (size_t index = 0; index < windowsInputs_.size(); ++index) {
        addWasapiDeviceItem(windowsInput_, windowsInputs_[index], index);
    }
    for (size_t index = 0; index < windowsOutputs_.size(); ++index) {
        addWasapiDeviceItem(windowsOutput_, windowsOutputs_[index], index);
    }

    if (windowsInputs_.empty()) {
        SendMessageW(windowsInput_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kNoWindowsInputs));
    } else {
        selectWasapiDevice(windowsInput_, windowsInputs_, settings_.windowsInputDeviceId);
    }

    if (windowsOutputs_.empty()) {
        SendMessageW(windowsOutput_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kNoWindowsOutputs));
    } else {
        selectWasapiDevice(windowsOutput_, windowsOutputs_, settings_.windowsOutputDeviceId);
    }
}

void SettingsDialog::populateChannelCombos() {
    if (!usingAsioBackend()) {
        populateWasapiChannelCombos();
        return;
    }

    const std::wstring driverText = getWindowTextValue(driver_);
    SendMessageW(mic_, CB_RESETCONTENT, 0, 0);
    SendMessageW(loopback_, CB_RESETCONTENT, 0, 0);
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
    populateCombo(loopback_, channels.inputs, settings_.loopbackInputChannel);
    populateCombo(left_, channels.outputs, settings_.leftOutputChannel);
    populateCombo(right_, channels.outputs, settings_.rightOutputChannel);
}

void SettingsDialog::populateWasapiChannelCombos() {
    SendMessageW(mic_, CB_RESETCONTENT, 0, 0);
    SendMessageW(loopback_, CB_RESETCONTENT, 0, 0);
    SendMessageW(left_, CB_RESETCONTENT, 0, 0);
    SendMessageW(right_, CB_RESETCONTENT, 0, 0);

    const auto populateCombo = [](HWND combo, int channelCount, int currentChannel) {
        const int safeCount = std::max(channelCount, 0);
        for (int channel = 1; channel <= safeCount; ++channel) {
            addChannelComboItem(combo, channel, formatSavedChannelLabel(channel));
        }
        if (SendMessageW(combo, CB_GETCOUNT, 0, 0) == 0) {
            addChannelComboItem(combo, currentChannel, formatSavedChannelLabel(currentChannel));
        }
        selectComboBoxItemData(combo, currentChannel);
    };

    const audio::WasapiDevice* inputDevice = selectedWasapiDevice(windowsInput_, windowsInputs_);
    const audio::WasapiDevice* outputDevice = selectedWasapiDevice(windowsOutput_, windowsOutputs_);
    populateCombo(mic_, inputDevice != nullptr ? inputDevice->channelCount : 0, settings_.micInputChannel);
    populateCombo(loopback_, inputDevice != nullptr ? inputDevice->channelCount : 0, settings_.loopbackInputChannel);
    populateCombo(left_, outputDevice != nullptr ? outputDevice->channelCount : 0, settings_.leftOutputChannel);
    populateCombo(right_, outputDevice != nullptr ? outputDevice->channelCount : 0, settings_.rightOutputChannel);
}

bool SettingsDialog::usingAsioBackend() const {
    return getWindowTextValue(backend_) == L"ASIO";
}

void SettingsDialog::refreshControlStates() {
    const bool useAsio = usingAsioBackend();
    EnableWindow(windowsInput_, useAsio ? FALSE : TRUE);
    EnableWindow(windowsOutput_, useAsio ? FALSE : TRUE);
    EnableWindow(driver_, useAsio ? TRUE : FALSE);
    EnableWindow(mic_, TRUE);
    EnableWindow(loopback_, SendMessageW(loopbackEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED ? TRUE : FALSE);
    EnableWindow(left_, TRUE);
    EnableWindow(right_, TRUE);
    EnableWindow(GetDlgItem(window_, kOpenControlPanelId), useAsio ? TRUE : FALSE);
}

void SettingsDialog::applyAndNotify() {
    settings_.backend = usingAsioBackend() ? "asio" : "windows";

    const LRESULT windowsInputIndex = SendMessageW(windowsInput_, CB_GETCURSEL, 0, 0);
    if (windowsInputIndex != CB_ERR) {
        const LRESULT itemData = SendMessageW(windowsInput_, CB_GETITEMDATA, static_cast<WPARAM>(windowsInputIndex), 0);
        if (itemData >= 0 && static_cast<size_t>(itemData) < windowsInputs_.size()) {
            const auto& device = windowsInputs_[static_cast<size_t>(itemData)];
            settings_.windowsInputDeviceId = device.id;
            settings_.windowsInputDeviceName = toUtf8(device.name);
        }
    }

    const LRESULT windowsOutputIndex = SendMessageW(windowsOutput_, CB_GETCURSEL, 0, 0);
    if (windowsOutputIndex != CB_ERR) {
        const LRESULT itemData = SendMessageW(windowsOutput_, CB_GETITEMDATA, static_cast<WPARAM>(windowsOutputIndex), 0);
        if (itemData >= 0 && static_cast<size_t>(itemData) < windowsOutputs_.size()) {
            const auto& device = windowsOutputs_[static_cast<size_t>(itemData)];
            settings_.windowsOutputDeviceId = device.id;
            settings_.windowsOutputDeviceName = toUtf8(device.name);
        }
    }

    const std::wstring driverText = getWindowTextValue(driver_);
    if (driverText != kNoAsioDrivers) {
        settings_.driver = toUtf8(driverText);
    }

    settings_.micInputChannel = selectedChannelNumber(mic_, settings_.micInputChannel);
    settings_.loopbackEnabled = SendMessageW(loopbackEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_.loopbackInputChannel = selectedChannelNumber(loopback_, settings_.loopbackInputChannel);
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
