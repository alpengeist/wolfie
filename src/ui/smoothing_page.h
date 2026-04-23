#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/models.h"
#include "ui/response_graph.h"

namespace wolfie::ui {

class SmoothingPage {
public:
    static void registerPageWindowClass(HINSTANCE instance);
    static const wchar_t* pageWindowClassName();

    void create(HWND parent, HINSTANCE instance);
    void layout();
    void setVisible(bool visible) const;
    void populate(const WorkspaceState& workspace);
    void syncToWorkspace(WorkspaceState& workspace) const;
    void invalidateGraph() const;
    bool handleCommand(WORD commandId,
                       WORD notificationCode,
                       WorkspaceState& workspace,
                       bool& smoothingModelChanged,
                       bool& graphZoomChanged);
    bool handleHScroll(HWND source, WorkspaceState& workspace, bool& smoothingResolutionChanged);

    [[nodiscard]] HWND window() const { return window_; }

private:
    struct Controls {
        HWND labelModel = nullptr;
        HWND comboModel = nullptr;
        HWND labelResolution = nullptr;
        HWND resolutionSlider = nullptr;
        HWND resolutionCoarseLabel = nullptr;
        HWND resolutionFineLabel = nullptr;
        HWND effectiveParameter = nullptr;
        HWND labelHighFrequencyCutoff = nullptr;
        HWND editHighFrequencyCutoff = nullptr;
        HWND unitHighFrequencyCutoff = nullptr;
    };

    static constexpr wchar_t kPageClassName[] = L"WolfieSmoothingPage";
    static constexpr int kComboModel = 3201;
    static constexpr int kResponseGraph = 3202;
    static constexpr int kResolutionSliderMax = 100;
    static LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static void setWindowTextValue(HWND control, const std::wstring& text);
    static void populateModelCombo(HWND combo);
    static int comboIndexFromModel(const std::string& model);
    static std::string modelFromComboIndex(int index);
    static std::wstring formatEffectiveParameter(const ResponseSmoothingSettings& settings);

    void createControls();
    ResponseGraphData buildGraphData(const SmoothedResponse& response) const;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Controls controls_;
    ResponseGraph responseGraph_;
};

}  // namespace wolfie::ui
