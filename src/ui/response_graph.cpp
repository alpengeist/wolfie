#include "ui/response_graph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <windowsx.h>

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr double kMinFrequencyHz = 10.0;
constexpr double kMaxFrequencyHz = 20000.0;
constexpr double kMinimumVisibleDbSpan = 1.0;
constexpr int kMinBrushPixels = 6;
constexpr int kResetButtonWidth = 54;
constexpr int kResetButtonHeight = 20;

struct AxisLabel {
    double value = 0.0;
    int pixel = 0;
    int left = 0;
    int right = 0;
    std::wstring text;
};

struct GraphLayout {
    RECT graph{};
    RECT resetButton{};
    int textHeight = 12;
    double axisMinDb = 0.0;
    double axisMaxDb = 1.0;
    double dbStep = 1.0;
    double visibleMinFrequencyHz = kMinFrequencyHz;
    double visibleMaxFrequencyHz = kMaxFrequencyHz;
    std::vector<double> yTickValues;
};

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

bool isMajorFrequencyTick(double frequencyHz) {
    return frequencyHz == 10.0 || frequencyHz == 100.0 || frequencyHz == 1000.0 || frequencyHz == 10000.0 ||
           frequencyHz == kMaxFrequencyHz;
}

int measureTextWidth(HDC hdc, const std::wstring& text) {
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

double responseGraphFullXT(double frequencyHz) {
    const double clamped = clampValue(frequencyHz, kMinFrequencyHz, 24000.0);
    if (clamped < 100.0) {
        return std::log10(clamped / 10.0) / 3.5;
    }
    if (clamped < 1000.0) {
        return (1.0 + std::log10(clamped / 100.0)) / 3.5;
    }
    if (clamped < 10000.0) {
        return (2.0 + std::log10(clamped / 1000.0)) / 3.5;
    }
    return (3.0 + (std::log10(clamped / 10000.0) / std::log10(2.0) * 0.5)) / 3.5;
}

std::wstring formatResponseTickLabel(double frequencyHz) {
    if (frequencyHz >= 1000.0) {
        return std::to_wstring(static_cast<int>(std::lround(frequencyHz / 1000.0))) + L"k";
    }
    return std::to_wstring(static_cast<int>(std::lround(frequencyHz)));
}

std::wstring formatDbTickLabel(double valueDb) {
    const double rounded0 = std::round(valueDb);
    if (std::abs(valueDb - rounded0) < 0.005) {
        return formatWideDouble(valueDb, 0);
    }

    const double rounded1 = std::round(valueDb * 10.0) / 10.0;
    if (std::abs(valueDb - rounded1) < 0.0005) {
        return formatWideDouble(valueDb, 1);
    }

    const double rounded2 = std::round(valueDb * 100.0) / 100.0;
    if (std::abs(valueDb - rounded2) < 0.00005) {
        return formatWideDouble(valueDb, 2);
    }

    return formatWideDouble(valueDb, 3);
}

double responseGraphFrequencyAtFullT(double xT) {
    const double scaled = clampValue(xT, 0.0, 1.0) * 3.5;
    if (scaled < 1.0) {
        return 10.0 * std::pow(10.0, scaled);
    }
    if (scaled < 2.0) {
        return 100.0 * std::pow(10.0, scaled - 1.0);
    }
    if (scaled < 3.0) {
        return 1000.0 * std::pow(10.0, scaled - 2.0);
    }
    return 10000.0 * std::pow(2.0, (scaled - 3.0) / 0.5);
}

double visibleResponseXT(double frequencyHz, double minVisibleFrequencyHz, double maxVisibleFrequencyHz) {
    const double startXT = responseGraphFullXT(minVisibleFrequencyHz);
    const double endXT = responseGraphFullXT(maxVisibleFrequencyHz);
    return (responseGraphFullXT(frequencyHz) - startXT) / std::max(endXT - startXT, 1.0e-9);
}

int graphXFromFrequency(const RECT& graph,
                        double minVisibleFrequencyHz,
                        double maxVisibleFrequencyHz,
                        double frequencyHz,
                        bool clampToGraph = true) {
    const double xT = clampToGraph ? clampValue(visibleResponseXT(frequencyHz, minVisibleFrequencyHz, maxVisibleFrequencyHz), 0.0, 1.0)
                                   : visibleResponseXT(frequencyHz, minVisibleFrequencyHz, maxVisibleFrequencyHz);
    return graph.left + static_cast<int>(std::lround(xT * (graph.right - graph.left)));
}

int graphYFromDb(const RECT& graph, double valueDb, double minDb, double maxDb) {
    const double range = std::max(maxDb - minDb, 1.0e-6);
    const double yT = clampValue((valueDb - minDb) / range, 0.0, 1.0);
    return graph.bottom - static_cast<int>(std::lround(yT * (graph.bottom - graph.top)));
}

int unclampedGraphYFromDb(const RECT& graph, double valueDb, double minDb, double maxDb) {
    const double range = std::max(maxDb - minDb, 1.0e-6);
    const double yT = (valueDb - minDb) / range;
    return graph.bottom - static_cast<int>(std::lround(yT * (graph.bottom - graph.top)));
}

double frequencyFromGraphX(const RECT& graph, double minVisibleFrequencyHz, double maxVisibleFrequencyHz, int x) {
    const int width = std::max(static_cast<int>(graph.right - graph.left), 1);
    const double xT =
        static_cast<double>(clampValue(static_cast<int>(x - graph.left), 0, width)) / static_cast<double>(width);
    const double startXT = responseGraphFullXT(minVisibleFrequencyHz);
    const double endXT = responseGraphFullXT(maxVisibleFrequencyHz);
    return clampValue(responseGraphFrequencyAtFullT(startXT + ((endXT - startXT) * xT)), kMinFrequencyHz, kMaxFrequencyHz);
}

double dbFromGraphY(const RECT& graph, int y, double minDb, double maxDb) {
    const int height = std::max(static_cast<int>(graph.bottom - graph.top), 1);
    const double yT = static_cast<double>(clampValue(static_cast<int>(graph.bottom - y), 0, height)) /
                      static_cast<double>(height);
    return minDb + yT * (maxDb - minDb);
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

bool scanVisibleDbRange(const ResponseGraphData& data,
                        double minVisibleFrequencyHz,
                        double maxVisibleFrequencyHz,
                        double& minDb,
                        double& maxDb) {
    bool found = false;
    for (const ResponseGraphSeries& series : data.series) {
        const size_t count = std::min(data.frequencyAxisHz.size(), series.values.size());
        for (size_t index = 0; index < count; ++index) {
            const double frequencyHz = data.frequencyAxisHz[index];
            if (frequencyHz < minVisibleFrequencyHz || frequencyHz > maxVisibleFrequencyHz) {
                continue;
            }

            const double valueDb = series.values[index];
            minDb = found ? std::min(minDb, valueDb) : valueDb;
            maxDb = found ? std::max(maxDb, valueDb) : valueDb;
            found = true;
        }
    }
    return found;
}

void expandVisibleDbRange(double& minDb, double& maxDb) {
    if (!std::isfinite(minDb) || !std::isfinite(maxDb)) {
        minDb = -1.0;
        maxDb = 1.0;
        return;
    }

    if (std::abs(maxDb - minDb) < 1.0e-9) {
        const double halfSpan = kMinimumVisibleDbSpan * 0.5;
        minDb -= halfSpan;
        maxDb += halfSpan;
        return;
    }

    const double range = maxDb - minDb;
    const double pad = std::max(range * 0.08, kMinimumVisibleDbSpan * 0.05);
    minDb -= pad;
    maxDb += pad;
}

std::vector<double> buildDbTickValues(double minDb, double maxDb, double stepDb) {
    std::vector<double> ticks;
    if (stepDb <= 0.0 || maxDb < minDb) {
        return ticks;
    }

    const double first = std::floor(minDb / stepDb) * stepDb;
    const double last = std::ceil(maxDb / stepDb) * stepDb;
    for (double value = first; value <= last + (stepDb * 0.25); value += stepDb) {
        ticks.push_back(value);
    }
    return ticks;
}

GraphLayout buildGraphLayout(HDC hdc,
                             const RECT& rect,
                             const ResponseGraphData& data,
                             bool hasCustomVisibleFrequencyRange,
                             double customMinFrequencyHz,
                             double customMaxFrequencyHz) {
    GraphLayout layout;

    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    layout.textHeight = std::max(static_cast<int>(metrics.tmHeight), 12);

    double fullMinFrequencyHz = kMinFrequencyHz;
    double fullMaxFrequencyHz = kMaxFrequencyHz;
    if (!data.frequencyAxisHz.empty()) {
        fullMinFrequencyHz = clampValue(data.frequencyAxisHz.front(), kMinFrequencyHz, kMaxFrequencyHz);
        fullMaxFrequencyHz = clampValue(data.frequencyAxisHz.back(), fullMinFrequencyHz + 1.0, kMaxFrequencyHz);
    }

    layout.visibleMinFrequencyHz = fullMinFrequencyHz;
    layout.visibleMaxFrequencyHz = fullMaxFrequencyHz;
    if (hasCustomVisibleFrequencyRange) {
        layout.visibleMinFrequencyHz =
            clampValue(std::min(customMinFrequencyHz, customMaxFrequencyHz), fullMinFrequencyHz, fullMaxFrequencyHz);
        layout.visibleMaxFrequencyHz =
            clampValue(std::max(customMinFrequencyHz, customMaxFrequencyHz), fullMinFrequencyHz, fullMaxFrequencyHz);
        if (std::abs(layout.visibleMaxFrequencyHz - layout.visibleMinFrequencyHz) < 1.0e-9) {
            layout.visibleMinFrequencyHz = fullMinFrequencyHz;
            layout.visibleMaxFrequencyHz = fullMaxFrequencyHz;
        }
    }

    double axisMinDb = std::numeric_limits<double>::max();
    double axisMaxDb = std::numeric_limits<double>::lowest();
    if (!scanVisibleDbRange(data, layout.visibleMinFrequencyHz, layout.visibleMaxFrequencyHz, axisMinDb, axisMaxDb)) {
        axisMinDb = data.minDb;
        axisMaxDb = data.minDb + 1.0;
        for (const ResponseGraphSeries& series : data.series) {
            for (const double value : series.values) {
                axisMinDb = std::min(axisMinDb, value);
                axisMaxDb = std::max(axisMaxDb, value);
            }
        }
    }
    expandVisibleDbRange(axisMinDb, axisMaxDb);
    layout.axisMinDb = axisMinDb;
    layout.axisMaxDb = axisMaxDb;

    const int infoLineHeight = std::max(layout.textHeight, kResetButtonHeight) + 6;
    const int estimatedGraphHeight = std::max(static_cast<int>(rect.bottom - rect.top) - (layout.textHeight + 22) -
                                                  infoLineHeight,
                                              80);
    const double rawDbStep = (layout.axisMaxDb - layout.axisMinDb) * static_cast<double>(layout.textHeight + 10) /
                             static_cast<double>(std::max(estimatedGraphHeight, 1));
    layout.dbStep = nextNiceStep(rawDbStep);
    layout.yTickValues = buildDbTickValues(layout.axisMinDb, layout.axisMaxDb, layout.dbStep);

    int widestYLabel = 0;
    for (const double tickValue : layout.yTickValues) {
        widestYLabel = std::max(widestYLabel, measureTextWidth(hdc, formatDbTickLabel(tickValue)));
    }

    layout.resetButton = RECT{
        rect.right - kResetButtonWidth - 8,
        rect.top + 2,
        rect.right - 8,
        rect.top + 2 + kResetButtonHeight,
    };
    layout.graph = RECT{
        rect.left + std::max(44, widestYLabel + 12),
        rect.top + infoLineHeight + 8,
        rect.right - 8,
        rect.bottom - (layout.textHeight + 14),
    };

    if (layout.graph.right - layout.graph.left < 40) {
        layout.graph.left = rect.left + 40;
        layout.graph.right = rect.right - 4;
    }
    if (layout.graph.bottom - layout.graph.top < 40) {
        layout.graph.top = rect.top + infoLineHeight + 4;
        layout.graph.bottom = rect.bottom - (layout.textHeight + 8);
    }

    return layout;
}

std::vector<double> buildFrequencyTickValues(const RECT& graph,
                                             double minVisibleFrequencyHz,
                                             double maxVisibleFrequencyHz) {
    std::vector<double> candidates;
    for (double decade = 10.0; decade <= 1000.0; decade *= 10.0) {
        for (int multiplier = 1; multiplier <= 9; ++multiplier) {
            const double frequencyHz = decade * static_cast<double>(multiplier);
            if (frequencyHz < minVisibleFrequencyHz || frequencyHz > maxVisibleFrequencyHz) {
                continue;
            }
            candidates.push_back(frequencyHz);
        }
    }
    for (double frequencyHz = 10000.0; frequencyHz <= kMaxFrequencyHz; frequencyHz += 1000.0) {
        if (frequencyHz >= minVisibleFrequencyHz && frequencyHz <= maxVisibleFrequencyHz) {
            candidates.push_back(frequencyHz);
        }
    }

    std::vector<double> ticks;
    const int graphWidth = std::max(static_cast<int>(graph.right - graph.left), 1);
    const int minPixelSpacing = clampValue(graphWidth / 60, 8, 12);
    int lastPixel = std::numeric_limits<int>::min() / 2;
    for (const double candidate : candidates) {
        const int pixel = graphXFromFrequency(graph, minVisibleFrequencyHz, maxVisibleFrequencyHz, candidate);
        if (ticks.empty() || isMajorFrequencyTick(candidate) || pixel - lastPixel >= minPixelSpacing ||
            candidate >= maxVisibleFrequencyHz) {
            ticks.push_back(candidate);
            lastPixel = pixel;
        }
    }

    if (ticks.empty() || ticks.front() > minVisibleFrequencyHz) {
        ticks.insert(ticks.begin(), minVisibleFrequencyHz);
    }
    if (ticks.back() < maxVisibleFrequencyHz) {
        ticks.push_back(maxVisibleFrequencyHz);
    }
    return ticks;
}

std::vector<AxisLabel> buildFrequencyLabels(HDC hdc,
                                            const RECT& graph,
                                            double minVisibleFrequencyHz,
                                            double maxVisibleFrequencyHz,
                                            const std::vector<double>& tickValues) {
    std::vector<AxisLabel> candidates;
    candidates.reserve(tickValues.size());
    for (const double tickValue : tickValues) {
        AxisLabel label;
        label.value = tickValue;
        label.pixel = graphXFromFrequency(graph, minVisibleFrequencyHz, maxVisibleFrequencyHz, tickValue);
        label.text = formatResponseTickLabel(tickValue);
        const int width = measureTextWidth(hdc, label.text);
        label.left = clampValue(label.pixel - width / 2,
                                static_cast<int>(graph.left - 2),
                                static_cast<int>(graph.right - width + 2));
        label.right = label.left + width;
        candidates.push_back(std::move(label));
    }

    if (candidates.empty()) {
        return {};
    }

    constexpr int kMinLabelGap = 8;
    std::vector<AxisLabel> labels;
    labels.reserve(candidates.size());

    for (const AxisLabel& candidate : candidates) {
        if (!isMajorFrequencyTick(candidate.value)) {
            continue;
        }

        while (!labels.empty() && candidate.left - labels.back().right < kMinLabelGap) {
            labels.pop_back();
        }
        labels.push_back(candidate);
    }

    size_t insertIndex = 0;
    for (const AxisLabel& candidate : candidates) {
        if (isMajorFrequencyTick(candidate.value)) {
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

std::wstring formatHoverAmplitude(double valueDb) {
    return formatWideDouble(valueDb, 1) + L" dB";
}

std::wstring formatHoverWavelength(double wavelengthMeters) {
    const int decimals = wavelengthMeters >= 10.0 ? 1 : (wavelengthMeters >= 1.0 ? 2 : 3);
    return L"lambda: " + formatWideDouble(wavelengthMeters, decimals) + L" m";
}

std::wstring formatHoverQuarterWavelength(double wavelengthMeters) {
    const double quarterWavelengthMeters = wavelengthMeters / 4.0;
    const int decimals = quarterWavelengthMeters >= 10.0 ? 1 : (quarterWavelengthMeters >= 1.0 ? 2 : 3);
    return L"lambda/4: " + formatWideDouble(quarterWavelengthMeters, decimals) + L" m";
}

std::wstring formatBrushFrequencyRange(double minFrequencyHz, double maxFrequencyHz) {
    return L"Zoom " + formatHoverFrequency(minFrequencyHz) + L" - " + formatHoverFrequency(maxFrequencyHz);
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

    RECT textRect = rect;
    SetTextColor(hdc, textColor);
    DrawTextW(hdc, L"Reset", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void drawSeries(HDC hdc,
                const RECT& graph,
                double axisMinDb,
                double axisMaxDb,
                double minVisibleFrequencyHz,
                double maxVisibleFrequencyHz,
                const std::vector<double>& frequencyAxisHz,
                const std::vector<double>& values,
                COLORREF color) {
    if (frequencyAxisHz.empty() || values.empty()) {
        return;
    }

    SetDCPenColor(hdc, color);
    const int savedDc = SaveDC(hdc);
    IntersectClipRect(hdc, graph.left, graph.top, graph.right, graph.bottom);
    for (size_t i = 0; i < values.size() && i < frequencyAxisHz.size(); ++i) {
        const int x = graphXFromFrequency(graph,
                                          minVisibleFrequencyHz,
                                          maxVisibleFrequencyHz,
                                          frequencyAxisHz[i],
                                          false);
        const int y = unclampedGraphYFromDb(graph, values[i], axisMinDb, axisMaxDb);
        if (i == 0) {
            MoveToEx(hdc, x, y, nullptr);
        } else {
            LineTo(hdc, x, y);
        }
    }
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

}  // namespace

void ResponseGraph::registerWindowClass(HINSTANCE instance) {
    WNDCLASSW graphClass{};
    graphClass.style = CS_DBLCLKS;
    graphClass.lpfnWndProc = WindowProc;
    graphClass.hInstance = instance;
    graphClass.lpszClassName = kWindowClassName;
    graphClass.hCursor = LoadCursor(nullptr, IDC_CROSS);
    graphClass.hbrBackground = CreateSolidBrush(RGB(248, 250, 252));
    RegisterClassW(&graphClass);
}

void ResponseGraph::create(HWND parent, HINSTANCE instance, int controlId) {
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

void ResponseGraph::setData(ResponseGraphData data) {
    data_ = std::move(data);
    if (data_.frequencyAxisHz.empty()) {
        resetVisibleFrequencyRange();
    }
    invalidate();
}

void ResponseGraph::setVisibleFrequencyRange(bool hasCustomRange, double minFrequencyHz, double maxFrequencyHz) {
    if (!hasCustomRange) {
        resetVisibleFrequencyRange();
        invalidate();
        return;
    }

    const double nextMin = std::min(minFrequencyHz, maxFrequencyHz);
    const double nextMax = std::max(minFrequencyHz, maxFrequencyHz);
    if (hasCustomVisibleFrequencyRange_ &&
        std::abs(visibleMinFrequencyHz_ - nextMin) < 0.001 &&
        std::abs(visibleMaxFrequencyHz_ - nextMax) < 0.001) {
        return;
    }

    hasCustomVisibleFrequencyRange_ = true;
    visibleMinFrequencyHz_ = nextMin;
    visibleMaxFrequencyHz_ = nextMax;
    invalidate();
}

void ResponseGraph::resetVisibleFrequencyRange() {
    hasCustomVisibleFrequencyRange_ = false;
    visibleMinFrequencyHz_ = kMinFrequencyHz;
    visibleMaxFrequencyHz_ = kMaxFrequencyHz;
    brush_ = {};
}

void ResponseGraph::layout(const RECT& bounds) const {
    if (window_ != nullptr) {
        MoveWindow(window_, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, TRUE);
    }
}

void ResponseGraph::invalidate() const {
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

LRESULT CALLBACK ResponseGraph::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ResponseGraph* graph = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        graph = reinterpret_cast<ResponseGraph*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(graph));
        if (graph != nullptr) {
            graph->window_ = window;
        }
        return TRUE;
    }

    graph = reinterpret_cast<ResponseGraph*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (graph == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_PAINT:
        graph->onPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        graph->onMouseMove(lParam);
        return 0;
    case WM_MOUSELEAVE:
        graph->onMouseLeave();
        return 0;
    case WM_LBUTTONDOWN:
        graph->onLButtonDown(lParam);
        return 0;
    case WM_LBUTTONUP:
        graph->onLButtonUp(lParam);
        return 0;
    case WM_CAPTURECHANGED:
        graph->onCaptureChanged();
        return 0;
    case WM_LBUTTONDBLCLK:
        graph->onLButtonDblClk(lParam);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

RECT ResponseGraph::infoLineRect() const {
    RECT rect{};
    if (window_ == nullptr) {
        return rect;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        GetClientRect(window_, &rect);
        rect.bottom = rect.top;
        return rect;
    }

    GetClientRect(window_, &rect);
    const GraphLayout layout =
        buildGraphLayout(hdc, rect, data_, hasCustomVisibleFrequencyRange_, visibleMinFrequencyHz_, visibleMaxFrequencyHz_);
    ReleaseDC(window_, hdc);

    rect.left = layout.graph.left;
    rect.top += 2;
    rect.right = layout.resetButton.left - 8;
    rect.bottom = layout.graph.top - 4;
    return rect;
}

void ResponseGraph::invalidateInfoLine() const {
    if (window_ == nullptr) {
        return;
    }

    RECT infoRect = infoLineRect();
    InvalidateRect(window_, &infoRect, FALSE);
}

void ResponseGraph::notifyZoomChanged() const {
    if (window_ == nullptr) {
        return;
    }

    HWND parent = GetParent(window_);
    if (parent == nullptr) {
        return;
    }

    SendMessageW(parent,
                 WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(window_), kZoomChangedNotification),
                 reinterpret_cast<LPARAM>(window_));
}

void ResponseGraph::onLButtonDown(LPARAM lParam) {
    if (window_ == nullptr) {
        return;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return;
    }

    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout =
        buildGraphLayout(hdc, rect, data_, hasCustomVisibleFrequencyRange_, visibleMinFrequencyHz_, visibleMaxFrequencyHz_);
    ReleaseDC(window_, hdc);

    const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (hasCustomVisibleFrequencyRange_ && PtInRect(&layout.resetButton, position) != FALSE) {
        resetVisibleFrequencyRange();
        invalidate();
        notifyZoomChanged();
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

void ResponseGraph::onLButtonUp(LPARAM lParam) {
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

void ResponseGraph::onMouseMove(LPARAM lParam) {
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
    const GraphLayout layout =
        buildGraphLayout(hdc, rect, data_, hasCustomVisibleFrequencyRange_, visibleMinFrequencyHz_, visibleMaxFrequencyHz_);
    ReleaseDC(window_, hdc);

    const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const bool insideGraph = PtInRect(&layout.graph, position) != FALSE;
    const bool changed =
        hover_.active != insideGraph || hover_.position.x != position.x || hover_.position.y != position.y;
    hover_.active = insideGraph;
    hover_.position = position;
    if (changed && !brush_.active) {
        invalidateInfoLine();
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

void ResponseGraph::onMouseLeave() {
    const bool hadHover = hover_.active || hover_.tracking;
    hover_.active = false;
    hover_.tracking = false;
    if (hadHover && !brush_.active) {
        invalidateInfoLine();
    }
}

void ResponseGraph::onCaptureChanged() {
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
    const GraphLayout layout =
        buildGraphLayout(hdc, rect, data_, hasCustomVisibleFrequencyRange_, visibleMinFrequencyHz_, visibleMaxFrequencyHz_);
    ReleaseDC(window_, hdc);

    bool changed = false;
    if (hasBrushWidth(brush_.anchor, brush_.current) && !data_.frequencyAxisHz.empty()) {
        const double nextMinFrequencyHz = frequencyFromGraphX(layout.graph,
                                                              layout.visibleMinFrequencyHz,
                                                              layout.visibleMaxFrequencyHz,
                                                              std::min(brush_.anchor.x, brush_.current.x));
        const double nextMaxFrequencyHz = frequencyFromGraphX(layout.graph,
                                                              layout.visibleMinFrequencyHz,
                                                              layout.visibleMaxFrequencyHz,
                                                              std::max(brush_.anchor.x, brush_.current.x));
        if ((nextMaxFrequencyHz - nextMinFrequencyHz) >
            std::max((layout.visibleMaxFrequencyHz - layout.visibleMinFrequencyHz) * 0.001, 1.0e-3)) {
            hasCustomVisibleFrequencyRange_ = true;
            visibleMinFrequencyHz_ = nextMinFrequencyHz;
            visibleMaxFrequencyHz_ = nextMaxFrequencyHz;
            changed = true;
        }
    }

    brush_ = {};
    invalidate();
    if (changed) {
        notifyZoomChanged();
    }
}

void ResponseGraph::onLButtonDblClk(LPARAM lParam) {
    if (window_ == nullptr || !hasCustomVisibleFrequencyRange_) {
        return;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return;
    }

    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout =
        buildGraphLayout(hdc, rect, data_, hasCustomVisibleFrequencyRange_, visibleMinFrequencyHz_, visibleMaxFrequencyHz_);
    ReleaseDC(window_, hdc);

    const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (PtInRect(&layout.graph, position) == FALSE && PtInRect(&layout.resetButton, position) == FALSE) {
        return;
    }

    resetVisibleFrequencyRange();
    invalidate();
    notifyZoomChanged();
}

void ResponseGraph::onPaint() const {
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
    const GraphLayout layout =
        buildGraphLayout(hdc, rect, data_, hasCustomVisibleFrequencyRange_, visibleMinFrequencyHz_, visibleMaxFrequencyHz_);
    const RECT& graph = layout.graph;
    const std::vector<double> xTickValues =
        buildFrequencyTickValues(graph, layout.visibleMinFrequencyHz, layout.visibleMaxFrequencyHz);
    const std::vector<AxisLabel> xLabels =
        buildFrequencyLabels(hdc, graph, layout.visibleMinFrequencyHz, layout.visibleMaxFrequencyHz, xTickValues);

    const COLORREF stripeColor = RGB(244, 247, 251);
    HBRUSH stripeBrush = CreateSolidBrush(stripeColor);
    for (size_t i = 0; i + 1 < layout.yTickValues.size(); ++i) {
        if ((i % 2) != 0) {
            continue;
        }

        const int y0 = graphYFromDb(graph, layout.yTickValues[i], layout.axisMinDb, layout.axisMaxDb);
        const int y1 = graphYFromDb(graph, layout.yTickValues[i + 1], layout.axisMinDb, layout.axisMaxDb);
        RECT stripeRect{graph.left, std::min(y0, y1), graph.right, std::max(y0, y1)};
        FillRect(hdc, &stripeRect, stripeBrush);
    }
    DeleteObject(stripeBrush);

    RECT infoRect{graph.left, rect.top + 2, layout.resetButton.left - 8, graph.top - 4};
    if (brush_.active && hasBrushWidth(brush_.anchor, brush_.current)) {
        const double minFrequencyHz = frequencyFromGraphX(graph,
                                                          layout.visibleMinFrequencyHz,
                                                          layout.visibleMaxFrequencyHz,
                                                          std::min(brush_.anchor.x, brush_.current.x));
        const double maxFrequencyHz = frequencyFromGraphX(graph,
                                                          layout.visibleMinFrequencyHz,
                                                          layout.visibleMaxFrequencyHz,
                                                          std::max(brush_.anchor.x, brush_.current.x));
        const std::wstring infoText = formatBrushFrequencyRange(minFrequencyHz, maxFrequencyHz);
        SetTextColor(hdc, ui_theme::kAccent);
        DrawTextW(hdc, infoText.c_str(), -1, &infoRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (hover_.active) {
        const double frequencyHz =
            frequencyFromGraphX(graph, layout.visibleMinFrequencyHz, layout.visibleMaxFrequencyHz, hover_.position.x);
        const double amplitudeDb = dbFromGraphY(graph, hover_.position.y, layout.axisMinDb, layout.axisMaxDb);
        const double wavelengthMeters = 343.0 / std::max(frequencyHz, 1.0e-6);
        const std::wstring infoText = formatHoverAmplitude(amplitudeDb) + L" @ " + formatHoverFrequency(frequencyHz) +
                                      L"    " + formatHoverWavelength(wavelengthMeters) + L"    " +
                                      formatHoverQuarterWavelength(wavelengthMeters);
        SetTextColor(hdc, ui_theme::kAccent);
        DrawTextW(hdc, infoText.c_str(), -1, &infoRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    drawResetButton(hdc, layout.resetButton, hasCustomVisibleFrequencyRange_);

    SetDCPenColor(hdc, ui_theme::kBorder);
    Rectangle(hdc, graph.left, graph.top, graph.right, graph.bottom);

    for (const double tickHz : xTickValues) {
        const int x = graphXFromFrequency(graph,
                                          layout.visibleMinFrequencyHz,
                                          layout.visibleMaxFrequencyHz,
                                          tickHz);
        MoveToEx(hdc, x, graph.top, nullptr);
        LineTo(hdc, x, graph.bottom);
    }
    for (const double tickValue : layout.yTickValues) {
        const int y = graphYFromDb(graph, tickValue, layout.axisMinDb, layout.axisMaxDb);
        MoveToEx(hdc, graph.left, y, nullptr);
        LineTo(hdc, graph.right, y);
    }

    constexpr int kMajorTickLength = 6;
    for (const double tickValue : layout.yTickValues) {
        const int y = graphYFromDb(graph, tickValue, layout.axisMinDb, layout.axisMaxDb);
        MoveToEx(hdc, graph.left - kMajorTickLength, y, nullptr);
        LineTo(hdc, graph.left, y);
        MoveToEx(hdc, graph.right, y, nullptr);
        LineTo(hdc, graph.right + kMajorTickLength, y);
    }

    if (!data_.frequencyAxisHz.empty()) {
        for (const ResponseGraphSeries& series : data_.series) {
            drawSeries(hdc,
                       graph,
                       layout.axisMinDb,
                       layout.axisMaxDb,
                       layout.visibleMinFrequencyHz,
                       layout.visibleMaxFrequencyHz,
                       data_.frequencyAxisHz,
                       series.values,
                       series.color);
        }
    }

    if (brush_.active && hasBrushWidth(brush_.anchor, brush_.current)) {
        const RECT selection = brushRect(graph, brush_.anchor, brush_.current);
        HBRUSH selectionBrush = CreateSolidBrush(RGB(214, 227, 244));
        FillRect(hdc, &selection, selectionBrush);
        DeleteObject(selectionBrush);

        HPEN selectionPen = CreatePen(PS_SOLID, 1, ui_theme::kAccent);
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, selectionPen));
        HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(HOLLOW_BRUSH)));
        Rectangle(hdc, selection.left, selection.top, selection.right, selection.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(selectionPen);
    }

    SetTextColor(hdc, ui_theme::kMuted);
    for (const AxisLabel& label : xLabels) {
        RECT textRect{label.left, graph.bottom + 6, label.right + 2, rect.bottom};
        DrawTextW(hdc, label.text.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    for (const double tickValue : layout.yTickValues) {
        const int y = graphYFromDb(graph, tickValue, layout.axisMinDb, layout.axisMaxDb);
        RECT labelRect{rect.left + 4, y - layout.textHeight / 2 - 2, graph.left - 6, y + layout.textHeight / 2 + 2};
        const std::wstring label = formatDbTickLabel(tickValue);
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    EndPaint(window_, &paint);
}

}  // namespace wolfie::ui
