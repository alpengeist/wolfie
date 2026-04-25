#include "ui/plot_graph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

struct GraphLayout {
    RECT graph{};
    int textHeight = 12;
    double minY = 0.0;
    double maxY = 1.0;
    std::vector<double> yTicks;
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

double xTFromValue(PlotGraphXAxisMode mode, const std::vector<double>& xValues, double value) {
    if (xValues.empty()) {
        return 0.0;
    }

    if (mode == PlotGraphXAxisMode::LogFrequency) {
        const double minValue = std::log10(std::max(xValues.front(), 1.0));
        const double maxValue = std::log10(std::max(xValues.back(), xValues.front() + 1.0));
        return clampValue((std::log10(std::max(value, 1.0)) - minValue) / std::max(maxValue - minValue, 1.0e-9), 0.0, 1.0);
    }

    const double minValue = xValues.front();
    const double maxValue = xValues.back();
    return clampValue((value - minValue) / std::max(maxValue - minValue, 1.0e-9), 0.0, 1.0);
}

int graphXFromValue(const RECT& graph, PlotGraphXAxisMode mode, const std::vector<double>& xValues, double value) {
    return graph.left + static_cast<int>(std::lround(xTFromValue(mode, xValues, value) * (graph.right - graph.left)));
}

int graphYFromValue(const RECT& graph, double value, double minY, double maxY) {
    const double range = std::max(maxY - minY, 1.0e-9);
    const double yT = clampValue((value - minY) / range, 0.0, 1.0);
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

GraphLayout buildLayout(HDC hdc, const RECT& rect, const PlotGraphData& data) {
    GraphLayout layout;
    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    layout.textHeight = std::max(static_cast<int>(metrics.tmHeight), 12);

    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    if (data.fixedYRange) {
        minY = data.minY;
        maxY = data.maxY;
    } else {
        for (const PlotGraphSeries& series : data.series) {
            for (const double value : series.values) {
                minY = std::min(minY, value);
                maxY = std::max(maxY, value);
            }
        }
        if (!std::isfinite(minY) || !std::isfinite(maxY)) {
            minY = 0.0;
            maxY = 1.0;
        } else if (std::abs(maxY - minY) < 1.0e-6) {
            minY -= 1.0;
            maxY += 1.0;
        } else {
            const double pad = (maxY - minY) * 0.08;
            minY -= pad;
            maxY += pad;
        }
    }
    layout.minY = minY;
    layout.maxY = maxY;
    layout.yTicks = buildYTicks(minY, maxY);

    int widestYLabel = 0;
    for (const double tick : layout.yTicks) {
        widestYLabel = std::max(widestYLabel, measureTextWidth(hdc, formatAxisValue(tick)));
    }

    layout.graph = RECT{
        rect.left + std::max(52, widestYLabel + 14),
        rect.top + 8,
        rect.right - 8,
        rect.bottom - (layout.textHeight + 18),
    };
    return layout;
}

std::vector<double> buildXTicks(const PlotGraphData& data) {
    if (data.xValues.empty()) {
        return {};
    }

    if (data.xAxisMode == PlotGraphXAxisMode::LogFrequency) {
        std::vector<double> ticks;
        const double minFrequencyHz = std::max(data.xValues.front(), 10.0);
        const double maxFrequencyHz = std::max(data.xValues.back(), minFrequencyHz + 1.0);
        for (double decade = 10.0; decade <= maxFrequencyHz; decade *= 10.0) {
            for (int multiplier = 1; multiplier <= 9; ++multiplier) {
                const double value = decade * static_cast<double>(multiplier);
                if (value < minFrequencyHz || value > maxFrequencyHz) {
                    continue;
                }
                ticks.push_back(value);
            }
        }
        if (ticks.empty() || ticks.back() < maxFrequencyHz) {
            ticks.push_back(maxFrequencyHz);
        }
        return ticks;
    }

    const double minValue = data.xValues.front();
    const double maxValue = data.xValues.back();
    const double step = (maxValue - minValue) / 6.0;
    std::vector<double> ticks;
    for (int index = 0; index <= 6; ++index) {
        ticks.push_back(minValue + (step * static_cast<double>(index)));
    }
    return ticks;
}

std::wstring formatXTickLabel(const PlotGraphData& data, double value) {
    if (data.xAxisMode == PlotGraphXAxisMode::LogFrequency) {
        if (value >= 1000.0) {
            return formatWideDouble(value / 1000.0, value >= 10000.0 ? 0 : 1) + L"k";
        }
        return formatWideDouble(value, value >= 100.0 ? 0 : 1);
    }
    return formatAxisValue(value);
}

void drawSeries(HDC hdc, const PlotGraphData& data, const GraphLayout& layout, const PlotGraphSeries& series) {
    if (data.xValues.empty() || series.values.empty()) {
        return;
    }

    SetDCPenColor(hdc, series.color);
    const int savedDc = SaveDC(hdc);
    IntersectClipRect(hdc, layout.graph.left, layout.graph.top, layout.graph.right, layout.graph.bottom);
    for (size_t index = 0; index < data.xValues.size() && index < series.values.size(); ++index) {
        const int x = graphXFromValue(layout.graph, data.xAxisMode, data.xValues, data.xValues[index]);
        const int y = graphYFromValue(layout.graph, series.values[index], layout.minY, layout.maxY);
        if (index == 0) {
            MoveToEx(hdc, x, y, nullptr);
        } else {
            LineTo(hdc, x, y);
        }
    }
    RestoreDC(hdc, savedDc);
}

}  // namespace

void PlotGraph::registerWindowClass(HINSTANCE instance) {
    WNDCLASSW graphClass{};
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
    invalidate();
}

void PlotGraph::layout(const RECT& bounds) const {
    if (window_ != nullptr) {
        MoveWindow(window_, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, TRUE);
    }
}

void PlotGraph::invalidate() const {
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
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
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void PlotGraph::onPaint() const {
    PAINTSTRUCT paint{};
    HDC hdc = BeginPaint(window_, &paint);

    RECT rect{};
    GetClientRect(window_, &rect);
    HBRUSH background = CreateSolidBrush(RGB(248, 250, 252));
    FillRect(hdc, &rect, background);
    DeleteObject(background);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, GetStockObject(DC_PEN));
    SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

    const GraphLayout layout = buildLayout(hdc, rect, data_);
    const std::vector<double> xTicks = buildXTicks(data_);

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
        const int x = graphXFromValue(layout.graph, data_.xAxisMode, data_.xValues, tick);
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
    for (const double tick : xTicks) {
        const int x = graphXFromValue(layout.graph, data_.xAxisMode, data_.xValues, tick);
        const std::wstring label = formatXTickLabel(data_, tick);
        const int width = measureTextWidth(hdc, label);
        RECT labelRect{x - (width / 2), layout.graph.bottom + 6, x + (width / 2) + 4, rect.bottom};
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    for (const double tick : layout.yTicks) {
        const int y = graphYFromValue(layout.graph, tick, layout.minY, layout.maxY);
        RECT labelRect{rect.left + 4, y - layout.textHeight / 2 - 2, layout.graph.left - 6, y + layout.textHeight / 2 + 2};
        const std::wstring label = formatAxisValue(tick);
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    EndPaint(window_, &paint);
}

}  // namespace wolfie::ui
