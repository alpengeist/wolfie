#include "ui/target_curve_graph.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

#include <windowsx.h>

#include "core/text_utils.h"
#include "measurement/target_curve_designer.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr double kMinVisibleRangeDb = 2.0;
constexpr double kWheelZoomScalePerStep = 0.8;
constexpr int kHandleHalfSize = 5;

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

struct AxisLabel {
    double value = 0.0;
    int pixel = 0;
    int left = 0;
    int right = 0;
    std::wstring text;
};

struct GraphLayout {
    RECT graph{};
    int textHeight = 12;
    double axisMinDb = -12.0;
    double axisMaxDb = 12.0;
    double currentVisibleRangeDb = 24.0;
    double defaultVisibleRangeDb = 24.0;
    double minVisibleRangeDb = kMinVisibleRangeDb;
    double maxVisibleRangeDb = 48.0;
    double minFrequencyHz = 20.0;
    double maxFrequencyHz = 20000.0;
    std::vector<double> yTickValues;
};

int measureTextWidth(HDC hdc, const std::wstring& text) {
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

double logFrequency(double frequencyHz) {
    return std::log10(std::max(frequencyHz, 1e-6));
}

int graphXFromFrequency(const RECT& graph, double minFrequencyHz, double maxFrequencyHz, double frequencyHz) {
    const double minLog = logFrequency(minFrequencyHz);
    const double maxLog = logFrequency(maxFrequencyHz);
    const double t = clampValue((logFrequency(frequencyHz) - minLog) / std::max(maxLog - minLog, 1e-9), 0.0, 1.0);
    return graph.left + static_cast<int>(std::lround(t * (graph.right - graph.left)));
}

double frequencyFromGraphX(const RECT& graph, double minFrequencyHz, double maxFrequencyHz, int x) {
    const int width = std::max(static_cast<int>(graph.right - graph.left), 1);
    const double t = static_cast<double>(clampValue(static_cast<int>(x - graph.left), 0, width)) / static_cast<double>(width);
    const double minLog = logFrequency(minFrequencyHz);
    const double maxLog = logFrequency(maxFrequencyHz);
    return std::pow(10.0, minLog + ((maxLog - minLog) * t));
}

int graphYFromDb(const RECT& graph, double valueDb, double minDb, double maxDb) {
    const double range = std::max(maxDb - minDb, 1e-6);
    const double t = clampValue((valueDb - minDb) / range, 0.0, 1.0);
    return graph.bottom - static_cast<int>(std::lround(t * (graph.bottom - graph.top)));
}

double dbFromGraphY(const RECT& graph, int y, double minDb, double maxDb) {
    const int height = std::max(static_cast<int>(graph.bottom - graph.top), 1);
    const double t = static_cast<double>(clampValue(static_cast<int>(graph.bottom - y), 0, height)) / static_cast<double>(height);
    return minDb + ((maxDb - minDb) * t);
}

std::wstring formatFrequencyLabel(double frequencyHz) {
    if (frequencyHz >= 1000.0) {
        return formatWideDouble(frequencyHz / 1000.0, frequencyHz >= 10000.0 ? 0 : 1) + L"k";
    }
    return formatWideDouble(frequencyHz, 0);
}

std::wstring formatDbLabel(double valueDb) {
    const int decimals = std::abs(valueDb) < 10.0 ? 1 : 0;
    return formatWideDouble(valueDb, decimals);
}

std::vector<double> buildFrequencyTickValues(double minFrequencyHz, double maxFrequencyHz) {
    static const double candidates[] = {
        20.0, 30.0, 40.0, 50.0, 60.0, 80.0,
        100.0, 200.0, 300.0, 500.0, 800.0,
        1000.0, 2000.0, 3000.0, 5000.0, 8000.0,
        10000.0, 15000.0, 20000.0
    };

    std::vector<double> ticks;
    for (const double candidate : candidates) {
        if (candidate >= minFrequencyHz && candidate <= maxFrequencyHz) {
            ticks.push_back(candidate);
        }
    }
    if (ticks.empty() || ticks.front() > minFrequencyHz) {
        ticks.insert(ticks.begin(), minFrequencyHz);
    }
    if (ticks.back() < maxFrequencyHz) {
        ticks.push_back(maxFrequencyHz);
    }
    return ticks;
}

std::vector<AxisLabel> buildFrequencyLabels(HDC hdc,
                                            const RECT& graph,
                                            double minFrequencyHz,
                                            double maxFrequencyHz,
                                            const std::vector<double>& tickValues) {
    std::vector<AxisLabel> labels;
    int previousRight = std::numeric_limits<int>::min() / 2;
    for (const double tickValue : tickValues) {
        AxisLabel label;
        label.value = tickValue;
        label.pixel = graphXFromFrequency(graph, minFrequencyHz, maxFrequencyHz, tickValue);
        label.text = formatFrequencyLabel(tickValue);
        const int width = measureTextWidth(hdc, label.text);
        label.left = label.pixel - (width / 2);
        label.right = label.left + width;
        if (!labels.empty() && label.left - previousRight < 8) {
            continue;
        }
        previousRight = label.right;
        labels.push_back(label);
    }
    return labels;
}

std::vector<double> buildDbTickValues(double minDb, double maxDb) {
    const double range = std::max(maxDb - minDb, 1.0);
    double step = 1.0;
    if (range > 8.0) {
        step = 2.0;
    }
    if (range > 20.0) {
        step = 5.0;
    }
    if (range > 50.0) {
        step = 10.0;
    }

    std::vector<double> ticks;
    const double start = std::floor(minDb / step) * step;
    for (double tick = start; tick <= maxDb + 0.001; tick += step) {
        if (tick >= minDb - 0.001) {
            ticks.push_back(tick);
        }
    }
    return ticks;
}

GraphLayout buildLayout(HDC hdc, const RECT& rect, const TargetCurveGraph& graph, double extraVisibleRangeDb);

double sampleSeriesAtFrequency(const std::vector<double>& axis,
                               const std::vector<double>& values,
                               double frequencyHz) {
    if (axis.empty() || values.empty()) {
        return 0.0;
    }
    if (frequencyHz <= axis.front()) {
        return values.front();
    }
    if (frequencyHz >= axis.back()) {
        return values[std::min(values.size(), axis.size()) - 1];
    }

    const auto upper = std::lower_bound(axis.begin(), axis.end(), frequencyHz);
    const size_t upperIndex = static_cast<size_t>(std::distance(axis.begin(), upper));
    if (upperIndex == 0) {
        return values.front();
    }
    const size_t lowerIndex = upperIndex - 1;
    const double x0 = logFrequency(axis[lowerIndex]);
    const double x1 = logFrequency(axis[upperIndex]);
    const double x = logFrequency(frequencyHz);
    const double y0 = values[std::min(lowerIndex, values.size() - 1)];
    const double y1 = values[std::min(upperIndex, values.size() - 1)];
    const double t = clampValue((x - x0) / std::max(x1 - x0, 1e-9), 0.0, 1.0);
    return y0 + ((y1 - y0) * t);
}

POINT pointOnCurve(const RECT& graph,
                   double minFrequencyHz,
                   double maxFrequencyHz,
                   double axisMinDb,
                   double axisMaxDb,
                   double frequencyHz,
                   double valueDb) {
    return POINT{
        graphXFromFrequency(graph, minFrequencyHz, maxFrequencyHz, frequencyHz),
        graphYFromDb(graph, valueDb, axisMinDb, axisMaxDb)
    };
}

void drawSeries(HDC hdc,
                const RECT& graph,
                double minFrequencyHz,
                double maxFrequencyHz,
                double axisMinDb,
                double axisMaxDb,
                const std::vector<double>& axis,
                const std::vector<double>& values,
                COLORREF color,
                int penWidth) {
    if (axis.empty() || values.empty()) {
        return;
    }

    HPEN pen = CreatePen(PS_SOLID, penWidth, color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    for (size_t i = 0; i < axis.size() && i < values.size(); ++i) {
        const POINT point = pointOnCurve(graph, minFrequencyHz, maxFrequencyHz, axisMinDb, axisMaxDb, axis[i], values[i]);
        if (i == 0) {
            MoveToEx(hdc, point.x, point.y, nullptr);
        } else {
            LineTo(hdc, point.x, point.y);
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void drawRectHandle(HDC hdc, const POINT& center, COLORREF fill, bool selected) {
    RECT rect{center.x - kHandleHalfSize, center.y - kHandleHalfSize, center.x + kHandleHalfSize + 1, center.y + kHandleHalfSize + 1};
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, selected ? 2 : 1, selected ? ui_theme::kText : ui_theme::kBorder);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(HOLLOW_BRUSH)));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void drawRoundHandle(HDC hdc, const POINT& center, COLORREF fill, bool selected) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, selected ? 2 : 1, selected ? ui_theme::kText : ui_theme::kBorder);
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    Ellipse(hdc,
            center.x - kHandleHalfSize,
            center.y - kHandleHalfSize,
            center.x + kHandleHalfSize + 1,
            center.y + kHandleHalfSize + 1);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

bool pointHitsHandle(const POINT& position, const POINT& handle) {
    return position.x >= handle.x - (kHandleHalfSize + 3) &&
           position.x <= handle.x + (kHandleHalfSize + 3) &&
           position.y >= handle.y - (kHandleHalfSize + 3) &&
           position.y <= handle.y + (kHandleHalfSize + 3);
}

GraphLayout buildLayout(HDC hdc, const RECT& rect, const TargetCurveGraph& graph, double extraVisibleRangeDb) {
    GraphLayout layout;

    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    layout.textHeight = std::max(static_cast<int>(metrics.tmHeight), 12);

    double minDb = std::numeric_limits<double>::max();
    double maxDb = std::numeric_limits<double>::lowest();
    auto scan = [&](const std::vector<double>& values) {
        for (const double value : values) {
            minDb = std::min(minDb, value);
            maxDb = std::max(maxDb, value);
        }
    };

    const measurement::TargetCurvePlotData& plotData = graph.plot();
    if (!graph.response().frequencyAxisHz.empty()) {
        scan(graph.response().leftChannelDb);
        scan(graph.response().rightChannelDb);
    } else {
        scan(plotData.basicCurveDb);
        scan(plotData.targetCurveDb);
    }

    if (minDb == std::numeric_limits<double>::max() || maxDb == std::numeric_limits<double>::lowest()) {
        minDb = -6.0;
        maxDb = 6.0;
    }

    minDb = std::floor(minDb - 2.0);
    maxDb = std::ceil(maxDb + 2.0);
    layout.axisMaxDb = maxDb;
    layout.defaultVisibleRangeDb = std::max(layout.axisMaxDb - minDb, 6.0);
    layout.currentVisibleRangeDb = clampValue(layout.defaultVisibleRangeDb + extraVisibleRangeDb,
                                              layout.minVisibleRangeDb,
                                              layout.defaultVisibleRangeDb + 60.0);
    layout.maxVisibleRangeDb = layout.defaultVisibleRangeDb + 60.0;
    layout.axisMinDb = layout.axisMaxDb - layout.currentVisibleRangeDb;
    layout.minFrequencyHz = plotData.minFrequencyHz;
    layout.maxFrequencyHz = plotData.maxFrequencyHz;
    layout.yTickValues = buildDbTickValues(layout.axisMinDb, layout.axisMaxDb);

    int widestYLabel = 0;
    for (const double tickValue : layout.yTickValues) {
        widestYLabel = std::max(widestYLabel, measureTextWidth(hdc, formatDbLabel(tickValue)));
    }

    layout.graph = RECT{
        rect.left + std::max(50, widestYLabel + 14),
        rect.top + layout.textHeight + 10,
        rect.right - 10,
        rect.bottom - (layout.textHeight + 18)
    };
    return layout;
}

}  // namespace

COLORREF targetCurveBandColor(int colorIndex) {
    static const COLORREF palette[] = {
        ui_theme::kOrange,
        ui_theme::kTeal,
        ui_theme::kGold,
        ui_theme::kMagenta,
        ui_theme::kGreen,
        ui_theme::kRed
    };
    const int count = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    const int index = colorIndex >= 0 ? colorIndex % count : ((colorIndex % count) + count) % count;
    return palette[index];
}

void TargetCurveGraph::registerWindowClass(HINSTANCE instance) {
    WNDCLASSW graphClass{};
    graphClass.lpfnWndProc = WindowProc;
    graphClass.hInstance = instance;
    graphClass.lpszClassName = kWindowClassName;
    graphClass.hCursor = LoadCursor(nullptr, IDC_CROSS);
    graphClass.hbrBackground = CreateSolidBrush(RGB(248, 250, 252));
    RegisterClassW(&graphClass);
}

void TargetCurveGraph::create(HWND parent, HINSTANCE instance, int controlId) {
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

void TargetCurveGraph::setModel(const SmoothedResponse& response,
                                const MeasurementSettings& measurement,
                                const TargetCurveSettings& settings,
                                int selectedBandIndex) {
    const bool responseChanged = response_.frequencyAxisHz != response.frequencyAxisHz ||
                                 response_.leftChannelDb != response.leftChannelDb ||
                                 response_.rightChannelDb != response.rightChannelDb;
    const bool boundsChanged = std::abs(measurement_.startFrequencyHz - measurement.startFrequencyHz) > 0.001 ||
                               std::abs(measurement_.endFrequencyHz - measurement.endFrequencyHz) > 0.001;
    response_ = response;
    measurement_ = measurement;
    settings_ = settings;
    selectedBandIndex_ = selectedBandIndex;
    rebuildPlot();
    if (responseChanged || boundsChanged) {
        invalidateBackgroundCache();
    }
    invalidate();
}

void TargetCurveGraph::setExtraVisibleRangeDb(double extraVisibleRangeDb) {
    if (std::abs(extraVisibleRangeDb_ - extraVisibleRangeDb) < 0.001) {
        return;
    }
    extraVisibleRangeDb_ = extraVisibleRangeDb;
    invalidateBackgroundCache();
    invalidate();
}

void TargetCurveGraph::layout(const RECT& bounds) const {
    if (window_ != nullptr) {
        MoveWindow(window_, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, TRUE);
    }
}

void TargetCurveGraph::invalidate() const {
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

void TargetCurveGraph::invalidateBackgroundCache() const {
    backgroundCacheValid_ = false;
}

void TargetCurveGraph::releaseBackgroundCache() const {
    if (backgroundCacheBitmap_ != nullptr) {
        DeleteObject(backgroundCacheBitmap_);
        backgroundCacheBitmap_ = nullptr;
    }
    backgroundCacheSize_ = {};
    backgroundCacheValid_ = false;
}

RECT TargetCurveGraph::infoLineRect() const {
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
    const GraphLayout layout = buildLayout(hdc, rect, *this, extraVisibleRangeDb_);
    ReleaseDC(window_, hdc);

    rect.left = layout.graph.left;
    rect.top += 2;
    rect.right -= 8;
    rect.bottom = layout.graph.top - 4;
    return rect;
}

void TargetCurveGraph::invalidateInfoLine() const {
    if (window_ == nullptr) {
        return;
    }

    RECT rect = infoLineRect();
    InvalidateRect(window_, &rect, FALSE);
}

void TargetCurveGraph::notifyParent(WORD code) const {
    if (window_ == nullptr) {
        return;
    }
    HWND parent = GetParent(window_);
    if (parent == nullptr) {
        return;
    }
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window_), code), reinterpret_cast<LPARAM>(window_));
}

void TargetCurveGraph::drawStaticLayer(HDC hdc, const RECT& rect, const RECT& paintRect) const {
    HBRUSH background = CreateSolidBrush(RGB(248, 250, 252));
    FillRect(hdc, &paintRect, background);
    DeleteObject(background);

    SetBkMode(hdc, TRANSPARENT);
    const GraphLayout layout = buildLayout(hdc, rect, *this, extraVisibleRangeDb_);
    const std::vector<double> xTicks = buildFrequencyTickValues(plot_.minFrequencyHz, plot_.maxFrequencyHz);
    const std::vector<AxisLabel> xLabels = buildFrequencyLabels(hdc, layout.graph, plot_.minFrequencyHz, plot_.maxFrequencyHz, xTicks);

    HBRUSH stripeBrush = CreateSolidBrush(RGB(244, 247, 251));
    for (size_t i = 0; i + 1 < layout.yTickValues.size(); ++i) {
        if ((i % 2) != 0) {
            continue;
        }
        const int y0 = graphYFromDb(layout.graph, layout.yTickValues[i], layout.axisMinDb, layout.axisMaxDb);
        const int y1 = graphYFromDb(layout.graph, layout.yTickValues[i + 1], layout.axisMinDb, layout.axisMaxDb);
        RECT stripe{layout.graph.left, std::min(y0, y1), layout.graph.right, std::max(y0, y1)};
        FillRect(hdc, &stripe, stripeBrush);
    }
    DeleteObject(stripeBrush);

    HPEN gridPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, gridPen));
    for (const double tickHz : xTicks) {
        const int x = graphXFromFrequency(layout.graph, plot_.minFrequencyHz, plot_.maxFrequencyHz, tickHz);
        MoveToEx(hdc, x, layout.graph.top, nullptr);
        LineTo(hdc, x, layout.graph.bottom);
    }
    for (const double tickDb : layout.yTickValues) {
        const int y = graphYFromDb(layout.graph, tickDb, layout.axisMinDb, layout.axisMaxDb);
        MoveToEx(hdc, layout.graph.left, y, nullptr);
        LineTo(hdc, layout.graph.right, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);

    if (!response_.frequencyAxisHz.empty()) {
        drawSeries(hdc,
                   layout.graph,
                   plot_.minFrequencyHz,
                   plot_.maxFrequencyHz,
                   layout.axisMinDb,
                   layout.axisMaxDb,
                   response_.frequencyAxisHz,
                   response_.leftChannelDb,
                   RGB(112, 146, 122),
                   1);
        drawSeries(hdc,
                   layout.graph,
                   plot_.minFrequencyHz,
                   plot_.maxFrequencyHz,
                   layout.axisMinDb,
                   layout.axisMaxDb,
                   response_.frequencyAxisHz,
                   response_.rightChannelDb,
                   RGB(160, 112, 112),
                   1);
    }

    HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kText);
    oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, borderPen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(HOLLOW_BRUSH)));
    Rectangle(hdc, layout.graph.left, layout.graph.top, layout.graph.right, layout.graph.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    SetTextColor(hdc, ui_theme::kMuted);
    for (const AxisLabel& label : xLabels) {
        RECT textRect{label.left, layout.graph.bottom + 6, label.right + 2, rect.bottom};
        DrawTextW(hdc, label.text.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    for (const double tickDb : layout.yTickValues) {
        const int y = graphYFromDb(layout.graph, tickDb, layout.axisMinDb, layout.axisMaxDb);
        RECT labelRect{rect.left + 4, y - layout.textHeight / 2 - 2, layout.graph.left - 6, y + layout.textHeight / 2 + 2};
        const std::wstring text = formatDbLabel(tickDb);
        DrawTextW(hdc, text.c_str(), -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void TargetCurveGraph::rebuildPlot() {
    plot_ = measurement::buildTargetCurvePlotData(response_,
                                                  measurement_,
                                                  settings_,
                                                  selectedBandIndex_ >= 0
                                                      ? std::optional<size_t>(static_cast<size_t>(selectedBandIndex_))
                                                      : std::nullopt);
    measurement::normalizeTargetCurveSettings(settings_, plot_.minFrequencyHz, plot_.maxFrequencyHz);
    if (selectedBandIndex_ >= static_cast<int>(settings_.eqBands.size())) {
        selectedBandIndex_ = settings_.eqBands.empty() ? -1 : static_cast<int>(settings_.eqBands.size() - 1);
    }
}

LRESULT CALLBACK TargetCurveGraph::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    TargetCurveGraph* graph = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        graph = reinterpret_cast<TargetCurveGraph*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(graph));
        if (graph != nullptr) {
            graph->window_ = window;
        }
        return TRUE;
    }

    graph = reinterpret_cast<TargetCurveGraph*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (graph == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_PAINT:
        graph->onPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL:
        if (graph->onMouseWheel(wParam, lParam)) {
            return 0;
        }
        break;
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
        graph->onLButtonUp();
        return 0;
    case WM_CAPTURECHANGED:
        graph->finishDrag();
        return 0;
    case WM_SIZE:
        graph->invalidateBackgroundCache();
        return 0;
    case WM_NCDESTROY:
        graph->releaseBackgroundCache();
        break;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool TargetCurveGraph::onMouseWheel(WPARAM wParam, LPARAM lParam) {
    if (window_ == nullptr || (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) == 0) {
        return false;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return false;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc, rect, *this, extraVisibleRangeDb_);
    ReleaseDC(window_, hdc);

    POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(window_, &cursor);
    if (PtInRect(&layout.graph, cursor) == FALSE) {
        return false;
    }

    const int wheelSteps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
    if (wheelSteps == 0) {
        return true;
    }

    double nextVisibleRangeDb = layout.currentVisibleRangeDb * std::pow(kWheelZoomScalePerStep, static_cast<double>(wheelSteps));
    nextVisibleRangeDb = clampValue(nextVisibleRangeDb, layout.minVisibleRangeDb, layout.maxVisibleRangeDb);
    const double nextExtra = nextVisibleRangeDb - layout.defaultVisibleRangeDb;
    if (std::abs(nextExtra - extraVisibleRangeDb_) < 0.001) {
        return true;
    }

    extraVisibleRangeDb_ = nextExtra;
    invalidateBackgroundCache();
    invalidate();
    notifyParent(kZoomChangedNotification);
    return true;
}

void TargetCurveGraph::onMouseMove(LPARAM lParam) {
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

    const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc, rect, *this, extraVisibleRangeDb_);
    ReleaseDC(window_, hdc);

    const bool insideGraph = PtInRect(&layout.graph, position) != FALSE;
    const bool changed = hover_.active != insideGraph || hover_.position.x != position.x || hover_.position.y != position.y;
    hover_.active = insideGraph;
    hover_.position = position;

    if (drag_.active) {
        updateDrag(position);
    } else if (changed) {
        invalidateInfoLine();
    }
}

void TargetCurveGraph::onMouseLeave() {
    const bool hadHover = hover_.active;
    hover_.active = false;
    hover_.tracking = false;
    if (hadHover) {
        invalidateInfoLine();
    }
}

void TargetCurveGraph::onLButtonDown(LPARAM lParam) {
    if (window_ == nullptr) {
        return;
    }

    const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    DragHandleType type = DragHandleType::None;
    const int bandIndex = hitTestHandle(position, type);
    if (type == DragHandleType::None) {
        return;
    }

    if (type == DragHandleType::EqBand && bandIndex != selectedBandIndex_) {
        selectedBandIndex_ = bandIndex;
        rebuildPlot();
        notifyParent(kSelectionChangedNotification);
    }

    drag_.active = true;
    drag_.type = type;
    drag_.bandIndex = bandIndex;
    drag_.origin = position;
    drag_.originalSettings = settings_;
    SetCapture(window_);
    invalidate();
}

void TargetCurveGraph::onLButtonUp() {
    if (window_ != nullptr && GetCapture() == window_) {
        ReleaseCapture();
    }
    finishDrag();
}

void TargetCurveGraph::updateDrag(const POINT& position) {
    if (!drag_.active || window_ == nullptr) {
        return;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc, rect, *this, extraVisibleRangeDb_);
    ReleaseDC(window_, hdc);

    const double currentDb = dbFromGraphY(layout.graph, position.y, layout.axisMinDb, layout.axisMaxDb);
    const double originDb = dbFromGraphY(layout.graph, drag_.origin.y, layout.axisMinDb, layout.axisMaxDb);
    const double deltaDb = currentDb - originDb;

    settings_ = drag_.originalSettings;
    switch (drag_.type) {
    case DragHandleType::BasicLow:
        settings_.lowGainDb += deltaDb;
        settings_.midGainDb += deltaDb;
        settings_.highGainDb += deltaDb;
        break;
    case DragHandleType::BasicMid:
        settings_.midGainDb = currentDb;
        settings_.midFrequencyHz = frequencyFromGraphX(layout.graph, plot_.minFrequencyHz, plot_.maxFrequencyHz, position.x);
        break;
    case DragHandleType::BasicHigh:
        settings_.highGainDb = currentDb;
        break;
    case DragHandleType::EqBand:
        if (drag_.bandIndex >= 0 && drag_.bandIndex < static_cast<int>(settings_.eqBands.size())) {
            TargetEqBand& band = settings_.eqBands[drag_.bandIndex];
            band.frequencyHz = frequencyFromGraphX(layout.graph, plot_.minFrequencyHz, plot_.maxFrequencyHz, position.x);
            band.gainDb += deltaDb;
        }
        break;
    case DragHandleType::None:
        return;
    }

    rebuildPlot();
    invalidate();
    notifyParent(kModelChangedNotification);
}

void TargetCurveGraph::finishDrag() {
    drag_ = {};
}

int TargetCurveGraph::hitTestHandle(const POINT& position, DragHandleType& type) const {
    type = DragHandleType::None;
    if (window_ == nullptr || plot_.frequencyAxisHz.empty()) {
        return -1;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return -1;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc, rect, *this, extraVisibleRangeDb_);
    ReleaseDC(window_, hdc);

    const POINT lowPoint = pointOnCurve(layout.graph,
                                        plot_.minFrequencyHz,
                                        plot_.maxFrequencyHz,
                                        layout.axisMinDb,
                                        layout.axisMaxDb,
                                        plot_.minFrequencyHz,
                                        settings_.lowGainDb);
    if (pointHitsHandle(position, lowPoint)) {
        type = DragHandleType::BasicLow;
        return -1;
    }

    const POINT midPoint = pointOnCurve(layout.graph,
                                        plot_.minFrequencyHz,
                                        plot_.maxFrequencyHz,
                                        layout.axisMinDb,
                                        layout.axisMaxDb,
                                        settings_.midFrequencyHz,
                                        settings_.midGainDb);
    if (pointHitsHandle(position, midPoint)) {
        type = DragHandleType::BasicMid;
        return -1;
    }

    const POINT highPoint = pointOnCurve(layout.graph,
                                         plot_.minFrequencyHz,
                                         plot_.maxFrequencyHz,
                                         layout.axisMinDb,
                                         layout.axisMaxDb,
                                         plot_.maxFrequencyHz,
                                         settings_.highGainDb);
    if (pointHitsHandle(position, highPoint)) {
        type = DragHandleType::BasicHigh;
        return -1;
    }

    for (int i = static_cast<int>(settings_.eqBands.size()) - 1; i >= 0; --i) {
        const TargetEqBand& band = settings_.eqBands[static_cast<size_t>(i)];
        if (!band.enabled) {
            continue;
        }
        const double handleDb = sampleSeriesAtFrequency(plot_.frequencyAxisHz, plot_.targetCurveDb, band.frequencyHz);
        const POINT point = pointOnCurve(layout.graph,
                                         plot_.minFrequencyHz,
                                         plot_.maxFrequencyHz,
                                         layout.axisMinDb,
                                         layout.axisMaxDb,
                                         band.frequencyHz,
                                         handleDb);
        if (pointHitsHandle(position, point)) {
            type = DragHandleType::EqBand;
            return i;
        }
    }

    return -1;
}

void TargetCurveGraph::onPaint() const {
    PAINTSTRUCT paint{};
    HDC hdc = BeginPaint(window_, &paint);

    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildLayout(hdc, rect, *this, extraVisibleRangeDb_);
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
        backgroundCacheBitmap_ = CreateCompatibleBitmap(hdc, std::max(rect.right - rect.left, 1L), std::max(rect.bottom - rect.top, 1L));
        backgroundCacheSize_.cx = rect.right - rect.left;
        backgroundCacheSize_.cy = rect.bottom - rect.top;
        backgroundCacheValid_ = false;
    }
    HBITMAP oldCacheBitmap = reinterpret_cast<HBITMAP>(SelectObject(cacheSource, backgroundCacheBitmap_));
    if (!backgroundCacheValid_) {
        drawStaticLayer(cacheSource, rect, rect);
        backgroundCacheValid_ = true;
    }

    HDC frameDc = CreateCompatibleDC(hdc);
    HBITMAP frameBitmap = CreateCompatibleBitmap(hdc, std::max(rect.right - rect.left, 1L), std::max(rect.bottom - rect.top, 1L));
    HBITMAP oldFrameBitmap = reinterpret_cast<HBITMAP>(SelectObject(frameDc, frameBitmap));
    BitBlt(frameDc, 0, 0, rect.right - rect.left, rect.bottom - rect.top, cacheSource, 0, 0, SRCCOPY);
    SetBkMode(frameDc, TRANSPARENT);

    drawSeries(frameDc,
               layout.graph,
               plot_.minFrequencyHz,
               plot_.maxFrequencyHz,
               layout.axisMinDb,
               layout.axisMaxDb,
               plot_.frequencyAxisHz,
               plot_.targetCurveDb,
               ui_theme::kAccent,
               2);

    const POINT lowPoint = pointOnCurve(layout.graph,
                                        plot_.minFrequencyHz,
                                        plot_.maxFrequencyHz,
                                        layout.axisMinDb,
                                        layout.axisMaxDb,
                                        plot_.minFrequencyHz,
                                        settings_.lowGainDb);
    const POINT midPoint = pointOnCurve(layout.graph,
                                        plot_.minFrequencyHz,
                                        plot_.maxFrequencyHz,
                                        layout.axisMinDb,
                                        layout.axisMaxDb,
                                        settings_.midFrequencyHz,
                                        settings_.midGainDb);
    const POINT highPoint = pointOnCurve(layout.graph,
                                         plot_.minFrequencyHz,
                                         plot_.maxFrequencyHz,
                                         layout.axisMinDb,
                                         layout.axisMaxDb,
                                         plot_.maxFrequencyHz,
                                         settings_.highGainDb);
    drawRectHandle(frameDc, lowPoint, RGB(92, 136, 196), drag_.active && drag_.type == DragHandleType::BasicLow);
    drawRectHandle(frameDc, midPoint, RGB(92, 136, 196), drag_.active && drag_.type == DragHandleType::BasicMid);
    drawRectHandle(frameDc, highPoint, RGB(92, 136, 196), drag_.active && drag_.type == DragHandleType::BasicHigh);

    for (size_t i = 0; i < settings_.eqBands.size(); ++i) {
        const TargetEqBand& band = settings_.eqBands[i];
        if (!band.enabled) {
            continue;
        }
        const double handleDb = sampleSeriesAtFrequency(plot_.frequencyAxisHz, plot_.targetCurveDb, band.frequencyHz);
        const POINT point = pointOnCurve(layout.graph,
                                         plot_.minFrequencyHz,
                                         plot_.maxFrequencyHz,
                                         layout.axisMinDb,
                                         layout.axisMaxDb,
                                         band.frequencyHz,
                                         handleDb);
        drawRoundHandle(frameDc,
                        point,
                        targetCurveBandColor(band.colorIndex),
                        static_cast<int>(i) == selectedBandIndex_);
    }

    if (hover_.active && PtInRect(&layout.graph, hover_.position) != FALSE) {
        const double frequencyHz = frequencyFromGraphX(layout.graph, plot_.minFrequencyHz, plot_.maxFrequencyHz, hover_.position.x);
        const double db = dbFromGraphY(layout.graph, hover_.position.y, layout.axisMinDb, layout.axisMaxDb);
        const std::wstring info = formatWideDouble(db, 1) + L" dB @ " + formatWideDouble(frequencyHz, frequencyHz < 100.0 ? 1 : 0) + L" Hz";
        RECT infoRect{layout.graph.left, rect.top + 2, rect.right - 8, layout.graph.top - 4};
        SetTextColor(frameDc, ui_theme::kAccent);
        DrawTextW(frameDc, info.c_str(), -1, &infoRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
