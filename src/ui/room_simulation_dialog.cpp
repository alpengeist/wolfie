#include "ui/room_simulation_dialog.h"

#include <algorithm>
#include <cwchar>

#include <commctrl.h>

#include "core/text_utils.h"
#include "measurement/room_simulator.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr wchar_t kWindowClassName[] = L"WolfieRoomSimulationWindow";
constexpr wchar_t kNamePromptClassName[] = L"WolfieRoomSimulationNamePrompt";
constexpr int kComboSimulations = 1;
constexpr int kButtonNew = 2;
constexpr int kButtonGenerate = 3;

constexpr int kEditStereoSkew = 20;
constexpr int kEditSpectralTilt = 21;
constexpr int kEditLowShelfGain = 22;
constexpr int kEditLowShelfCorner = 23;
constexpr int kEditModalPeakFrequency = 24;
constexpr int kEditModalPeakGain = 25;
constexpr int kEditModalPeakQ = 26;
constexpr int kEditModalNullFrequency = 27;
constexpr int kEditModalNullDepth = 28;
constexpr int kEditModalNullQ = 29;
constexpr int kEditReflectionCount = 30;
constexpr int kEditReflectionStart = 31;
constexpr int kEditReflectionSpacing = 32;
constexpr int kEditReflectionDecay = 33;
constexpr int kEditLateDecayRt60 = 34;
constexpr int kEditLateDecayStart = 35;
constexpr int kEditLateDensity = 36;
constexpr int kEditNoiseFloor = 37;
constexpr int kEditSeed = 38;

void registerWindowClass(HINSTANCE instance) {
    WNDCLASSW dialogClass{};
    dialogClass.lpfnWndProc = RoomSimulationDialog::WindowProc;
    dialogClass.hInstance = instance;
    dialogClass.lpszClassName = kWindowClassName;
    dialogClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    dialogClass.hbrBackground = ui_theme::backgroundBrush();
    RegisterClassW(&dialogClass);
}

LRESULT CALLBACK NamePromptWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<RoomSimulationDialog::NamePromptDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring value(length + 1, L'\0');
            GetWindowTextW(state->edit, value.data(), length + 1);
            value.resize(length);
            state->value = std::move(value);
            state->accepted = true;
            state->finished = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            state->finished = true;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        state->finished = true;
        DestroyWindow(window);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ensureNamePromptWindowClass(HINSTANCE instance) {
    WNDCLASSW promptClass{};
    promptClass.lpfnWndProc = NamePromptWindowProc;
    promptClass.hInstance = instance;
    promptClass.lpszClassName = kNamePromptClassName;
    promptClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    promptClass.hbrBackground = ui_theme::backgroundBrush();
    RegisterClassW(&promptClass);
}

struct FieldDefinition {
    const wchar_t* label;
    const wchar_t* unit;
    const wchar_t* tooltip;
    int controlId;
    HWND* edit;
};

HWND createTooltipWindow(HWND parent, HINSTANCE instance) {
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST,
                                   TOOLTIPS_CLASSW,
                                   nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT,
                                   CW_USEDEFAULT,
                                   CW_USEDEFAULT,
                                   CW_USEDEFAULT,
                                   parent,
                                   nullptr,
                                   instance,
                                   nullptr);
    if (tooltip == nullptr) {
        return nullptr;
    }

    SetWindowPos(tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 320);
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 700);
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_RESHOW, 150);
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 12000);
    return tooltip;
}

void addLabelTooltip(HWND tooltip, HWND label, const wchar_t* text) {
    if (tooltip == nullptr || label == nullptr || text == nullptr || text[0] == L'\0') {
        return;
    }

    TOOLINFOW toolInfo{};
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    toolInfo.hwnd = GetParent(label);
    toolInfo.uId = reinterpret_cast<UINT_PTR>(label);
    toolInfo.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&toolInfo));
}

}  // namespace

void RoomSimulationDialog::show(HINSTANCE instance,
                                HWND owner,
                                SaveCallback onSave,
                                GenerateCallback onGenerate) {
    instance_ = instance;
    owner_ = owner;
    onSave_ = std::move(onSave);
    onGenerate_ = std::move(onGenerate);

    if (window_ != nullptr) {
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        return;
    }

    registerWindowClass(instance);
    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME,
                              kWindowClassName,
                              L"Room Simulation",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              760,
                              500,
                              owner,
                              nullptr,
                              instance,
                              this);
}

void RoomSimulationDialog::close() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

void RoomSimulationDialog::populate(const WorkspaceState& workspace) {
    simulations_ = workspace.roomSimulations;
    if (!workspace.activeRoomSimulationName.empty() &&
        simulationIndexByName(workspace.activeRoomSimulationName) >= 0) {
        activeSimulationName_ = workspace.activeRoomSimulationName;
    } else if (!simulations_.empty()) {
        activeSimulationName_ = simulations_.front().name;
    } else {
        activeSimulationName_.clear();
    }

    const int activeIndex = simulationIndexByName(activeSimulationName_);
    if (activeIndex >= 0) {
        currentSettings_ = simulations_[static_cast<size_t>(activeIndex)].settings;
    } else {
        currentSettings_ = measurement::defaultRoomSimulationSettings();
    }

    if (window_ != nullptr) {
        refreshComboItems();
        refreshFieldValues();
        refreshGenerateButton();
    }
}

LRESULT CALLBACK RoomSimulationDialog::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    RoomSimulationDialog* dialog = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        dialog = reinterpret_cast<RoomSimulationDialog*>(create->lpCreateParams);
        dialog->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        return TRUE;
    }

    dialog = reinterpret_cast<RoomSimulationDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (dialog == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_CREATE:
        dialog->createControls();
        dialog->layoutControls();
        dialog->refreshComboItems();
        dialog->refreshFieldValues();
        dialog->refreshGenerateButton();
        return 0;
    case WM_SIZE:
        dialog->layoutControls();
        return 0;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    }
    case WM_COMMAND: {
        const WORD commandId = LOWORD(wParam);
        const WORD notificationCode = HIWORD(wParam);
        if (commandId == kComboSimulations && notificationCode == CBN_SELCHANGE && !dialog->populatingControls_) {
            if (!dialog->persistCurrentSimulation(true)) {
                dialog->refreshComboItems();
                return 0;
            }
            dialog->selectSimulationByName(dialog->selectedSimulationNameFromCombo());
            dialog->refreshFieldValues();
            dialog->refreshGenerateButton();
            return 0;
        }
        if (commandId == kButtonNew && notificationCode == BN_CLICKED) {
            dialog->onNewSimulation();
            return 0;
        }
        if (commandId == kButtonGenerate && notificationCode == BN_CLICKED) {
            dialog->onGenerate();
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (dialog->persistCurrentSimulation(true)) {
            DestroyWindow(window);
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        dialog->tooltip_ = nullptr;
        dialog->window_ = nullptr;
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

std::wstring RoomSimulationDialog::requestSimulationName(HWND owner, HINSTANCE instance) {
    ensureNamePromptWindowClass(instance);

    NamePromptDialogState state;
    const int dialogWidth = 360;
    const int dialogHeight = 144;
    RECT ownerRect{0, 0, dialogWidth, dialogHeight};
    if (owner != nullptr) {
        GetWindowRect(owner, &ownerRect);
    }
    const int x = ownerRect.left + std::max(0L, ((ownerRect.right - ownerRect.left) - dialogWidth) / 2);
    const int y = ownerRect.top + std::max(0L, ((ownerRect.bottom - ownerRect.top) - dialogHeight) / 2);

    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kNamePromptClassName,
                                  L"New Room Simulation",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  dialogWidth,
                                  dialogHeight,
                                  owner,
                                  nullptr,
                                  instance,
                                  &state);
    if (dialog == nullptr) {
        return {};
    }

    CreateWindowW(L"STATIC", L"Simulation name", WS_CHILD | WS_VISIBLE, 16, 16, 120, 18, dialog, nullptr, instance, nullptr);
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE,
                                 L"EDIT",
                                 L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 16,
                                 38,
                                 312,
                                 24,
                                 dialog,
                                 nullptr,
                                 instance,
                                 nullptr);
    CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 168, 78, 72, 26, dialog, reinterpret_cast<HMENU>(IDOK), instance, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 256, 78, 72, 26, dialog, reinterpret_cast<HMENU>(IDCANCEL), instance, nullptr);

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(state.edit);

    MSG message{};
    while (!state.finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return state.accepted ? state.value : std::wstring();
}

std::wstring RoomSimulationDialog::getWindowTextValue(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}

void RoomSimulationDialog::setWindowTextValue(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

bool RoomSimulationDialog::isValidSimulationName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (name.back() == ' ' || name.back() == '.') {
        return false;
    }
    for (const char ch : name) {
        if (ch < 32 || ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            return false;
        }
    }
    return true;
}

bool RoomSimulationDialog::tryParseDouble(const std::wstring& text, double& value) {
    try {
        size_t cursor = 0;
        value = std::stod(text, &cursor);
        return cursor == text.size();
    } catch (...) {
        return false;
    }
}

bool RoomSimulationDialog::tryParseInt(const std::wstring& text, int& value) {
    try {
        size_t cursor = 0;
        value = std::stoi(text, &cursor);
        return cursor == text.size();
    } catch (...) {
        return false;
    }
}

void RoomSimulationDialog::createControls() {
    fieldLabels_.clear();
    fieldUnits_.clear();
    tooltip_ = createTooltipWindow(window_, instance_);

    controls_.labelSimulation = CreateWindowW(L"STATIC", L"Simulation", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboSimulations = CreateWindowW(L"COMBOBOX",
                                               nullptr,
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                               0,
                                               0,
                                               0,
                                               0,
                                               window_,
                                               reinterpret_cast<HMENU>(kComboSimulations),
                                               instance_,
                                               nullptr);
    controls_.buttonNew = CreateWindowW(L"BUTTON",
                                        L"New",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        0,
                                        0,
                                        0,
                                        0,
                                        window_,
                                        reinterpret_cast<HMENU>(kButtonNew),
                                        instance_,
                                        nullptr);
    controls_.buttonGenerate = CreateWindowW(L"BUTTON",
                                             L"Generate",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                             0,
                                             0,
                                             0,
                                             0,
                                             window_,
                                             reinterpret_cast<HMENU>(kButtonGenerate),
                                             instance_,
                                             nullptr);

    const FieldDefinition fields[] = {
        {L"Stereo Skew", L"ms", L"Offsets one stereo channel in time to simulate asymmetric speaker placement or arrival time.", kEditStereoSkew, &controls_.editStereoSkew},
        {L"Spectral Tilt", L"dB/oct", L"Applies a broadband tonal slope so the simulated room gets brighter or darker across the spectrum.", kEditSpectralTilt, &controls_.editSpectralTilt},
        {L"LF Shelf Gain", L"dB", L"Boosts or cuts the low-frequency shelf that shapes overall bass balance in the simulated room.", kEditLowShelfGain, &controls_.editLowShelfGain},
        {L"LF Shelf Corner", L"Hz", L"Sets the turnover frequency where the low-frequency shelf begins to take effect.", kEditLowShelfCorner, &controls_.editLowShelfCorner},
        {L"Peak Freq", L"Hz", L"Chooses the center frequency of the main resonant room mode peak.", kEditModalPeakFrequency, &controls_.editModalPeakFrequency},
        {L"Peak Gain", L"dB", L"Sets how strongly the main resonant room mode is boosted at its center frequency.", kEditModalPeakGain, &controls_.editModalPeakGain},
        {L"Peak Q", L"", L"Controls how narrow or broad the resonant room mode peak is around its center frequency.", kEditModalPeakQ, &controls_.editModalPeakQ},
        {L"Null Freq", L"Hz", L"Chooses the center frequency of the main cancellation null in the simulated response.", kEditModalNullFrequency, &controls_.editModalNullFrequency},
        {L"Null Depth", L"dB", L"Sets how deep the simulated cancellation null dips below the surrounding response.", kEditModalNullDepth, &controls_.editModalNullDepth},
        {L"Null Q", L"", L"Controls how narrow or broad the simulated cancellation null is around its center frequency.", kEditModalNullQ, &controls_.editModalNullQ},
        {L"Reflections", L"count", L"Sets how many discrete early reflections are added before the late reverberant tail.", kEditReflectionCount, &controls_.editReflectionCount},
        {L"Reflection Start", L"ms", L"Sets when the first early reflection arrives after the direct sound.", kEditReflectionStart, &controls_.editReflectionStart},
        {L"Reflection Spacing", L"ms", L"Sets the average time gap between successive early reflections.", kEditReflectionSpacing, &controls_.editReflectionSpacing},
        {L"Reflection Decay", L"dB/tap", L"Sets how much each early reflection level drops relative to the previous one.", kEditReflectionDecay, &controls_.editReflectionDecay},
        {L"Late RT60", L"ms", L"Sets the decay time for the late reverberant field to fall by about 60 dB.", kEditLateDecayRt60, &controls_.editLateDecayRt60},
        {L"Late Tail Start", L"dB", L"Sets the starting level of the late reverberant tail relative to the direct sound.", kEditLateDecayStart, &controls_.editLateDecayStart},
        {L"Late Density", L"taps/s", L"Sets how densely late reflections are packed into the reverberant tail.", kEditLateDensity, &controls_.editLateDensity},
        {L"Noise Floor", L"dBFS", L"Sets the background noise level that the simulated response settles into.", kEditNoiseFloor, &controls_.editNoiseFloor},
        {L"Seed", L"", L"Changes the random pattern used for reflections and noise while keeping the same overall parameter values.", kEditSeed, &controls_.editSeed},
    };

    for (const FieldDefinition& field : fields) {
        HWND label = CreateWindowW(L"STATIC",
                                   field.label,
                                   WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                   0,
                                   0,
                                   0,
                                   0,
                                   window_,
                                   nullptr,
                                   instance_,
                                   nullptr);
        fieldLabels_.push_back(label);
        addLabelTooltip(tooltip_, label, field.tooltip);
        *field.edit = CreateWindowExW(WS_EX_CLIENTEDGE,
                                      L"EDIT",
                                      L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      0,
                                      0,
                                      0,
                                      0,
                                      window_,
                                      reinterpret_cast<HMENU>(field.controlId),
                                      instance_,
                                      nullptr);
        if (field.unit != nullptr && field.unit[0] != L'\0') {
            fieldUnits_.push_back(CreateWindowW(L"STATIC", field.unit, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr));
        } else {
            fieldUnits_.push_back(nullptr);
        }
    }
}

void RoomSimulationDialog::layoutControls() const {
    if (window_ == nullptr) {
        return;
    }

    RECT rect{};
    GetClientRect(window_, &rect);
    const int contentLeft = 18;
    const int contentTop = 18;
    const int contentWidth = std::max(640L, rect.right - (contentLeft * 2));
    const int comboWidth = 340;
    const int comboHeight = 220;
    const int comboLeft = contentLeft + 82;
    MoveWindow(controls_.labelSimulation, contentLeft, contentTop + 5, 72, 18, TRUE);
    MoveWindow(controls_.comboSimulations, comboLeft, contentTop, comboWidth, comboHeight, TRUE);
    MoveWindow(controls_.buttonNew, comboLeft + comboWidth + 12, contentTop - 1, 72, 28, TRUE);
    MoveWindow(controls_.buttonGenerate, rect.right - 122, contentTop - 1, 104, 28, TRUE);

    const FieldDefinition fields[] = {
        {L"Stereo Skew", L"ms", nullptr, kEditStereoSkew, const_cast<HWND*>(&controls_.editStereoSkew)},
        {L"Spectral Tilt", L"dB/oct", nullptr, kEditSpectralTilt, const_cast<HWND*>(&controls_.editSpectralTilt)},
        {L"LF Shelf Gain", L"dB", nullptr, kEditLowShelfGain, const_cast<HWND*>(&controls_.editLowShelfGain)},
        {L"LF Shelf Corner", L"Hz", nullptr, kEditLowShelfCorner, const_cast<HWND*>(&controls_.editLowShelfCorner)},
        {L"Peak Freq", L"Hz", nullptr, kEditModalPeakFrequency, const_cast<HWND*>(&controls_.editModalPeakFrequency)},
        {L"Peak Gain", L"dB", nullptr, kEditModalPeakGain, const_cast<HWND*>(&controls_.editModalPeakGain)},
        {L"Peak Q", L"", nullptr, kEditModalPeakQ, const_cast<HWND*>(&controls_.editModalPeakQ)},
        {L"Null Freq", L"Hz", nullptr, kEditModalNullFrequency, const_cast<HWND*>(&controls_.editModalNullFrequency)},
        {L"Null Depth", L"dB", nullptr, kEditModalNullDepth, const_cast<HWND*>(&controls_.editModalNullDepth)},
        {L"Null Q", L"", nullptr, kEditModalNullQ, const_cast<HWND*>(&controls_.editModalNullQ)},
        {L"Reflections", L"count", nullptr, kEditReflectionCount, const_cast<HWND*>(&controls_.editReflectionCount)},
        {L"Reflection Start", L"ms", nullptr, kEditReflectionStart, const_cast<HWND*>(&controls_.editReflectionStart)},
        {L"Reflection Spacing", L"ms", nullptr, kEditReflectionSpacing, const_cast<HWND*>(&controls_.editReflectionSpacing)},
        {L"Reflection Decay", L"dB/tap", nullptr, kEditReflectionDecay, const_cast<HWND*>(&controls_.editReflectionDecay)},
        {L"Late RT60", L"ms", nullptr, kEditLateDecayRt60, const_cast<HWND*>(&controls_.editLateDecayRt60)},
        {L"Late Tail Start", L"dB", nullptr, kEditLateDecayStart, const_cast<HWND*>(&controls_.editLateDecayStart)},
        {L"Late Density", L"taps/s", nullptr, kEditLateDensity, const_cast<HWND*>(&controls_.editLateDensity)},
        {L"Noise Floor", L"dBFS", nullptr, kEditNoiseFloor, const_cast<HWND*>(&controls_.editNoiseFloor)},
        {L"Seed", L"", nullptr, kEditSeed, const_cast<HWND*>(&controls_.editSeed)},
    };

    const int fieldsTop = contentTop + 48;
    const int columnWidth = (contentWidth - 30) / 2;
    const int labelWidth = 118;
    const int editWidth = 88;
    const int unitWidth = 64;
    const int rowHeight = 34;
    for (size_t index = 0; index < std::size(fields); ++index) {
        const int row = static_cast<int>(index % 10);
        const int column = static_cast<int>(index / 10);
        const int left = contentLeft + (column * (columnWidth + 30));
        const int top = fieldsTop + (row * rowHeight);
        MoveWindow(fieldLabels_[index], left, top + 4, labelWidth, 18, TRUE);
        MoveWindow(*fields[index].edit, left + labelWidth + 8, top, editWidth, 24, TRUE);
        if (fieldUnits_[index] != nullptr) {
            MoveWindow(fieldUnits_[index], left + labelWidth + 8 + editWidth + 8, top + 4, unitWidth, 18, TRUE);
        }
    }
}

void RoomSimulationDialog::refreshComboItems() {
    if (controls_.comboSimulations == nullptr) {
        return;
    }

    populatingControls_ = true;
    SendMessageW(controls_.comboSimulations, CB_RESETCONTENT, 0, 0);
    for (const RoomSimulationDefinition& simulation : simulations_) {
        SendMessageW(controls_.comboSimulations, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(toWide(simulation.name).c_str()));
    }

    const int activeIndex = simulationIndexByName(activeSimulationName_);
    if (activeIndex >= 0) {
        SendMessageW(controls_.comboSimulations, CB_SETCURSEL, activeIndex, 0);
    } else {
        SendMessageW(controls_.comboSimulations, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    populatingControls_ = false;
}

void RoomSimulationDialog::refreshFieldValues() {
    measurement::normalizeRoomSimulationSettings(currentSettings_);
    setWindowTextValue(controls_.editStereoSkew, formatWideDouble(currentSettings_.stereoSkewMs, 2));
    setWindowTextValue(controls_.editSpectralTilt, formatWideDouble(currentSettings_.spectralTiltDbPerOctave, 2));
    setWindowTextValue(controls_.editLowShelfGain, formatWideDouble(currentSettings_.lowShelfGainDb, 1));
    setWindowTextValue(controls_.editLowShelfCorner, formatWideDouble(currentSettings_.lowShelfCornerHz, 0));
    setWindowTextValue(controls_.editModalPeakFrequency, formatWideDouble(currentSettings_.modalPeakFrequencyHz, 0));
    setWindowTextValue(controls_.editModalPeakGain, formatWideDouble(currentSettings_.modalPeakGainDb, 1));
    setWindowTextValue(controls_.editModalPeakQ, formatWideDouble(currentSettings_.modalPeakQ, 2));
    setWindowTextValue(controls_.editModalNullFrequency, formatWideDouble(currentSettings_.modalNullFrequencyHz, 0));
    setWindowTextValue(controls_.editModalNullDepth, formatWideDouble(currentSettings_.modalNullDepthDb, 1));
    setWindowTextValue(controls_.editModalNullQ, formatWideDouble(currentSettings_.modalNullQ, 2));
    setWindowTextValue(controls_.editReflectionCount, std::to_wstring(currentSettings_.earlyReflectionCount));
    setWindowTextValue(controls_.editReflectionStart, formatWideDouble(currentSettings_.earlyReflectionStartMs, 1));
    setWindowTextValue(controls_.editReflectionSpacing, formatWideDouble(currentSettings_.earlyReflectionSpacingMs, 1));
    setWindowTextValue(controls_.editReflectionDecay, formatWideDouble(currentSettings_.earlyReflectionDecayDbPerTap, 1));
    setWindowTextValue(controls_.editLateDecayRt60, formatWideDouble(currentSettings_.lateDecayRt60Ms, 0));
    setWindowTextValue(controls_.editLateDecayStart, formatWideDouble(currentSettings_.lateDecayStartDb, 1));
    setWindowTextValue(controls_.editLateDensity, formatWideDouble(currentSettings_.lateDensityPerSecond, 0));
    setWindowTextValue(controls_.editNoiseFloor, formatWideDouble(currentSettings_.noiseFloorDb, 1));
    setWindowTextValue(controls_.editSeed, std::to_wstring(currentSettings_.seed));
}

void RoomSimulationDialog::refreshGenerateButton() {
    EnableWindow(controls_.buttonGenerate, activeSimulationName_.empty() ? FALSE : TRUE);
}

bool RoomSimulationDialog::tryReadSettingsFromControls(RoomSimulationSettings& settings,
                                                       std::wstring& errorMessage) const {
    settings = currentSettings_;
    if (!tryParseDouble(getWindowTextValue(controls_.editStereoSkew), settings.stereoSkewMs)) {
        errorMessage = L"Stereo Skew must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editSpectralTilt), settings.spectralTiltDbPerOctave)) {
        errorMessage = L"Spectral Tilt must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editLowShelfGain), settings.lowShelfGainDb)) {
        errorMessage = L"LF Shelf Gain must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editLowShelfCorner), settings.lowShelfCornerHz)) {
        errorMessage = L"LF Shelf Corner must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editModalPeakFrequency), settings.modalPeakFrequencyHz)) {
        errorMessage = L"Peak Freq must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editModalPeakGain), settings.modalPeakGainDb)) {
        errorMessage = L"Peak Gain must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editModalPeakQ), settings.modalPeakQ)) {
        errorMessage = L"Peak Q must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editModalNullFrequency), settings.modalNullFrequencyHz)) {
        errorMessage = L"Null Freq must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editModalNullDepth), settings.modalNullDepthDb)) {
        errorMessage = L"Null Depth must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editModalNullQ), settings.modalNullQ)) {
        errorMessage = L"Null Q must be a number.";
        return false;
    }
    if (!tryParseInt(getWindowTextValue(controls_.editReflectionCount), settings.earlyReflectionCount)) {
        errorMessage = L"Reflections must be an integer.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editReflectionStart), settings.earlyReflectionStartMs)) {
        errorMessage = L"Reflection Start must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editReflectionSpacing), settings.earlyReflectionSpacingMs)) {
        errorMessage = L"Reflection Spacing must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editReflectionDecay), settings.earlyReflectionDecayDbPerTap)) {
        errorMessage = L"Reflection Decay must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editLateDecayRt60), settings.lateDecayRt60Ms)) {
        errorMessage = L"Late RT60 must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editLateDecayStart), settings.lateDecayStartDb)) {
        errorMessage = L"Late Tail Start must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editLateDensity), settings.lateDensityPerSecond)) {
        errorMessage = L"Late Density must be a number.";
        return false;
    }
    if (!tryParseDouble(getWindowTextValue(controls_.editNoiseFloor), settings.noiseFloorDb)) {
        errorMessage = L"Noise Floor must be a number.";
        return false;
    }
    if (!tryParseInt(getWindowTextValue(controls_.editSeed), settings.seed)) {
        errorMessage = L"Seed must be an integer.";
        return false;
    }

    measurement::normalizeRoomSimulationSettings(settings);
    return true;
}

void RoomSimulationDialog::applyCurrentSettingsToCache(const RoomSimulationSettings& settings) {
    currentSettings_ = settings;
    const int index = simulationIndexByName(activeSimulationName_);
    if (index >= 0) {
        simulations_[static_cast<size_t>(index)].settings = currentSettings_;
    }
}

bool RoomSimulationDialog::persistCurrentSimulation(bool showErrors) {
    if (activeSimulationName_.empty()) {
        return true;
    }

    RoomSimulationSettings settings;
    std::wstring errorMessage;
    if (!tryReadSettingsFromControls(settings, errorMessage)) {
        if (showErrors) {
            MessageBoxW(window_, errorMessage.c_str(), L"Room Simulation", MB_OK | MB_ICONERROR);
        }
        return false;
    }

    applyCurrentSettingsToCache(settings);
    if (onSave_) {
        onSave_(activeSimulationName_, currentSettings_);
    }
    return true;
}

void RoomSimulationDialog::selectSimulationByName(const std::string& name) {
    activeSimulationName_ = name;
    const int index = simulationIndexByName(activeSimulationName_);
    if (index >= 0) {
        currentSettings_ = simulations_[static_cast<size_t>(index)].settings;
    } else {
        currentSettings_ = measurement::defaultRoomSimulationSettings();
    }
}

int RoomSimulationDialog::simulationIndexByName(const std::string& name) const {
    for (size_t index = 0; index < simulations_.size(); ++index) {
        if (simulations_[index].name == name) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

std::string RoomSimulationDialog::selectedSimulationNameFromCombo() const {
    const LRESULT selection = SendMessageW(controls_.comboSimulations, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR || static_cast<size_t>(selection) >= simulations_.size()) {
        return {};
    }
    return simulations_[static_cast<size_t>(selection)].name;
}

void RoomSimulationDialog::onNewSimulation() {
    if (!persistCurrentSimulation(true)) {
        return;
    }

    const std::wstring nameText = requestSimulationName(window_, instance_);
    if (nameText.empty()) {
        return;
    }

    const std::string name = toUtf8(nameText);
    if (!isValidSimulationName(name)) {
        MessageBoxW(window_,
                    L"Invalid simulation name. Use a valid Windows filename.",
                    L"Room Simulation",
                    MB_OK | MB_ICONERROR);
        return;
    }
    if (simulationIndexByName(name) >= 0) {
        MessageBoxW(window_,
                    L"A simulation with that name already exists.",
                    L"Room Simulation",
                    MB_OK | MB_ICONERROR);
        return;
    }

    RoomSimulationDefinition simulation;
    simulation.name = name;
    simulation.settings = measurement::defaultRoomSimulationSettings();
    simulations_.push_back(simulation);
    std::sort(simulations_.begin(), simulations_.end(), [](const RoomSimulationDefinition& left, const RoomSimulationDefinition& right) {
        return left.name < right.name;
    });
    activeSimulationName_ = name;
    currentSettings_ = simulation.settings;
    refreshComboItems();
    refreshFieldValues();
    refreshGenerateButton();
}

void RoomSimulationDialog::onGenerate() {
    if (!persistCurrentSimulation(true)) {
        return;
    }
    if (activeSimulationName_.empty()) {
        MessageBoxW(window_, L"Create or select a simulation first.", L"Room Simulation", MB_OK | MB_ICONERROR);
        return;
    }
    if (onGenerate_) {
        onGenerate_(activeSimulationName_, currentSettings_);
    }
}

}  // namespace wolfie::ui
