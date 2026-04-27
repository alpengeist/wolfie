#include "ui/plot_graph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <windowsx.h>

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr int kMinBrushPixels = 6;
constexpr int kResetButtonHeight = 20;
constexpr int kResetButtonWidth = 54;
constexpr double kMinimumAutoYSpan = 1.0;
constexpr double kMinimumVisibleXSpan = 1.0e-6;

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

struct GraphLayout {
    RECT graph{};
    RECT resetButton{};
    int textHeight = 12;
    double minY = 0.0;
    double maxY = 1.0;
    double fullMinX = 0.0;
    double fullMaxX = 1.0;
    double visibleMinX = 0.0;
    double visibleMaxX = 1.0;
    std::vector<double> yTicks;
};

struct AxisLabel {
    double value = 0.0;
    int pixel = 0;
    int left = 0;
    int right = 0;
    std::wstring text;
};

int measureTextWidth(HDC hdc, const std::wstring& text) {
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

double nextNiceStep(double rawStep) {
    if (rawStep <= 0.0) {
        return 1.0;
    }

    const double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    const double normalized = rawStep / magnitude;
    if (normalized <= 1.0) {
        return magnitude;
    }
    if (normalized <= 2.0) {
        return 2.0 * magnitude;
    }
    if (normalized <= 2.5) {
        return 2.5 * magnitude;
    }
    if (normalized <= 5.0) {
        return 5.0 * magnitude;
    }
    return 10.0 * magnitude;
}

std::wstring formatAxisValue(double value) {
    const double rounded0 = std::round(value);
    if (std::abs(value - rounded0) < 0.01) {
        return formatWideDouble(value, 0);
    }
    const double rounded1 = std::round(value * 10.0) / 10.0;
    if (std::abs(value - rounded1) < 0.001) {
        return formatWideDouble(value, 1);
    }
    return formatWideDouble(value, 2);
}

double rawXTFromValue(PlotGraphXAxisMode mode, double minValue, double maxValue, double value) {
    if (mode == PlotGraphXAxisMode::LogFrequency) {
        const double minLog = std::log10(std::max(minValue, 1.0e-6));
        const double maxLog = std::log10(std::max(maxValue, minValue + 1.0e-6));
        return (std::log10(std::max(value, 1.0e-6)) - minLog) / std::max(maxLog - minLog, 1.0e-9);
    }

    return (value - minValue) / std::max(maxValue - minValue, 1.0e-9);
}

double xTFromValue(PlotGraphXAxisMode mode, double minValue, double maxValue, double value) {
    return clampValue(rawXTFromValue(mode, minValue, maxValue, value), 0.0, 1.0);
}

double valueFromXT(PlotGraphXAxisMode mode, double minValue, double maxValue, double t) {
    const double clampedT = clampValue(t, 0.0, 1.0);
    if (mode == PlotGraphXAxisMode::LogFrequency) {
        const double minLog = std::log10(std::max(minValue, 1.0e-6));
        const double maxLog = std::log10(std::max(maxValue, minValue + 1.0e-6));
        return std::pow(10.0, minLog + ((maxLog - minLog) * clampedT));
    }

    return minValue + ((maxValue - minValue) * clampedT);
}

int graphXFromValue(const RECT& graph,
                    PlotGraphXAxisMode mode,
                    double minValue,
                    double maxValue,
                    double value,
                    bool clampToGraph = true) {
    const double xT = clampToGraph ? xTFromValue(mode, minValue, maxValue, value)
                                   : rawXTFromValue(mode, minValue, maxValue, value);
    return graph.left + static_cast<int>(std::lround(xT * (graph.right - graph.left)));
}

double valueFromGraphX(const RECT& graph, PlotGraphXAxisMode mode, double minValue, double maxValue, int x) {
    const int width = std::max(static_cast<int>(graph.right - graph.left), 1);
    const double xT =
        static_cast<double>(clampValue(static_cast<int>(x - graph.left), 0, width)) / static_cast<double>(width);
    return valueFromXT(mode, minValue, maxValue, xT);
}

int graphYFromValue(const RECT& graph, double value, double minY, double maxY) {
    const double range = std::max(maxY - minY, 1.0e-9);
    const double yT = clampValue((value - minY) / range, 0.0, 1.0);
    return graph.bottom - static_cast<int>(std::lround(yT * (graph.bottom - graph.top)));
}

int unclampedGraphYFromValue(const RECT& graph, double value, double minY, double maxY) {
    const double range = std::max(maxY - minY, 1.0e-9);
    const double yT = (value - minY) / range;
    return graph.bottom - static_cast<int>(std::lround(yT * (graph.bottom - graph.top)));
}

std::vector<double> buildYTicks(double minY, double maxY) {
    const double range = std::max(maxY - minY, 1.0);
    const double step = nextNiceStep(range / 6.0);
    const double first = std::floor(minY / step) * step;
    const double last = std::ceil(maxY / step) * step;
    std::vector<double> ticks;
    for (double value = first; value <= last + (step * 0.25); value += step) {
        ticks.push_back(value);
    }
    return ticks;
}

void expandAutoYRange(double& minY, double& maxY) {
    if (!std::isfinite(minY) || !std::isfinite(maxY)) {
        minY = 0.0;
        maxY = 1.0;
        return;
    }

    if (std::abs(maxY - minY) < 1.0e-9) {
        const double halfSpan = kMinimumAutoYSpan * 0.5;
        minY -= halfSpan;
        maxY += halfSpan;
        return;
    }

    const double range = maxY - minY;
    const double pad = std::max(range * 0.08, kMinimumAutoYSpan * 0.05);
    minY -= pad;
    maxY += pad;
}

void expandSymmetricYRange(double& minY, double& maxY) {
    const double absMax = std::max(std::abs(minY), std::abs(maxY));
    const double span = std::max(absMax * 1.08, kMinimumAutoYSpan * 0.5);
    minY = -span;
    maxY = span;
}

bool scanVisibleYRange(const PlotGraphData& data, double minVisibleX, double maxVisibleX, double& minY, double& maxY) {
    bool found = false;
    for (const PlotGraphSeries& series : data.series) {
        const size_t count = std::min(data.xValues.size(), series.values.size());
        for (size_t index = 0; index < count; ++index) {
            const double x = data.xValues[index];
            if (x < minVisibleX || x > maxVisibleX) {
                continue;
            }

            const double value = series.values[index];
            if (!std::isfinite(value)) {
                continue;
            }

            minY = found ? std::min(minY, value) : value;
            maxY = found ? std::max(maxY, value) : value;
            found = true;
        }
    }
    return found;
}

GraphLayout buildLayout(HDC hdc,
                        const RECT& rect,
                        const PlotGraphData& data,
                        bool hasCustomXRange,
                        double customMinX,
                        double customMaxX,
                        bool hasDefaultXRange,
                        double defaultMinX,
                        double defaultMaxX,
                        bool hasCustomYRange,
                        double customMinY,
                        double customMaxY,
                        bool hasDefaultYRange,
                        double defaultMinY,
                        double defaultMaxY) {
    GraphLayout layout;
    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    layout.textHeight = std::max(static_cast<int>(metrics.tmHeight), 12);

    if (!data.xValues.empty()) {
        layout.fullMinX = data.xValues.front();
        layout.fullMaxX = data.xValues.back();
        if (std::abs(layout.fullMaxX - layout.fullMinX) < 1.0e-9) {
            layout.fullMaxX = layout.fullMinX + 1.0;
        }
    }

    double baseMinX = hasDefaultXRange ? defaultMinX : layout.fullMinX;
    double baseMaxX = hasDefaultXRange ? defaultMaxX : layout.fullMaxX;
    layout.visibleMinX = clampValue(std::min(baseMinX, baseMaxX), layout.fullMinX, layout.fullMaxX);
    layout.visibleMaxX = clampValue(std::max(baseMinX, baseMaxX), layout.fullMinX, layout.fullMaxX);
    if (hasCustomXRange && !data.xValues.empty()) {
        layout.visibleMinX = clampValue(std::min(customMinX, customMaxX), layout.fullMinX, layout.fullMaxX);
        layout.visibleMaxX = clampValue(std::max(customMinX, customMaxX), layout.fullMinX, layout.fullMaxX);
        if (std::abs(layout.visibleMaxX - layout.visibleMinX) < 1.0e-9) {
            layout.visibleMinX = hasDefaultXRange ? defaultMinX : layout.fullMinX;
            layout.visibleMaxX = hasDefaultXRange ? defaultMaxX : layout.fullMaxX;
        }
    }

    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    if (hasCustomYRange) {
        minY = std::min(customMinY, customMaxY);
        maxY = std::max(customMinY, customMaxY);
    } else if (hasDefaultYRange) {
        minY = std::min(defaultMinY, defaultMaxY);
        maxY = std::max(defaultMinY, defaultMaxY);
    } else if (data.fixedYRange && !hasCustomXRange) {
        minY = data.minY;
        maxY = data.maxY;
    } else if (scanVisibleYRange(data, layout.visibleMinX, layout.visibleMaxX, minY, maxY)) {
        if (data.yAxisMode == PlotGraphYAxisMode::SymmetricAroundZero) {
            expandSymmetricYRange(minY, maxY);
        } else {
            expandAutoYRange(minY, maxY);
        }
    } else if (data.fixedYRange) {
        minY = data.minY;
        maxY = data.maxY;
    } else {
        minY = data.yAxisMode == PlotGraphYAxisMode::SymmetricAroundZero ? -1.0 : 0.0;
        maxY = 1.0;
    }

    if (std::abs(maxY - minY) < 1.0e-9) {
        if (data.yAxisMode == PlotGraphYAxisMode::SymmetricAroundZero) {
            expandSymmetricYRange(minY, maxY);
        } else {
            expandAutoYRange(minY, maxY);
        }
    }
    layout.minY = minY;
    layout.maxY = maxY;
    layout.yTicks = buildYTicks(minY, maxY);

    int widestYLabel = 0;
    for (const double tick : layout.yTicks) {
        widestYLabel = std::max(widestYLabel, measureTextWidth(hdc, formatAxisValue(tick)));
    }

    layout.resetButton = RECT{
        rect.right - kResetButtonWidth - 8,
        rect.top + 8,
        rect.right - 8,
        rect.top + 8 + kResetButtonHeight,
    };
    layout.graph = RECT{
        rect.left + std::max(52, widestYLabel + 14),
        rect.top + kResetButtonHeight + 16,
        rect.right - 8,
        rect.bottom - (layout.textHeight + 18),
    };
    return layout;
}

bool isMajorLogFrequencyTick(double value, double maxValue) {
    if (std::abs(value - maxValue) < 0.5) {
        return true;
    }

    const double magnitude = std::pow(10.0, std::floor(std::log10(std::max(value, 1.0))));
    return std::abs(value - magnitude) < 0.001;
}

std::vector<double> buildXTicks(const RECT& graph,
                                const PlotGraphData& data,
                                double minVisibleX,
                                double maxVisibleX) {
    if (data.xValues.empty()) {
        return {};
    }

    if (data.xAxisMode == PlotGraphXAxisMode::LogFrequency) {
        std::vector<double> candidates;
        const double minFrequencyHz = std::max(minVisibleX, 1.0);
        const double maxFrequencyHz = std::max(maxVisibleX, minFrequencyHz + 1.0);
        const double firstDecade = std::pow(10.0, std::floor(std::log10(minFrequencyHz)));
        for (double decade = firstDecade; decade <= maxFrequencyHz * 1.001; decade *= 10.0) {
            for (int multiplier = 1; multiplier <= 9; ++multiplier) {
                const double value = decade * static_cast<double>(multiplier);
                if (value < minFrequencyHz || value > maxFrequencyHz) {
                    continue;
                }
                candidates.push_back(value);
            }

            if (decade > maxFrequencyHz / 10.0) {
                break;
            }
        }

        std::vector<double> ticks;
        const int graphWidth = std::max(static_cast<int>(graph.right - graph.left), 1);
        const int minPixelSpacing = clampValue(graphWidth / 60, 8, 12);
        int lastPixel = std::numeric_limits<int>::min() / 2;
        for (const double candidate : candidates) {
            const int pixel =
                graphXFromValue(graph, data.xAxisMode, minVisibleX, maxVisibleX, candidate);
            if (ticks.empty() || isMajorLogFrequencyTick(candidate, maxFrequencyHz) || pixel - lastPixel >= minPixelSpacing) {
                ticks.push_back(candidate);
                lastPixel = pixel;
            }
        }

        if (ticks.empty() || std::abs(ticks.back() - maxFrequencyHz) > 0.5) {
            ticks.push_back(maxFrequencyHz);
        }
        if (!ticks.empty() && std::abs(ticks.front() - minFrequencyHz) > 0.5) {
            ticks.insert(ticks.begin(), minFrequencyHz);
        }
        return ticks;
    }

    const double step = (maxVisibleX - minVisibleX) / 6.0;
    std::vector<double> ticks;
    for (int index = 0; index <= 6; ++index) {
        ticks.push_back(minVisibleX + (step * static_cast<double>(index)));
    }
    return ticks;
}

std::wstring formatXTickLabel(const PlotGraphData& data, double value) {
    if (data.xAxisMode == PlotGraphXAxisMode::LogFrequency) {
        if (value >= 1000.0) {
            return std::to_wstring(static_cast<int>(std::lround(value / 1000.0))) + L"k";
        }
        return std::to_wstring(static_cast<int>(std::lround(value)));
    }
    return formatAxisValue(value);
}

std::vector<AxisLabel> buildXLabels(HDC hdc,
                                    const RECT& graph,
                                    const PlotGraphData& data,
                                    double minVisibleX,
                                    double maxVisibleX,
                                    const std::vector<double>& ticks) {
    std::vector<AxisLabel> candidates;
    candidates.reserve(ticks.size());
    const double maxValue = maxVisibleX;
    for (const double tick : ticks) {
        AxisLabel label;
        label.value = tick;
        label.pixel = graphXFromValue(graph, data.xAxisMode, minVisibleX, maxVisibleX, tick);
        label.text = formatXTickLabel(data, tick);
        const int width = measureTextWidth(hdc, label.text);
        label.left = clampValue(label.pixel - width / 2,
                                static_cast<int>(graph.left - 2),
                                static_cast<int>(graph.right - width + 2));
        label.right = label.left + width;
        candidates.push_back(std::move(label));
    }

    if (data.xAxisMode != PlotGraphXAxisMode::LogFrequency || candidates.empty()) {
        return candidates;
    }

    constexpr int kMinLabelGap = 8;
    std::vector<AxisLabel> labels;
    labels.reserve(candidates.size());

    for (const AxisLabel& candidate : candidates) {
        if (!isMajorLogFrequencyTick(candidate.value, maxValue)) {
            continue;
        }

        while (!labels.empty() && candidate.left - labels.back().right < kMinLabelGap) {
            labels.pop_back();
        }
        labels.push_back(candidate);
    }

    size_t insertIndex = 0;
    for (const AxisLabel& candidate : candidates) {
        if (isMajorLogFrequencyTick(candidate.value, maxValue)) {
            while (insertIndex < labels.size() && labels[insertIndex].value < candidate.value) {
                ++insertIndex;
            }
            continue;
        }

        while (insertIndex < labels.size() && labels[insertIndex].value < candidate.value) {
            ++insertIndex;
        }

        const int previousRight = insertIndex == 0 ? graph.left - kMinLabelGap : labels[insertIndex - 1].right;
        const int nextLeft = insertIndex == labels.size() ? graph.right + kMinLabelGap : labels[insertIndex].left;
        if (candidate.left - previousRight >= kMinLabelGap && nextLeft - candidate.right >= kMinLabelGap) {
            labels.insert(labels.begin() + static_cast<std::ptrdiff_t>(insertIndex), candidate);
            ++insertIndex;
        }
    }

    return labels.empty() ? candidates : labels;
}

std::wstring formatHoverFrequency(double frequencyHz) {
    const int decimals = frequencyHz < 100.0 ? 1 : 0;
    return formatWideDouble(frequencyHz, decimals) + L" Hz";
}

std::wstring formatHoverValue(double value, const std::wstring& unit) {
    std::wstring text = formatWideDouble(value, 1);
    if (!unit.empty()) {
        text += L" ";
        text += unit;
    }
    return text;
}

std::wstring formatBrushXRange(const PlotGraphData& data, double minX, double maxX) {
    if (data.xAxisMode == PlotGraphXAxisMode::LogFrequency) {
        return L"Zoom " + formatHoverFrequency(minX) + L" to " + formatHoverFrequency(maxX);
    }

    std::wstring info = L"Zoom " + formatAxisValue(minX);
    if (!data.xUnit.empty()) {
        info += L" ";
        info += data.xUnit;
    }
    info += L" to ";
    info += formatAxisValue(maxX);
    if (!data.xUnit.empty()) {
        info += L" ";
        info += data.xUnit;
    }
    return info;
}

bool sampleSeriesValue(const PlotGraphData& data, const PlotGraphSeries& series, double xValue, double& sampledValue) {
    if (data.xValues.empty() || series.values.empty()) {
        return false;
    }

    const size_t count = std::min(data.xValues.size(), series.values.size());
    if (count == 0) {
        return false;
    }

    if (xValue <= data.xValues.front()) {
        sampledValue = series.values.front();
        return std::isfinite(sampledValue);
    }
    if (xValue >= data.xValues[count - 1]) {
        sampledValue = series.values[count - 1];
        return std::isfinite(sampledValue);
    }

    const auto upper = std::lower_bound(data.xValues.begin(),
                                        data.xValues.begin() + static_cast<std::ptrdiff_t>(count),
                                        xValue);
    const size_t upperIndex = static_cast<size_t>(std::distance(data.xValues.begin(), upper));
    if (upperIndex == 0) {
        sampledValue = series.values.front();
        return std::isfinite(sampledValue);
    }

    const size_t lowerIndex = upperIndex - 1;
    const double x0 = data.xValues[lowerIndex];
    const double x1 = data.xValues[upperIndex];
    const double y0 = series.values[lowerIndex];
    const double y1 = series.values[upperIndex];
    if (!std::isfinite(y0) || !std::isfinite(y1)) {
        return false;
    }

    const double position0 = data.xAxisMode == PlotGraphXAxisMode::LogFrequency ? std::log10(std::max(x0, 1.0e-6)) : x0;
    const double position1 = data.xAxisMode == PlotGraphXAxisMode::LogFrequency ? std::log10(std::max(x1, 1.0e-6)) : x1;
    const double position = data.xAxisMode == PlotGraphXAxisMode::LogFrequency ? std::log10(std::max(xValue, 1.0e-6)) : xValue;
    const double t = clampValue((position - position0) / std::max(position1 - position0, 1.0e-9), 0.0, 1.0);
    sampledValue = y0 + ((y1 - y0) * t);
    return std::isfinite(sampledValue);
}

std::wstring buildHoverInfoText(const PlotGraphData& data, double xValue) {
    if (data.xAxisMode == PlotGraphXAxisMode::LogFrequency) {
        std::wstring info = formatHoverFrequency(xValue);
        bool appendedSeriesValue = false;
        for (const PlotGraphSeries& series : data.series) {
            double sampledValue = 0.0;
            if (!sampleSeriesValue(data, series, xValue, sampledValue)) {
                continue;
            }

            info += appendedSeriesValue ? L"    " : L" @ ";
            appendedSeriesValue = true;
            if (!series.label.empty()) {
                info += series.label;
                info += L" ";
            }
            info += formatHoverValue(sampledValue, data.yUnit);
        }
        return info;
    }

    return {};
}

void drawResetButton(HDC hdc, const RECT& rect, bool enabled) {
    const COLORREF fill = enabled ? RGB(235, 240, 247) : RGB(243, 246, 250);
    const COLORREF textColor = enabled ? ui_theme::kText : ui_theme::kMuted;
    HBRUSH fillBrush = CreateSolidBrush(fill);
    FillRect(hdc, &rect, fillBrush);
    DeleteObject(fillBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, borderPen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(HOLLOW_BRUSH)));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    SetTextColor(hdc, textColor);
    DrawTextW(hdc, L"Reset", -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void drawSeries(HDC hdc, const PlotGraphData& data, const GraphLayout& layout, const PlotGraphSeries& series) {
    if (data.xValues.empty() || series.values.empty()) {
        return;
    }

    const int savedDc = SaveDC(hdc);
    HPEN seriesPen = CreatePen(series.lineStyle == PlotGraphLineStyle::Dash ? PS_DASH : PS_SOLID, 1, series.color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, seriesPen));
    IntersectClipRect(hdc, layout.graph.left, layout.graph.top, layout.graph.right, layout.graph.bottom);
    bool penActive = false;
    for (size_t index = 0; index < data.xValues.size() && index < series.values.size(); ++index) {
        if (!std::isfinite(data.xValues[index]) || !std::isfinite(series.values[index])) {
            penActive = false;
            continue;
        }

        const int x = graphXFromValue(layout.graph,
                                      data.xAxisMode,
                                      layout.visibleMinX,
                                      layout.visibleMaxX,
                                      data.xValues[index],
                                      false);
        const int y = unclampedGraphYFromValue(layout.graph, series.values[index], layout.minY, layout.maxY);
        if (!penActive) {
            MoveToEx(hdc, x, y, nullptr);
            penActive = true;
        } else {
            LineTo(hdc, x, y);
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(seriesPen);
    RestoreDC(hdc, savedDc);
}

RECT brushRect(const RECT& graph, const POINT& anchor, const POINT& current) {
    RECT rect{
        std::min(anchor.x, current.x),
        graph.top,
        std::max(anchor.x, current.x),
        graph.bottom,
    };
    rect.left = clampValue(rect.left, graph.left, graph.right);
    rect.right = clampValue(rect.right, graph.left, graph.right);
    return rect;
}

bool hasBrushWidth(const POINT& anchor, const POINT& current) {
    return std::abs(current.x - anchor.x) >= kMinBrushPixels;
}

void fillSelectionOverlay(HDC hdc, const RECT& selection) {
    const int width = std::max(selection.right - selection.left, 1L);
    const int height = std::max(selection.bottom - selection.top, 1L);

    HDC overlayDc = CreateCompatibleDC(hdc);
    if (overlayDc == nullptr) {
        return;
    }

    HBITMAP overlayBitmap = CreateCompatibleBitmap(hdc, width, height);
    if (overlayBitmap == nullptr) {
        DeleteDC(overlayDc);
        return;
    }

    HBITMAP oldOverlayBitmap = reinterpret_cast<HBITMAP>(SelectObject(overlayDc, overlayBitmap));
    RECT overlayRect{0, 0, width, height};
    HBRUSH overlayBrush = CreateSolidBrush(RGB(214, 227, 244));
    FillRect(overlayDc, &overlayRect, overlayBrush);
    DeleteObject(overlayBrush);

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 80;
    AlphaBlend(hdc, selection.left, selection.top, width, height, overlayDc, 0, 0, width, height, blend);

    SelectObject(overlayDc, oldOverlayBitmap);
    DeleteObject(overlayBitmap);
    DeleteDC(overlayDc);
}

void drawSharedHoverMarker(HDC hdc, const GraphLayout& layout, const PlotGraphData& data, double xValue) {
    if (data.xValues.empty() || xValue < layout.visibleMinX || xValue > layout.visibleMaxX) {
        return;
    }

    const int x = graphXFromValue(layout.graph, data.xAxisMode, layout.visibleMinX, layout.visibleMaxX, xValue);
    HPEN markerPen = CreatePen(PS_DOT, 1, ui_theme::kAccent);
    if (markerPen == nullptr) {
        return;
    }

    const int savedDc = SaveDC(hdc);
    SelectObject(hdc, markerPen);
    MoveToEx(hdc, x, layout.graph.top, nullptr);
    LineTo(hdc, x, layout.graph.bottom);
    RestoreDC(hdc, savedDc);
    DeleteObject(markerPen);
}

}  // namespace

void PlotGraph::registerWindowClass(HINSTANCE instance) {
    WNDCLASSW graphClass{};
    graphClass.style = CS_DBLCLKS;
    graphClass.lpfnWndProc = WindowProc;
    graphClass.hInstance = instance;
    graphClass.lpszClassName = kWindowClassName;
    graphClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    graphClass.hbrBackground = CreateSolidBrush(RGB(248, 250, 252));
    RegisterClassW(&graphClass);
}

void PlotGraph::create(HWND parent, HINSTANCE instance, int controlId) {
    window_ = CreateWindowExW(0,
                              kWindowClassName,
                              nullptr,
                              WS_CHILD | WS_VISIBLE,
                              0,
                              0,
                              0,
                              0,
                              parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              instance,
                              this);
}

void PlotGraph::setData(PlotGraphData data) {
    data_ = std::move(data);
    if (data_.xValues.empty()) {
        resetView();
    }
    invalidateBackgroundCache();
    invalidate();
}

void PlotGraph::setSharedHoverMarker(bool enabled, bool active, double xValue) {
    const bool nextActive = enabled && active;
    if (sharedHoverMarker_.enabled == enabled &&
        sharedHoverMarker_.active == nextActive &&
        std::abs(sharedHoverMarker_.xValue - xValue) < 1.0e-9) {
        return;
    }

    sharedHoverMarker_.enabled = enabled;
    sharedHoverMarker_.active = nextActive;
    sharedHoverMarker_.xValue = xValue;
    invalidate();
}

void PlotGraph::setDefaultXRange(bool enabled, double minX, double maxX) {
    hasDefaultXRange_ = enabled;
    defaultMinX_ = std::min(minX, maxX);
    defaultMaxX_ = std::max(minX, maxX);
    if (!hasCustomXRange_) {
        invalidateBackgroundCache();
        invalidate();
    }
}

void PlotGraph::setDefaultYRange(bool enabled, double minY, double maxY) {
    hasDefaultYRange_ = enabled;
    defaultMinY_ = std::min(minY, maxY);
    defaultMaxY_ = std::max(minY, maxY);
    if (!hasCustomYRange_) {
        invalidateBackgroundCache();
        invalidate();
    }
}

void PlotGraph::resetXRange() {
    hasCustomXRange_ = false;
    visibleMinX_ = 0.0;
    visibleMaxX_ = 1.0;
    brush_ = {};
    invalidateBackgroundCache();
    invalidate();
}

void PlotGraph::resetYRange() {
    hasCustomYRange_ = false;
    visibleMinY_ = -1.0;
    visibleMaxY_ = 1.0;
    invalidateBackgroundCache();
    invalidate();
}

void PlotGraph::resetView() {
    hasCustomXRange_ = false;
    visibleMinX_ = 0.0;
    visibleMaxX_ = 1.0;
    hasCustomYRange_ = false;
    visibleMinY_ = -1.0;
    visibleMaxY_ = 1.0;
    brush_ = {};
    invalidateBackgroundCache();
    invalidate();
}

bool PlotGraph::zoomX(double factor) {
    if (factor <= 0.0 || data_.xValues.empty()) {
        return false;
    }

    const double fullMinX = data_.xValues.front();
    const double fullMaxX = data_.xValues.back();
    const double fullSpan = std::max(fullMaxX - fullMinX, kMinimumVisibleXSpan);
    const double currentMinX = hasCustomXRange_ ? visibleMinX_ : (hasDefaultXRange_ ? defaultMinX_ : fullMinX);
    const double currentMaxX = hasCustomXRange_ ? visibleMaxX_ : (hasDefaultXRange_ ? defaultMaxX_ : fullMaxX);
    const double currentSpan = std::max(currentMaxX - currentMinX, kMinimumVisibleXSpan);
    const double nextSpan = clampValue(currentSpan / factor, kMinimumVisibleXSpan, fullSpan);
    const double centerX = (currentMinX + currentMaxX) * 0.5;

    double nextMinX = centerX - (nextSpan * 0.5);
    double nextMaxX = centerX + (nextSpan * 0.5);
    if (nextMinX < fullMinX) {
        nextMaxX += fullMinX - nextMinX;
        nextMinX = fullMinX;
    }
    if (nextMaxX > fullMaxX) {
        nextMinX -= nextMaxX - fullMaxX;
        nextMaxX = fullMaxX;
    }
    nextMinX = std::max(nextMinX, fullMinX);
    nextMaxX = std::min(nextMaxX, fullMaxX);

    hasCustomXRange_ = true;
    visibleMinX_ = nextMinX;
    visibleMaxX_ = nextMaxX;
    invalidateBackgroundCache();
    invalidate();
    return true;
}

bool PlotGraph::zoomXFromMin(double factor) {
    if (factor <= 0.0 || data_.xValues.empty()) {
        return false;
    }

    const double fullMinX = data_.xValues.front();
    const double fullMaxX = data_.xValues.back();
    const double fullSpan = std::max(fullMaxX - fullMinX, kMinimumVisibleXSpan);
    const double currentMinX = hasCustomXRange_ ? visibleMinX_ : (hasDefaultXRange_ ? defaultMinX_ : fullMinX);
    const double currentMaxX = hasCustomXRange_ ? visibleMaxX_ : (hasDefaultXRange_ ? defaultMaxX_ : fullMaxX);
    const double currentSpan = std::max(currentMaxX - currentMinX, kMinimumVisibleXSpan);
    const double nextSpan = clampValue(currentSpan / factor, kMinimumVisibleXSpan, fullSpan);

    const double anchorMinX = clampValue(currentMinX, fullMinX, fullMaxX);
    double nextMinX = anchorMinX;
    double nextMaxX = anchorMinX + nextSpan;
    if (nextMaxX > fullMaxX) {
        nextMaxX = fullMaxX;
        nextMinX = std::max(fullMinX, nextMaxX - nextSpan);
    }

    hasCustomXRange_ = true;
    visibleMinX_ = nextMinX;
    visibleMaxX_ = nextMaxX;
    invalidateBackgroundCache();
    invalidate();
    return true;
}

bool PlotGraph::zoomY(double factor) {
    if (factor <= 0.0 || window_ == nullptr) {
        return false;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return false;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc,
                                           rect,
                                           data_,
                                           hasCustomXRange_,
                                           visibleMinX_,
                                           visibleMaxX_,
                                           hasDefaultXRange_,
                                           defaultMinX_,
                                           defaultMaxX_,
                                           hasCustomYRange_,
                                           visibleMinY_,
                                           visibleMaxY_,
                                           hasDefaultYRange_,
                                           defaultMinY_,
                                           defaultMaxY_);
    ReleaseDC(window_, hdc);

    double nextMinY = layout.minY;
    double nextMaxY = layout.maxY;
    if (data_.yAxisMode == PlotGraphYAxisMode::SymmetricAroundZero) {
        const double currentHalfSpan = std::max(std::abs(layout.minY), std::abs(layout.maxY));
        const double defaultHalfSpan = hasDefaultYRange_ ? std::max(std::abs(defaultMinY_), std::abs(defaultMaxY_)) : currentHalfSpan;
        const double nextHalfSpan = clampValue(currentHalfSpan / factor, 1.0e-3, std::max(defaultHalfSpan, 1.0e-3));
        nextMinY = -nextHalfSpan;
        nextMaxY = nextHalfSpan;
    } else {
        const double currentSpan = std::max(layout.maxY - layout.minY, 1.0e-6);
        const double centerY = (layout.minY + layout.maxY) * 0.5;
        const double nextSpan = currentSpan / factor;
        nextMinY = centerY - (nextSpan * 0.5);
        nextMaxY = centerY + (nextSpan * 0.5);
    }

    hasCustomYRange_ = true;
    visibleMinY_ = nextMinY;
    visibleMaxY_ = nextMaxY;
    invalidateBackgroundCache();
    invalidate();
    return true;
}

void PlotGraph::layout(const RECT& bounds) const {
    if (window_ != nullptr) {
        MoveWindow(window_, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, TRUE);
    }
}

void PlotGraph::invalidate() const {
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void PlotGraph::invalidateBackgroundCache() const {
    backgroundCacheValid_ = false;
}

void PlotGraph::releaseBackgroundCache() const {
    if (backgroundCacheBitmap_ != nullptr) {
        DeleteObject(backgroundCacheBitmap_);
        backgroundCacheBitmap_ = nullptr;
    }

    backgroundCacheSize_ = {};
    backgroundCacheValid_ = false;
}

LRESULT CALLBACK PlotGraph::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    PlotGraph* graph = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        graph = reinterpret_cast<PlotGraph*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(graph));
        if (graph != nullptr) {
            graph->window_ = window;
        }
        return TRUE;
    }

    graph = reinterpret_cast<PlotGraph*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (graph == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_PAINT:
        graph->onPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        graph->onLButtonDown(lParam);
        return 0;
    case WM_MOUSEMOVE:
        graph->onMouseMove(lParam);
        return 0;
    case WM_MOUSELEAVE:
        graph->onMouseLeave();
        return 0;
    case WM_LBUTTONUP:
        graph->onLButtonUp(lParam);
        return 0;
    case WM_CAPTURECHANGED:
        graph->onCaptureChanged();
        return 0;
    case WM_SIZE:
        graph->invalidateBackgroundCache();
        return 0;
    case WM_NCDESTROY:
        graph->releaseBackgroundCache();
        break;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void PlotGraph::drawStaticLayer(HDC hdc, const RECT& rect) const {
    HBRUSH background = CreateSolidBrush(RGB(248, 250, 252));
    FillRect(hdc, &rect, background);
    DeleteObject(background);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, GetStockObject(DC_PEN));
    SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

    const GraphLayout layout = buildLayout(hdc,
                                           rect,
                                           data_,
                                           hasCustomXRange_,
                                           visibleMinX_,
                                           visibleMaxX_,
                                           hasDefaultXRange_,
                                           defaultMinX_,
                                           defaultMaxX_,
                                           hasCustomYRange_,
                                           visibleMinY_,
                                           visibleMaxY_,
                                           hasDefaultYRange_,
                                           defaultMinY_,
                                           defaultMaxY_);
    const std::vector<double> xTicks = buildXTicks(layout.graph, data_, layout.visibleMinX, layout.visibleMaxX);
    const std::vector<AxisLabel> xLabels =
        buildXLabels(hdc, layout.graph, data_, layout.visibleMinX, layout.visibleMaxX, xTicks);

    HBRUSH stripeBrush = CreateSolidBrush(RGB(244, 247, 251));
    for (size_t index = 0; index + 1 < layout.yTicks.size(); ++index) {
        if ((index % 2) != 0) {
            continue;
        }

        const int y0 = graphYFromValue(layout.graph, layout.yTicks[index], layout.minY, layout.maxY);
        const int y1 = graphYFromValue(layout.graph, layout.yTicks[index + 1], layout.minY, layout.maxY);
        RECT stripeRect{layout.graph.left, std::min(y0, y1), layout.graph.right, std::max(y0, y1)};
        FillRect(hdc, &stripeRect, stripeBrush);
    }
    DeleteObject(stripeBrush);

    SetDCPenColor(hdc, ui_theme::kBorder);
    Rectangle(hdc, layout.graph.left, layout.graph.top, layout.graph.right, layout.graph.bottom);

    for (const double tick : xTicks) {
        const int x =
            graphXFromValue(layout.graph, data_.xAxisMode, layout.visibleMinX, layout.visibleMaxX, tick);
        MoveToEx(hdc, x, layout.graph.top, nullptr);
        LineTo(hdc, x, layout.graph.bottom);
    }
    for (const double tick : layout.yTicks) {
        const int y = graphYFromValue(layout.graph, tick, layout.minY, layout.maxY);
        MoveToEx(hdc, layout.graph.left, y, nullptr);
        LineTo(hdc, layout.graph.right, y);
    }

    if (!data_.xValues.empty()) {
        for (const PlotGraphSeries& series : data_.series) {
            drawSeries(hdc, data_, layout, series);
        }
    }

    SetTextColor(hdc, ui_theme::kMuted);
    for (const AxisLabel& label : xLabels) {
        RECT labelRect{label.left, layout.graph.bottom + 6, label.right + 2, rect.bottom};
        DrawTextW(hdc, label.text.c_str(), -1, &labelRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    for (const double tick : layout.yTicks) {
        const int y = graphYFromValue(layout.graph, tick, layout.minY, layout.maxY);
        RECT labelRect{
            rect.left + 4,
            y - layout.textHeight / 2 - 2,
            layout.graph.left - 6,
            y + layout.textHeight / 2 + 2,
        };
        const std::wstring label = formatAxisValue(tick);
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    drawResetButton(hdc, layout.resetButton, hasCustomXRange_ || hasCustomYRange_);
}

void PlotGraph::notifyHoverChanged() const {
    if (window_ == nullptr) {
        return;
    }

    HWND parent = GetParent(window_);
    if (parent == nullptr) {
        return;
    }

    SendMessageW(parent,
                 WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(window_), kHoverChangedNotification),
                 reinterpret_cast<LPARAM>(window_));
}

void PlotGraph::onLButtonDown(LPARAM lParam) {
    if (window_ == nullptr) {
        return;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc,
                                           rect,
                                           data_,
                                           hasCustomXRange_,
                                           visibleMinX_,
                                           visibleMaxX_,
                                           hasDefaultXRange_,
                                           defaultMinX_,
                                           defaultMaxX_,
                                           hasCustomYRange_,
                                           visibleMinY_,
                                           visibleMaxY_,
                                           hasDefaultYRange_,
                                           defaultMinY_,
                                           defaultMaxY_);
    ReleaseDC(window_, hdc);

    const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if ((hasCustomXRange_ || hasCustomYRange_) && PtInRect(&layout.resetButton, position) != FALSE) {
        resetView();
        return;
    }
    if (PtInRect(&layout.graph, position) == FALSE) {
        return;
    }

    SetFocus(window_);
    brush_.active = true;
    brush_.anchor = position;
    brush_.current = position;
    SetCapture(window_);
    invalidate();
}

void PlotGraph::onLButtonUp(LPARAM lParam) {
    if (!brush_.active || window_ == nullptr) {
        return;
    }

    brush_.current = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (GetCapture() == window_) {
        ReleaseCapture();
    } else {
        onCaptureChanged();
    }
}

void PlotGraph::onMouseMove(LPARAM lParam) {
    if (window_ == nullptr) {
        return;
    }

    if (!hover_.tracking) {
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window_;
        TrackMouseEvent(&tracking);
        hover_.tracking = true;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc,
                                           rect,
                                           data_,
                                           hasCustomXRange_,
                                           visibleMinX_,
                                           visibleMaxX_,
                                           hasDefaultXRange_,
                                           defaultMinX_,
                                           defaultMaxX_,
                                           hasCustomYRange_,
                                           visibleMinY_,
                                           visibleMaxY_,
                                           hasDefaultYRange_,
                                           defaultMinY_,
                                           defaultMaxY_);
    ReleaseDC(window_, hdc);

    const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const bool insideGraph = PtInRect(&layout.graph, position) != FALSE;
    const double hoveredX =
        insideGraph ? valueFromGraphX(layout.graph, data_.xAxisMode, layout.visibleMinX, layout.visibleMaxX, position.x)
                    : hover_.xValue;
    const bool hoverChanged = hover_.active != insideGraph ||
                              hover_.position.x != position.x ||
                              hover_.position.y != position.y ||
                              std::abs(hover_.xValue - hoveredX) >= 1.0e-9;
    hover_.active = insideGraph;
    hover_.position = position;
    hover_.xValue = hoveredX;
    if (hoverChanged) {
        notifyHoverChanged();
        if (!brush_.active && (sharedHoverMarker_.enabled || data_.xAxisMode == PlotGraphXAxisMode::LogFrequency)) {
            invalidate();
        }
    }

    if (!brush_.active) {
        return;
    }

    if (position.x == brush_.current.x && position.y == brush_.current.y) {
        return;
    }

    brush_.current = position;
    invalidate();
}

void PlotGraph::onMouseLeave() {
    const bool hoverWasActive = hover_.active;
    hover_.active = false;
    hover_.tracking = false;
    if (hoverWasActive) {
        notifyHoverChanged();
        if (!brush_.active && (sharedHoverMarker_.enabled || data_.xAxisMode == PlotGraphXAxisMode::LogFrequency)) {
            invalidate();
        }
    }
}

void PlotGraph::onCaptureChanged() {
    if (!brush_.active || window_ == nullptr) {
        return;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        brush_ = {};
        invalidate();
        return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc,
                                           rect,
                                           data_,
                                           hasCustomXRange_,
                                           visibleMinX_,
                                           visibleMaxX_,
                                           hasDefaultXRange_,
                                           defaultMinX_,
                                           defaultMaxX_,
                                           hasCustomYRange_,
                                           visibleMinY_,
                                           visibleMaxY_,
                                           hasDefaultYRange_,
                                           defaultMinY_,
                                           defaultMaxY_);
    ReleaseDC(window_, hdc);

    if (hasBrushWidth(brush_.anchor, brush_.current) && !data_.xValues.empty()) {
        const double nextMinX = valueFromGraphX(layout.graph,
                                                data_.xAxisMode,
                                                layout.visibleMinX,
                                                layout.visibleMaxX,
                                                std::min(brush_.anchor.x, brush_.current.x));
        const double nextMaxX = valueFromGraphX(layout.graph,
                                                data_.xAxisMode,
                                                layout.visibleMinX,
                                                layout.visibleMaxX,
                                                std::max(brush_.anchor.x, brush_.current.x));
        const double fullMinX = data_.xValues.front();
        const double fullMaxX = data_.xValues.back();
        if ((nextMaxX - nextMinX) > kMinimumVisibleXSpan) {
            hasCustomXRange_ = true;
            visibleMinX_ = nextMinX;
            visibleMaxX_ = nextMaxX;
            invalidateBackgroundCache();
        }
    }

    brush_ = {};
    invalidate();
}

void PlotGraph::onPaint() const {
    PAINTSTRUCT paint{};
    HDC hdc = BeginPaint(window_, &paint);

    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc,
                                           rect,
                                           data_,
                                           hasCustomXRange_,
                                           visibleMinX_,
                                           visibleMaxX_,
                                           hasDefaultXRange_,
                                           defaultMinX_,
                                           defaultMaxX_,
                                           hasCustomYRange_,
                                           visibleMinY_,
                                           visibleMaxY_,
                                           hasDefaultYRange_,
                                           defaultMinY_,
                                           defaultMaxY_);
    HDC cacheSource = CreateCompatibleDC(hdc);
    if (cacheSource == nullptr) {
        EndPaint(window_, &paint);
        return;
    }

    if (backgroundCacheBitmap_ == nullptr ||
        backgroundCacheSize_.cx != rect.right - rect.left ||
        backgroundCacheSize_.cy != rect.bottom - rect.top) {
        releaseBackgroundCache();
    }
    if (backgroundCacheBitmap_ == nullptr) {
        backgroundCacheBitmap_ =
            CreateCompatibleBitmap(hdc, std::max(rect.right - rect.left, 1L), std::max(rect.bottom - rect.top, 1L));
        backgroundCacheSize_.cx = rect.right - rect.left;
        backgroundCacheSize_.cy = rect.bottom - rect.top;
        backgroundCacheValid_ = false;
    }
    if (backgroundCacheBitmap_ == nullptr) {
        DeleteDC(cacheSource);
        EndPaint(window_, &paint);
        return;
    }

    HBITMAP oldCacheBitmap = reinterpret_cast<HBITMAP>(SelectObject(cacheSource, backgroundCacheBitmap_));
    if (!backgroundCacheValid_) {
        drawStaticLayer(cacheSource, rect);
        backgroundCacheValid_ = true;
    }

    HDC frameDc = CreateCompatibleDC(hdc);
    if (frameDc == nullptr) {
        SelectObject(cacheSource, oldCacheBitmap);
        DeleteDC(cacheSource);
        EndPaint(window_, &paint);
        return;
    }

    HBITMAP frameBitmap =
        CreateCompatibleBitmap(hdc, std::max(rect.right - rect.left, 1L), std::max(rect.bottom - rect.top, 1L));
    if (frameBitmap == nullptr) {
        DeleteDC(frameDc);
        SelectObject(cacheSource, oldCacheBitmap);
        DeleteDC(cacheSource);
        EndPaint(window_, &paint);
        return;
    }

    HBITMAP oldFrameBitmap = reinterpret_cast<HBITMAP>(SelectObject(frameDc, frameBitmap));
    BitBlt(frameDc, 0, 0, rect.right - rect.left, rect.bottom - rect.top, cacheSource, 0, 0, SRCCOPY);
    SetBkMode(frameDc, TRANSPARENT);

    if (brush_.active && hasBrushWidth(brush_.anchor, brush_.current)) {
        const RECT selection = brushRect(layout.graph, brush_.anchor, brush_.current);
        fillSelectionOverlay(frameDc, selection);

        HPEN selectionPen = CreatePen(PS_SOLID, 1, ui_theme::kAccent);
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(frameDc, selectionPen));
        HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(frameDc, GetStockObject(HOLLOW_BRUSH)));
        Rectangle(frameDc, selection.left, selection.top, selection.right, selection.bottom);
        SelectObject(frameDc, oldBrush);
        SelectObject(frameDc, oldPen);
        DeleteObject(selectionPen);
    }

    if (sharedHoverMarker_.enabled && sharedHoverMarker_.active) {
        drawSharedHoverMarker(frameDc, layout, data_, sharedHoverMarker_.xValue);
    }

    RECT infoRect{layout.graph.left, rect.top + 2, layout.resetButton.left - 8, layout.graph.top - 4};
    if (brush_.active && hasBrushWidth(brush_.anchor, brush_.current)) {
        const double minX = valueFromGraphX(layout.graph,
                                            data_.xAxisMode,
                                            layout.visibleMinX,
                                            layout.visibleMaxX,
                                            std::min(brush_.anchor.x, brush_.current.x));
        const double maxX = valueFromGraphX(layout.graph,
                                            data_.xAxisMode,
                                            layout.visibleMinX,
                                            layout.visibleMaxX,
                                            std::max(brush_.anchor.x, brush_.current.x));
        const std::wstring infoText = formatBrushXRange(data_, minX, maxX);
        SetTextColor(frameDc, ui_theme::kAccent);
        DrawTextW(frameDc, infoText.c_str(), -1, &infoRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        double infoXValue = 0.0;
        bool hasInfoXValue = false;
        if (hover_.active) {
            infoXValue = hover_.xValue;
            hasInfoXValue = true;
        } else if (sharedHoverMarker_.enabled && sharedHoverMarker_.active) {
            infoXValue = sharedHoverMarker_.xValue;
            hasInfoXValue = true;
        }

        if (hasInfoXValue) {
            const std::wstring infoText = buildHoverInfoText(data_, infoXValue);
            if (!infoText.empty()) {
                SetTextColor(frameDc, ui_theme::kAccent);
                DrawTextW(frameDc, infoText.c_str(), -1, &infoRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
    }

    BitBlt(hdc, 0, 0, rect.right - rect.left, rect.bottom - rect.top, frameDc, 0, 0, SRCCOPY);
    SelectObject(frameDc, oldFrameBitmap);
    DeleteObject(frameBitmap);
    DeleteDC(frameDc);
    SelectObject(cacheSource, oldCacheBitmap);
    DeleteDC(cacheSource);

    EndPaint(window_, &paint);
}

}  // namespace wolfie::ui
