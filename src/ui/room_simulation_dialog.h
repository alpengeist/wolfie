#pragma once

#include <functional>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/models.h"
#include "ui/help_bubble.h"

namespace wolfie::ui {

class RoomSimulationDialog {
public:
    using SaveCallback = std::function<void(const std::string&, const RoomSimulationSettings&)>;
    using GenerateCallback = std::function<void(const std::string&, const RoomSimulationSettings&)>;

    struct NamePromptDialogState {
        HWND edit = nullptr;
        bool finished = false;
        bool accepted = false;
        std::wstring value;
    };

    void show(HINSTANCE instance,
              HWND owner,
              SaveCallback onSave,
              GenerateCallback onGenerate);
    void close();
    void populate(const WorkspaceState& workspace);
    [[nodiscard]] bool isOpen() const { return window_ != nullptr; }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    struct Controls {
        HWND labelSimulation = nullptr;
        HWND comboSimulations = nullptr;
        HWND buttonNew = nullptr;
        HWND buttonGenerate = nullptr;
        HWND editStereoSkew = nullptr;
        HWND editSpectralTilt = nullptr;
        HWND editLowShelfGain = nullptr;
        HWND editLowShelfCorner = nullptr;
        HWND editModalPeakFrequency = nullptr;
        HWND editModalPeakGain = nullptr;
        HWND editModalPeakQ = nullptr;
        HWND editModalNullFrequency = nullptr;
        HWND editModalNullDepth = nullptr;
        HWND editModalNullQ = nullptr;
        HWND editReflectionCount = nullptr;
        HWND editReflectionStart = nullptr;
        HWND editReflectionSpacing = nullptr;
        HWND editReflectionDecay = nullptr;
        HWND editLateDecayRt60 = nullptr;
        HWND editLateDecayStart = nullptr;
        HWND editLateDensity = nullptr;
        HWND editNoiseFloor = nullptr;
        HWND editSeed = nullptr;
    };

    static std::wstring requestSimulationName(HWND owner, HINSTANCE instance);
    static std::wstring getWindowTextValue(HWND control);
    static void setWindowTextValue(HWND control, const std::wstring& text);
    static bool isValidSimulationName(std::string_view name);
    static bool tryParseDouble(const std::wstring& text, double& value);
    static bool tryParseInt(const std::wstring& text, int& value);
    void createControls();
    void layoutControls() const;
    void refreshComboItems();
    void refreshFieldValues();
    void refreshGenerateButton();
    bool tryReadSettingsFromControls(RoomSimulationSettings& settings, std::wstring& errorMessage) const;
    void applyCurrentSettingsToCache(const RoomSimulationSettings& settings);
    bool persistCurrentSimulation(bool showErrors);
    void selectSimulationByName(const std::string& name);
    [[nodiscard]] int simulationIndexByName(const std::string& name) const;
    [[nodiscard]] std::string selectedSimulationNameFromCombo() const;
    void onNewSimulation();
    void onGenerate();

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    SaveCallback onSave_;
    GenerateCallback onGenerate_;
    std::vector<RoomSimulationDefinition> simulations_;
    std::string activeSimulationName_;
    RoomSimulationSettings currentSettings_ = {};
    bool populatingControls_ = false;
    std::vector<HWND> fieldLabels_;
    std::vector<HWND> fieldUnits_;
    HelpBubbleController helpBubble_;
};

}  // namespace wolfie::ui
