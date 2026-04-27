#include "ui/filters_page.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <commctrl.h>

#include "core/text_utils.h"
#include "measurement/filter_designer.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr double kSmoothnessSteps[] = {0.1, 1.0, 2.0, 4.0};
constexpr int kSmoothnessStepCount = static_cast<int>(sizeof(kSmoothnessSteps) / sizeof(kSmoothnessSteps[0]));
constexpr double kImpulseGraphNegativeWindowMs = 10.0;
constexpr double kImpulseGraphPositiveWindowMs = 50.0;
constexpr double kImpulseGraphMinZoomFactor = 0.5;
constexpr double kImpulseGraphMaxZoomFactor = 2.0;

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

int smoothnessSliderPositionFromValue(double smoothness) {
    int bestIndex = 0;
    double bestDistance = std::abs(smoothness - kSmoothnessSteps[0]);
    for (int index = 1; index < kSmoothnessStepCount; ++index) {
        const double distance = std::abs(smoothness - kSmoothnessSteps[index]);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = index;
        }
    }
    return bestIndex;
}

double smoothnessValueFromSliderPosition(LRESULT position) {
    const int index = clampValue(static_cast<int>(position), 0, kSmoothnessStepCount - 1);
    return kSmoothnessSteps[index];
}

void drawLegendFrame(const DRAWITEMSTRUCT& draw) {
    HBRUSH backgroundBrush = CreateSolidBrush(ui_theme::kBackground);
    FillRect(draw.hDC, &draw.rcItem, backgroundBrush);
    DeleteObject(backgroundBrush);

    const int savedDc = SaveDC(draw.hDC);
    SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
    HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
    SelectObject(draw.hDC, borderPen);
    Rectangle(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right, draw.rcItem.bottom);
    RestoreDC(draw.hDC, savedDc);
    DeleteObject(borderPen);
}

COLORREF blendColor(COLORREF color, COLORREF target, double ratio) {
    const auto blendChannel = [ratio](BYTE from, BYTE to) -> BYTE {
        return static_cast<BYTE>(std::lround((static_cast<double>(from) * (1.0 - ratio)) +
                                             (static_cast<double>(to) * ratio)));
    };
    return RGB(blendChannel(GetRValue(color), GetRValue(target)),
               blendChannel(GetGValue(color), GetGValue(target)),
               blendChannel(GetBValue(color), GetBValue(target)));
}

double interpolateLinear(double x, double x0, double y0, double x1, double y1) {
    if (std::abs(x1 - x0) < 1.0e-9) {
        return y0;
    }
    const double t = (x - x0) / (x1 - x0);
    return y0 + (t * (y1 - y0));
}

double interpolateLogFrequency(const std::vector<double>& sourceAxisHz,
                               const std::vector<double>& sourceValues,
                               double frequencyHz) {
    if (sourceAxisHz.empty() || sourceValues.size() != sourceAxisHz.size()) {
        return 0.0;
    }

    if (frequencyHz <= sourceAxisHz.front()) {
        return sourceValues.front();
    }
    if (frequencyHz >= sourceAxisHz.back()) {
        return sourceValues.back();
    }

    const auto upper = std::lower_bound(sourceAxisHz.begin(), sourceAxisHz.end(), frequencyHz);
    if (upper == sourceAxisHz.begin()) {
        return sourceValues.front();
    }
    if (upper == sourceAxisHz.end()) {
        return sourceValues.back();
    }

    const size_t upperIndex = static_cast<size_t>(upper - sourceAxisHz.begin());
    const size_t lowerIndex = upperIndex - 1;
    const double x = std::log10(std::max(frequencyHz, 1.0));
    const double x0 = std::log10(std::max(sourceAxisHz[lowerIndex], 1.0));
    const double x1 = std::log10(std::max(sourceAxisHz[upperIndex], 1.0));
    return interpolateLinear(x, x0, sourceValues[lowerIndex], x1, sourceValues[upperIndex]);
}

std::vector<double> resampleLogFrequency(const std::vector<double>& sourceAxisHz,
                                         const std::vector<double>& sourceValues,
                                         const std::vector<double>& targetAxisHz) {
    std::vector<double> resampled;
    if (sourceAxisHz.empty() || sourceValues.size() != sourceAxisHz.size() || targetAxisHz.empty()) {
        return resampled;
    }

    resampled.reserve(targetAxisHz.size());
    for (const double frequencyHz : targetAxisHz) {
        resampled.push_back(interpolateLogFrequency(sourceAxisHz, sourceValues, frequencyHz));
    }
    return resampled;
}

}  // namespace

void FiltersPage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = CreateSolidBrush(ui_theme::kBackground);
    RegisterClassW(&pageClass);
}

const wchar_t* FiltersPage::pageWindowClassName() {
    return kPageClassName;
}

void FiltersPage::create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    window_ = CreateWindowExW(0,
                              kPageClassName,
                              nullptr,
                              WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
                              0,
                              0,
                              0,
                              0,
                              parent,
                              nullptr,
                              instance,
                              this);
    createControls();
}

void FiltersPage::createControls() {
    controls_.labelTapCount = CreateWindowW(L"STATIC", L"Tap Count", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboTapCount = CreateWindowW(L"COMBOBOX",
                                            nullptr,
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            0,
                                            0,
                                            0,
                                            0,
                                            window_,
                                            reinterpret_cast<HMENU>(kComboTapCount),
                                            instance_,
                                            nullptr);
    controls_.labelPhaseMode = CreateWindowW(L"STATIC", L"Phase Mode", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.phaseModeValue = CreateWindowW(L"STATIC", L"Minimum phase", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelLowCorrection = CreateWindowW(L"STATIC", L"Low Bound", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editLowCorrection = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditLowCorrection), instance_, nullptr);
    controls_.unitLowCorrection = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelHighCorrection = CreateWindowW(L"STATIC", L"High Bound", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editHighCorrection = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                                   0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditHighCorrection), instance_, nullptr);
    controls_.unitHighCorrection = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMaxBoost = CreateWindowW(L"STATIC", L"Max Boost", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMaxBoost = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                             0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditMaxBoost), instance_, nullptr);
    controls_.unitMaxBoost = CreateWindowW(L"STATIC", L"dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMaxCut = CreateWindowW(L"STATIC", L"Max Cut", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMaxCut = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                           0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditMaxCut), instance_, nullptr);
    controls_.unitMaxCut = CreateWindowW(L"STATIC", L"dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelSmoothness = CreateWindowW(L"STATIC", L"Smoothness", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.sliderSmoothness = CreateWindowW(TRACKBAR_CLASSW,
                                               nullptr,
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS | TBS_HORZ,
                                               0,
                                               0,
                                               0,
                                               0,
                                               window_,
                                               reinterpret_cast<HMENU>(kSliderSmoothness),
                                               instance_,
                                               nullptr);
    controls_.valueSmoothness = CreateWindowW(L"STATIC", L"1", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonRecalculate = CreateWindowW(L"BUTTON", L"Recalculate", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonRecalculate), instance_, nullptr);
    controls_.checkboxSyncHoverFrequency = CreateWindowW(L"BUTTON",
                                                         L"Sync hover cursor",
                                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                         0,
                                                         0,
                                                         0,
                                                         0,
                                                         window_,
                                                         reinterpret_cast<HMENU>(kCheckboxSyncHoverFrequency),
                                                         instance_,
                                                         nullptr);
    controls_.inversionTitle = CreateWindowW(L"STATIC", L"Inversion", WS_CHILD | WS_VISIBLE,
                                             0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.inversionLegendFrame = CreateWindowW(L"STATIC",
                                                   L"",
                                                   WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   window_,
                                                   nullptr,
                                                   instance_,
                                                   nullptr);
    controls_.checkboxShowInputRight = CreateWindowW(L"BUTTON",
                                                     L"",
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     window_,
                                                     reinterpret_cast<HMENU>(kCheckboxShowInputRight),
                                                     instance_,
                                                     nullptr);
    controls_.lineInputRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInputRight = CreateWindowW(L"STATIC", L"R", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowInputLeft = CreateWindowW(L"BUTTON",
                                                    L"",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    window_,
                                                    reinterpret_cast<HMENU>(kCheckboxShowInputLeft),
                                                    instance_,
                                                    nullptr);
    controls_.lineInputLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInputLeft = CreateWindowW(L"STATIC", L"L", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowInversionRight = CreateWindowW(L"BUTTON",
                                                         L"",
                                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                         0,
                                                         0,
                                                         0,
                                                         0,
                                                         window_,
                                                         reinterpret_cast<HMENU>(kCheckboxShowInversionRight),
                                                         instance_,
                                                         nullptr);
    controls_.lineInversionRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInversionRight = CreateWindowW(L"STATIC", L"R inv", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowInversionLeft = CreateWindowW(L"BUTTON",
                                                        L"",
                                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                        0,
                                                        0,
                                                        0,
                                                        0,
                                                        window_,
                                                        reinterpret_cast<HMENU>(kCheckboxShowInversionLeft),
                                                        instance_,
                                                        nullptr);
    controls_.lineInversionLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInversionLeft = CreateWindowW(L"STATIC", L"L inv", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.correctedTitle = CreateWindowW(L"STATIC", L"Predicted Corrected Response", WS_CHILD | WS_VISIBLE,
                                             0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.correctedLegendFrame = CreateWindowW(L"STATIC",
                                                   L"",
                                                   WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   window_,
                                                   nullptr,
                                                   instance_,
                                                   nullptr);
    controls_.lineCorrectedTarget = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelCorrectedTarget = CreateWindowW(L"STATIC", L"Target", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowCorrectedInputLeft = CreateWindowW(L"BUTTON",
                                                             L"",
                                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             window_,
                                                             reinterpret_cast<HMENU>(kCheckboxShowCorrectedInputLeft),
                                                             instance_,
                                                             nullptr);
    controls_.lineCorrectedInputLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelCorrectedInputLeft = CreateWindowW(L"STATIC", L"L", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowCorrectedInputRight = CreateWindowW(L"BUTTON",
                                                              L"",
                                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                              0,
                                                              0,
                                                              0,
                                                              0,
                                                              window_,
                                                              reinterpret_cast<HMENU>(kCheckboxShowCorrectedInputRight),
                                                              instance_,
                                                              nullptr);
    controls_.lineCorrectedInputRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelCorrectedInputRight = CreateWindowW(L"STATIC", L"R", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowCorrectedLeft = CreateWindowW(L"BUTTON",
                                                         L"",
                                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                        0,
                                                        0,
                                                        0,
                                                        0,
                                                         window_,
                                                         reinterpret_cast<HMENU>(kCheckboxShowCorrectedLeft),
                                                         instance_,
                                                         nullptr);
    controls_.lineCorrectedLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelCorrectedLeft = CreateWindowW(L"STATIC", L"L pred", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowCorrectedRight = CreateWindowW(L"BUTTON",
                                                          L"",
                                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                         0,
                                                         0,
                                                         0,
                                                         0,
                                                          window_,
                                                          reinterpret_cast<HMENU>(kCheckboxShowCorrectedRight),
                                                          instance_,
                                                          nullptr);
    controls_.lineCorrectedRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelCorrectedRight = CreateWindowW(L"STATIC", L"R pred", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.excessPhaseTitle = CreateWindowW(L"STATIC", L"Excess Phase", WS_CHILD | WS_VISIBLE,
                                               0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.excessPhaseLegendFrame = CreateWindowW(L"STATIC",
                                                     L"",
                                                     WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     window_,
                                                     nullptr,
                                                     instance_,
                                                     nullptr);
    controls_.checkboxShowExcessPhaseInputRight = CreateWindowW(L"BUTTON",
                                                                L"",
                                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                0,
                                                                0,
                                                                0,
                                                                0,
                                                                window_,
                                                                reinterpret_cast<HMENU>(kCheckboxShowExcessPhaseInputRight),
                                                                instance_,
                                                                nullptr);
    controls_.lineExcessPhaseInputRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhaseInputRight = CreateWindowW(L"STATIC", L"R before", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowExcessPhaseInputLeft = CreateWindowW(L"BUTTON",
                                                               L"",
                                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                               0,
                                                               0,
                                                               0,
                                                               0,
                                                               window_,
                                                               reinterpret_cast<HMENU>(kCheckboxShowExcessPhaseInputLeft),
                                                               instance_,
                                                               nullptr);
    controls_.lineExcessPhaseInputLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhaseInputLeft = CreateWindowW(L"STATIC", L"L before", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowExcessPhasePredictedRight = CreateWindowW(L"BUTTON",
                                                                    L"",
                                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                    0,
                                                                    0,
                                                                    0,
                                                                    0,
                                                                    window_,
                                                                    reinterpret_cast<HMENU>(kCheckboxShowExcessPhasePredictedRight),
                                                                    instance_,
                                                                    nullptr);
    controls_.lineExcessPhasePredictedRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhasePredictedRight = CreateWindowW(L"STATIC", L"R after", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowExcessPhasePredictedLeft = CreateWindowW(L"BUTTON",
                                                                   L"",
                                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   window_,
                                                                   reinterpret_cast<HMENU>(kCheckboxShowExcessPhasePredictedLeft),
                                                                   instance_,
                                                                   nullptr);
    controls_.lineExcessPhasePredictedLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhasePredictedLeft = CreateWindowW(L"STATIC", L"L after", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowPredictedGroupDelayRight = CreateWindowW(L"BUTTON",
                                                                   L"",
                                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   window_,
                                                                   reinterpret_cast<HMENU>(kCheckboxShowPredictedGroupDelayRight),
                                                                   instance_,
                                                                   nullptr);
    controls_.linePredictedGroupDelayRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelPredictedGroupDelayRight = CreateWindowW(L"STATIC", L"R pred", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowPredictedGroupDelayLeft = CreateWindowW(L"BUTTON",
                                                                  L"",
                                                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                  0,
                                                                  0,
                                                                  0,
                                                                  0,
                                                                  window_,
                                                                  reinterpret_cast<HMENU>(kCheckboxShowPredictedGroupDelayLeft),
                                                                  instance_,
                                                                  nullptr);
    controls_.linePredictedGroupDelayLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelPredictedGroupDelayLeft = CreateWindowW(L"STATIC", L"L pred", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.groupDelayTitle = CreateWindowW(L"STATIC", L"Filter + Predicted Group Delay", WS_CHILD | WS_VISIBLE,
                                              0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.groupDelayLegendFrame = CreateWindowW(L"STATIC",
                                                    L"",
                                                    WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    window_,
                                                    nullptr,
                                                    instance_,
                                                    nullptr);
    controls_.checkboxShowFilterGroupDelayLeft = CreateWindowW(L"BUTTON",
                                                               L"",
                                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                               0,
                                                               0,
                                                               0,
                                                               0,
                                                               window_,
                                                               reinterpret_cast<HMENU>(kCheckboxShowFilterGroupDelayLeft),
                                                               instance_,
                                                               nullptr);
    controls_.lineGroupDelayLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelGroupDelayLeft = CreateWindowW(L"STATIC", L"L", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowFilterGroupDelayRight = CreateWindowW(L"BUTTON",
                                                                L"",
                                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                0,
                                                                0,
                                                                0,
                                                                0,
                                                                window_,
                                                                reinterpret_cast<HMENU>(kCheckboxShowFilterGroupDelayRight),
                                                                instance_,
                                                                nullptr);
    controls_.lineGroupDelayRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelGroupDelayRight = CreateWindowW(L"STATIC", L"R", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.impulseTitle = CreateWindowW(L"STATIC", L"Filter Impulse", WS_CHILD | WS_VISIBLE,
                                           0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonImpulseZoomOutX = CreateWindowW(L"BUTTON", L"X-", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonImpulseZoomOutX), instance_, nullptr);
    controls_.buttonImpulseZoomInX = CreateWindowW(L"BUTTON", L"X+", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                   0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonImpulseZoomInX), instance_, nullptr);
    controls_.buttonImpulseResetX = CreateWindowW(L"BUTTON", L"Reset X", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonImpulseResetX), instance_, nullptr);
    controls_.buttonImpulseZoomOutY = CreateWindowW(L"BUTTON", L"Y-", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonImpulseZoomOutY), instance_, nullptr);
    controls_.buttonImpulseZoomInY = CreateWindowW(L"BUTTON", L"Y+", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                   0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonImpulseZoomInY), instance_, nullptr);
    controls_.buttonImpulseResetY = CreateWindowW(L"BUTTON", L"Reset Y", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonImpulseResetY), instance_, nullptr);
    controls_.buttonImpulseFit = CreateWindowW(L"BUTTON", L"Fit", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonImpulseFit), instance_, nullptr);

    populateTapCountCombo(controls_.comboTapCount);
    SendMessageW(controls_.sliderSmoothness, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.sliderSmoothness, TBM_SETRANGEMAX, FALSE, static_cast<LPARAM>(kSmoothnessStepCount - 1));
    SendMessageW(controls_.sliderSmoothness, TBM_SETTICFREQ, 1, 0);
    SendMessageW(controls_.sliderSmoothness, TBM_SETLINESIZE, 0, 1);
    SendMessageW(controls_.sliderSmoothness, TBM_SETPAGESIZE, 0, 1);
    setSelectedSmoothness(1.0);
    SendMessageW(controls_.checkboxShowInputRight, BM_SETCHECK, showInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInputLeft, BM_SETCHECK, showInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionRight, BM_SETCHECK, showInversionRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionLeft, BM_SETCHECK, showInversionLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedInputLeft, BM_SETCHECK, showCorrectedInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedInputRight, BM_SETCHECK, showCorrectedInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedLeft, BM_SETCHECK, showCorrectedLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedRight, BM_SETCHECK, showCorrectedRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhaseInputRight, BM_SETCHECK, showExcessPhaseInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhaseInputLeft, BM_SETCHECK, showExcessPhaseInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhasePredictedRight, BM_SETCHECK, showExcessPhasePredictedRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhasePredictedLeft, BM_SETCHECK, showExcessPhasePredictedLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowPredictedGroupDelayRight, BM_SETCHECK, showPredictedGroupDelayRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowPredictedGroupDelayLeft, BM_SETCHECK, showPredictedGroupDelayLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowFilterGroupDelayLeft, BM_SETCHECK, showFilterGroupDelayLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowFilterGroupDelayRight, BM_SETCHECK, showFilterGroupDelayRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxSyncHoverFrequency, BM_SETCHECK, syncHoverFrequencyEnabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    correctionGraph_.create(window_, instance_, kCorrectionGraph);
    correctedGraph_.create(window_, instance_, kCorrectedGraph);
    excessPhaseGraph_.create(window_, instance_, kExcessPhaseGraph);
    groupDelayGraph_.create(window_, instance_, kGroupDelayGraph);
    impulseGraph_.create(window_, instance_, kImpulseGraph);
    refreshRecalculateButton();
}

void FiltersPage::layout() {
    RECT pageRect{};
    GetClientRect(window_, &pageRect);
    const int viewportWidth = std::max(480L, pageRect.right);
    const int viewportHeight = std::max(360L, pageRect.bottom);
    const int contentLeft = 20;
    const int contentWidth = std::max(420, viewportWidth - (contentLeft * 2) - GetSystemMetrics(SM_CXVSCROLL));
    const int graphHeight = 320;
    const int graphGap = 34;
    const int sectionGap = 26;
    const int legendGap = 14;
    const int legendWidth = 128;
    const int top = 20 - scrollOffset_;
    const int comboDropHeight = 220;

    MoveWindow(controls_.labelTapCount, contentLeft, top, 84, 18, TRUE);
    MoveWindow(controls_.comboTapCount, contentLeft, top + 22, 120, comboDropHeight, TRUE);
    MoveWindow(controls_.labelPhaseMode, contentLeft + 148, top, 84, 18, TRUE);
    MoveWindow(controls_.phaseModeValue, contentLeft + 148, top + 24, 120, 18, TRUE);
    MoveWindow(controls_.labelLowCorrection, contentLeft + 292, top, 72, 18, TRUE);
    MoveWindow(controls_.editLowCorrection, contentLeft + 292, top + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitLowCorrection, contentLeft + 364, top + 26, 22, 18, TRUE);
    MoveWindow(controls_.labelHighCorrection, contentLeft + 402, top, 76, 18, TRUE);
    MoveWindow(controls_.editHighCorrection, contentLeft + 402, top + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitHighCorrection, contentLeft + 474, top + 26, 22, 18, TRUE);
    MoveWindow(controls_.labelMaxBoost, contentLeft + 512, top, 70, 18, TRUE);
    MoveWindow(controls_.editMaxBoost, contentLeft + 512, top + 22, 58, 26, TRUE);
    MoveWindow(controls_.unitMaxBoost, contentLeft + 574, top + 26, 24, 18, TRUE);
    MoveWindow(controls_.labelMaxCut, contentLeft + 614, top, 70, 18, TRUE);
    MoveWindow(controls_.editMaxCut, contentLeft + 614, top + 22, 58, 26, TRUE);
    MoveWindow(controls_.unitMaxCut, contentLeft + 676, top + 26, 24, 18, TRUE);
    MoveWindow(controls_.labelSmoothness, contentLeft + 716, top, 78, 18, TRUE);
    MoveWindow(controls_.sliderSmoothness, contentLeft + 716, top + 20, 120, 32, TRUE);
    MoveWindow(controls_.valueSmoothness, contentLeft + 842, top + 24, 36, 18, TRUE);
    MoveWindow(controls_.buttonRecalculate, contentLeft, top + 62, contentWidth, 32, TRUE);

    int y = top + 112;
    const int legendLeft = contentLeft + contentWidth - legendWidth;
    const int graphRight = legendLeft - legendGap;
    MoveWindow(controls_.checkboxSyncHoverFrequency, graphRight - 168, y - 2, 168, 20, TRUE);
    MoveWindow(controls_.inversionTitle, contentLeft, y, 120, 18, TRUE);
    const int frameTop = y + 24;
    MoveWindow(controls_.inversionLegendFrame, legendLeft, frameTop, legendWidth, graphHeight, TRUE);
    correctionGraph_.layout(RECT{contentLeft, y + 24, graphRight, y + 24 + graphHeight});

    const int checkboxLeft = legendLeft + 14;
    const int checkboxWidth = 18;
    const int rowStep = 30;
    const int firstRowTop = frameTop + 18;
    const int lineLeft = checkboxLeft + checkboxWidth + 8;
    const int lineWidth = 24;
    const int lineHeight = 3;
    const int labelLeft = lineLeft + lineWidth + 8;
    const int labelWidth = legendWidth - (labelLeft - legendLeft) - 12;
    MoveWindow(controls_.checkboxShowInputRight, checkboxLeft, firstRowTop, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInputRight, lineLeft, firstRowTop + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInputRight, labelLeft, firstRowTop + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInputLeft, checkboxLeft, firstRowTop + rowStep, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInputLeft, lineLeft, firstRowTop + rowStep + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInputLeft, labelLeft, firstRowTop + rowStep + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInversionRight, checkboxLeft, firstRowTop + (rowStep * 2), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInversionRight, lineLeft, firstRowTop + (rowStep * 2) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInversionRight, labelLeft, firstRowTop + (rowStep * 2) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInversionLeft, checkboxLeft, firstRowTop + (rowStep * 3), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInversionLeft, lineLeft, firstRowTop + (rowStep * 3) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInversionLeft, labelLeft, firstRowTop + (rowStep * 3) + 2, labelWidth, 18, TRUE);

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.correctedTitle, contentLeft, y, contentWidth, 18, TRUE);
    MoveWindow(controls_.correctedLegendFrame, legendLeft, y + 24, legendWidth, graphHeight, TRUE);
    correctedGraph_.layout(RECT{contentLeft, y + 24, graphRight, y + 24 + graphHeight});
    const int correctedFirstRowTop = y + 24 + 18;
    MoveWindow(controls_.lineCorrectedTarget, lineLeft, correctedFirstRowTop + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelCorrectedTarget, labelLeft, correctedFirstRowTop + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowCorrectedInputLeft, checkboxLeft, correctedFirstRowTop + rowStep, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineCorrectedInputLeft, lineLeft, correctedFirstRowTop + rowStep + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelCorrectedInputLeft, labelLeft, correctedFirstRowTop + rowStep + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowCorrectedInputRight, checkboxLeft, correctedFirstRowTop + (rowStep * 2), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineCorrectedInputRight, lineLeft, correctedFirstRowTop + (rowStep * 2) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelCorrectedInputRight, labelLeft, correctedFirstRowTop + (rowStep * 2) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowCorrectedLeft, checkboxLeft, correctedFirstRowTop + (rowStep * 3), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineCorrectedLeft, lineLeft, correctedFirstRowTop + (rowStep * 3) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelCorrectedLeft, labelLeft, correctedFirstRowTop + (rowStep * 3) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowCorrectedRight, checkboxLeft, correctedFirstRowTop + (rowStep * 4), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineCorrectedRight, lineLeft, correctedFirstRowTop + (rowStep * 4) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelCorrectedRight, labelLeft, correctedFirstRowTop + (rowStep * 4) + 2, labelWidth, 18, TRUE);

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.excessPhaseTitle, contentLeft, y, contentWidth, 18, TRUE);
    MoveWindow(controls_.excessPhaseLegendFrame, legendLeft, y + 24, legendWidth, graphHeight, TRUE);
    excessPhaseGraph_.layout(RECT{contentLeft, y + 24, graphRight, y + 24 + graphHeight});
    const int excessPhaseFirstRowTop = y + 24 + 18;
    MoveWindow(controls_.checkboxShowExcessPhaseInputRight, checkboxLeft, excessPhaseFirstRowTop, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineExcessPhaseInputRight, lineLeft, excessPhaseFirstRowTop + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelExcessPhaseInputRight, labelLeft, excessPhaseFirstRowTop + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowExcessPhaseInputLeft, checkboxLeft, excessPhaseFirstRowTop + rowStep, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineExcessPhaseInputLeft, lineLeft, excessPhaseFirstRowTop + rowStep + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelExcessPhaseInputLeft, labelLeft, excessPhaseFirstRowTop + rowStep + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowExcessPhasePredictedRight, checkboxLeft, excessPhaseFirstRowTop + (rowStep * 2), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineExcessPhasePredictedRight, lineLeft, excessPhaseFirstRowTop + (rowStep * 2) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelExcessPhasePredictedRight, labelLeft, excessPhaseFirstRowTop + (rowStep * 2) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowExcessPhasePredictedLeft, checkboxLeft, excessPhaseFirstRowTop + (rowStep * 3), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineExcessPhasePredictedLeft, lineLeft, excessPhaseFirstRowTop + (rowStep * 3) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelExcessPhasePredictedLeft, labelLeft, excessPhaseFirstRowTop + (rowStep * 3) + 2, labelWidth, 18, TRUE);

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.groupDelayTitle, contentLeft, y, contentWidth, 18, TRUE);
    MoveWindow(controls_.groupDelayLegendFrame, legendLeft, y + 24, legendWidth, graphHeight, TRUE);
    groupDelayGraph_.layout(RECT{contentLeft, y + 24, graphRight, y + 24 + graphHeight});
    const int groupDelayFirstRowTop = y + 24 + 18;
    MoveWindow(controls_.checkboxShowFilterGroupDelayLeft, checkboxLeft, groupDelayFirstRowTop, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineGroupDelayLeft, lineLeft, groupDelayFirstRowTop + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelGroupDelayLeft, labelLeft, groupDelayFirstRowTop + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowFilterGroupDelayRight, checkboxLeft, groupDelayFirstRowTop + rowStep, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineGroupDelayRight, lineLeft, groupDelayFirstRowTop + rowStep + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelGroupDelayRight, labelLeft, groupDelayFirstRowTop + rowStep + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowPredictedGroupDelayRight, checkboxLeft, groupDelayFirstRowTop + (rowStep * 2), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.linePredictedGroupDelayRight, lineLeft, groupDelayFirstRowTop + (rowStep * 2) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelPredictedGroupDelayRight, labelLeft, groupDelayFirstRowTop + (rowStep * 2) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowPredictedGroupDelayLeft, checkboxLeft, groupDelayFirstRowTop + (rowStep * 3), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.linePredictedGroupDelayLeft, lineLeft, groupDelayFirstRowTop + (rowStep * 3) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelPredictedGroupDelayLeft, labelLeft, groupDelayFirstRowTop + (rowStep * 3) + 2, labelWidth, 18, TRUE);

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.impulseTitle, contentLeft, y, contentWidth, 18, TRUE);
    const int impulseButtonTop = y + 20;
    const int impulseButtonHeight = 24;
    int impulseButtonLeft = contentLeft;
    MoveWindow(controls_.buttonImpulseZoomOutX, impulseButtonLeft, impulseButtonTop, 42, impulseButtonHeight, TRUE);
    impulseButtonLeft += 48;
    MoveWindow(controls_.buttonImpulseZoomInX, impulseButtonLeft, impulseButtonTop, 42, impulseButtonHeight, TRUE);
    impulseButtonLeft += 48;
    MoveWindow(controls_.buttonImpulseResetX, impulseButtonLeft, impulseButtonTop, 70, impulseButtonHeight, TRUE);
    impulseButtonLeft += 82;
    MoveWindow(controls_.buttonImpulseZoomOutY, impulseButtonLeft, impulseButtonTop, 42, impulseButtonHeight, TRUE);
    impulseButtonLeft += 48;
    MoveWindow(controls_.buttonImpulseZoomInY, impulseButtonLeft, impulseButtonTop, 42, impulseButtonHeight, TRUE);
    impulseButtonLeft += 48;
    MoveWindow(controls_.buttonImpulseResetY, impulseButtonLeft, impulseButtonTop, 70, impulseButtonHeight, TRUE);
    impulseButtonLeft += 82;
    MoveWindow(controls_.buttonImpulseFit, impulseButtonLeft, impulseButtonTop, 54, impulseButtonHeight, TRUE);
    impulseGraph_.layout(RECT{contentLeft, y + 52, contentLeft + contentWidth, y + 52 + graphHeight});

    contentHeight_ = y + scrollOffset_ + 52 + graphHeight + sectionGap + 20;
    updateScrollBar();

    if (contentHeight_ - scrollOffset_ < viewportHeight) {
        setScrollOffset(std::max(0, contentHeight_ - viewportHeight));
    }
}

void FiltersPage::setVisible(bool visible) const {
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
}

void FiltersPage::populate(const WorkspaceState& workspace) {
    FilterDesignSettings settings = workspace.filters;
    measurement::normalizeFilterDesignSettings(settings, workspace.measurement.sampleRate);
    sampleRate_ = workspace.measurement.sampleRate;
    SendMessageW(controls_.comboTapCount, CB_SETCURSEL, comboIndexFromTapCount(settings.tapCount), 0);
    SetWindowTextW(controls_.phaseModeValue, L"Minimum phase");
    setWindowTextValue(controls_.editLowCorrection, formatWideDouble(settings.lowCorrectionHz, 0));
    setWindowTextValue(controls_.editHighCorrection, formatWideDouble(settings.highCorrectionHz, 0));
    setWindowTextValue(controls_.editMaxBoost, formatWideDouble(settings.maxBoostDb, 1));
    setWindowTextValue(controls_.editMaxCut, formatWideDouble(settings.maxCutDb, 1));
    setSelectedSmoothness(settings.smoothness);
    SendMessageW(controls_.checkboxShowInputRight, BM_SETCHECK, showInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInputLeft, BM_SETCHECK, showInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionRight, BM_SETCHECK, showInversionRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionLeft, BM_SETCHECK, showInversionLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedInputLeft, BM_SETCHECK, showCorrectedInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedInputRight, BM_SETCHECK, showCorrectedInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedLeft, BM_SETCHECK, showCorrectedLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedRight, BM_SETCHECK, showCorrectedRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhaseInputRight, BM_SETCHECK, showExcessPhaseInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhaseInputLeft, BM_SETCHECK, showExcessPhaseInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhasePredictedRight, BM_SETCHECK, showExcessPhasePredictedRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhasePredictedLeft, BM_SETCHECK, showExcessPhasePredictedLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowPredictedGroupDelayRight, BM_SETCHECK, showPredictedGroupDelayRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowPredictedGroupDelayLeft, BM_SETCHECK, showPredictedGroupDelayLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowFilterGroupDelayLeft, BM_SETCHECK, showFilterGroupDelayLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowFilterGroupDelayRight, BM_SETCHECK, showFilterGroupDelayRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxSyncHoverFrequency, BM_SETCHECK, syncHoverFrequencyEnabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    appliedSettings_ = settings;
    filterDesignValid_ = workspace.filterResult.valid;
    refreshRecalculateButton();

    correctionGraph_.setData(buildCorrectionGraphData(workspace));
    correctedGraph_.setData(buildCorrectedResponseGraphData(workspace));
    excessPhaseGraph_.setData(buildExcessPhaseGraphData(workspace));
    groupDelayGraph_.setData(buildGroupDelayGraphData(workspace));
    impulseGraph_.setData(buildImpulseGraphData(workspace));
    configureImpulseGraphViewport(workspace);
    applySharedFrequencyHoverMarker();
}

void FiltersPage::syncToWorkspace(WorkspaceState& workspace) const {
    workspace.filters.tapCount = tapCountFromComboIndex(static_cast<int>(SendMessageW(controls_.comboTapCount, CB_GETCURSEL, 0, 0)));
    double value = 0.0;
    workspace.filters.smoothness = selectedSmoothness();
    if (tryParseDouble(getWindowTextValue(controls_.editLowCorrection), value)) {
        workspace.filters.lowCorrectionHz = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editHighCorrection), value)) {
        workspace.filters.highCorrectionHz = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMaxBoost), value)) {
        workspace.filters.maxBoostDb = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMaxCut), value)) {
        workspace.filters.maxCutDb = value;
    }
    measurement::normalizeFilterDesignSettings(workspace.filters, workspace.measurement.sampleRate);
}

double FiltersPage::selectedSmoothness() const {
    return smoothnessValueFromSliderPosition(SendMessageW(controls_.sliderSmoothness, TBM_GETPOS, 0, 0));
}

void FiltersPage::setSelectedSmoothness(double smoothness) const {
    SendMessageW(controls_.sliderSmoothness, TBM_SETPOS, TRUE, smoothnessSliderPositionFromValue(smoothness));
    refreshSmoothnessValue();
}

void FiltersPage::refreshSmoothnessValue() const {
    const double smoothness = selectedSmoothness();
    const int digits = std::abs(smoothness - std::round(smoothness)) < 0.001 ? 0 : 1;
    setWindowTextValue(controls_.valueSmoothness, formatWideDouble(smoothness, digits));
}

FilterDesignSettings FiltersPage::currentSettings() const {
    FilterDesignSettings settings = appliedSettings_;
    settings.tapCount = tapCountFromComboIndex(static_cast<int>(SendMessageW(controls_.comboTapCount, CB_GETCURSEL, 0, 0)));
    settings.smoothness = selectedSmoothness();

    double value = 0.0;
    if (tryParseDouble(getWindowTextValue(controls_.editLowCorrection), value)) {
        settings.lowCorrectionHz = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editHighCorrection), value)) {
        settings.highCorrectionHz = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMaxBoost), value)) {
        settings.maxBoostDb = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMaxCut), value)) {
        settings.maxCutDb = value;
    }

    measurement::normalizeFilterDesignSettings(settings, sampleRate_);
    return settings;
}

bool FiltersPage::areSettingsEqual(const FilterDesignSettings& left, const FilterDesignSettings& right) {
    return left.tapCount == right.tapCount &&
           std::abs(left.smoothness - right.smoothness) < 0.001 &&
           std::abs(left.lowCorrectionHz - right.lowCorrectionHz) < 0.001 &&
           std::abs(left.highCorrectionHz - right.highCorrectionHz) < 0.001 &&
           std::abs(left.maxBoostDb - right.maxBoostDb) < 0.001 &&
           std::abs(left.maxCutDb - right.maxCutDb) < 0.001;
}

void FiltersPage::refreshRecalculateButton() {
    recalculatePending_ = !filterDesignValid_ || !areSettingsEqual(currentSettings(), appliedSettings_);
    if (controls_.buttonRecalculate != nullptr) {
        InvalidateRect(controls_.buttonRecalculate, nullptr, TRUE);
    }
}

bool FiltersPage::drawRecalculateButton(const DRAWITEMSTRUCT& draw) const {
    HDC hdc = draw.hDC;
    RECT rect = draw.rcItem;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;

    const COLORREF baseFill = recalculatePending_ ? ui_theme::kGreen : ui_theme::kGray;
    const COLORREF hoverFill = blendColor(baseFill, RGB(255, 255, 255), 0.12);
    const COLORREF pressedFill = blendColor(baseFill, RGB(0, 0, 0), 0.18);
    const COLORREF border = blendColor(baseFill, RGB(0, 0, 0), 0.28);

    HBRUSH brush = CreateSolidBrush(pressed ? pressedFill : (hot ? hoverFill : baseFill));
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    LOGFONTW baseFont{};
    HFONT guiFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    GetObjectW(guiFont, sizeof(baseFont), &baseFont);
    baseFont.lfWeight = FW_BOLD;
    baseFont.lfHeight = -17;
    HFONT buttonFont = CreateFontIndirectW(&baseFont);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, buttonFont));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT textRect = rect;
    if (pressed) {
        OffsetRect(&textRect, 0, 1);
    }
    DrawTextW(hdc, L"Recalculate", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (focused) {
        RECT focusRect = rect;
        InflateRect(&focusRect, -4, -4);
        DrawFocusRect(hdc, &focusRect);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(buttonFont);
    return true;
}

bool FiltersPage::handleCommand(WORD commandId,
                                WORD notificationCode,
                                WorkspaceState& workspace,
                                bool& settingsChanged,
                                bool& recalculateRequested) {
    if ((commandId == kCorrectionGraph ||
         commandId == kCorrectedGraph ||
         commandId == kExcessPhaseGraph ||
         commandId == kGroupDelayGraph) &&
        notificationCode == PlotGraph::kHoverChangedNotification) {
        if (!syncHoverFrequencyEnabled_) {
            return true;
        }

        const PlotGraph* sourceGraph = commandId == kCorrectionGraph ? &correctionGraph_
                                     : commandId == kCorrectedGraph ? &correctedGraph_
                                     : commandId == kExcessPhaseGraph ? &excessPhaseGraph_
                                                                    : &groupDelayGraph_;
        sharedFrequencyHoverActive_ = sourceGraph->hasHoveredXValue();
        if (sharedFrequencyHoverActive_) {
            sharedFrequencyHoverHz_ = sourceGraph->hoveredXValue();
        }
        applySharedFrequencyHoverMarker();
        return true;
    }

    if (commandId == kComboTapCount && notificationCode == CBN_SELCHANGE) {
        workspace.filters.tapCount =
            tapCountFromComboIndex(static_cast<int>(SendMessageW(controls_.comboTapCount, CB_GETCURSEL, 0, 0)));
        measurement::normalizeFilterDesignSettings(workspace.filters, workspace.measurement.sampleRate);
        filterDesignValid_ = false;
        refreshRecalculateButton();
        settingsChanged = true;
        return true;
    }

    if ((commandId == kEditLowCorrection ||
         commandId == kEditHighCorrection ||
         commandId == kEditMaxBoost ||
         commandId == kEditMaxCut) &&
        notificationCode == EN_CHANGE) {
        refreshRecalculateButton();
        return true;
    }

    if ((commandId == kCheckboxShowInputRight ||
         commandId == kCheckboxShowInputLeft ||
         commandId == kCheckboxShowInversionRight ||
         commandId == kCheckboxShowInversionLeft ||
         commandId == kCheckboxShowCorrectedInputLeft ||
         commandId == kCheckboxShowCorrectedInputRight ||
         commandId == kCheckboxShowCorrectedLeft ||
         commandId == kCheckboxShowCorrectedRight ||
         commandId == kCheckboxShowExcessPhaseInputRight ||
         commandId == kCheckboxShowExcessPhaseInputLeft ||
         commandId == kCheckboxShowExcessPhasePredictedRight ||
         commandId == kCheckboxShowExcessPhasePredictedLeft ||
         commandId == kCheckboxShowPredictedGroupDelayRight ||
         commandId == kCheckboxShowPredictedGroupDelayLeft ||
         commandId == kCheckboxShowFilterGroupDelayLeft ||
         commandId == kCheckboxShowFilterGroupDelayRight ||
         commandId == kCheckboxSyncHoverFrequency) &&
        notificationCode == BN_CLICKED) {
        showInputRight_ = SendMessageW(controls_.checkboxShowInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showInputLeft_ = SendMessageW(controls_.checkboxShowInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showInversionRight_ = SendMessageW(controls_.checkboxShowInversionRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showInversionLeft_ = SendMessageW(controls_.checkboxShowInversionLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showCorrectedInputLeft_ = SendMessageW(controls_.checkboxShowCorrectedInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showCorrectedInputRight_ = SendMessageW(controls_.checkboxShowCorrectedInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showCorrectedLeft_ = SendMessageW(controls_.checkboxShowCorrectedLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showCorrectedRight_ = SendMessageW(controls_.checkboxShowCorrectedRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showExcessPhaseInputRight_ = SendMessageW(controls_.checkboxShowExcessPhaseInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showExcessPhaseInputLeft_ = SendMessageW(controls_.checkboxShowExcessPhaseInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showExcessPhasePredictedRight_ = SendMessageW(controls_.checkboxShowExcessPhasePredictedRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showExcessPhasePredictedLeft_ = SendMessageW(controls_.checkboxShowExcessPhasePredictedLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showPredictedGroupDelayRight_ = SendMessageW(controls_.checkboxShowPredictedGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showPredictedGroupDelayLeft_ = SendMessageW(controls_.checkboxShowPredictedGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showFilterGroupDelayLeft_ = SendMessageW(controls_.checkboxShowFilterGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showFilterGroupDelayRight_ = SendMessageW(controls_.checkboxShowFilterGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        syncHoverFrequencyEnabled_ = SendMessageW(controls_.checkboxSyncHoverFrequency, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!syncHoverFrequencyEnabled_) {
            sharedFrequencyHoverActive_ = false;
        }
        correctionGraph_.setData(buildCorrectionGraphData(workspace));
        correctedGraph_.setData(buildCorrectedResponseGraphData(workspace));
        excessPhaseGraph_.setData(buildExcessPhaseGraphData(workspace));
        groupDelayGraph_.setData(buildGroupDelayGraphData(workspace));
        applySharedFrequencyHoverMarker();
        return true;
    }

    if (commandId == kButtonRecalculate && notificationCode == BN_CLICKED) {
        syncToWorkspace(workspace);
        recalculateRequested = true;
        return true;
    }

    if (notificationCode == BN_CLICKED) {
        switch (commandId) {
        case kButtonImpulseZoomOutX:
            return impulseGraph_.zoomXFromMin(kImpulseGraphMinZoomFactor);
        case kButtonImpulseZoomInX:
            return impulseGraph_.zoomXFromMin(kImpulseGraphMaxZoomFactor);
        case kButtonImpulseResetX:
            impulseGraph_.resetXRange();
            return true;
        case kButtonImpulseZoomOutY:
            return impulseGraph_.zoomY(kImpulseGraphMinZoomFactor);
        case kButtonImpulseZoomInY:
            return impulseGraph_.zoomY(kImpulseGraphMaxZoomFactor);
        case kButtonImpulseResetY:
            impulseGraph_.resetYRange();
            return true;
        case kButtonImpulseFit:
            impulseGraph_.resetView();
            return true;
        default:
            break;
        }
    }

    return false;
}

LRESULT CALLBACK FiltersPage::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* page = reinterpret_cast<FiltersPage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    static HBRUSH pageBackgroundBrush = CreateSolidBrush(ui_theme::kBackground);

    switch (message) {
    case WM_SIZE:
        if (page != nullptr) {
            page->layout();
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (page != nullptr && page->handleMouseWheel(wParam)) {
            return 0;
        }
        break;
    case WM_VSCROLL:
        if (page != nullptr) {
            page->handleVScroll(LOWORD(wParam), HIWORD(wParam));
            return 0;
        }
        break;
    case WM_HSCROLL:
        if (page != nullptr && reinterpret_cast<HWND>(lParam) == page->controls_.sliderSmoothness) {
            page->refreshSmoothnessValue();
            page->refreshRecalculateButton();
            return 0;
        }
        break;
    case WM_COMMAND: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
    case WM_DRAWITEM:
        if (page != nullptr) {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw != nullptr) {
                if (draw->hwndItem == page->controls_.buttonRecalculate) {
                    return page->drawRecalculateButton(*draw) ? TRUE : FALSE;
                }
                if (draw->hwndItem == page->controls_.inversionLegendFrame ||
                    draw->hwndItem == page->controls_.correctedLegendFrame ||
                    draw->hwndItem == page->controls_.excessPhaseLegendFrame ||
                    draw->hwndItem == page->controls_.groupDelayLegendFrame) {
                    drawLegendFrame(*draw);
                    return TRUE;
                }
            }
        }
        break;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(hdc, &rect, pageBackgroundBrush);
        return 1;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        static HBRUSH lineInputRightBrush = CreateSolidBrush(ui_theme::kRed);
        static HBRUSH lineInputLeftBrush = CreateSolidBrush(ui_theme::kGreen);
        static HBRUSH lineInversionRightBrush = CreateSolidBrush(ui_theme::kMagenta);
        static HBRUSH lineInversionLeftBrush = CreateSolidBrush(ui_theme::kGray);
        static HBRUSH lineCorrectedTargetBrush = CreateSolidBrush(ui_theme::kAccent);
        static HBRUSH lineCorrectedInputLeftBrush = CreateSolidBrush(ui_theme::kGreen);
        static HBRUSH lineCorrectedInputRightBrush = CreateSolidBrush(ui_theme::kRed);
        static HBRUSH lineCorrectedLeftBrush = CreateSolidBrush(ui_theme::kGray);
        static HBRUSH lineCorrectedRightBrush = CreateSolidBrush(ui_theme::kMagenta);
        static HBRUSH lineExcessPhaseInputRightBrush = CreateSolidBrush(ui_theme::kRed);
        static HBRUSH lineExcessPhaseInputLeftBrush = CreateSolidBrush(ui_theme::kGreen);
        static HBRUSH lineExcessPhasePredictedRightBrush = CreateSolidBrush(ui_theme::kMagenta);
        static HBRUSH lineExcessPhasePredictedLeftBrush = CreateSolidBrush(ui_theme::kGray);
        static HBRUSH linePredictedGroupDelayRightBrush = CreateSolidBrush(ui_theme::kMagenta);
        static HBRUSH linePredictedGroupDelayLeftBrush = CreateSolidBrush(ui_theme::kGray);
        static HBRUSH lineGroupDelayLeftBrush = CreateSolidBrush(ui_theme::kGreen);
        static HBRUSH lineGroupDelayRightBrush = CreateSolidBrush(ui_theme::kRed);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        if (page != nullptr) {
            if (control == page->controls_.lineInputRight) {
                SetBkColor(hdc, ui_theme::kRed);
                return reinterpret_cast<INT_PTR>(lineInputRightBrush);
            }
            if (control == page->controls_.lineInputLeft) {
                SetBkColor(hdc, ui_theme::kGreen);
                return reinterpret_cast<INT_PTR>(lineInputLeftBrush);
            }
            if (control == page->controls_.lineInversionRight) {
                SetBkColor(hdc, ui_theme::kMagenta);
                return reinterpret_cast<INT_PTR>(lineInversionRightBrush);
            }
            if (control == page->controls_.lineInversionLeft) {
                SetBkColor(hdc, ui_theme::kGray);
                return reinterpret_cast<INT_PTR>(lineInversionLeftBrush);
            }
            if (control == page->controls_.lineCorrectedTarget) {
                SetBkColor(hdc, ui_theme::kAccent);
                return reinterpret_cast<INT_PTR>(lineCorrectedTargetBrush);
            }
            if (control == page->controls_.lineCorrectedInputLeft) {
                SetBkColor(hdc, ui_theme::kGreen);
                return reinterpret_cast<INT_PTR>(lineCorrectedInputLeftBrush);
            }
            if (control == page->controls_.lineCorrectedInputRight) {
                SetBkColor(hdc, ui_theme::kRed);
                return reinterpret_cast<INT_PTR>(lineCorrectedInputRightBrush);
            }
            if (control == page->controls_.lineCorrectedLeft) {
                SetBkColor(hdc, ui_theme::kGray);
                return reinterpret_cast<INT_PTR>(lineCorrectedLeftBrush);
            }
            if (control == page->controls_.lineCorrectedRight) {
                SetBkColor(hdc, ui_theme::kMagenta);
                return reinterpret_cast<INT_PTR>(lineCorrectedRightBrush);
            }
            if (control == page->controls_.lineExcessPhaseInputRight) {
                SetBkColor(hdc, ui_theme::kRed);
                return reinterpret_cast<INT_PTR>(lineExcessPhaseInputRightBrush);
            }
            if (control == page->controls_.lineExcessPhaseInputLeft) {
                SetBkColor(hdc, ui_theme::kGreen);
                return reinterpret_cast<INT_PTR>(lineExcessPhaseInputLeftBrush);
            }
            if (control == page->controls_.lineExcessPhasePredictedRight) {
                SetBkColor(hdc, ui_theme::kMagenta);
                return reinterpret_cast<INT_PTR>(lineExcessPhasePredictedRightBrush);
            }
            if (control == page->controls_.lineExcessPhasePredictedLeft) {
                SetBkColor(hdc, ui_theme::kGray);
                return reinterpret_cast<INT_PTR>(lineExcessPhasePredictedLeftBrush);
            }
            if (control == page->controls_.linePredictedGroupDelayRight) {
                SetBkColor(hdc, ui_theme::kMagenta);
                return reinterpret_cast<INT_PTR>(linePredictedGroupDelayRightBrush);
            }
            if (control == page->controls_.linePredictedGroupDelayLeft) {
                SetBkColor(hdc, ui_theme::kGray);
                return reinterpret_cast<INT_PTR>(linePredictedGroupDelayLeftBrush);
            }
            if (control == page->controls_.lineGroupDelayLeft) {
                SetBkColor(hdc, ui_theme::kGreen);
                return reinterpret_cast<INT_PTR>(lineGroupDelayLeftBrush);
            }
            if (control == page->controls_.lineGroupDelayRight) {
                SetBkColor(hdc, ui_theme::kRed);
                return reinterpret_cast<INT_PTR>(lineGroupDelayRightBrush);
            }
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    }
    case WM_CTLCOLOREDIT: {
        static HBRUSH editBackgroundBrush = CreateSolidBrush(ui_theme::kPanelBackground);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, ui_theme::kPanelBackground);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(editBackgroundBrush);
    }
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool FiltersPage::tryParseDouble(const std::wstring& text, double& value) {
    if (text.empty()) {
        return false;
    }
    try {
        size_t cursor = 0;
        value = std::stod(text, &cursor);
        return cursor == text.size();
    } catch (...) {
        return false;
    }
}

std::wstring FiltersPage::getWindowTextValue(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}

void FiltersPage::setWindowTextValue(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

void FiltersPage::populateTapCountCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"16384"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"32768"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"65536"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"131072"));
}

int FiltersPage::comboIndexFromTapCount(int tapCount) {
    switch (tapCount) {
    case 16384:
        return 0;
    case 32768:
        return 1;
    case 65536:
        return 2;
    case 131072:
    default:
        return 3;
    }
}

int FiltersPage::tapCountFromComboIndex(int index) {
    switch (index) {
    case 0:
        return 16384;
    case 1:
        return 32768;
    case 2:
        return 65536;
    case 3:
    default:
        return 131072;
    }
}

void FiltersPage::updateScrollBar() {
    if (window_ == nullptr) {
        return;
    }

    RECT rect{};
    GetClientRect(window_, &rect);
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_PAGE | SIF_RANGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(contentHeight_ - 1, 0);
    info.nPage = static_cast<UINT>(std::max(rect.bottom, 1L));
    info.nPos = scrollOffset_;
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
}

void FiltersPage::setScrollOffset(int scrollOffset) {
    RECT rect{};
    GetClientRect(window_, &rect);
    const int maxScrollOffset = std::max(contentHeight_ - rect.bottom, 0L);
    const int nextScrollOffset = clampValue(scrollOffset, 0, maxScrollOffset);
    if (nextScrollOffset == scrollOffset_) {
        updateScrollBar();
        return;
    }

    const int deltaY = scrollOffset_ - nextScrollOffset;
    scrollOffset_ = nextScrollOffset;
    updateScrollBar();
    ScrollWindowEx(window_,
                   0,
                   deltaY,
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr,
                   SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN);
    UpdateWindow(window_);
}

bool FiltersPage::handleMouseWheel(WPARAM wParam) {
    const int wheelSteps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
    if (wheelSteps == 0) {
        return false;
    }
    setScrollOffset(scrollOffset_ - (wheelSteps * 60));
    return true;
}

void FiltersPage::handleVScroll(WORD code, WORD thumbPosition) {
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    GetScrollInfo(window_, SB_VERT, &info);

    int nextScrollOffset = scrollOffset_;
    switch (code) {
    case SB_LINEUP:
        nextScrollOffset -= 40;
        break;
    case SB_LINEDOWN:
        nextScrollOffset += 40;
        break;
    case SB_PAGEUP:
        nextScrollOffset -= static_cast<int>(info.nPage);
        break;
    case SB_PAGEDOWN:
        nextScrollOffset += static_cast<int>(info.nPage);
        break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK:
        nextScrollOffset = thumbPosition;
        break;
    case SB_TOP:
        nextScrollOffset = 0;
        break;
    case SB_BOTTOM:
        nextScrollOffset = info.nMax;
        break;
    default:
        return;
    }

    setScrollOffset(nextScrollOffset);
}

void FiltersPage::applySharedFrequencyHoverMarker() {
    correctionGraph_.setSharedHoverMarker(syncHoverFrequencyEnabled_, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
    correctedGraph_.setSharedHoverMarker(syncHoverFrequencyEnabled_, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
    excessPhaseGraph_.setSharedHoverMarker(syncHoverFrequencyEnabled_, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
    groupDelayGraph_.setSharedHoverMarker(syncHoverFrequencyEnabled_, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
}

void FiltersPage::configureImpulseGraphViewport(const WorkspaceState& workspace) {
    if (!workspace.filterResult.valid ||
        workspace.filterResult.left.filterTaps.empty() ||
        workspace.filterResult.right.filterTaps.empty()) {
        impulseGraph_.setDefaultXRange(false, 0.0, 1.0);
        impulseGraph_.setDefaultYRange(false, -1.0, 1.0);
        impulseGraph_.resetView();
        return;
    }

    const size_t tapCount = std::min(workspace.filterResult.left.filterTaps.size(),
                                     workspace.filterResult.right.filterTaps.size());
    const double sampleRate = static_cast<double>(std::max(workspace.filterResult.sampleRate, 1));
    const size_t leftPeak = std::min(static_cast<size_t>(std::max(workspace.filterResult.left.impulsePeakIndex, 0)),
                                     tapCount - 1);
    const size_t rightPeak = std::min(static_cast<size_t>(std::max(workspace.filterResult.right.impulsePeakIndex, 0)),
                                      tapCount - 1);
    const double centerIndex = (static_cast<double>(leftPeak) + static_cast<double>(rightPeak)) * 0.5;
    const double centerMs = centerIndex * 1000.0 / sampleRate;
    const double fullMaxMs = static_cast<double>(tapCount - 1) * 1000.0 / sampleRate;

    impulseGraph_.setDefaultXRange(true,
                                   clampValue(centerMs - kImpulseGraphNegativeWindowMs, 0.0, fullMaxMs),
                                   clampValue(centerMs + kImpulseGraphPositiveWindowMs, 0.0, fullMaxMs));
    impulseGraph_.setDefaultYRange(true, -1.0, 1.0);
    impulseGraph_.resetView();
}

PlotGraphData FiltersPage::buildCorrectionGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xValues = workspace.filterResult.frequencyAxisHz;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"dB";
    if (!workspace.filterResult.valid) {
        data.fixedYRange = true;
        data.minY = -workspace.filters.maxCutDb - 3.0;
        data.maxY = workspace.filters.maxBoostDb + 3.0;
        return data;
    }

    double minY = -workspace.filters.maxCutDb - 3.0;
    double maxY = workspace.filters.maxBoostDb + 3.0;
    const auto accumulateRange = [&](const std::vector<double>& values) {
        for (const double value : values) {
            minY = std::min(minY, value);
            maxY = std::max(maxY, value);
        }
    };

    if (showInputRight_) {
        accumulateRange(workspace.smoothedResponse.rightChannelDb);
    }
    if (showInputLeft_) {
        accumulateRange(workspace.smoothedResponse.leftChannelDb);
    }
    if (showInversionRight_) {
        accumulateRange(workspace.filterResult.right.correctionCurveDb);
    }
    if (showInversionLeft_) {
        accumulateRange(workspace.filterResult.left.correctionCurveDb);
    }

    data.fixedYRange = true;
    data.minY = std::floor((minY - 1.5) / 3.0) * 3.0;
    data.maxY = std::ceil((maxY + 1.5) / 3.0) * 3.0;
    if (showInputRight_) {
        data.series.push_back({L"Right input",
                               ui_theme::kRed,
                               resampleLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                                                    workspace.smoothedResponse.rightChannelDb,
                                                    workspace.filterResult.frequencyAxisHz)});
    }
    if (showInputLeft_) {
        data.series.push_back({L"Left input",
                               ui_theme::kGreen,
                               resampleLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                                                    workspace.smoothedResponse.leftChannelDb,
                                                    workspace.filterResult.frequencyAxisHz)});
    }
    if (showInversionRight_) {
        data.series.push_back({L"Right inversion", ui_theme::kMagenta, workspace.filterResult.right.correctionCurveDb});
    }
    if (showInversionLeft_) {
        data.series.push_back({L"Left inversion", ui_theme::kGray, workspace.filterResult.left.correctionCurveDb});
    }
    return data;
}

PlotGraphData FiltersPage::buildCorrectedResponseGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xValues = workspace.filterResult.frequencyAxisHz;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"dB";
    data.fixedYRange = true;
    if (!workspace.filterResult.valid) {
        data.minY = -18.0;
        data.maxY = 12.0;
        return data;
    }

    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    const auto accumulateRange = [&](const std::vector<double>& values) {
        for (const double value : values) {
            minY = std::min(minY, value);
            maxY = std::max(maxY, value);
        }
    };
    accumulateRange(workspace.filterResult.targetCurveDb);
    const std::vector<double> leftInputResponseDb =
        resampleLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                             workspace.smoothedResponse.leftChannelDb,
                             workspace.filterResult.frequencyAxisHz);
    const std::vector<double> rightInputResponseDb =
        resampleLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                             workspace.smoothedResponse.rightChannelDb,
                             workspace.filterResult.frequencyAxisHz);
    if (showCorrectedInputLeft_) {
        accumulateRange(leftInputResponseDb);
    }
    if (showCorrectedInputRight_) {
        accumulateRange(rightInputResponseDb);
    }
    if (showCorrectedLeft_) {
        accumulateRange(workspace.filterResult.left.correctedResponseDb);
    }
    if (showCorrectedRight_) {
        accumulateRange(workspace.filterResult.right.correctedResponseDb);
    }
    if (!std::isfinite(minY) || !std::isfinite(maxY)) {
        minY = -18.0;
        maxY = 12.0;
    }
    const double paddedMin = std::floor((minY - 2.0) / 3.0) * 3.0;
    const double paddedMax = std::ceil((maxY + 2.0) / 3.0) * 3.0;
    const double minimumSpan = 18.0;
    const double center = (paddedMin + paddedMax) * 0.5;
    const double halfSpan = std::max((paddedMax - paddedMin) * 0.5, minimumSpan * 0.5);
    data.minY = center - halfSpan;
    data.maxY = center + halfSpan;

    data.series.push_back({L"Target", ui_theme::kAccent, workspace.filterResult.targetCurveDb});
    if (showCorrectedInputLeft_) {
        data.series.push_back({L"Left input", ui_theme::kGreen, leftInputResponseDb});
    }
    if (showCorrectedInputRight_) {
        data.series.push_back({L"Right input", ui_theme::kRed, rightInputResponseDb});
    }
    if (showCorrectedLeft_) {
        data.series.push_back({L"Left predicted", ui_theme::kGray, workspace.filterResult.left.correctedResponseDb});
    }
    if (showCorrectedRight_) {
        data.series.push_back({L"Right predicted", ui_theme::kMagenta, workspace.filterResult.right.correctedResponseDb});
    }
    return data;
}

PlotGraphData FiltersPage::buildExcessPhaseGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xValues = workspace.filterResult.frequencyAxisHz;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"deg";
    if (!workspace.filterResult.valid) {
        return data;
    }

    if (showExcessPhaseInputRight_) {
        data.series.push_back({L"Right before",
                               ui_theme::kRed,
                               workspace.filterResult.right.inputExcessPhaseDegrees});
    }
    if (showExcessPhaseInputLeft_) {
        data.series.push_back({L"Left before",
                               ui_theme::kGreen,
                               workspace.filterResult.left.inputExcessPhaseDegrees});
    }
    if (showExcessPhasePredictedRight_) {
        data.series.push_back({L"Right after",
                               ui_theme::kMagenta,
                               workspace.filterResult.right.predictedExcessPhaseDegrees});
    }
    if (showExcessPhasePredictedLeft_) {
        data.series.push_back({L"Left after",
                               ui_theme::kGray,
                               workspace.filterResult.left.predictedExcessPhaseDegrees});
    }
    return data;
}

PlotGraphData FiltersPage::buildGroupDelayGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xValues = workspace.filterResult.frequencyAxisHz;
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"ms";
    if (!workspace.filterResult.valid) {
        return data;
    }

    if (showFilterGroupDelayLeft_) {
        data.series.push_back({L"Left", ui_theme::kGreen, workspace.filterResult.left.groupDelayMs});
    }
    if (showFilterGroupDelayRight_) {
        data.series.push_back({L"Right", ui_theme::kRed, workspace.filterResult.right.groupDelayMs});
    }
    if (showPredictedGroupDelayRight_) {
        data.series.push_back({L"Right predicted",
                               ui_theme::kMagenta,
                               workspace.filterResult.right.predictedGroupDelayMs,
                               PlotGraphLineStyle::Dash});
    }
    if (showPredictedGroupDelayLeft_) {
        data.series.push_back({L"Left predicted",
                               ui_theme::kGray,
                               workspace.filterResult.left.predictedGroupDelayMs,
                               PlotGraphLineStyle::Dash});
    }
    return data;
}

PlotGraphData FiltersPage::buildImpulseGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    data.xAxisMode = PlotGraphXAxisMode::Linear;
    data.yAxisMode = PlotGraphYAxisMode::SymmetricAroundZero;
    data.xUnit = L"ms";
    data.yUnit = L"amp";
    if (!workspace.filterResult.valid ||
        workspace.filterResult.left.filterTaps.empty() ||
        workspace.filterResult.right.filterTaps.empty()) {
        return data;
    }

    const size_t tapCount = std::min(workspace.filterResult.left.filterTaps.size(),
                                     workspace.filterResult.right.filterTaps.size());
    const double sampleRate = static_cast<double>(std::max(workspace.filterResult.sampleRate, 1));

    std::vector<double> xValues;
    std::vector<double> leftValues;
    std::vector<double> rightValues;
    xValues.reserve(tapCount);
    leftValues.reserve(tapCount);
    rightValues.reserve(tapCount);
    for (size_t index = 0; index < tapCount; ++index) {
        xValues.push_back(static_cast<double>(index) * 1000.0 / sampleRate);
        leftValues.push_back(workspace.filterResult.left.filterTaps[index]);
        rightValues.push_back(workspace.filterResult.right.filterTaps[index]);
    }

    data.xValues = std::move(xValues);
    data.series.push_back({L"Left", ui_theme::kGreen, std::move(leftValues)});
    data.series.push_back({L"Right", ui_theme::kRed, std::move(rightValues)});
    return data;
}

}  // namespace wolfie::ui
