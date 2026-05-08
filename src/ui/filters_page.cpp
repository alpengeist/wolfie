#include "ui/filters_page.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include <commctrl.h>

#include "core/text_utils.h"
#include "measurement/filter_designer.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr double kSmoothnessSteps[] = {
    0.1,
    1.0,
    2.0,
    4.0,
};
constexpr int kSmoothnessStepCount = static_cast<int>(sizeof(kSmoothnessSteps) / sizeof(kSmoothnessSteps[0]));
constexpr int kGroupDelayZoomRangesMs[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
constexpr int kGroupDelayZoomPresetCount =
    static_cast<int>(sizeof(kGroupDelayZoomRangesMs) / sizeof(kGroupDelayZoomRangesMs[0]));
constexpr int kGroupDelayZoomFitPreset = kGroupDelayZoomPresetCount;
constexpr int kPreRingingCompensationStrengthSteps = 10;
constexpr double kImpulseGraphNegativeWindowMs = 10.0;
constexpr double kImpulseGraphMixedNegativeWindowMs = 50.0;
constexpr double kImpulseGraphPositiveWindowMs = 50.0;
constexpr double kImpulseGraphMinZoomFactor = 0.5;
constexpr double kImpulseGraphMaxZoomFactor = 2.0;
constexpr double kImpulseGraphDefaultYLimit = 0.2;

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

int smoothnessDisplayValueFromSliderPosition(LRESULT position) {
    return clampValue(static_cast<int>(position), 0, kSmoothnessStepCount - 1) + 1;
}

int preRingingCompensationStrengthSliderPositionFromValue(double strength) {
    const double clamped = clampValue(strength, 0.0, 1.0);
    return clampValue(static_cast<int>(std::lround(clamped * kPreRingingCompensationStrengthSteps)),
                      0,
                      kPreRingingCompensationStrengthSteps);
}

double preRingingCompensationStrengthValueFromSliderPosition(LRESULT position) {
    const int clamped = clampValue(static_cast<int>(position), 0, kPreRingingCompensationStrengthSteps);
    return static_cast<double>(clamped) / static_cast<double>(kPreRingingCompensationStrengthSteps);
}

std::wstring formatIntList(const std::vector<int>& values) {
    if (values.empty()) {
        return L"";
    }

    std::wstring text;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            text += L", ";
        }
        text += std::to_wstring(values[index]);
    }
    return text;
}

std::vector<int> parseIntList(const std::wstring& text) {
    std::wstring normalized = text;
    for (wchar_t& ch : normalized) {
        if (ch == L',' || ch == L';' || ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ch = L' ';
        }
    }

    std::wistringstream stream(normalized);
    std::vector<int> values;
    long long value = 0;
    while (stream >> value) {
        if (value >= std::numeric_limits<int>::min() &&
            value <= std::numeric_limits<int>::max()) {
            values.push_back(static_cast<int>(value));
        }
    }
    return values;
}

double interpolateSeriesAtFrequency(const std::vector<double>& frequencyAxisHz,
                                    const std::vector<double>& values,
                                    double frequencyHz) {
    const size_t count = std::min(frequencyAxisHz.size(), values.size());
    if (count == 0 || !std::isfinite(frequencyHz) || frequencyHz <= 0.0) {
        return 0.0;
    }
    if (count == 1 || frequencyHz <= frequencyAxisHz.front()) {
        return values.front();
    }
    if (frequencyHz >= frequencyAxisHz[count - 1]) {
        return values[count - 1];
    }

    const auto upper = std::lower_bound(frequencyAxisHz.begin(),
                                        frequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count),
                                        frequencyHz);
    const size_t upperIndex = static_cast<size_t>(std::distance(frequencyAxisHz.begin(), upper));
    if (upperIndex == 0) {
        return values.front();
    }

    const size_t lowerIndex = upperIndex - 1;
    const double x0 = std::log10(std::max(frequencyAxisHz[lowerIndex], 1.0e-6));
    const double x1 = std::log10(std::max(frequencyAxisHz[upperIndex], 1.0e-6));
    const double x = std::log10(std::max(frequencyHz, 1.0e-6));
    const double y0 = values[lowerIndex];
    const double y1 = values[upperIndex];
    if (!std::isfinite(y0) || !std::isfinite(y1)) {
        return 0.0;
    }

    const double t = clampValue((x - x0) / std::max(x1 - x0, 1.0e-9), 0.0, 1.0);
    return y0 + ((y1 - y0) * t);
}

int clampGroupDelayZoomPreset(int preset) {
    return clampValue(preset, 0, kGroupDelayZoomFitPreset);
}

double impulseGraphNegativeWindowMsForViewMode(const std::string& filterViewMode) {
    if (filterViewMode == "mixed") {
        return kImpulseGraphMixedNegativeWindowMs;
    }
    return kImpulseGraphNegativeWindowMs;
}

std::wstring groupDelayZoomLabelFromPreset(int preset) {
    const int clampedPreset = clampGroupDelayZoomPreset(preset);
    if (clampedPreset == kGroupDelayZoomFitPreset) {
        return L"Fit";
    }

    return L"+-" + std::to_wstring(kGroupDelayZoomRangesMs[clampedPreset]) + L" ms";
}

void drawLegendFrame(const DRAWITEMSTRUCT& draw) {
    FillRect(draw.hDC, &draw.rcItem, ui_theme::backgroundBrush());

    const int savedDc = SaveDC(draw.hDC);
    SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
    HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
    SelectObject(draw.hDC, borderPen);
    Rectangle(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right, draw.rcItem.bottom);
    RestoreDC(draw.hDC, savedDc);
    DeleteObject(borderPen);
}

void drawPhaseCorrectionGroup(const DRAWITEMSTRUCT& draw) {
    FillRect(draw.hDC, &draw.rcItem, ui_theme::backgroundBrush());

    const int savedDc = SaveDC(draw.hDC);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
    if (font != nullptr) {
        SelectObject(draw.hDC, font);
    }

    constexpr wchar_t kTitle[] = L"Phase Correction";
    SIZE titleSize{};
    GetTextExtentPoint32W(draw.hDC, kTitle, static_cast<int>((sizeof(kTitle) / sizeof(kTitle[0])) - 1), &titleSize);

    const int frameTop = draw.rcItem.top + 8;
    const int frameLeft = draw.rcItem.left;
    const int frameRight = draw.rcItem.right - 1;
    const int frameBottom = draw.rcItem.bottom - 1;
    const int titleLeft = draw.rcItem.left + 10;
    const int titlePadding = 4;
    const int titleRight = titleLeft + titleSize.cx + titlePadding;
    const int titleTop = draw.rcItem.top;
    const int titleBottom = titleTop + std::max(static_cast<int>(titleSize.cy), 16);

    HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
    SelectObject(draw.hDC, borderPen);
    MoveToEx(draw.hDC, frameLeft, frameTop, nullptr);
    LineTo(draw.hDC, titleLeft - titlePadding, frameTop);
    MoveToEx(draw.hDC, titleRight + titlePadding, frameTop, nullptr);
    LineTo(draw.hDC, frameRight, frameTop);
    MoveToEx(draw.hDC, frameLeft, frameTop, nullptr);
    LineTo(draw.hDC, frameLeft, frameBottom);
    LineTo(draw.hDC, frameRight, frameBottom);
    LineTo(draw.hDC, frameRight, frameTop);

    RECT titleRect{titleLeft, titleTop, titleRight, titleBottom};
    SetBkMode(draw.hDC, OPAQUE);
    SetBkColor(draw.hDC, ui_theme::backgroundColor());
    SetTextColor(draw.hDC, IsWindowEnabled(draw.hwndItem) ? ui_theme::kText : ui_theme::kMuted);
    DrawTextW(draw.hDC, kTitle, -1, &titleRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

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

bool getComboBoxInfoSafe(HWND combo, COMBOBOXINFO& info) {
    info = {};
    info.cbSize = sizeof(info);
    return combo != nullptr && GetComboBoxInfo(combo, &info) != FALSE;
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

double interpolateLinearAxisValue(const std::vector<double>& sourceAxis,
                                  const std::vector<double>& sourceValues,
                                  double xValue) {
    if (sourceAxis.empty() || sourceValues.size() != sourceAxis.size()) {
        return 0.0;
    }

    if (xValue <= sourceAxis.front()) {
        return sourceValues.front();
    }
    if (xValue >= sourceAxis.back()) {
        return sourceValues.back();
    }

    const auto upper = std::lower_bound(sourceAxis.begin(), sourceAxis.end(), xValue);
    if (upper == sourceAxis.begin()) {
        return sourceValues.front();
    }
    if (upper == sourceAxis.end()) {
        return sourceValues.back();
    }

    const size_t upperIndex = static_cast<size_t>(upper - sourceAxis.begin());
    const size_t lowerIndex = upperIndex - 1;
    return interpolateLinear(xValue,
                             sourceAxis[lowerIndex],
                             sourceValues[lowerIndex],
                             sourceAxis[upperIndex],
                             sourceValues[upperIndex]);
}

double interpolateLinearAxisValueZeroOutside(const std::vector<double>& sourceAxis,
                                             const std::vector<double>& sourceValues,
                                             double xValue) {
    if (sourceAxis.empty() || sourceValues.size() != sourceAxis.size()) {
        return 0.0;
    }

    if (xValue < sourceAxis.front() || xValue > sourceAxis.back()) {
        return 0.0;
    }
    if (xValue <= sourceAxis.front()) {
        return sourceValues.front();
    }
    if (xValue >= sourceAxis.back()) {
        return sourceValues.back();
    }

    return interpolateLinearAxisValue(sourceAxis, sourceValues, xValue);
}

std::vector<double> resampleLinearAxis(const std::vector<double>& sourceAxis,
                                       const std::vector<double>& sourceValues,
                                       const std::vector<double>& targetAxis) {
    std::vector<double> resampled;
    if (sourceAxis.empty() || sourceValues.size() != sourceAxis.size() || targetAxis.empty()) {
        return resampled;
    }

    resampled.reserve(targetAxis.size());
    for (const double xValue : targetAxis) {
        resampled.push_back(interpolateLinearAxisValue(sourceAxis, sourceValues, xValue));
    }
    return resampled;
}

std::vector<double> resampleLinearAxisZeroOutside(const std::vector<double>& sourceAxis,
                                                  const std::vector<double>& sourceValues,
                                                  const std::vector<double>& targetAxis) {
    std::vector<double> resampled;
    if (sourceAxis.empty() || sourceValues.size() != sourceAxis.size() || targetAxis.empty()) {
        return resampled;
    }

    resampled.reserve(targetAxis.size());
    for (const double xValue : targetAxis) {
        resampled.push_back(interpolateLinearAxisValueZeroOutside(sourceAxis, sourceValues, xValue));
    }
    return resampled;
}

std::vector<double> subtractSeries(const std::vector<double>& minuend, const std::vector<double>& subtrahend) {
    const size_t count = std::min(minuend.size(), subtrahend.size());
    std::vector<double> delta;
    delta.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        delta.push_back(minuend[index] - subtrahend[index]);
    }
    return delta;
}

std::vector<double> buildLogDeltaSeries(const FilterDesignResult& minimumResult,
                                        const std::vector<double>& minimumValues,
                                        const FilterDesignResult& mixedResult,
                                        const std::vector<double>& mixedValues,
                                        const std::vector<double>& targetAxisHz) {
    const std::vector<double> minimumResampled =
        resampleLogFrequency(minimumResult.frequencyAxisHz, minimumValues, targetAxisHz);
    const std::vector<double> mixedResampled =
        resampleLogFrequency(mixedResult.frequencyAxisHz, mixedValues, targetAxisHz);
    return subtractSeries(mixedResampled, minimumResampled);
}

std::vector<double> buildLinearDeltaSeries(const std::vector<double>& minimumAxis,
                                           const std::vector<double>& minimumValues,
                                           const std::vector<double>& mixedAxis,
                                           const std::vector<double>& mixedValues,
                                           const std::vector<double>& targetAxis) {
    const std::vector<double> minimumResampled =
        resampleLinearAxisZeroOutside(minimumAxis, minimumValues, targetAxis);
    const std::vector<double> mixedResampled =
        resampleLinearAxisZeroOutside(mixedAxis, mixedValues, targetAxis);
    return subtractSeries(mixedResampled, minimumResampled);
}

void accumulateFiniteRange(const std::vector<double>& values, double& minValue, double& maxValue) {
    for (const double value : values) {
        if (!std::isfinite(value)) {
            continue;
        }
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
    }
}

std::vector<double> buildSharedImpulseTimeAxisMs(const FilterDesignResult& filterResult) {
    const size_t tapCount = std::min(filterResult.left.filterTaps.size(), filterResult.right.filterTaps.size());
    if (tapCount == 0) {
        return {};
    }

    const bool hasLeftAxis = filterResult.left.impulseTimeMs.size() >= tapCount;
    const bool hasRightAxis = filterResult.right.impulseTimeMs.size() >= tapCount;
    std::vector<double> xValues;
    xValues.reserve(tapCount);
    if (hasLeftAxis && hasRightAxis) {
        for (size_t index = 0; index < tapCount; ++index) {
            xValues.push_back((filterResult.left.impulseTimeMs[index] + filterResult.right.impulseTimeMs[index]) * 0.5);
        }
        return xValues;
    }

    if (hasLeftAxis) {
        xValues.insert(xValues.end(),
                       filterResult.left.impulseTimeMs.begin(),
                       filterResult.left.impulseTimeMs.begin() + static_cast<std::ptrdiff_t>(tapCount));
        return xValues;
    }

    if (hasRightAxis) {
        xValues.insert(xValues.end(),
                       filterResult.right.impulseTimeMs.begin(),
                       filterResult.right.impulseTimeMs.begin() + static_cast<std::ptrdiff_t>(tapCount));
        return xValues;
    }

    const double sampleRate = static_cast<double>(std::max(filterResult.sampleRate, 1));
    const double centerIndex = (static_cast<double>(std::max(filterResult.left.impulsePeakIndex, 0)) +
                                static_cast<double>(std::max(filterResult.right.impulsePeakIndex, 0))) * 0.5;
    for (size_t index = 0; index < tapCount; ++index) {
        xValues.push_back((static_cast<double>(index) - centerIndex) * 1000.0 / sampleRate);
    }
    return xValues;
}

double impulseAxisStepMs(const std::vector<double>& axis) {
    if (axis.size() < 2) {
        return 0.0;
    }

    double step = axis[1] - axis[0];
    if (!std::isfinite(step) || std::abs(step) < 1.0e-9) {
        return 0.0;
    }
    return step;
}

std::vector<double> buildImpulseDifferenceTimeAxisMs(const FilterDesignResult& minimumResult,
                                                     const FilterDesignResult& mixedResult) {
    const std::vector<double> minimumAxis = buildSharedImpulseTimeAxisMs(minimumResult);
    const std::vector<double> mixedAxis = buildSharedImpulseTimeAxisMs(mixedResult);
    if (minimumAxis.empty()) {
        return mixedAxis;
    }
    if (mixedAxis.empty()) {
        return minimumAxis;
    }

    double stepMs = impulseAxisStepMs(mixedAxis);
    if (stepMs <= 0.0) {
        stepMs = impulseAxisStepMs(minimumAxis);
    }
    if (stepMs <= 0.0) {
        return mixedAxis;
    }

    const double minMs = std::min(minimumAxis.front(), mixedAxis.front());
    const double maxMs = std::max(minimumAxis.back(), mixedAxis.back());
    const size_t pointCount =
        static_cast<size_t>(std::max(std::llround((maxMs - minMs) / stepMs), 0ll)) + 1;

    std::vector<double> axis;
    axis.reserve(pointCount);
    for (size_t index = 0; index < pointCount; ++index) {
        axis.push_back(minMs + (static_cast<double>(index) * stepMs));
    }
    return axis;
}

std::vector<double> subtractConstant(const std::vector<double>& values, double offset) {
    std::vector<double> shifted = values;
    for (double& value : shifted) {
        value -= offset;
    }
    return shifted;
}

void configureFilterPlotAppearance(PlotGraphData& data) {
    data.zebraStripeYBands = true;
    data.yTickSubdivision = 2;
}

}  // namespace

void FiltersPage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = ui_theme::backgroundBrush();
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
    helpBubble_.create(window_, instance_);

    controls_.labelTapCount = CreateWindowW(L"STATIC", L"Tap Count", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboTapCount = CreateWindowW(L"COMBOBOX",
                                            nullptr,
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL | kHelpBubbleChildClipStyle,
                                            0,
                                            0,
                                            0,
                                            0,
                                            window_,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboTapCount)),
                                            instance_,
                                            nullptr);
    controls_.labelPhaseMode = CreateWindowW(L"STATIC", L"Filter View", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboPhaseMode = CreateWindowW(L"COMBOBOX",
                                             nullptr,
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL | kHelpBubbleChildClipStyle,
                                             0,
                                             0,
                                             0,
                                             0,
                                             window_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboPhaseMode)),
                                             instance_,
                                             nullptr);
    controls_.labelLowCorrection = CreateWindowW(L"STATIC", L"Low Bound", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editLowCorrection = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditLowCorrection)), instance_, nullptr);
    controls_.unitLowCorrection = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelHighCorrection = CreateWindowW(L"STATIC", L"High Bound", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editHighCorrection = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                                   0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditHighCorrection)), instance_, nullptr);
    controls_.unitHighCorrection = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMaxBoost = CreateWindowW(L"STATIC", L"Max Boost", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMaxBoost = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                             0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditMaxBoost)), instance_, nullptr);
    controls_.unitMaxBoost = CreateWindowW(L"STATIC", L"dB", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMaxCut = CreateWindowW(L"STATIC", L"Max Cut", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMaxCut = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                           0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditMaxCut)), instance_, nullptr);
    controls_.unitMaxCut = CreateWindowW(L"STATIC", L"dB", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelSmoothness = CreateWindowW(L"STATIC", L"Inversion Smoothness", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.sliderSmoothness = CreateWindowW(TRACKBAR_CLASSW,
                                               nullptr,
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS | TBS_HORZ | kHelpBubbleChildClipStyle,
                                               0,
                                               0,
                                               0,
                                               0,
                                               window_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSliderSmoothness)),
                                               instance_,
                                               nullptr);
    controls_.valueSmoothness = CreateWindowW(L"STATIC", L"1", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.phaseCorrectionGroup = CreateWindowW(L"STATIC",
                                                   L"Phase Correction",
                                                   WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | kHelpBubbleChildClipStyle,
                                                   0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMixedPhaseMax = CreateWindowW(L"STATIC", L"Limit", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMixedPhaseMax = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditMixedPhaseMax)), instance_, nullptr);
    controls_.unitMixedPhaseMax = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhaseWindow = CreateWindowW(L"STATIC", L"Window", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editExcessPhaseWindow = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                                      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditExcessPhaseWindow)), instance_, nullptr);
    controls_.unitExcessPhaseWindow = CreateWindowW(L"STATIC", L"ms", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMixedPhaseStrength = CreateWindowW(L"STATIC", L"Strength", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMixedPhaseStrength = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                                       0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditMixedPhaseStrength)), instance_, nullptr);
    controls_.unitMixedPhaseStrength = CreateWindowW(L"STATIC", L"0..1", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelMixedPhaseCap = CreateWindowW(L"STATIC", L"Cap", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMixedPhaseCap = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditMixedPhaseCap)), instance_, nullptr);
    controls_.unitMixedPhaseCap = CreateWindowW(L"STATIC", L"deg", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelPreRingingCompensationFrequencies = CreateWindowW(L"STATIC", L"Ring Spots", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editPreRingingCompensationFrequencies = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle,
                                                                      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditPreRingingCompensationFrequencies)), instance_, nullptr);
    controls_.unitPreRingingCompensationFrequencies = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelPreRingingCompensationStrength = CreateWindowW(L"STATIC", L"Ring Compensation", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.sliderPreRingingCompensationStrength = CreateWindowW(TRACKBAR_CLASSW,
                                                                   nullptr,
                                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS | TBS_HORZ | kHelpBubbleChildClipStyle,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   window_,
                                                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSliderPreRingingCompensationStrength)),
                                                                   instance_,
                                                                   nullptr);
    controls_.valuePreRingingCompensationStrength = CreateWindowW(L"STATIC", L"0.00", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonRecalculate = CreateWindowW(L"BUTTON", L"Recalculate", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | kHelpBubbleChildClipStyle,
                                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonRecalculate)), instance_, nullptr);
    helpBubble_.registerLabel(controls_.labelTapCount, L"Sets the FIR length. More taps allow finer correction but increase latency and processing cost.");
    helpBubble_.registerLabel(controls_.labelPhaseMode, L"Chooses which stored filter to display: minimum, mixed, or the mode delta between them.");
    helpBubble_.registerLabel(controls_.labelLowCorrection, L"Sets the lowest frequency where correction is allowed to operate.");
    helpBubble_.registerLabel(controls_.labelHighCorrection, L"Sets the highest frequency where correction is allowed to operate.");
    helpBubble_.registerLabel(controls_.labelMaxBoost, L"Limits how much positive gain the calculated correction may apply.");
    helpBubble_.registerLabel(controls_.labelMaxCut, L"Limits how much attenuation the calculated correction may apply.");
    helpBubble_.registerLabel(controls_.labelSmoothness, L"Controls how tightly the correction follows response detail instead of smoothing it out.");
    helpBubble_.registerLabel(controls_.labelMixedPhaseMax, L"Sets the highest frequency where mixed-phase correction is allowed.");
    helpBubble_.registerLabel(controls_.labelExcessPhaseWindow, L"Sets the time window in milliseconds used to derive the phase-preparation transfer before mixed-phase correction is computed. Increase it only while the requested mixed group-delay chart keeps showing believable low-frequency structure inside the intended correction range. If larger windows start introducing group-delay spikes in the lower mids, especially above the Limit, reduce the window.");
    helpBubble_.registerLabel(controls_.labelMixedPhaseStrength, L"Controls how strongly mixed-phase correction is applied within the allowed range.");
    helpBubble_.registerLabel(controls_.labelMixedPhaseCap, L"Caps the maximum phase rotation the mixed-phase solver may request.");
    helpBubble_.registerLabel(controls_.labelPreRingingCompensationFrequencies,
                              L"Enter space- or comma-separated center frequencies where mixed-phase correction should back off to reduce pre-ringing. Use the requested mixed group-delay chart to inspect the candidate peaks. Peak + and Peak - step through the ranked spot suggestions, and Recalc refreshes that chart after settings changes.");
    helpBubble_.registerLabel(controls_.labelPreRingingCompensationStrength, L"Controls how aggressively the listed pre-ringing spots suppress local mixed-phase correction.");
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
                                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowInputRight)),
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
                                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowInputLeft)),
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
                                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowInversionRight)),
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
                                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowInversionLeft)),
                                                        instance_,
                                                        nullptr);
    controls_.lineInversionLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInversionLeft = CreateWindowW(L"STATIC", L"L inv", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.correctedTitle = CreateWindowW(L"STATIC", L"Predicted Corrected Response", WS_CHILD | WS_VISIBLE,
                                             0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonCorrectedEffect = CreateWindowW(L"BUTTON",
                                                    L"Effect",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    window_,
                                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonCorrectedEffect)),
                                                    instance_,
                                                    nullptr);
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
                                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowCorrectedInputLeft)),
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
                                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowCorrectedInputRight)),
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
                                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowCorrectedLeft)),
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
                                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowCorrectedRight)),
                                                          instance_,
                                                          nullptr);
    controls_.lineCorrectedRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelCorrectedRight = CreateWindowW(L"STATIC", L"R pred", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.excessPhaseTitle = CreateWindowW(L"STATIC", L"Excess Phase", WS_CHILD | WS_VISIBLE,
                                               0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonExcessPhaseEffect = CreateWindowW(L"BUTTON",
                                                      L"Effect",
                                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                                                      0,
                                                      0,
                                                      0,
                                                      0,
                                                      window_,
                                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonExcessPhaseEffect)),
                                                      instance_,
                                                      nullptr);
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
                                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowExcessPhaseInputRight)),
                                                                instance_,
                                                                nullptr);
    controls_.lineExcessPhaseInputRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhaseInputRight = CreateWindowW(L"STATIC", L"R", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowExcessPhaseInputLeft = CreateWindowW(L"BUTTON",
                                                               L"",
                                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                               0,
                                                               0,
                                                               0,
                                                               0,
                                                               window_,
                                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowExcessPhaseInputLeft)),
                                                               instance_,
                                                               nullptr);
    controls_.lineExcessPhaseInputLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhaseInputLeft = CreateWindowW(L"STATIC", L"L", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowExcessPhasePredictedRight = CreateWindowW(L"BUTTON",
                                                                    L"",
                                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                    0,
                                                                    0,
                                                                    0,
                                                                    0,
                                                                    window_,
                                                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowExcessPhasePredictedRight)),
                                                                    instance_,
                                                                    nullptr);
    controls_.lineExcessPhasePredictedRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhasePredictedRight = CreateWindowW(L"STATIC", L"R pred", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowExcessPhasePredictedLeft = CreateWindowW(L"BUTTON",
                                                                   L"",
                                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   window_,
                                                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowExcessPhasePredictedLeft)),
                                                                   instance_,
                                                                   nullptr);
    controls_.lineExcessPhasePredictedLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelExcessPhasePredictedLeft = CreateWindowW(L"STATIC", L"L pred", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.requestedMixedGroupDelayTitle = CreateWindowW(L"STATIC", L"Requested Mixed Group Delay", WS_CHILD | WS_VISIBLE,
                                                            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.requestedMixedGroupDelayLegendFrame = CreateWindowW(L"STATIC",
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
    controls_.checkboxShowRequestedMixedGroupDelayPreRight = CreateWindowW(
        L"BUTTON",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowRequestedMixedGroupDelayPreRight)),
        instance_,
        nullptr);
    controls_.lineRequestedMixedGroupDelayPreRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelRequestedMixedGroupDelayPreRight = CreateWindowW(L"STATIC", L"R pre", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowRequestedMixedGroupDelayPreLeft = CreateWindowW(
        L"BUTTON",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowRequestedMixedGroupDelayPreLeft)),
        instance_,
        nullptr);
    controls_.lineRequestedMixedGroupDelayPreLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelRequestedMixedGroupDelayPreLeft = CreateWindowW(L"STATIC", L"L pre", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowRequestedMixedGroupDelayRight = CreateWindowW(
        L"BUTTON",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowRequestedMixedGroupDelayRight)),
        instance_,
        nullptr);
    controls_.lineRequestedMixedGroupDelayRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelRequestedMixedGroupDelayRight = CreateWindowW(L"STATIC", L"R post", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowRequestedMixedGroupDelayLeft = CreateWindowW(
        L"BUTTON",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowRequestedMixedGroupDelayLeft)),
        instance_,
        nullptr);
    controls_.lineRequestedMixedGroupDelayLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelRequestedMixedGroupDelayLeft = CreateWindowW(L"STATIC", L"L post", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonAddRequestedMixedGroupDelaySpot = CreateWindowW(L"BUTTON",
                                                                    L"Peak +",
                                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | kHelpBubbleChildClipStyle,
                                                                    0,
                                                                    0,
                                                                    0,
                                                                    0,
                                                                    window_,
                                                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonAddRequestedMixedGroupDelaySpot)),
                                                                    instance_,
                                                                    nullptr);
    controls_.buttonRemoveRequestedMixedGroupDelaySpot = CreateWindowW(L"BUTTON",
                                                                       L"Peak -",
                                                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | kHelpBubbleChildClipStyle,
                                                                       0,
                                                                       0,
                                                                       0,
                                                                       0,
                                                                       window_,
                                                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonRemoveRequestedMixedGroupDelaySpot)),
                                                                       instance_,
                                                                       nullptr);
    controls_.buttonResetRequestedMixedGroupDelaySpot = CreateWindowW(L"BUTTON",
                                                                      L"Reset",
                                                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | kHelpBubbleChildClipStyle,
                                                                      0,
                                                                      0,
                                                                      0,
                                                                      0,
                                                                      window_,
                                                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonResetRequestedMixedGroupDelaySpot)),
                                                                      instance_,
                                                                      nullptr);
    controls_.buttonRecalculateRequestedMixedGroupDelay = CreateWindowW(L"BUTTON",
                                                                        L"Recalc",
                                                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | kHelpBubbleChildClipStyle,
                                                                        0,
                                                                        0,
                                                                        0,
                                                                        0,
                                                                        window_,
                                                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonRecalculateRequestedMixedGroupDelay)),
                                                                        instance_,
                                                                        nullptr);
    controls_.groupDelayTitle = CreateWindowW(L"STATIC", L"Group Delay", WS_CHILD | WS_VISIBLE,
                                              0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonGroupDelayEffect = CreateWindowW(L"BUTTON",
                                                     L"Effect",
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     window_,
                                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonGroupDelayEffect)),
                                                     instance_,
                                                     nullptr);
    controls_.labelGroupDelayZoom = CreateWindowW(L"STATIC", L"Y Range", WS_CHILD | WS_VISIBLE,
                                                  0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.sliderGroupDelayZoom = CreateWindowW(TRACKBAR_CLASSW,
                                                   nullptr,
                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS | TBS_HORZ,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   window_,
                                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSliderGroupDelayZoom)),
                                                   instance_,
                                                   nullptr);
    controls_.valueGroupDelayZoom = CreateWindowW(L"STATIC", L"Fit", WS_CHILD | WS_VISIBLE,
                                                  0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    SetWindowSubclass(controls_.sliderGroupDelayZoom, GroupDelayZoomSliderProc, 1, reinterpret_cast<DWORD_PTR>(this));
    helpBubble_.registerLabel(controls_.labelGroupDelayZoom,
                              L"Sets a fixed vertical range for the group-delay chart, or lets the chart fit automatically.");
    controls_.checkboxAlignGroupDelayLatency = CreateWindowW(L"BUTTON",
                                                             L"Align latency",
                                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             window_,
                                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxAlignGroupDelayLatency)),
                                                             instance_,
                                                             nullptr);
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
    controls_.checkboxShowInputGroupDelayLeft = CreateWindowW(L"BUTTON",
                                                              L"",
                                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                              0,
                                                              0,
                                                              0,
                                                              0,
                                                              window_,
                                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowInputGroupDelayLeft)),
                                                              instance_,
                                                              nullptr);
    controls_.lineInputGroupDelayLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInputGroupDelayLeft = CreateWindowW(L"STATIC", L"L", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowInputGroupDelayRight = CreateWindowW(L"BUTTON",
                                                               L"",
                                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                               0,
                                                               0,
                                                               0,
                                                               0,
                                                               window_,
                                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowInputGroupDelayRight)),
                                                               instance_,
                                                               nullptr);
    controls_.lineInputGroupDelayRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelInputGroupDelayRight = CreateWindowW(L"STATIC", L"R", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowFilterGroupDelayLeft = CreateWindowW(L"BUTTON",
                                                               L"",
                                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                               0,
                                                               0,
                                                               0,
                                                               0,
                                                               window_,
                                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowFilterGroupDelayLeft)),
                                                               instance_,
                                                               nullptr);
    controls_.lineGroupDelayLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelGroupDelayLeft = CreateWindowW(L"STATIC", L"L filter", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowFilterGroupDelayRight = CreateWindowW(L"BUTTON",
                                                                L"",
                                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                0,
                                                                0,
                                                                0,
                                                                0,
                                                                window_,
                                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowFilterGroupDelayRight)),
                                                                instance_,
                                                                nullptr);
    controls_.lineGroupDelayRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelGroupDelayRight = CreateWindowW(L"STATIC", L"R filter", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowPredictedGroupDelayRight = CreateWindowW(L"BUTTON",
                                                                   L"",
                                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   window_,
                                                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowPredictedGroupDelayRight)),
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
                                                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckboxShowPredictedGroupDelayLeft)),
                                                                  instance_,
                                                                  nullptr);
    controls_.linePredictedGroupDelayLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelPredictedGroupDelayLeft = CreateWindowW(L"STATIC", L"L pred", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.impulseTitle = CreateWindowW(L"STATIC", L"Filter Impulse", WS_CHILD | WS_VISIBLE,
                                           0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonImpulseZoomOutX = CreateWindowW(L"BUTTON", L"X-", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonImpulseZoomOutX)), instance_, nullptr);
    controls_.buttonImpulseZoomInX = CreateWindowW(L"BUTTON", L"X+", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                   0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonImpulseZoomInX)), instance_, nullptr);
    controls_.buttonImpulseResetX = CreateWindowW(L"BUTTON", L"Reset X", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonImpulseResetX)), instance_, nullptr);
    controls_.buttonImpulseZoomOutY = CreateWindowW(L"BUTTON", L"Y-", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonImpulseZoomOutY)), instance_, nullptr);
    controls_.buttonImpulseZoomInY = CreateWindowW(L"BUTTON", L"Y+", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                   0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonImpulseZoomInY)), instance_, nullptr);
    controls_.buttonImpulseResetY = CreateWindowW(L"BUTTON", L"Reset Y", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonImpulseResetY)), instance_, nullptr);
    controls_.buttonImpulseFit = CreateWindowW(L"BUTTON", L"Fit", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonImpulseFit)), instance_, nullptr);

    populateTapCountCombo(controls_.comboTapCount);
    populatePhaseModeCombo(controls_.comboPhaseMode);
    SendMessageW(controls_.comboTapCount, CB_SETCURSEL, comboIndexFromTapCount(appliedSettings_.tapCount), 0);
    SendMessageW(controls_.comboPhaseMode, CB_SETCURSEL, comboIndexFromFilterViewMode(filterViewMode_), 0);
    SendMessageW(controls_.sliderSmoothness, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.sliderSmoothness, TBM_SETRANGEMAX, FALSE, static_cast<LPARAM>(kSmoothnessStepCount - 1));
    SendMessageW(controls_.sliderSmoothness, TBM_SETTICFREQ, 1, 0);
    SendMessageW(controls_.sliderSmoothness, TBM_SETLINESIZE, 0, 1);
    SendMessageW(controls_.sliderSmoothness, TBM_SETPAGESIZE, 0, 1);
    setSelectedSmoothness(1.0);
    SendMessageW(controls_.sliderPreRingingCompensationStrength, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.sliderPreRingingCompensationStrength,
                 TBM_SETRANGEMAX,
                 FALSE,
                 static_cast<LPARAM>(kPreRingingCompensationStrengthSteps));
    SendMessageW(controls_.sliderPreRingingCompensationStrength, TBM_SETTICFREQ, 1, 0);
    SendMessageW(controls_.sliderPreRingingCompensationStrength, TBM_SETLINESIZE, 0, 1);
    SendMessageW(controls_.sliderPreRingingCompensationStrength, TBM_SETPAGESIZE, 0, 1);
    setSelectedPreRingingCompensationStrength(0.0);
    SendMessageW(controls_.sliderGroupDelayZoom, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.sliderGroupDelayZoom, TBM_SETRANGEMAX, FALSE, static_cast<LPARAM>(kGroupDelayZoomFitPreset));
    SendMessageW(controls_.sliderGroupDelayZoom, TBM_SETTICFREQ, 1, 0);
    SendMessageW(controls_.sliderGroupDelayZoom, TBM_SETLINESIZE, 0, 1);
    SendMessageW(controls_.sliderGroupDelayZoom, TBM_SETPAGESIZE, 0, 1);
    setSelectedGroupDelayZoomPreset(groupDelayZoomPreset_);
    syncViewSettingsToControls();
    correctionGraph_.create(window_, instance_, kCorrectionGraph);
    correctedGraph_.create(window_, instance_, kCorrectedGraph);
    excessPhaseGraph_.create(window_, instance_, kExcessPhaseGraph);
    requestedMixedGroupDelayGraph_.create(window_, instance_, kRequestedMixedGroupDelayGraph);
    groupDelayGraph_.create(window_, instance_, kGroupDelayGraph);
    impulseGraph_.create(window_, instance_, kImpulseGraph);
    correctionGraph_.setHoverCrosshairEnabled(true);
    correctedGraph_.setHoverCrosshairEnabled(true);
    excessPhaseGraph_.setHoverCrosshairEnabled(true);
    requestedMixedGroupDelayGraph_.setHoverCrosshairEnabled(true);
    groupDelayGraph_.setHoverCrosshairEnabled(true);
    refreshPhaseModeControls();
    refreshRecalculateButton();
}

void FiltersPage::layout() {
    helpBubble_.hide();
    RECT pageRect{};
    GetClientRect(window_, &pageRect);
    const int viewportWidth = std::max(480L, pageRect.right);
    const int viewportHeight = std::max(360L, pageRect.bottom);
    const int contentLeft = 20;
    const int contentTop = 20;
    const int contentWidth = std::max(420, viewportWidth - (contentLeft * 2) - GetSystemMetrics(SM_CXVSCROLL));
    const int graphHeight = 320;
    const int graphGap = 34;
    const int sectionGap = 26;
    const int legendGap = 14;
    const int legendWidth = 128;
    const int top = contentTop - scrollOffset_;
    const int comboDropHeight = 220;

    MoveWindow(controls_.labelPhaseMode, contentLeft, top, 84, 18, TRUE);
    MoveWindow(controls_.comboPhaseMode, contentLeft, top + 22, 132, comboDropHeight, TRUE);
    MoveWindow(controls_.labelTapCount, contentLeft + 160, top, 84, 18, TRUE);
    MoveWindow(controls_.comboTapCount, contentLeft + 160, top + 22, 120, comboDropHeight, TRUE);
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
    MoveWindow(controls_.labelSmoothness, contentLeft + 716, top, 250, 18, TRUE);
    MoveWindow(controls_.sliderSmoothness, contentLeft + 716, top + 20, 120, 32, TRUE);
    MoveWindow(controls_.valueSmoothness, contentLeft + 842, top + 24, 36, 18, TRUE);
    const int phaseCorrectionTop = top + 56;
    const int phaseCorrectionHeight = 94;
    MoveWindow(controls_.phaseCorrectionGroup, contentLeft, phaseCorrectionTop, contentWidth, phaseCorrectionHeight, TRUE);
    SetWindowPos(controls_.phaseCorrectionGroup,
                 HWND_BOTTOM,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const int phaseFieldTop = phaseCorrectionTop + 18;
    const int phaseFieldLeft = contentLeft + 14;
    const int limitLeft = phaseFieldLeft;
    const int windowLeft = limitLeft + 118;
    const int strengthLeft = windowLeft + 122;
    const int capLeft = strengthLeft + 122;
    const int spotsLeft = capLeft + 112;
    const int preRingLeft = spotsLeft + 190;
    MoveWindow(controls_.labelMixedPhaseMax, limitLeft, phaseFieldTop, 52, 18, TRUE);
    MoveWindow(controls_.editMixedPhaseMax, limitLeft, phaseFieldTop + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitMixedPhaseMax, limitLeft + 72, phaseFieldTop + 26, 22, 18, TRUE);
    MoveWindow(controls_.labelExcessPhaseWindow, windowLeft, phaseFieldTop, 58, 18, TRUE);
    MoveWindow(controls_.editExcessPhaseWindow, windowLeft, phaseFieldTop + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitExcessPhaseWindow, windowLeft + 72, phaseFieldTop + 26, 24, 18, TRUE);
    MoveWindow(controls_.labelMixedPhaseStrength, strengthLeft, phaseFieldTop, 64, 18, TRUE);
    MoveWindow(controls_.editMixedPhaseStrength, strengthLeft, phaseFieldTop + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitMixedPhaseStrength, strengthLeft + 72, phaseFieldTop + 26, 34, 18, TRUE);
    MoveWindow(controls_.labelMixedPhaseCap, capLeft, phaseFieldTop, 42, 18, TRUE);
    MoveWindow(controls_.editMixedPhaseCap, capLeft, phaseFieldTop + 22, 68, 26, TRUE);
    MoveWindow(controls_.unitMixedPhaseCap, capLeft + 72, phaseFieldTop + 26, 28, 18, TRUE);
    MoveWindow(controls_.labelPreRingingCompensationFrequencies, spotsLeft, phaseFieldTop, 74, 18, TRUE);
    MoveWindow(controls_.editPreRingingCompensationFrequencies, spotsLeft, phaseFieldTop + 22, 130, 26, TRUE);
    MoveWindow(controls_.unitPreRingingCompensationFrequencies, spotsLeft + 136, phaseFieldTop + 26, 22, 18, TRUE);
    MoveWindow(controls_.labelPreRingingCompensationStrength, preRingLeft, phaseFieldTop, 160, 18, TRUE);
    MoveWindow(controls_.sliderPreRingingCompensationStrength, preRingLeft, phaseFieldTop + 20, 118, 32, TRUE);
    MoveWindow(controls_.valuePreRingingCompensationStrength, preRingLeft + 124, phaseFieldTop + 24, 36, 18, TRUE);
    MoveWindow(controls_.buttonRecalculate, contentLeft, phaseCorrectionTop + phaseCorrectionHeight + 12, contentWidth, 32, TRUE);

    int y = phaseCorrectionTop + phaseCorrectionHeight + 62;
    const int legendLeft = contentLeft + contentWidth - legendWidth;
    const int graphRight = legendLeft - legendGap;
    const int effectButtonWidth = 72;
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
    MoveWindow(controls_.checkboxShowInputLeft, checkboxLeft, firstRowTop, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInputLeft, lineLeft, firstRowTop + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInputLeft, labelLeft, firstRowTop + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInputRight, checkboxLeft, firstRowTop + rowStep, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInputRight, lineLeft, firstRowTop + rowStep + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInputRight, labelLeft, firstRowTop + rowStep + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInversionLeft, checkboxLeft, firstRowTop + (rowStep * 2), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInversionLeft, lineLeft, firstRowTop + (rowStep * 2) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInversionLeft, labelLeft, firstRowTop + (rowStep * 2) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInversionRight, checkboxLeft, firstRowTop + (rowStep * 3), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInversionRight, lineLeft, firstRowTop + (rowStep * 3) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInversionRight, labelLeft, firstRowTop + (rowStep * 3) + 2, labelWidth, 18, TRUE);

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.correctedTitle, contentLeft, y, contentWidth, 18, TRUE);
    MoveWindow(controls_.buttonCorrectedEffect, graphRight - effectButtonWidth, y - 2, effectButtonWidth, 22, TRUE);
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
    MoveWindow(controls_.buttonExcessPhaseEffect, graphRight - effectButtonWidth, y - 2, effectButtonWidth, 22, TRUE);
    MoveWindow(controls_.excessPhaseLegendFrame, legendLeft, y + 24, legendWidth, graphHeight, TRUE);
    excessPhaseGraph_.layout(RECT{contentLeft, y + 24, graphRight, y + 24 + graphHeight});
    const int excessPhaseFirstRowTop = y + 24 + 18;
    MoveWindow(controls_.checkboxShowExcessPhaseInputLeft, checkboxLeft, excessPhaseFirstRowTop, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineExcessPhaseInputLeft, lineLeft, excessPhaseFirstRowTop + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelExcessPhaseInputLeft, labelLeft, excessPhaseFirstRowTop + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowExcessPhaseInputRight, checkboxLeft, excessPhaseFirstRowTop + rowStep, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineExcessPhaseInputRight, lineLeft, excessPhaseFirstRowTop + rowStep + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelExcessPhaseInputRight, labelLeft, excessPhaseFirstRowTop + rowStep + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowExcessPhasePredictedLeft, checkboxLeft, excessPhaseFirstRowTop + (rowStep * 2), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineExcessPhasePredictedLeft, lineLeft, excessPhaseFirstRowTop + (rowStep * 2) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelExcessPhasePredictedLeft, labelLeft, excessPhaseFirstRowTop + (rowStep * 2) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowExcessPhasePredictedRight, checkboxLeft, excessPhaseFirstRowTop + (rowStep * 3), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineExcessPhasePredictedRight, lineLeft, excessPhaseFirstRowTop + (rowStep * 3) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelExcessPhasePredictedRight, labelLeft, excessPhaseFirstRowTop + (rowStep * 3) + 2, labelWidth, 18, TRUE);

    y += 24 + graphHeight + graphGap;
    MoveWindow(controls_.requestedMixedGroupDelayTitle, contentLeft, y, contentWidth, 18, TRUE);
    MoveWindow(controls_.requestedMixedGroupDelayLegendFrame, legendLeft, y + 24, legendWidth, graphHeight, TRUE);
    requestedMixedGroupDelayGraph_.layout(RECT{contentLeft, y + 24, graphRight, y + 24 + graphHeight});
    const int requestedMixedGroupDelayFirstRowTop = y + 24 + 18;
    MoveWindow(controls_.checkboxShowRequestedMixedGroupDelayPreLeft,
               checkboxLeft,
               requestedMixedGroupDelayFirstRowTop,
               checkboxWidth,
               20,
               TRUE);
    MoveWindow(controls_.lineRequestedMixedGroupDelayPreLeft,
               lineLeft,
               requestedMixedGroupDelayFirstRowTop + 8,
               lineWidth,
               lineHeight,
               TRUE);
    MoveWindow(controls_.labelRequestedMixedGroupDelayPreLeft,
               labelLeft,
               requestedMixedGroupDelayFirstRowTop + 2,
               labelWidth,
               18,
               TRUE);
    MoveWindow(controls_.checkboxShowRequestedMixedGroupDelayPreRight,
               checkboxLeft,
               requestedMixedGroupDelayFirstRowTop + rowStep,
               checkboxWidth,
               20,
               TRUE);
    MoveWindow(controls_.lineRequestedMixedGroupDelayPreRight,
               lineLeft,
               requestedMixedGroupDelayFirstRowTop + rowStep + 8,
               lineWidth,
               lineHeight,
               TRUE);
    MoveWindow(controls_.labelRequestedMixedGroupDelayPreRight,
               labelLeft,
               requestedMixedGroupDelayFirstRowTop + rowStep + 2,
               labelWidth,
               18,
               TRUE);
    MoveWindow(controls_.checkboxShowRequestedMixedGroupDelayLeft,
               checkboxLeft,
               requestedMixedGroupDelayFirstRowTop + (rowStep * 2),
               checkboxWidth,
               20,
               TRUE);
    MoveWindow(controls_.lineRequestedMixedGroupDelayLeft,
               lineLeft,
               requestedMixedGroupDelayFirstRowTop + (rowStep * 2) + 8,
               lineWidth,
               lineHeight,
               TRUE);
    MoveWindow(controls_.labelRequestedMixedGroupDelayLeft,
               labelLeft,
               requestedMixedGroupDelayFirstRowTop + (rowStep * 2) + 2,
               labelWidth,
               18,
               TRUE);
    MoveWindow(controls_.checkboxShowRequestedMixedGroupDelayRight,
               checkboxLeft,
               requestedMixedGroupDelayFirstRowTop + (rowStep * 3),
               checkboxWidth,
               20,
               TRUE);
    MoveWindow(controls_.lineRequestedMixedGroupDelayRight,
               lineLeft,
               requestedMixedGroupDelayFirstRowTop + (rowStep * 3) + 8,
               lineWidth,
               lineHeight,
               TRUE);
    MoveWindow(controls_.labelRequestedMixedGroupDelayRight,
               labelLeft,
               requestedMixedGroupDelayFirstRowTop + (rowStep * 3) + 2,
               labelWidth,
               18,
               TRUE);
    const int requestedMixedButtonLeft = legendLeft + 12;
    const int requestedMixedButtonWidth = legendWidth - 24;
    const int requestedMixedButtonsTop = requestedMixedGroupDelayFirstRowTop + (rowStep * 4) + 8;
    MoveWindow(controls_.buttonAddRequestedMixedGroupDelaySpot,
               requestedMixedButtonLeft,
               requestedMixedButtonsTop,
               requestedMixedButtonWidth,
               24,
               TRUE);
    SetWindowPos(controls_.buttonAddRequestedMixedGroupDelaySpot,
                 HWND_TOP,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MoveWindow(controls_.buttonRemoveRequestedMixedGroupDelaySpot,
               requestedMixedButtonLeft,
               requestedMixedButtonsTop + 30,
               requestedMixedButtonWidth,
               24,
               TRUE);
    SetWindowPos(controls_.buttonRemoveRequestedMixedGroupDelaySpot,
                 HWND_TOP,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MoveWindow(controls_.buttonResetRequestedMixedGroupDelaySpot,
               requestedMixedButtonLeft,
               requestedMixedButtonsTop + 60,
               requestedMixedButtonWidth,
               24,
               TRUE);
    SetWindowPos(controls_.buttonResetRequestedMixedGroupDelaySpot,
                 HWND_TOP,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MoveWindow(controls_.buttonRecalculateRequestedMixedGroupDelay,
               requestedMixedButtonLeft,
               requestedMixedButtonsTop + 90,
               requestedMixedButtonWidth,
               24,
               TRUE);
    SetWindowPos(controls_.buttonRecalculateRequestedMixedGroupDelay,
                 HWND_TOP,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    y += 52 + graphHeight + graphGap;
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

    y += 52 + graphHeight + graphGap;
    MoveWindow(controls_.groupDelayTitle, contentLeft, y, contentWidth, 18, TRUE);
    MoveWindow(controls_.checkboxAlignGroupDelayLatency, graphRight - 204, y - 2, 124, 20, TRUE);
    MoveWindow(controls_.buttonGroupDelayEffect, graphRight - effectButtonWidth, y - 2, effectButtonWidth, 22, TRUE);
    MoveWindow(controls_.labelGroupDelayZoom, contentLeft, y + 26, 52, 18, TRUE);
    MoveWindow(controls_.valueGroupDelayZoom, graphRight - 64, y + 26, 64, 18, TRUE);
    const int groupDelaySliderLeft = contentLeft + 58;
    const int groupDelaySliderWidth = std::max(100, (graphRight - 72) - groupDelaySliderLeft);
    MoveWindow(controls_.sliderGroupDelayZoom, groupDelaySliderLeft, y + 22, groupDelaySliderWidth, 28, TRUE);
    MoveWindow(controls_.groupDelayLegendFrame, legendLeft, y + 52, legendWidth, graphHeight, TRUE);
    groupDelayGraph_.layout(RECT{contentLeft, y + 52, graphRight, y + 52 + graphHeight});
    const int groupDelayFirstRowTop = y + 52 + 18;
    MoveWindow(controls_.checkboxShowInputGroupDelayLeft, checkboxLeft, groupDelayFirstRowTop, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInputGroupDelayLeft, lineLeft, groupDelayFirstRowTop + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInputGroupDelayLeft, labelLeft, groupDelayFirstRowTop + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowInputGroupDelayRight, checkboxLeft, groupDelayFirstRowTop + rowStep, checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineInputGroupDelayRight, lineLeft, groupDelayFirstRowTop + rowStep + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelInputGroupDelayRight, labelLeft, groupDelayFirstRowTop + rowStep + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowFilterGroupDelayLeft, checkboxLeft, groupDelayFirstRowTop + (rowStep * 2), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineGroupDelayLeft, lineLeft, groupDelayFirstRowTop + (rowStep * 2) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelGroupDelayLeft, labelLeft, groupDelayFirstRowTop + (rowStep * 2) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowFilterGroupDelayRight, checkboxLeft, groupDelayFirstRowTop + (rowStep * 3), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.lineGroupDelayRight, lineLeft, groupDelayFirstRowTop + (rowStep * 3) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelGroupDelayRight, labelLeft, groupDelayFirstRowTop + (rowStep * 3) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowPredictedGroupDelayLeft, checkboxLeft, groupDelayFirstRowTop + (rowStep * 4), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.linePredictedGroupDelayLeft, lineLeft, groupDelayFirstRowTop + (rowStep * 4) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelPredictedGroupDelayLeft, labelLeft, groupDelayFirstRowTop + (rowStep * 4) + 2, labelWidth, 18, TRUE);
    MoveWindow(controls_.checkboxShowPredictedGroupDelayRight, checkboxLeft, groupDelayFirstRowTop + (rowStep * 5), checkboxWidth, 20, TRUE);
    MoveWindow(controls_.linePredictedGroupDelayRight, lineLeft, groupDelayFirstRowTop + (rowStep * 5) + 8, lineWidth, lineHeight, TRUE);
    MoveWindow(controls_.labelPredictedGroupDelayRight, labelLeft, groupDelayFirstRowTop + (rowStep * 5) + 2, labelWidth, 18, TRUE);

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
    filterViewMode_ = workspace.ui.filterViewMode;
    SendMessageW(controls_.comboPhaseMode, CB_SETCURSEL, comboIndexFromFilterViewMode(filterViewMode_), 0);
    setWindowTextValue(controls_.editLowCorrection, formatWideDouble(settings.lowCorrectionHz, 0));
    setWindowTextValue(controls_.editHighCorrection, formatWideDouble(settings.highCorrectionHz, 0));
    setWindowTextValue(controls_.editMaxBoost, formatWideDouble(settings.maxBoostDb, 1));
    setWindowTextValue(controls_.editMaxCut, formatWideDouble(settings.maxCutDb, 1));
    setSelectedSmoothness(settings.smoothness);
    setWindowTextValue(controls_.editMixedPhaseMax, formatWideDouble(settings.mixedPhaseMaxFrequencyHz, 0));
    setWindowTextValue(controls_.editExcessPhaseWindow, formatWideDouble(settings.excessPhaseWindowMs, 0));
    setWindowTextValue(controls_.editMixedPhaseStrength, formatWideDouble(settings.mixedPhaseStrength, 2));
    setWindowTextValue(controls_.editMixedPhaseCap, formatWideDouble(settings.mixedPhaseMaxCorrectionDegrees, 0));
    setWindowTextValue(controls_.editPreRingingCompensationFrequencies,
                       formatIntList(settings.preRingingCompensationFrequenciesHz));
    setSelectedPreRingingCompensationStrength(settings.preRingingCompensationStrength);
    refreshExcessPhaseWindowLabel();
    loadViewSettings(workspace.ui);
    syncViewSettingsToControls();
    refreshPhaseModeControls();
    refreshFilterViewPresentation();
    appliedSettings_ = settings;
    refreshRecalculateButton();

    correctionGraph_.setData(buildCorrectionGraphData(workspace));
    correctedGraph_.setData(buildCorrectedResponseGraphData(workspace));
    excessPhaseGraph_.setData(buildExcessPhaseGraphData(workspace));
    requestedMixedGroupDelayGraph_.setData(buildRequestedMixedGroupDelayGraphData(workspace));
    groupDelayGraph_.setData(buildGroupDelayGraphData(workspace));
    applyGroupDelayZoomRange();
    impulseGraph_.setData(buildImpulseGraphData(workspace));
    configureImpulseGraphViewport(workspace);
    applySharedFrequencyHoverMarker();
}

void FiltersPage::syncToWorkspace(WorkspaceState& workspace) const {
    workspace.filters.tapCount = tapCountFromComboIndex(static_cast<int>(SendMessageW(controls_.comboTapCount, CB_GETCURSEL, 0, 0)));
    workspace.ui.filterViewMode =
        filterViewModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPhaseMode, CB_GETCURSEL, 0, 0)));
    if (workspace.ui.filterViewMode != "difference") {
        workspace.filters.phaseMode = workspace.ui.filterViewMode;
    }
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
    if (tryParseDouble(getWindowTextValue(controls_.editMixedPhaseMax), value)) {
        workspace.filters.mixedPhaseMaxFrequencyHz = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editExcessPhaseWindow), value)) {
        workspace.filters.excessPhaseWindowMs = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMixedPhaseStrength), value)) {
        workspace.filters.mixedPhaseStrength = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMixedPhaseCap), value)) {
        workspace.filters.mixedPhaseMaxCorrectionDegrees = value;
    }
    workspace.filters.preRingingCompensationFrequenciesHz =
        parseIntList(getWindowTextValue(controls_.editPreRingingCompensationFrequencies));
    workspace.filters.preRingingCompensationStrength = selectedPreRingingCompensationStrength();
    measurement::normalizeFilterDesignSettings(workspace.filters, workspace.measurement.sampleRate);
    saveViewSettings(workspace.ui);
}

void FiltersPage::setRecalculateInProgress(bool running) {
    recalculateInProgress_ = running;
    if (!running) {
        const HWND graphWindows[] = {
            correctionGraph_.window(),
            correctedGraph_.window(),
            excessPhaseGraph_.window(),
            requestedMixedGroupDelayGraph_.window(),
            groupDelayGraph_.window(),
            impulseGraph_.window(),
        };
        for (HWND graphWindow : graphWindows) {
            if (graphWindow != nullptr && IsWindowVisible(graphWindow)) {
                RedrawWindow(graphWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            }
        }
    }
    if (controls_.buttonRecalculate != nullptr) {
        EnableWindow(controls_.buttonRecalculate, (!running && !differenceViewSelected()) ? TRUE : FALSE);
        RedrawWindow(controls_.buttonRecalculate, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    if (controls_.buttonRecalculateRequestedMixedGroupDelay != nullptr) {
        EnableWindow(controls_.buttonRecalculateRequestedMixedGroupDelay,
                     (!running && !differenceViewSelected() && mixedModeSelected()) ? TRUE : FALSE);
        RedrawWindow(controls_.buttonRecalculateRequestedMixedGroupDelay,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW);
    }
    if (controls_.buttonResetRequestedMixedGroupDelaySpot != nullptr) {
        EnableWindow(controls_.buttonResetRequestedMixedGroupDelaySpot,
                     (!running && !differenceViewSelected() && mixedModeSelected()) ? TRUE : FALSE);
        RedrawWindow(controls_.buttonResetRequestedMixedGroupDelaySpot,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

double FiltersPage::selectedSmoothness() const {
    return smoothnessValueFromSliderPosition(SendMessageW(controls_.sliderSmoothness, TBM_GETPOS, 0, 0));
}

void FiltersPage::setSelectedSmoothness(double smoothness) const {
    SendMessageW(controls_.sliderSmoothness, TBM_SETPOS, TRUE, smoothnessSliderPositionFromValue(smoothness));
    refreshSmoothnessValue();
}

double FiltersPage::selectedPreRingingCompensationStrength() const {
    return preRingingCompensationStrengthValueFromSliderPosition(
        SendMessageW(controls_.sliderPreRingingCompensationStrength, TBM_GETPOS, 0, 0));
}

void FiltersPage::setSelectedPreRingingCompensationStrength(double strength) const {
    SendMessageW(controls_.sliderPreRingingCompensationStrength,
                 TBM_SETPOS,
                 TRUE,
                 preRingingCompensationStrengthSliderPositionFromValue(strength));
    refreshPreRingingCompensationStrengthValue();
}

void FiltersPage::refreshSmoothnessValue() const {
    setWindowTextValue(controls_.valueSmoothness,
                       std::to_wstring(smoothnessDisplayValueFromSliderPosition(
                           SendMessageW(controls_.sliderSmoothness, TBM_GETPOS, 0, 0))));
}

void FiltersPage::refreshPreRingingCompensationStrengthValue() const {
    setWindowTextValue(controls_.valuePreRingingCompensationStrength,
                       formatWideDouble(selectedPreRingingCompensationStrength(), 2));
}

void FiltersPage::refreshExcessPhaseWindowLabel() const {
    SetWindowTextW(controls_.labelExcessPhaseWindow, L"Window");
}

int FiltersPage::selectedGroupDelayZoomPreset() const {
    return clampGroupDelayZoomPreset(static_cast<int>(SendMessageW(controls_.sliderGroupDelayZoom, TBM_GETPOS, 0, 0)));
}

void FiltersPage::setSelectedGroupDelayZoomPreset(int preset) const {
    SendMessageW(controls_.sliderGroupDelayZoom, TBM_SETPOS, TRUE, clampGroupDelayZoomPreset(preset));
    refreshGroupDelayZoomValue();
}

void FiltersPage::refreshGroupDelayZoomValue() const {
    setWindowTextValue(controls_.valueGroupDelayZoom, groupDelayZoomLabelFromPreset(selectedGroupDelayZoomPreset()));
}

bool FiltersPage::mixedModeSelected() const {
    const std::string filterViewMode =
        filterViewModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPhaseMode, CB_GETCURSEL, 0, 0)));
    return filterViewMode == "mixed";
}

bool FiltersPage::differenceViewSelected() const {
    const std::string filterViewMode =
        filterViewModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPhaseMode, CB_GETCURSEL, 0, 0)));
    return filterViewMode == "difference";
}

void FiltersPage::refreshPhaseModeControls() const {
    const BOOL mixedEnabled = mixedModeSelected() ? TRUE : FALSE;
    EnableWindow(controls_.phaseCorrectionGroup, mixedEnabled);
    EnableWindow(controls_.labelMixedPhaseMax, mixedEnabled);
    EnableWindow(controls_.editMixedPhaseMax, mixedEnabled);
    EnableWindow(controls_.unitMixedPhaseMax, mixedEnabled);
    EnableWindow(controls_.labelExcessPhaseWindow, mixedEnabled);
    EnableWindow(controls_.editExcessPhaseWindow, mixedEnabled);
    EnableWindow(controls_.unitExcessPhaseWindow, mixedEnabled);
    EnableWindow(controls_.labelMixedPhaseStrength, mixedEnabled);
    EnableWindow(controls_.editMixedPhaseStrength, mixedEnabled);
    EnableWindow(controls_.unitMixedPhaseStrength, mixedEnabled);
    EnableWindow(controls_.labelMixedPhaseCap, mixedEnabled);
    EnableWindow(controls_.editMixedPhaseCap, mixedEnabled);
    EnableWindow(controls_.unitMixedPhaseCap, mixedEnabled);
    EnableWindow(controls_.labelPreRingingCompensationFrequencies, mixedEnabled);
    EnableWindow(controls_.editPreRingingCompensationFrequencies, mixedEnabled);
    EnableWindow(controls_.unitPreRingingCompensationFrequencies, mixedEnabled);
    EnableWindow(controls_.labelPreRingingCompensationStrength, mixedEnabled);
    EnableWindow(controls_.sliderPreRingingCompensationStrength, mixedEnabled);
    EnableWindow(controls_.valuePreRingingCompensationStrength, mixedEnabled);
    EnableWindow(controls_.buttonAddRequestedMixedGroupDelaySpot, mixedEnabled);
    EnableWindow(controls_.buttonRemoveRequestedMixedGroupDelaySpot, mixedEnabled);
    EnableWindow(controls_.buttonResetRequestedMixedGroupDelaySpot, mixedEnabled);
    EnableWindow(controls_.buttonRecalculateRequestedMixedGroupDelay, mixedEnabled);
}

void FiltersPage::refreshFilterViewPresentation() const {
    const bool differenceView = differenceViewSelected();
    const bool mixedView = mixedModeSelected();
    const bool correctedEffectView = !differenceView && showCorrectedEffect_;
    const bool excessPhaseEffectView = !differenceView && showExcessPhaseEffect_;
    const bool groupDelayEffectView = !differenceView && showGroupDelayEffect_;

    ShowWindow(controls_.buttonCorrectedEffect, differenceView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.buttonExcessPhaseEffect, differenceView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.buttonGroupDelayEffect, differenceView ? SW_HIDE : SW_SHOW);

    ShowWindow(controls_.checkboxShowInputRight, differenceView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineInputRight, differenceView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelInputRight, differenceView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.checkboxShowInputLeft, differenceView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineInputLeft, differenceView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelInputLeft, differenceView ? SW_HIDE : SW_SHOW);
    SetWindowTextW(controls_.labelInputRight, L"R");
    SetWindowTextW(controls_.labelInputLeft, L"L");
    SetWindowTextW(controls_.labelInversionRight, L"R inv");
    SetWindowTextW(controls_.labelInversionLeft, L"L inv");

    ShowWindow(controls_.lineCorrectedTarget, (differenceView || correctedEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelCorrectedTarget, (differenceView || correctedEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.checkboxShowCorrectedInputLeft, (differenceView || correctedEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineCorrectedInputLeft, (differenceView || correctedEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelCorrectedInputLeft, (differenceView || correctedEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.checkboxShowCorrectedInputRight, (differenceView || correctedEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineCorrectedInputRight, (differenceView || correctedEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelCorrectedInputRight, (differenceView || correctedEffectView) ? SW_HIDE : SW_SHOW);
    SetWindowTextW(controls_.labelCorrectedInputLeft, L"L");
    SetWindowTextW(controls_.labelCorrectedInputRight, L"R");
    SetWindowTextW(controls_.labelCorrectedLeft, correctedEffectView ? L"L effect" : L"L pred");
    SetWindowTextW(controls_.labelCorrectedRight, correctedEffectView ? L"R effect" : L"R pred");

    ShowWindow(controls_.checkboxShowExcessPhaseInputRight, (differenceView || excessPhaseEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineExcessPhaseInputRight, (differenceView || excessPhaseEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelExcessPhaseInputRight, (differenceView || excessPhaseEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.checkboxShowExcessPhaseInputLeft, (differenceView || excessPhaseEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineExcessPhaseInputLeft, (differenceView || excessPhaseEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelExcessPhaseInputLeft, (differenceView || excessPhaseEffectView) ? SW_HIDE : SW_SHOW);
    SetWindowTextW(controls_.labelExcessPhaseInputRight, L"R");
    SetWindowTextW(controls_.labelExcessPhaseInputLeft, L"L");
    SetWindowTextW(controls_.labelExcessPhasePredictedRight, excessPhaseEffectView ? L"R effect" : L"R pred");
    SetWindowTextW(controls_.labelExcessPhasePredictedLeft, excessPhaseEffectView ? L"L effect" : L"L pred");

    ShowWindow(controls_.buttonAddRequestedMixedGroupDelaySpot, (!differenceView && mixedView) ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.buttonRemoveRequestedMixedGroupDelaySpot, (!differenceView && mixedView) ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.buttonResetRequestedMixedGroupDelaySpot, (!differenceView && mixedView) ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.buttonRecalculateRequestedMixedGroupDelay, (!differenceView && mixedView) ? SW_SHOW : SW_HIDE);

    ShowWindow(controls_.checkboxShowInputGroupDelayLeft, (differenceView || groupDelayEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineInputGroupDelayLeft, (differenceView || groupDelayEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelInputGroupDelayLeft, (differenceView || groupDelayEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.checkboxShowInputGroupDelayRight, (differenceView || groupDelayEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineInputGroupDelayRight, (differenceView || groupDelayEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelInputGroupDelayRight, (differenceView || groupDelayEffectView) ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.checkboxShowFilterGroupDelayLeft, groupDelayEffectView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineGroupDelayLeft, groupDelayEffectView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelGroupDelayLeft, groupDelayEffectView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.checkboxShowFilterGroupDelayRight, groupDelayEffectView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.lineGroupDelayRight, groupDelayEffectView ? SW_HIDE : SW_SHOW);
    ShowWindow(controls_.labelGroupDelayRight, groupDelayEffectView ? SW_HIDE : SW_SHOW);
    SetWindowTextW(controls_.labelInputGroupDelayLeft, L"L");
    SetWindowTextW(controls_.labelInputGroupDelayRight, L"R");
    SetWindowTextW(controls_.labelGroupDelayLeft, L"L filter");
    SetWindowTextW(controls_.labelGroupDelayRight, L"R filter");
    SetWindowTextW(controls_.labelPredictedGroupDelayLeft, groupDelayEffectView ? L"L effect" : L"L pred");
    SetWindowTextW(controls_.labelPredictedGroupDelayRight, groupDelayEffectView ? L"R effect" : L"R pred");
}

void FiltersPage::applyGroupDelayZoomRange() {
    const int preset = selectedGroupDelayZoomPreset();
    if (preset == kGroupDelayZoomFitPreset) {
        groupDelayGraph_.setDefaultYRange(false, -1.0, 1.0);
        return;
    }

    const double rangeMs = static_cast<double>(kGroupDelayZoomRangesMs[preset]);
    groupDelayGraph_.setDefaultYRange(true, -rangeMs, rangeMs);
}

void FiltersPage::loadViewSettings(const UiSettings& ui) {
    filterViewMode_ = ui.filterViewMode;
    showInputRight_ = ui.filterShowInputRight;
    showInputLeft_ = ui.filterShowInputLeft;
    showInversionRight_ = ui.filterShowInversionRight;
    showInversionLeft_ = ui.filterShowInversionLeft;
    showCorrectedInputLeft_ = ui.filterShowCorrectedInputLeft;
    showCorrectedInputRight_ = ui.filterShowCorrectedInputRight;
    showCorrectedLeft_ = ui.filterShowCorrectedLeft;
    showCorrectedRight_ = ui.filterShowCorrectedRight;
    showExcessPhaseInputRight_ = ui.filterShowExcessPhaseInputRight;
    showExcessPhaseInputLeft_ = ui.filterShowExcessPhaseInputLeft;
    showExcessPhasePredictedRight_ = ui.filterShowExcessPhasePredictedRight;
    showExcessPhasePredictedLeft_ = ui.filterShowExcessPhasePredictedLeft;
    showRequestedMixedGroupDelayPreRight_ = ui.filterShowRequestedMixedGroupDelayPreRight;
    showRequestedMixedGroupDelayPreLeft_ = ui.filterShowRequestedMixedGroupDelayPreLeft;
    showRequestedMixedGroupDelayRight_ = ui.filterShowRequestedMixedGroupDelayRight;
    showRequestedMixedGroupDelayLeft_ = ui.filterShowRequestedMixedGroupDelayLeft;
    showInputGroupDelayLeft_ = ui.filterShowInputGroupDelayLeft;
    showInputGroupDelayRight_ = ui.filterShowInputGroupDelayRight;
    showPredictedGroupDelayRight_ = ui.filterShowPredictedGroupDelayRight;
    showPredictedGroupDelayLeft_ = ui.filterShowPredictedGroupDelayLeft;
    showFilterGroupDelayLeft_ = ui.filterShowFilterGroupDelayLeft;
    showFilterGroupDelayRight_ = ui.filterShowFilterGroupDelayRight;
    alignGroupDelayLatency_ = ui.filterAlignGroupDelayLatency;
    groupDelayZoomPreset_ = clampGroupDelayZoomPreset(ui.filterGroupDelayZoomPreset);
    sharedFrequencyHoverActive_ = false;
}

void FiltersPage::syncViewSettingsToControls() const {
    SendMessageW(controls_.checkboxShowInputRight, BM_SETCHECK, showInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInputLeft, BM_SETCHECK, showInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionRight, BM_SETCHECK, showInversionRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInversionLeft, BM_SETCHECK, showInversionLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedInputLeft, BM_SETCHECK, showCorrectedInputLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedInputRight, BM_SETCHECK, showCorrectedInputRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedLeft, BM_SETCHECK, showCorrectedLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowCorrectedRight, BM_SETCHECK, showCorrectedRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.buttonCorrectedEffect, BM_SETCHECK, showCorrectedEffect_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowExcessPhaseInputRight,
                 BM_SETCHECK,
                 showExcessPhaseInputRight_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowExcessPhaseInputLeft,
                 BM_SETCHECK,
                 showExcessPhaseInputLeft_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowExcessPhasePredictedRight,
                 BM_SETCHECK,
                 showExcessPhasePredictedRight_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowExcessPhasePredictedLeft,
                 BM_SETCHECK,
                 showExcessPhasePredictedLeft_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayPreRight,
                 BM_SETCHECK,
                 showRequestedMixedGroupDelayPreRight_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayPreLeft,
                 BM_SETCHECK,
                 showRequestedMixedGroupDelayPreLeft_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayRight,
                 BM_SETCHECK,
                 showRequestedMixedGroupDelayRight_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayLeft,
                 BM_SETCHECK,
                 showRequestedMixedGroupDelayLeft_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.buttonExcessPhaseEffect, BM_SETCHECK, showExcessPhaseEffect_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowInputGroupDelayLeft,
                 BM_SETCHECK,
                 showInputGroupDelayLeft_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowInputGroupDelayRight,
                 BM_SETCHECK,
                 showInputGroupDelayRight_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowPredictedGroupDelayRight,
                 BM_SETCHECK,
                 showPredictedGroupDelayRight_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowPredictedGroupDelayLeft,
                 BM_SETCHECK,
                 showPredictedGroupDelayLeft_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowFilterGroupDelayLeft,
                 BM_SETCHECK,
                 showFilterGroupDelayLeft_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.checkboxShowFilterGroupDelayRight,
                 BM_SETCHECK,
                 showFilterGroupDelayRight_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(controls_.buttonGroupDelayEffect, BM_SETCHECK, showGroupDelayEffect_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxAlignGroupDelayLatency,
                 BM_SETCHECK,
                 alignGroupDelayLatency_ ? BST_CHECKED : BST_UNCHECKED,
                 0);
    setSelectedGroupDelayZoomPreset(groupDelayZoomPreset_);
}

void FiltersPage::syncViewSettingsFromControls() {
    showInputRight_ = SendMessageW(controls_.checkboxShowInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showInputLeft_ = SendMessageW(controls_.checkboxShowInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showInversionRight_ = SendMessageW(controls_.checkboxShowInversionRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showInversionLeft_ = SendMessageW(controls_.checkboxShowInversionLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showCorrectedInputLeft_ = SendMessageW(controls_.checkboxShowCorrectedInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showCorrectedInputRight_ = SendMessageW(controls_.checkboxShowCorrectedInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showCorrectedLeft_ = SendMessageW(controls_.checkboxShowCorrectedLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showCorrectedRight_ = SendMessageW(controls_.checkboxShowCorrectedRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showCorrectedEffect_ = SendMessageW(controls_.buttonCorrectedEffect, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showExcessPhaseInputRight_ =
        SendMessageW(controls_.checkboxShowExcessPhaseInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showExcessPhaseInputLeft_ =
        SendMessageW(controls_.checkboxShowExcessPhaseInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showExcessPhasePredictedRight_ =
        SendMessageW(controls_.checkboxShowExcessPhasePredictedRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showExcessPhasePredictedLeft_ =
        SendMessageW(controls_.checkboxShowExcessPhasePredictedLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showRequestedMixedGroupDelayPreRight_ =
        SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayPreRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showRequestedMixedGroupDelayPreLeft_ =
        SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayPreLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showRequestedMixedGroupDelayRight_ =
        SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showRequestedMixedGroupDelayLeft_ =
        SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showExcessPhaseEffect_ = SendMessageW(controls_.buttonExcessPhaseEffect, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showInputGroupDelayLeft_ =
        SendMessageW(controls_.checkboxShowInputGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showInputGroupDelayRight_ =
        SendMessageW(controls_.checkboxShowInputGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showPredictedGroupDelayRight_ =
        SendMessageW(controls_.checkboxShowPredictedGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showPredictedGroupDelayLeft_ =
        SendMessageW(controls_.checkboxShowPredictedGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showFilterGroupDelayLeft_ =
        SendMessageW(controls_.checkboxShowFilterGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showFilterGroupDelayRight_ =
        SendMessageW(controls_.checkboxShowFilterGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    showGroupDelayEffect_ = SendMessageW(controls_.buttonGroupDelayEffect, BM_GETCHECK, 0, 0) == BST_CHECKED;
    alignGroupDelayLatency_ = SendMessageW(controls_.checkboxAlignGroupDelayLatency, BM_GETCHECK, 0, 0) == BST_CHECKED;
    groupDelayZoomPreset_ = selectedGroupDelayZoomPreset();
}

void FiltersPage::saveViewSettings(UiSettings& ui) const {
    ui.filterViewMode =
        filterViewModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPhaseMode, CB_GETCURSEL, 0, 0)));
    ui.filterShowInputRight = SendMessageW(controls_.checkboxShowInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowInputLeft = SendMessageW(controls_.checkboxShowInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowInversionRight = SendMessageW(controls_.checkboxShowInversionRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowInversionLeft = SendMessageW(controls_.checkboxShowInversionLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowCorrectedInputLeft =
        SendMessageW(controls_.checkboxShowCorrectedInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowCorrectedInputRight =
        SendMessageW(controls_.checkboxShowCorrectedInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowCorrectedLeft = SendMessageW(controls_.checkboxShowCorrectedLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowCorrectedRight = SendMessageW(controls_.checkboxShowCorrectedRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowExcessPhaseInputRight =
        SendMessageW(controls_.checkboxShowExcessPhaseInputRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowExcessPhaseInputLeft =
        SendMessageW(controls_.checkboxShowExcessPhaseInputLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowExcessPhasePredictedRight =
        SendMessageW(controls_.checkboxShowExcessPhasePredictedRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowExcessPhasePredictedLeft =
        SendMessageW(controls_.checkboxShowExcessPhasePredictedLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowRequestedMixedGroupDelayPreRight =
        SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayPreRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowRequestedMixedGroupDelayPreLeft =
        SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayPreLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowRequestedMixedGroupDelayRight =
        SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowRequestedMixedGroupDelayLeft =
        SendMessageW(controls_.checkboxShowRequestedMixedGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowInputGroupDelayLeft =
        SendMessageW(controls_.checkboxShowInputGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowInputGroupDelayRight =
        SendMessageW(controls_.checkboxShowInputGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowPredictedGroupDelayRight =
        SendMessageW(controls_.checkboxShowPredictedGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowPredictedGroupDelayLeft =
        SendMessageW(controls_.checkboxShowPredictedGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowFilterGroupDelayLeft =
        SendMessageW(controls_.checkboxShowFilterGroupDelayLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterShowFilterGroupDelayRight =
        SendMessageW(controls_.checkboxShowFilterGroupDelayRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterAlignGroupDelayLatency =
        SendMessageW(controls_.checkboxAlignGroupDelayLatency, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ui.filterGroupDelayZoomPreset = selectedGroupDelayZoomPreset();
}

FilterDesignSettings FiltersPage::currentSettings() const {
    FilterDesignSettings settings = appliedSettings_;
    settings.tapCount =
        tapCountFromComboIndex(static_cast<int>(SendMessageW(controls_.comboTapCount, CB_GETCURSEL, 0, 0)));
    const std::string filterViewMode =
        filterViewModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPhaseMode, CB_GETCURSEL, 0, 0)));
    if (filterViewMode != "difference") {
        settings.phaseMode = filterViewMode;
    }
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
    if (tryParseDouble(getWindowTextValue(controls_.editMixedPhaseMax), value)) {
        settings.mixedPhaseMaxFrequencyHz = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editExcessPhaseWindow), value)) {
        settings.excessPhaseWindowMs = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMixedPhaseStrength), value)) {
        settings.mixedPhaseStrength = value;
    }
    if (tryParseDouble(getWindowTextValue(controls_.editMixedPhaseCap), value)) {
        settings.mixedPhaseMaxCorrectionDegrees = value;
    }
    settings.preRingingCompensationFrequenciesHz =
        parseIntList(getWindowTextValue(controls_.editPreRingingCompensationFrequencies));
    settings.preRingingCompensationStrength = selectedPreRingingCompensationStrength();

    measurement::normalizeFilterDesignSettings(settings, sampleRate_);
    return settings;
}

const FilterDesignResult* FiltersPage::requestedMixedGroupDelaySourceResult(const WorkspaceState& workspace) const {
    if (workspace.ui.filterViewMode == "difference") {
        return workspace.mixedFilter.available() ? &workspace.mixedFilter.result : nullptr;
    }
    if (workspace.filterResult.valid) {
        return &workspace.filterResult;
    }
    return nullptr;
}

const FilterDesignSettings* FiltersPage::requestedMixedGroupDelaySourceSettings(const WorkspaceState& workspace) const {
    if (workspace.ui.filterViewMode == "difference") {
        return workspace.mixedFilter.available() ? &workspace.mixedFilter.settings : nullptr;
    }
    if (workspace.filterResult.valid && workspace.filterResult.phaseMode == "mixed") {
        return &appliedSettings_;
    }
    return nullptr;
}

std::vector<int> FiltersPage::requestedMixedGroupDelayCandidateFrequencies(const WorkspaceState& workspace) const {
    const FilterDesignResult* result = requestedMixedGroupDelaySourceResult(workspace);
    const FilterDesignSettings* settings = requestedMixedGroupDelaySourceSettings(workspace);
    if (result == nullptr || settings == nullptr || !result->valid || result->phaseMode != "mixed") {
        return {};
    }
    return measurement::suggestPreRingingCompensationFrequencies(*result, *settings);
}

std::vector<int> FiltersPage::requestedMixedGroupDelayDisplayedSpotFrequencies() const {
    return parseIntList(getWindowTextValue(controls_.editPreRingingCompensationFrequencies));
}

bool FiltersPage::areSettingsEqual(const FilterDesignSettings& left, const FilterDesignSettings& right) {
    return left.tapCount == right.tapCount &&
           left.phaseMode == right.phaseMode &&
           std::abs(left.smoothness - right.smoothness) < 0.001 &&
           std::abs(left.lowCorrectionHz - right.lowCorrectionHz) < 0.001 &&
           std::abs(left.highCorrectionHz - right.highCorrectionHz) < 0.001 &&
           std::abs(left.maxBoostDb - right.maxBoostDb) < 0.001 &&
           std::abs(left.maxCutDb - right.maxCutDb) < 0.001 &&
           std::abs(left.mixedPhaseMaxFrequencyHz - right.mixedPhaseMaxFrequencyHz) < 0.001 &&
           std::abs(left.excessPhaseWindowMs - right.excessPhaseWindowMs) < 0.001 &&
           std::abs(left.mixedPhaseStrength - right.mixedPhaseStrength) < 0.001 &&
           std::abs(left.mixedPhaseMaxCorrectionDegrees - right.mixedPhaseMaxCorrectionDegrees) < 0.001 &&
           left.preRingingCompensationFrequenciesHz == right.preRingingCompensationFrequenciesHz &&
           std::abs(left.preRingingCompensationStrength - right.preRingingCompensationStrength) < 0.001;
}

void FiltersPage::refreshPendingHighlightState() {
    const FilterDesignSettings settings = currentSettings();
    const auto hasPendingNumericEdit = [this](HWND control, double appliedValue, double currentValue) {
        double parsedValue = 0.0;
        if (!tryParseDouble(getWindowTextValue(control), parsedValue)) {
            return true;
        }
        return std::abs(currentValue - appliedValue) >= 0.001;
    };

    tapCountPending_ = settings.tapCount != appliedSettings_.tapCount;
    lowCorrectionPending_ =
        hasPendingNumericEdit(controls_.editLowCorrection, appliedSettings_.lowCorrectionHz, settings.lowCorrectionHz);
    highCorrectionPending_ =
        hasPendingNumericEdit(controls_.editHighCorrection, appliedSettings_.highCorrectionHz, settings.highCorrectionHz);
    maxBoostPending_ =
        hasPendingNumericEdit(controls_.editMaxBoost, appliedSettings_.maxBoostDb, settings.maxBoostDb);
    maxCutPending_ = hasPendingNumericEdit(controls_.editMaxCut, appliedSettings_.maxCutDb, settings.maxCutDb);
    smoothnessPending_ = std::abs(settings.smoothness - appliedSettings_.smoothness) >= 0.001;
    mixedPhaseMaxPending_ = hasPendingNumericEdit(controls_.editMixedPhaseMax,
                                                  appliedSettings_.mixedPhaseMaxFrequencyHz,
                                                  settings.mixedPhaseMaxFrequencyHz);
    excessPhaseWindowPending_ = hasPendingNumericEdit(controls_.editExcessPhaseWindow,
                                                      appliedSettings_.excessPhaseWindowMs,
                                                      settings.excessPhaseWindowMs);
    mixedPhaseStrengthPending_ = hasPendingNumericEdit(controls_.editMixedPhaseStrength,
                                                       appliedSettings_.mixedPhaseStrength,
                                                       settings.mixedPhaseStrength);
    mixedPhaseCapPending_ = hasPendingNumericEdit(controls_.editMixedPhaseCap,
                                                  appliedSettings_.mixedPhaseMaxCorrectionDegrees,
                                                  settings.mixedPhaseMaxCorrectionDegrees);
    const std::wstring preRingingFrequencyText =
        getWindowTextValue(controls_.editPreRingingCompensationFrequencies);
    const std::vector<int> preRingingFrequencies = parseIntList(preRingingFrequencyText);
    const bool hasNonWhitespacePreRingingText =
        preRingingFrequencyText.find_first_not_of(L" \t\r\n,;") != std::wstring::npos;
    preRingingCompensationFrequenciesPending_ =
        preRingingFrequencies != appliedSettings_.preRingingCompensationFrequenciesHz ||
        (hasNonWhitespacePreRingingText && preRingingFrequencies.empty());
    preRingingCompensationStrengthPending_ =
        std::abs(settings.preRingingCompensationStrength - appliedSettings_.preRingingCompensationStrength) >= 0.001;

    const HWND controlsToInvalidate[] = {
        controls_.labelTapCount,
        controls_.comboTapCount,
        controls_.labelLowCorrection,
        controls_.editLowCorrection,
        controls_.unitLowCorrection,
        controls_.labelHighCorrection,
        controls_.editHighCorrection,
        controls_.unitHighCorrection,
        controls_.labelMaxBoost,
        controls_.editMaxBoost,
        controls_.unitMaxBoost,
        controls_.labelMaxCut,
        controls_.editMaxCut,
        controls_.unitMaxCut,
        controls_.labelSmoothness,
        controls_.sliderSmoothness,
        controls_.valueSmoothness,
        controls_.labelMixedPhaseMax,
        controls_.editMixedPhaseMax,
        controls_.unitMixedPhaseMax,
        controls_.labelExcessPhaseWindow,
        controls_.editExcessPhaseWindow,
        controls_.unitExcessPhaseWindow,
        controls_.labelMixedPhaseStrength,
        controls_.editMixedPhaseStrength,
        controls_.unitMixedPhaseStrength,
        controls_.labelMixedPhaseCap,
        controls_.editMixedPhaseCap,
        controls_.unitMixedPhaseCap,
        controls_.labelPreRingingCompensationFrequencies,
        controls_.editPreRingingCompensationFrequencies,
        controls_.unitPreRingingCompensationFrequencies,
        controls_.labelPreRingingCompensationStrength,
        controls_.sliderPreRingingCompensationStrength,
        controls_.valuePreRingingCompensationStrength,
    };
    for (HWND control : controlsToInvalidate) {
        if (control != nullptr) {
            InvalidateRect(control, nullptr, TRUE);
        }
    }

    COMBOBOXINFO comboInfo{};
    if (getComboBoxInfoSafe(controls_.comboTapCount, comboInfo)) {
        if (comboInfo.hwndItem != nullptr) {
            InvalidateRect(comboInfo.hwndItem, nullptr, TRUE);
        }
        if (comboInfo.hwndList != nullptr) {
            InvalidateRect(comboInfo.hwndList, nullptr, TRUE);
        }
    }
}

void FiltersPage::refreshRecalculateButton() {
    refreshPendingHighlightState();
    if (controls_.buttonRecalculate != nullptr) {
        EnableWindow(controls_.buttonRecalculate, (!recalculateInProgress_ && !differenceViewSelected()) ? TRUE : FALSE);
        InvalidateRect(controls_.buttonRecalculate, nullptr, TRUE);
    }
    if (controls_.buttonRecalculateRequestedMixedGroupDelay != nullptr) {
        EnableWindow(controls_.buttonRecalculateRequestedMixedGroupDelay,
                     (!recalculateInProgress_ && !differenceViewSelected() && mixedModeSelected()) ? TRUE : FALSE);
        InvalidateRect(controls_.buttonRecalculateRequestedMixedGroupDelay, nullptr, TRUE);
    }
    if (controls_.buttonResetRequestedMixedGroupDelaySpot != nullptr) {
        EnableWindow(controls_.buttonResetRequestedMixedGroupDelaySpot,
                     (!recalculateInProgress_ && !differenceViewSelected() && mixedModeSelected()) ? TRUE : FALSE);
        InvalidateRect(controls_.buttonResetRequestedMixedGroupDelaySpot, nullptr, TRUE);
    }
}

bool FiltersPage::drawRecalculateButton(const DRAWITEMSTRUCT& draw) const {
    HDC hdc = draw.hDC;
    RECT rect = draw.rcItem;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool isMainRecalculateButton = draw.hwndItem == controls_.buttonRecalculate;

    const COLORREF baseFill = recalculateInProgress_ ? ui_theme::kAccent : ui_theme::kGreen;
    const COLORREF hoverFill = blendColor(baseFill, RGB(255, 255, 255), 0.12);
    const COLORREF pressedFill = blendColor(baseFill, RGB(0, 0, 0), 0.18);
    const COLORREF border = blendColor(baseFill, RGB(0, 0, 0), 0.28);
    const COLORREF disabledFill = blendColor(baseFill, ui_theme::backgroundColor(), 0.35);
    const COLORREF disabledBorder = blendColor(border, ui_theme::backgroundColor(), 0.3);

    const COLORREF fill = disabled ? disabledFill : (pressed ? pressedFill : (hot ? hoverFill : baseFill));
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, 1, disabled ? disabledBorder : border);
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
    SetTextColor(hdc,
                 disabled ? blendColor(RGB(255, 255, 255), ui_theme::backgroundColor(), 0.2)
                          : RGB(255, 255, 255));
    RECT textRect = rect;
    if (pressed) {
        OffsetRect(&textRect, 0, 1);
    }
    const std::wstring currentLabel = getWindowTextValue(draw.hwndItem);
    const wchar_t* label = currentLabel.c_str();
    if (isMainRecalculateButton) {
        label = differenceViewSelected()
                    ? L"Mode Delta View"
                    : (recalculateInProgress_
                           ? L"Calculating..."
                           : L"Recalculate");
    } else if (recalculateInProgress_) {
        label = L"Running";
    }
    DrawTextW(hdc, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (focused && !disabled) {
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
                                bool& recalculateRequested,
                                bool& viewSettingsChanged,
                                bool& selectionChanged,
                                std::vector<std::wstring>& logMessages) {
    PlotGraph* frequencyGraph = frequencyGraphForCommandId(commandId);
    if (frequencyGraph != nullptr && notificationCode == PlotGraph::kHoverChangedNotification) {
        sharedFrequencyHoverActive_ = frequencyGraph->hasHoveredXValue();
        if (sharedFrequencyHoverActive_) {
            sharedFrequencyHoverHz_ = frequencyGraph->hoveredXValue();
        }
        applySharedFrequencyHoverMarker();
        return true;
    }

    if (frequencyGraph != nullptr && notificationCode == PlotGraph::kXRangeChangedNotification) {
        applySharedFrequencyXRange(*frequencyGraph);
        return true;
    }

    if (commandId == kComboTapCount && notificationCode == CBN_SELCHANGE) {
        refreshRecalculateButton();
        return true;
    }

    if (commandId == kComboPhaseMode && notificationCode == CBN_SELCHANGE) {
        const std::string nextViewMode =
            filterViewModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPhaseMode, CB_GETCURSEL, 0, 0)));
        if (nextViewMode == "difference" &&
            (!workspace.minimumFilter.available() || !workspace.mixedFilter.available())) {
            std::wstring missingMessage = L"Mode Delta view requires both stored filters.\r\n\r\nMissing:";
            if (!workspace.minimumFilter.available()) {
                missingMessage += L"\r\n- Minimum";
            }
            if (!workspace.mixedFilter.available()) {
                missingMessage += L"\r\n- Mixed";
            }
            MessageBoxW(window_, missingMessage.c_str(), L"Filters", MB_OK | MB_ICONINFORMATION);
            SendMessageW(controls_.comboPhaseMode, CB_SETCURSEL, comboIndexFromFilterViewMode(filterViewMode_), 0);
            return true;
        }

        filterViewMode_ = nextViewMode;
        workspace.ui.filterViewMode = filterViewMode_;
        if (filterViewMode_ != "difference") {
            workspace.filters.phaseMode = filterViewMode_;
            measurement::normalizeFilterDesignSettings(workspace.filters, workspace.measurement.sampleRate);
        }
        refreshPhaseModeControls();
        refreshFilterViewPresentation();
        refreshRecalculateButton();
        selectionChanged = true;
        return true;
    }

    if ((commandId == kEditLowCorrection ||
         commandId == kEditHighCorrection ||
         commandId == kEditMaxBoost ||
         commandId == kEditMaxCut ||
         commandId == kEditMixedPhaseMax ||
         commandId == kEditExcessPhaseWindow ||
         commandId == kEditMixedPhaseStrength ||
         commandId == kEditMixedPhaseCap ||
         commandId == kEditPreRingingCompensationFrequencies) &&
        notificationCode == EN_CHANGE) {
        if (commandId == kEditExcessPhaseWindow) {
            refreshExcessPhaseWindowLabel();
        }
        refreshRecalculateButton();
        if (commandId == kEditPreRingingCompensationFrequencies) {
            requestedMixedGroupDelayGraph_.setData(buildRequestedMixedGroupDelayGraphData(workspace));
            applySharedFrequencyHoverMarker();
        }
        return true;
    }

    if ((commandId == kButtonAddRequestedMixedGroupDelaySpot ||
         commandId == kButtonRemoveRequestedMixedGroupDelaySpot) &&
        notificationCode == BN_CLICKED) {
        const std::vector<int> candidateFrequenciesHz = requestedMixedGroupDelayCandidateFrequencies(workspace);
        if (candidateFrequenciesHz.empty()) {
            logMessages.push_back(L"Requested mixed group delay has no ranked spot candidates. Recalculate in Mixed mode first.");
            return true;
        }

        const std::vector<int> currentFrequenciesHz = requestedMixedGroupDelayDisplayedSpotFrequencies();
        size_t nextCount = std::min(currentFrequenciesHz.size(), candidateFrequenciesHz.size());
        if (commandId == kButtonAddRequestedMixedGroupDelaySpot) {
            const size_t candidateCount = candidateFrequenciesHz.size();
            if (nextCount >= candidateCount) {
                logMessages.push_back(L"Peak +: all ranked peaks are already selected.");
                return true;
            }
            ++nextCount;
        } else {
            if (nextCount == 0) {
                logMessages.push_back(L"Peak -: no ranked peaks are currently selected.");
                return true;
            }
            --nextCount;
        }

        const std::vector<int> nextFrequenciesHz(candidateFrequenciesHz.begin(),
                                                 candidateFrequenciesHz.begin() +
                                                     static_cast<std::ptrdiff_t>(std::min(nextCount,
                                                                                          candidateFrequenciesHz.size())));
        setWindowTextValue(controls_.editPreRingingCompensationFrequencies, formatIntList(nextFrequenciesHz));
        if (nextFrequenciesHz.empty()) {
            logMessages.push_back(L"Peak -: cleared Ring Spots.");
        } else {
            logMessages.push_back((commandId == kButtonAddRequestedMixedGroupDelaySpot ? L"Peak +: using " : L"Peak -: using ") +
                                  formatIntList(nextFrequenciesHz) + L".");
        }
        refreshRecalculateButton();
        requestedMixedGroupDelayGraph_.setData(buildRequestedMixedGroupDelayGraphData(workspace));
        applySharedFrequencyHoverMarker();
        return true;
    }

    if (commandId == kButtonResetRequestedMixedGroupDelaySpot &&
        notificationCode == BN_CLICKED) {
        setWindowTextValue(controls_.editPreRingingCompensationFrequencies, L"");
        logMessages.push_back(L"Reset: cleared Ring Spots.");
        refreshRecalculateButton();
        requestedMixedGroupDelayGraph_.setData(buildRequestedMixedGroupDelayGraphData(workspace));
        applySharedFrequencyHoverMarker();
        return true;
    }

    if (commandId == kButtonRecalculateRequestedMixedGroupDelay &&
        notificationCode == BN_CLICKED) {
        if (differenceViewSelected()) {
            return true;
        }
        syncToWorkspace(workspace);
        recalculateRequested = true;
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
         commandId == kButtonCorrectedEffect ||
         commandId == kCheckboxShowExcessPhaseInputRight ||
         commandId == kCheckboxShowExcessPhaseInputLeft ||
         commandId == kCheckboxShowExcessPhasePredictedRight ||
         commandId == kCheckboxShowExcessPhasePredictedLeft ||
         commandId == kCheckboxShowRequestedMixedGroupDelayPreRight ||
         commandId == kCheckboxShowRequestedMixedGroupDelayPreLeft ||
         commandId == kCheckboxShowRequestedMixedGroupDelayRight ||
         commandId == kCheckboxShowRequestedMixedGroupDelayLeft ||
         commandId == kButtonExcessPhaseEffect ||
         commandId == kCheckboxShowInputGroupDelayLeft ||
         commandId == kCheckboxShowInputGroupDelayRight ||
         commandId == kCheckboxShowPredictedGroupDelayRight ||
         commandId == kCheckboxShowPredictedGroupDelayLeft ||
         commandId == kCheckboxShowFilterGroupDelayLeft ||
         commandId == kCheckboxShowFilterGroupDelayRight ||
         commandId == kButtonGroupDelayEffect ||
         commandId == kCheckboxAlignGroupDelayLatency) &&
        notificationCode == BN_CLICKED) {
        syncViewSettingsFromControls();
        saveViewSettings(workspace.ui);
        viewSettingsChanged = true;
        refreshFilterViewPresentation();
        correctionGraph_.setData(buildCorrectionGraphData(workspace));
        correctedGraph_.setData(buildCorrectedResponseGraphData(workspace));
        excessPhaseGraph_.setData(buildExcessPhaseGraphData(workspace));
        requestedMixedGroupDelayGraph_.setData(buildRequestedMixedGroupDelayGraphData(workspace));
        groupDelayGraph_.setData(buildGroupDelayGraphData(workspace));
        applySharedFrequencyHoverMarker();
        return true;
    }

    if (commandId == kButtonRecalculate && notificationCode == BN_CLICKED) {
        if (differenceViewSelected()) {
            return true;
        }
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
    switch (message) {
    case WM_SIZE:
        if (page != nullptr) {
            page->helpBubble_.hide();
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
        if (page != nullptr) {
            const HWND source = reinterpret_cast<HWND>(lParam);
            if (source == page->controls_.sliderSmoothness) {
                page->refreshSmoothnessValue();
                page->refreshRecalculateButton();
            } else if (source == page->controls_.sliderPreRingingCompensationStrength) {
                page->refreshPreRingingCompensationStrengthValue();
                page->refreshRecalculateButton();
            } else if (source == page->controls_.sliderGroupDelayZoom) {
                page->refreshGroupDelayZoomValue();
            }
        }
        {
            HWND root = GetAncestor(window, GA_ROOT);
            if (root != nullptr) {
                return SendMessageW(root, message, wParam, lParam);
            }
            return 0;
        }
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
                if (draw->hwndItem == page->controls_.buttonRecalculate ||
                    draw->hwndItem == page->controls_.buttonRecalculateRequestedMixedGroupDelay) {
                    return page->drawRecalculateButton(*draw) ? TRUE : FALSE;
                }
                if (draw->hwndItem == page->controls_.phaseCorrectionGroup) {
                    drawPhaseCorrectionGroup(*draw);
                    return TRUE;
                }
                if (draw->hwndItem == page->controls_.inversionLegendFrame ||
                    draw->hwndItem == page->controls_.correctedLegendFrame ||
                    draw->hwndItem == page->controls_.excessPhaseLegendFrame ||
                    draw->hwndItem == page->controls_.requestedMixedGroupDelayLegendFrame ||
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
        FillRect(hdc, &rect, ui_theme::backgroundBrush());
        return 1;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        static HBRUSH inputBrush = GetSysColorBrush(COLOR_WINDOW);
        static HBRUSH buttonBrush = GetSysColorBrush(COLOR_BTNFACE);
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
        static HBRUSH lineRequestedMixedGroupDelayPreRightBrush = CreateSolidBrush(ui_theme::kRed);
        static HBRUSH lineRequestedMixedGroupDelayPreLeftBrush = CreateSolidBrush(ui_theme::kGreen);
        static HBRUSH lineRequestedMixedGroupDelayRightBrush = CreateSolidBrush(ui_theme::kMagenta);
        static HBRUSH lineRequestedMixedGroupDelayLeftBrush = CreateSolidBrush(ui_theme::kGray);
        static HBRUSH lineInputGroupDelayLeftBrush = CreateSolidBrush(ui_theme::kGreen);
        static HBRUSH lineInputGroupDelayRightBrush = CreateSolidBrush(ui_theme::kRed);
        static HBRUSH linePredictedGroupDelayRightBrush = CreateSolidBrush(ui_theme::kMagenta);
        static HBRUSH linePredictedGroupDelayLeftBrush = CreateSolidBrush(ui_theme::kGray);
        static HBRUSH lineGroupDelayLeftBrush = CreateSolidBrush(ui_theme::kTeal);
        static HBRUSH lineGroupDelayRightBrush = CreateSolidBrush(ui_theme::kOrange);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        LRESULT helpColorResult = 0;
        if (page != nullptr && page->helpBubble_.handleCtlColorStatic(hdc, control, helpColorResult)) {
            return helpColorResult;
        }
        if (message == WM_CTLCOLORBTN &&
            page != nullptr &&
            (control == page->controls_.buttonAddRequestedMixedGroupDelaySpot ||
             control == page->controls_.buttonRemoveRequestedMixedGroupDelaySpot ||
             control == page->controls_.buttonResetRequestedMixedGroupDelaySpot)) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            SetTextColor(hdc, ui_theme::kText);
            return reinterpret_cast<INT_PTR>(buttonBrush);
        }
        SetTextColor(hdc, ui_theme::kText);
        if (page != nullptr) {
            COMBOBOXINFO tapCountInfo{};
            const bool hasTapCountInfo = getComboBoxInfoSafe(page->controls_.comboTapCount, tapCountInfo);
            if (control == page->controls_.phaseCorrectionGroup) {
                SetBkMode(hdc, OPAQUE);
                SetBkColor(hdc, ui_theme::backgroundColor());
                SetTextColor(hdc, page->mixedModeSelected() ? ui_theme::kText : ui_theme::kMuted);
                return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
            }
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
            if (control == page->controls_.lineRequestedMixedGroupDelayPreRight) {
                SetBkColor(hdc, ui_theme::kRed);
                return reinterpret_cast<INT_PTR>(lineRequestedMixedGroupDelayPreRightBrush);
            }
            if (control == page->controls_.lineRequestedMixedGroupDelayPreLeft) {
                SetBkColor(hdc, ui_theme::kGreen);
                return reinterpret_cast<INT_PTR>(lineRequestedMixedGroupDelayPreLeftBrush);
            }
            if (control == page->controls_.lineRequestedMixedGroupDelayRight) {
                SetBkColor(hdc, ui_theme::kMagenta);
                return reinterpret_cast<INT_PTR>(lineRequestedMixedGroupDelayRightBrush);
            }
            if (control == page->controls_.lineRequestedMixedGroupDelayLeft) {
                SetBkColor(hdc, ui_theme::kGray);
                return reinterpret_cast<INT_PTR>(lineRequestedMixedGroupDelayLeftBrush);
            }
            if (control == page->controls_.lineInputGroupDelayLeft) {
                SetBkColor(hdc, ui_theme::kGreen);
                return reinterpret_cast<INT_PTR>(lineInputGroupDelayLeftBrush);
            }
            if (control == page->controls_.lineInputGroupDelayRight) {
                SetBkColor(hdc, ui_theme::kRed);
                return reinterpret_cast<INT_PTR>(lineInputGroupDelayRightBrush);
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
                SetBkColor(hdc, ui_theme::kTeal);
                return reinterpret_cast<INT_PTR>(lineGroupDelayLeftBrush);
            }
            if (control == page->controls_.lineGroupDelayRight) {
                SetBkColor(hdc, ui_theme::kOrange);
                return reinterpret_cast<INT_PTR>(lineGroupDelayRightBrush);
            }
            if (hasTapCountInfo && control == tapCountInfo.hwndItem) {
                SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                SetTextColor(hdc, page->tapCountPending_ ? ui_theme::kMagenta : ui_theme::kText);
                return reinterpret_cast<INT_PTR>(inputBrush);
            }
            if (control == page->controls_.labelTapCount) {
                SetTextColor(hdc, page->tapCountPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelLowCorrection ||
                       control == page->controls_.unitLowCorrection) {
                SetTextColor(hdc, page->lowCorrectionPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelHighCorrection ||
                       control == page->controls_.unitHighCorrection) {
                SetTextColor(hdc, page->highCorrectionPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelMaxBoost ||
                       control == page->controls_.unitMaxBoost) {
                SetTextColor(hdc, page->maxBoostPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelMaxCut ||
                       control == page->controls_.unitMaxCut) {
                SetTextColor(hdc, page->maxCutPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelSmoothness ||
                       control == page->controls_.valueSmoothness) {
                SetTextColor(hdc, page->smoothnessPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelMixedPhaseMax ||
                       control == page->controls_.unitMixedPhaseMax) {
                SetTextColor(hdc, page->mixedPhaseMaxPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelExcessPhaseWindow ||
                       control == page->controls_.unitExcessPhaseWindow) {
                SetTextColor(hdc, page->excessPhaseWindowPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelMixedPhaseStrength ||
                       control == page->controls_.unitMixedPhaseStrength) {
                SetTextColor(hdc, page->mixedPhaseStrengthPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelMixedPhaseCap ||
                       control == page->controls_.unitMixedPhaseCap) {
                SetTextColor(hdc, page->mixedPhaseCapPending_ ? ui_theme::kMagenta : ui_theme::kText);
            } else if (control == page->controls_.labelPreRingingCompensationFrequencies ||
                       control == page->controls_.unitPreRingingCompensationFrequencies) {
                SetTextColor(hdc,
                             page->preRingingCompensationFrequenciesPending_ ? ui_theme::kMagenta
                                                                            : ui_theme::kText);
            } else if (control == page->controls_.labelPreRingingCompensationStrength ||
                       control == page->controls_.valuePreRingingCompensationStrength) {
                SetTextColor(hdc,
                             page->preRingingCompensationStrengthPending_ ? ui_theme::kMagenta
                                                                         : ui_theme::kText);
            }
        }
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    }
    case WM_NCDESTROY:
        if (page != nullptr) {
            page->helpBubble_.destroy();
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        break;
    case WM_CTLCOLOREDIT: {
        static HBRUSH inputBrush = GetSysColorBrush(COLOR_WINDOW);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        if (page != nullptr) {
            const bool pending = control == page->controls_.editLowCorrection      ? page->lowCorrectionPending_
                                 : control == page->controls_.editHighCorrection   ? page->highCorrectionPending_
                                 : control == page->controls_.editMaxBoost         ? page->maxBoostPending_
                                 : control == page->controls_.editMaxCut           ? page->maxCutPending_
                                 : control == page->controls_.editMixedPhaseMax    ? page->mixedPhaseMaxPending_
                                 : control == page->controls_.editExcessPhaseWindow ? page->excessPhaseWindowPending_
                                 : control == page->controls_.editMixedPhaseStrength ? page->mixedPhaseStrengthPending_
                                 : control == page->controls_.editMixedPhaseCap      ? page->mixedPhaseCapPending_
                                 : control == page->controls_.editPreRingingCompensationFrequencies
                                     ? page->preRingingCompensationFrequenciesPending_
                                                                                     : false;
            SetTextColor(hdc, pending ? ui_theme::kMagenta : ui_theme::kText);
        } else {
            SetTextColor(hdc, ui_theme::kText);
        }
        return reinterpret_cast<INT_PTR>(inputBrush);
    }
    case WM_CTLCOLORLISTBOX: {
        static HBRUSH inputBrush = GetSysColorBrush(COLOR_WINDOW);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        if (page != nullptr) {
            COMBOBOXINFO tapCountInfo{};
            if (getComboBoxInfoSafe(page->controls_.comboTapCount, tapCountInfo) && control == tapCountInfo.hwndList) {
                SetTextColor(hdc, page->tapCountPending_ ? ui_theme::kMagenta : ui_theme::kText);
                return reinterpret_cast<INT_PTR>(inputBrush);
            }
        }
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(inputBrush);
    }
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK FiltersPage::GroupDelayZoomSliderProc(HWND window,
                                                       UINT message,
                                                       WPARAM wParam,
                                                       LPARAM lParam,
                                                       UINT_PTR subclassId,
                                                       DWORD_PTR refData) {
    auto* page = reinterpret_cast<FiltersPage*>(refData);
    if (message == WM_MOUSEWHEEL) {
        if (page != nullptr && page->window_ != nullptr) {
            SendMessageW(page->window_, message, wParam, lParam);
        }
        return 0;
    }

    return DefSubclassProc(window, message, wParam, lParam);
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

void FiltersPage::populatePhaseModeCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Minimum"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Mixed"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Mode Delta"));
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

int FiltersPage::comboIndexFromFilterViewMode(const std::string& filterViewMode) {
    if (filterViewMode == "mixed") {
        return 1;
    }
    if (filterViewMode == "difference") {
        return 2;
    }
    return 0;
}

std::string FiltersPage::filterViewModeFromComboIndex(int index) {
    if (index == 1) {
        return "mixed";
    }
    if (index == 2) {
        return "difference";
    }
    return "minimum";
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
    RedrawWindow(controls_.phaseCorrectionGroup,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
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

bool FiltersPage::handleHScroll(HWND source, WorkspaceState& workspace) {
    if (source == controls_.sliderPreRingingCompensationStrength) {
        refreshPreRingingCompensationStrengthValue();
        refreshRecalculateButton();
        return false;
    }

    if (source == controls_.sliderSmoothness) {
        refreshSmoothnessValue();
        refreshRecalculateButton();
        return false;
    }

    if (source != controls_.sliderGroupDelayZoom) {
        return false;
    }

    groupDelayZoomPreset_ = selectedGroupDelayZoomPreset();
    refreshGroupDelayZoomValue();
    applyGroupDelayZoomRange();
    workspace.ui.filterGroupDelayZoomPreset = groupDelayZoomPreset_;
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
    correctionGraph_.setSharedHoverMarker(true, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
    correctedGraph_.setSharedHoverMarker(true, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
    excessPhaseGraph_.setSharedHoverMarker(true, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
    requestedMixedGroupDelayGraph_.setSharedHoverMarker(true, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
    groupDelayGraph_.setSharedHoverMarker(true, sharedFrequencyHoverActive_, sharedFrequencyHoverHz_);
}

PlotGraph* FiltersPage::frequencyGraphForCommandId(WORD commandId) {
    switch (commandId) {
    case kCorrectionGraph:
        return &correctionGraph_;
    case kCorrectedGraph:
        return &correctedGraph_;
    case kExcessPhaseGraph:
        return &excessPhaseGraph_;
    case kRequestedMixedGroupDelayGraph:
        return &requestedMixedGroupDelayGraph_;
    case kGroupDelayGraph:
        return &groupDelayGraph_;
    default:
        return nullptr;
    }
}

void FiltersPage::applySharedFrequencyXRange(const PlotGraph& sourceGraph) {
    if (!sourceGraph.hasCustomXRange()) {
        resetSharedFrequencyXRange();
        return;
    }

    const double minX = sourceGraph.visibleMinX();
    const double maxX = sourceGraph.visibleMaxX();
    correctionGraph_.setVisibleXRange(minX, maxX);
    correctedGraph_.setVisibleXRange(minX, maxX);
    excessPhaseGraph_.setVisibleXRange(minX, maxX);
    requestedMixedGroupDelayGraph_.setVisibleXRange(minX, maxX);
    groupDelayGraph_.setVisibleXRange(minX, maxX);
}

void FiltersPage::resetSharedFrequencyXRange() {
    correctionGraph_.resetXRange();
    correctedGraph_.resetXRange();
    excessPhaseGraph_.resetXRange();
    requestedMixedGroupDelayGraph_.resetXRange();
    groupDelayGraph_.resetXRange();
}

void FiltersPage::configureImpulseGraphViewport(const WorkspaceState& workspace) {
    std::vector<double> impulseTimeMs;
    const bool differenceView =
        workspace.ui.filterViewMode == "difference" &&
        workspace.minimumFilter.available() &&
        workspace.mixedFilter.available();
    if (differenceView) {
        impulseTimeMs =
            buildImpulseDifferenceTimeAxisMs(workspace.minimumFilter.result, workspace.mixedFilter.result);
    } else if (workspace.filterResult.valid &&
               !workspace.filterResult.left.filterTaps.empty() &&
               !workspace.filterResult.right.filterTaps.empty()) {
        impulseTimeMs = buildSharedImpulseTimeAxisMs(workspace.filterResult);
    }

    if (impulseTimeMs.empty()) {
        impulseGraph_.setDefaultXRange(false, 0.0, 1.0);
        impulseGraph_.setDefaultYRange(false, -kImpulseGraphDefaultYLimit, kImpulseGraphDefaultYLimit);
        return;
    }

    const double fullMinMs = impulseTimeMs.front();
    const double fullMaxMs = impulseTimeMs.back();

    const double negativeWindowMs = impulseGraphNegativeWindowMsForViewMode(workspace.ui.filterViewMode);
    impulseGraph_.setDefaultXRange(true,
                                   clampValue(-negativeWindowMs, fullMinMs, fullMaxMs),
                                   clampValue(kImpulseGraphPositiveWindowMs, fullMinMs, fullMaxMs));
    impulseGraph_.setDefaultYRange(!differenceView,
                                   -kImpulseGraphDefaultYLimit,
                                   kImpulseGraphDefaultYLimit);
}

PlotGraphData FiltersPage::buildCorrectionGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    configureFilterPlotAppearance(data);
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"dB";
    if (workspace.ui.filterViewMode == "difference" &&
        workspace.minimumFilter.available() &&
        workspace.mixedFilter.available()) {
        const FilterDesignResult& minimumResult = workspace.minimumFilter.result;
        const FilterDesignResult& mixedResult = workspace.mixedFilter.result;
        data.xValues = !mixedResult.frequencyAxisHz.empty() ? mixedResult.frequencyAxisHz : minimumResult.frequencyAxisHz;
        if (data.xValues.empty()) {
            return data;
        }

        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        if (showInversionRight_) {
            std::vector<double> delta = buildLogDeltaSeries(minimumResult,
                                                            minimumResult.right.correctionCurveDb,
                                                            mixedResult,
                                                            mixedResult.right.correctionCurveDb,
                                                            data.xValues);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Right delta", ui_theme::kMagenta, std::move(delta)});
        }
        if (showInversionLeft_) {
            std::vector<double> delta = buildLogDeltaSeries(minimumResult,
                                                            minimumResult.left.correctionCurveDb,
                                                            mixedResult,
                                                            mixedResult.left.correctionCurveDb,
                                                            data.xValues);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Left delta", ui_theme::kGray, std::move(delta)});
        }
        data.fixedYRange = true;
        if (std::isfinite(minY) && std::isfinite(maxY)) {
            const double halfSpan = std::max(std::max(std::abs(minY), std::abs(maxY)) + 0.5, 3.0);
            data.minY = -std::ceil(halfSpan / 1.0);
            data.maxY = std::ceil(halfSpan / 1.0);
        } else {
            data.minY = -3.0;
            data.maxY = 3.0;
        }
        return data;
    }

    data.xValues = workspace.filterResult.frequencyAxisHz;
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
    configureFilterPlotAppearance(data);
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"dB";
    data.fixedYRange = true;
    if (workspace.ui.filterViewMode == "difference" &&
        workspace.minimumFilter.available() &&
        workspace.mixedFilter.available()) {
        const FilterDesignResult& minimumResult = workspace.minimumFilter.result;
        const FilterDesignResult& mixedResult = workspace.mixedFilter.result;
        data.xValues = !mixedResult.frequencyAxisHz.empty() ? mixedResult.frequencyAxisHz : minimumResult.frequencyAxisHz;
        if (data.xValues.empty()) {
            data.minY = -3.0;
            data.maxY = 3.0;
            return data;
        }

        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        if (showCorrectedLeft_) {
            std::vector<double> delta = buildLogDeltaSeries(minimumResult,
                                                            minimumResult.left.correctedResponseDb,
                                                            mixedResult,
                                                            mixedResult.left.correctedResponseDb,
                                                            data.xValues);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Left delta", ui_theme::kGray, std::move(delta)});
        }
        if (showCorrectedRight_) {
            std::vector<double> delta = buildLogDeltaSeries(minimumResult,
                                                            minimumResult.right.correctedResponseDb,
                                                            mixedResult,
                                                            mixedResult.right.correctedResponseDb,
                                                            data.xValues);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Right delta", ui_theme::kMagenta, std::move(delta)});
        }
        if (std::isfinite(minY) && std::isfinite(maxY)) {
            const double halfSpan = std::max(std::max(std::abs(minY), std::abs(maxY)) + 0.5, 3.0);
            data.minY = -halfSpan;
            data.maxY = halfSpan;
        } else {
            data.minY = -3.0;
            data.maxY = 3.0;
        }
        return data;
    }

    data.xValues = workspace.filterResult.frequencyAxisHz;
    if (!workspace.filterResult.valid) {
        data.minY = -18.0;
        data.maxY = 12.0;
        return data;
    }

    const std::vector<double> leftInputResponseDb =
        resampleLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                             workspace.smoothedResponse.leftChannelDb,
                             workspace.filterResult.frequencyAxisHz);
    const std::vector<double> rightInputResponseDb =
        resampleLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                             workspace.smoothedResponse.rightChannelDb,
                             workspace.filterResult.frequencyAxisHz);
    if (showCorrectedEffect_) {
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        if (showCorrectedLeft_) {
            std::vector<double> delta =
                subtractSeries(workspace.filterResult.left.correctedResponseDb, leftInputResponseDb);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Left effect", ui_theme::kGray, std::move(delta)});
        }
        if (showCorrectedRight_) {
            std::vector<double> delta =
                subtractSeries(workspace.filterResult.right.correctedResponseDb, rightInputResponseDb);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Right effect", ui_theme::kMagenta, std::move(delta)});
        }
        if (std::isfinite(minY) && std::isfinite(maxY)) {
            const double halfSpan = std::max(std::max(std::abs(minY), std::abs(maxY)) + 0.5, 3.0);
            data.minY = -halfSpan;
            data.maxY = halfSpan;
        } else {
            data.minY = -3.0;
            data.maxY = 3.0;
        }
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
    configureFilterPlotAppearance(data);
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"deg";
    if (workspace.ui.filterViewMode == "difference" &&
        workspace.minimumFilter.available() &&
        workspace.mixedFilter.available()) {
        const FilterDesignResult& minimumResult = workspace.minimumFilter.result;
        const FilterDesignResult& mixedResult = workspace.mixedFilter.result;
        data.xValues = !mixedResult.frequencyAxisHz.empty() ? mixedResult.frequencyAxisHz : minimumResult.frequencyAxisHz;
        if (data.xValues.empty()) {
            return data;
        }

        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        if (showExcessPhasePredictedRight_) {
            std::vector<double> delta = buildLogDeltaSeries(minimumResult,
                                                            minimumResult.right.predictedExcessPhaseContinuousDegrees,
                                                            mixedResult,
                                                            mixedResult.right.predictedExcessPhaseContinuousDegrees,
                                                            data.xValues);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Right delta", ui_theme::kMagenta, std::move(delta)});
        }
        if (showExcessPhasePredictedLeft_) {
            std::vector<double> delta = buildLogDeltaSeries(minimumResult,
                                                            minimumResult.left.predictedExcessPhaseContinuousDegrees,
                                                            mixedResult,
                                                            mixedResult.left.predictedExcessPhaseContinuousDegrees,
                                                            data.xValues);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Left delta", ui_theme::kGray, std::move(delta)});
        }

        if (std::isfinite(minY) && std::isfinite(maxY)) {
            const double padded = std::max(std::max(std::abs(minY), std::abs(maxY)) + 15.0, 180.0);
            data.minY = -padded;
            data.maxY = padded;
        }
        return data;
    }

    data.xValues = workspace.filterResult.frequencyAxisHz;
    if (!workspace.filterResult.valid) {
        return data;
    }

    if (showExcessPhaseEffect_) {
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        if (showExcessPhasePredictedRight_) {
            std::vector<double> delta =
                subtractSeries(workspace.filterResult.right.predictedExcessPhaseContinuousDegrees,
                               workspace.filterResult.right.inputExcessPhaseContinuousDegrees);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Right effect", ui_theme::kMagenta, std::move(delta)});
        }
        if (showExcessPhasePredictedLeft_) {
            std::vector<double> delta =
                subtractSeries(workspace.filterResult.left.predictedExcessPhaseContinuousDegrees,
                               workspace.filterResult.left.inputExcessPhaseContinuousDegrees);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Left effect", ui_theme::kGray, std::move(delta)});
        }

        if (std::isfinite(minY) && std::isfinite(maxY)) {
            const double padded = std::max(std::max(std::abs(minY), std::abs(maxY)) + 10.0, 60.0);
            data.minY = -padded;
            data.maxY = padded;
        }
        return data;
    }

    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    const auto accumulateRange = [&minY, &maxY](const std::vector<double>& values) {
        for (const double value : values) {
            if (!std::isfinite(value)) {
                continue;
            }
            minY = std::min(minY, value);
            maxY = std::max(maxY, value);
        }
    };

    if (showExcessPhaseInputRight_) {
        std::vector<double> values = workspace.filterResult.right.inputExcessPhaseContinuousDegrees;
        accumulateRange(values);
        data.series.push_back({L"Right before", ui_theme::kRed, std::move(values)});
    }
    if (showExcessPhaseInputLeft_) {
        std::vector<double> values = workspace.filterResult.left.inputExcessPhaseContinuousDegrees;
        accumulateRange(values);
        data.series.push_back({L"Left before", ui_theme::kGreen, std::move(values)});
    }
    if (showExcessPhasePredictedRight_) {
        std::vector<double> values = workspace.filterResult.right.predictedExcessPhaseContinuousDegrees;
        accumulateRange(values);
        data.series.push_back({L"Right after", ui_theme::kMagenta, std::move(values)});
    }
    if (showExcessPhasePredictedLeft_) {
        std::vector<double> values = workspace.filterResult.left.predictedExcessPhaseContinuousDegrees;
        accumulateRange(values);
        data.series.push_back({L"Left after", ui_theme::kGray, std::move(values)});
    }

    if (std::isfinite(minY) && std::isfinite(maxY)) {
        const double paddedMin = std::floor((minY - 30.0) / 60.0) * 60.0;
        const double paddedMax = std::ceil((maxY + 30.0) / 60.0) * 60.0;
        const double minimumSpan = 360.0;
        const double center = (paddedMin + paddedMax) * 0.5;
        const double halfSpan = std::max((paddedMax - paddedMin) * 0.5, minimumSpan * 0.5);
        data.minY = center - halfSpan;
        data.maxY = center + halfSpan;
    }
    return data;
}

PlotGraphData FiltersPage::buildRequestedMixedGroupDelayGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    configureFilterPlotAppearance(data);
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"ms";

    const FilterDesignResult* sourceResult = requestedMixedGroupDelaySourceResult(workspace);

    if (sourceResult == nullptr || !sourceResult->valid) {
        return data;
    }

    data.xValues = sourceResult->frequencyAxisHz;
    if (data.xValues.empty()) {
        return data;
    }

    if (sourceResult->requestedMixedTransitionEndHz > sourceResult->requestedMixedTransitionStartHz &&
        sourceResult->requestedMixedTransitionStartHz > 0.0) {
        data.xSpans.push_back({sourceResult->requestedMixedTransitionStartHz,
                               sourceResult->requestedMixedTransitionEndHz,
                               blendColor(ui_theme::kAccent, ui_theme::graphBackgroundColor(), 0.88),
                               34});
        data.xMarkers.push_back({sourceResult->requestedMixedTransitionStartHz,
                                 ui_theme::kAccent,
                                 PlotGraphLineStyle::Dash});
        data.xMarkers.push_back({sourceResult->requestedMixedTransitionEndHz,
                                 blendColor(ui_theme::kMuted, ui_theme::kAccent, 0.35),
                                 PlotGraphLineStyle::Dash});
    }

    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    if (showRequestedMixedGroupDelayPreRight_ &&
        !sourceResult->right.requestedMixedGroupDelayPreSolveMs.empty()) {
        accumulateFiniteRange(sourceResult->right.requestedMixedGroupDelayPreSolveMs, minY, maxY);
        data.series.push_back({L"Right pre",
                               ui_theme::kRed,
                               sourceResult->right.requestedMixedGroupDelayPreSolveMs});
    }
    if (showRequestedMixedGroupDelayPreLeft_ &&
        !sourceResult->left.requestedMixedGroupDelayPreSolveMs.empty()) {
        accumulateFiniteRange(sourceResult->left.requestedMixedGroupDelayPreSolveMs, minY, maxY);
        data.series.push_back({L"Left pre",
                               ui_theme::kGreen,
                               sourceResult->left.requestedMixedGroupDelayPreSolveMs});
    }
    if (showRequestedMixedGroupDelayRight_ &&
        !sourceResult->right.requestedMixedGroupDelayMs.empty()) {
        accumulateFiniteRange(sourceResult->right.requestedMixedGroupDelayMs, minY, maxY);
        data.series.push_back({L"Right post",
                               ui_theme::kMagenta,
                               sourceResult->right.requestedMixedGroupDelayMs});
    }
    if (showRequestedMixedGroupDelayLeft_ &&
        !sourceResult->left.requestedMixedGroupDelayMs.empty()) {
        accumulateFiniteRange(sourceResult->left.requestedMixedGroupDelayMs, minY, maxY);
        data.series.push_back({L"Left post",
                               ui_theme::kGray,
                               sourceResult->left.requestedMixedGroupDelayMs});
    }

    if (sourceResult->phaseMode == "mixed") {
        const std::vector<int> displayedFrequenciesHz = requestedMixedGroupDelayDisplayedSpotFrequencies();
        for (const int frequencyHz : displayedFrequenciesHz) {
            const double leftValue =
                interpolateSeriesAtFrequency(sourceResult->frequencyAxisHz,
                                             sourceResult->left.requestedMixedGroupDelayPreSolveMs,
                                             static_cast<double>(frequencyHz));
            const double rightValue =
                interpolateSeriesAtFrequency(sourceResult->frequencyAxisHz,
                                             sourceResult->right.requestedMixedGroupDelayPreSolveMs,
                                             static_cast<double>(frequencyHz));
            const double yValue = std::abs(leftValue) >= std::abs(rightValue) ? leftValue : rightValue;
            if (!std::isfinite(yValue)) {
                continue;
            }
            data.pointMarkers.push_back({static_cast<double>(frequencyHz), yValue, ui_theme::kAccent, 7});
            minY = std::min(minY, yValue);
            maxY = std::max(maxY, yValue);
        }
    }

    if (std::isfinite(minY) && std::isfinite(maxY)) {
        data.fixedYRange = true;
        const double padded = std::max(std::max(std::abs(minY), std::abs(maxY)) + 0.5, 5.0);
        data.minY = -padded;
        data.maxY = padded;
    }
    return data;
}

PlotGraphData FiltersPage::buildGroupDelayGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    configureFilterPlotAppearance(data);
    data.xAxisMode = PlotGraphXAxisMode::LogFrequency;
    data.xUnit = L"Hz";
    data.yUnit = L"ms";
    if (workspace.ui.filterViewMode == "difference" &&
        workspace.minimumFilter.available() &&
        workspace.mixedFilter.available()) {
        const FilterDesignResult& minimumResult = workspace.minimumFilter.result;
        const FilterDesignResult& mixedResult = workspace.mixedFilter.result;
        data.xValues = !mixedResult.frequencyAxisHz.empty() ? mixedResult.frequencyAxisHz : minimumResult.frequencyAxisHz;
        if (data.xValues.empty()) {
            return data;
        }

        if (showFilterGroupDelayLeft_) {
            data.series.push_back({L"Left filter delta",
                                   ui_theme::kTeal,
                                   buildLogDeltaSeries(minimumResult,
                                                       minimumResult.left.groupDelayMs,
                                                       mixedResult,
                                                       mixedResult.left.groupDelayMs,
                                                       data.xValues)});
        }
        if (showFilterGroupDelayRight_) {
            data.series.push_back({L"Right filter delta",
                                   ui_theme::kOrange,
                                   buildLogDeltaSeries(minimumResult,
                                                       minimumResult.right.groupDelayMs,
                                                       mixedResult,
                                                       mixedResult.right.groupDelayMs,
                                                       data.xValues)});
        }
        if (showPredictedGroupDelayRight_) {
            data.series.push_back({L"Right predicted delta",
                                   ui_theme::kMagenta,
                                   buildLogDeltaSeries(minimumResult,
                                                       minimumResult.right.predictedGroupDelayMs,
                                                       mixedResult,
                                                       mixedResult.right.predictedGroupDelayMs,
                                                       data.xValues)});
        }
        if (showPredictedGroupDelayLeft_) {
            data.series.push_back({L"Left predicted delta",
                                   ui_theme::kGray,
                                   buildLogDeltaSeries(minimumResult,
                                                       minimumResult.left.predictedGroupDelayMs,
                                                       mixedResult,
                                                       mixedResult.left.predictedGroupDelayMs,
                                                       data.xValues)});
        }
        return data;
    }

    data.xValues = workspace.filterResult.frequencyAxisHz;
    if (!workspace.filterResult.valid) {
        return data;
    }

    const double safeSampleRate = static_cast<double>(std::max(workspace.filterResult.sampleRate, 1));
    const double leftLatencyMs =
        std::max(workspace.filterResult.left.impulsePeakIndex, 0) * 1000.0 / safeSampleRate;
    const double rightLatencyMs =
        std::max(workspace.filterResult.right.impulsePeakIndex, 0) * 1000.0 / safeSampleRate;
    if (showGroupDelayEffect_) {
        std::vector<double> leftPredictedGroupDelay = workspace.filterResult.left.predictedGroupDelayMs;
        std::vector<double> rightPredictedGroupDelay = workspace.filterResult.right.predictedGroupDelayMs;
        data.fixedYRange = true;
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        if (showPredictedGroupDelayRight_) {
            std::vector<double> delta =
                subtractSeries(rightPredictedGroupDelay, workspace.filterResult.right.inputGroupDelayMs);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Right effect", ui_theme::kMagenta, std::move(delta)});
        }
        if (showPredictedGroupDelayLeft_) {
            std::vector<double> delta =
                subtractSeries(leftPredictedGroupDelay, workspace.filterResult.left.inputGroupDelayMs);
            accumulateFiniteRange(delta, minY, maxY);
            data.series.push_back({L"Left effect", ui_theme::kGray, std::move(delta)});
        }
        if (std::isfinite(minY) && std::isfinite(maxY)) {
            const double padded = std::max(std::max(std::abs(minY), std::abs(maxY)) + 0.5, 5.0);
            data.minY = -padded;
            data.maxY = padded;
        } else {
            data.minY = -5.0;
            data.maxY = 5.0;
        }
        return data;
    }

    if (showInputGroupDelayLeft_) {
        data.series.push_back({L"Left input", ui_theme::kGreen, workspace.filterResult.left.inputGroupDelayMs});
    }
    if (showInputGroupDelayRight_) {
        data.series.push_back({L"Right input", ui_theme::kRed, workspace.filterResult.right.inputGroupDelayMs});
    }
    const std::vector<double> leftFilterGroupDelay =
        alignGroupDelayLatency_
            ? subtractConstant(workspace.filterResult.left.groupDelayMs, leftLatencyMs)
            : workspace.filterResult.left.groupDelayMs;
    const std::vector<double> rightFilterGroupDelay =
        alignGroupDelayLatency_
            ? subtractConstant(workspace.filterResult.right.groupDelayMs, rightLatencyMs)
            : workspace.filterResult.right.groupDelayMs;
    if (showFilterGroupDelayLeft_) {
        data.series.push_back({L"Left filter", ui_theme::kTeal, std::move(leftFilterGroupDelay)});
    }
    if (showFilterGroupDelayRight_) {
        data.series.push_back({L"Right filter", ui_theme::kOrange, std::move(rightFilterGroupDelay)});
    }
    if (showPredictedGroupDelayRight_) {
        data.series.push_back({L"Right predicted",
                               ui_theme::kMagenta,
                               workspace.filterResult.right.predictedGroupDelayMs});
    }
    if (showPredictedGroupDelayLeft_) {
        data.series.push_back({L"Left predicted",
                               ui_theme::kGray,
                               workspace.filterResult.left.predictedGroupDelayMs});
    }
    return data;
}

PlotGraphData FiltersPage::buildImpulseGraphData(const WorkspaceState& workspace) const {
    PlotGraphData data;
    configureFilterPlotAppearance(data);
    data.xAxisMode = PlotGraphXAxisMode::Linear;
    data.yAxisMode = PlotGraphYAxisMode::SymmetricAroundZero;
    data.xUnit = L"ms";
    data.yUnit = L"linear";
    data.measurementDerivedValueMode =
        PlotGraphMeasurementDerivedValueMode::QuarterCycleFrequencyFromDeltaX;
    if (workspace.ui.filterViewMode == "difference" &&
        workspace.minimumFilter.available() &&
        workspace.mixedFilter.available()) {
        const std::vector<double> minimumAxis = buildSharedImpulseTimeAxisMs(workspace.minimumFilter.result);
        const std::vector<double> mixedAxis = buildSharedImpulseTimeAxisMs(workspace.mixedFilter.result);
        data.xValues =
            buildImpulseDifferenceTimeAxisMs(workspace.minimumFilter.result, workspace.mixedFilter.result);
        if (data.xValues.empty()) {
            return data;
        }

        data.series.push_back({L"Left delta",
                               ui_theme::kGray,
                               buildLinearDeltaSeries(minimumAxis,
                                                      workspace.minimumFilter.result.left.filterTaps,
                                                      mixedAxis,
                                                      workspace.mixedFilter.result.left.filterTaps,
                                                      data.xValues)});
        data.series.push_back({L"Right delta",
                               ui_theme::kMagenta,
                               buildLinearDeltaSeries(minimumAxis,
                                                      workspace.minimumFilter.result.right.filterTaps,
                                                      mixedAxis,
                                                      workspace.mixedFilter.result.right.filterTaps,
                                                      data.xValues)});
        return data;
    }

    if (!workspace.filterResult.valid ||
        workspace.filterResult.left.filterTaps.empty() ||
        workspace.filterResult.right.filterTaps.empty()) {
        return data;
    }

    const size_t tapCount = std::min(workspace.filterResult.left.filterTaps.size(),
                                     workspace.filterResult.right.filterTaps.size());
    const std::vector<double> impulseTimeMs = buildSharedImpulseTimeAxisMs(workspace.filterResult);
    if (impulseTimeMs.size() < tapCount) {
        return data;
    }

    std::vector<double> xValues;
    std::vector<double> leftValues;
    std::vector<double> rightValues;
    xValues.reserve(tapCount);
    leftValues.reserve(tapCount);
    rightValues.reserve(tapCount);
    for (size_t index = 0; index < tapCount; ++index) {
        xValues.push_back(impulseTimeMs[index]);
        leftValues.push_back(workspace.filterResult.left.filterTaps[index]);
        rightValues.push_back(workspace.filterResult.right.filterTaps[index]);
    }

    data.xValues = std::move(xValues);
    data.series.push_back({L"Left", ui_theme::kGreen, std::move(leftValues)});
    data.series.push_back({L"Right", ui_theme::kRed, std::move(rightValues)});
    return data;
}

}  // namespace wolfie::ui

