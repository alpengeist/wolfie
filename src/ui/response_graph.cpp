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
constexpr double kMinVisibleRangeDb = 0.05;
constexpr double kWheelZoomScalePerStep = 0.8;

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
    double baseAxisMaxDb = 1.0;
    double axisMinDb = 0.0;
    double axisMaxDb = 1.0;
    double dbStep = 1.0;
    double minVisibleRangeDb = 1.0;
    double defaultVisibleRangeDb = 1.0;
    double currentVisibleRangeDb = 1.0;
    std::vector<double> yTickValues;
};

bool isMajorFrequencyTick(double frequencyHz) {
    return frequencyHz == 10.0 || frequencyHz == 100.0 || frequencyHz == 1000.0 || frequencyHz == 10000.0 ||
           frequencyHz == kMaxFrequencyHz;
}

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double responseGraphXT(double frequencyHz) {
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

    const int decimals = 3;
    return formatWideDouble(valueDb, decimals);
}

int measureTextWidth(HDC hdc, const std::wstring& text) {
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

int graphXFromFrequency(const RECT& graph, double frequencyHz) {
    return graph.left + static_cast<int>(std::lround(responseGraphXT(frequencyHz) * (graph.right - graph.left)));
}

int graphYFromDb(const RECT& graph, double valueDb, double minDb, double maxDb) {
    const double range = std::max(maxDb - minDb, 1e-6);
    const double yT = clampValue((valueDb - minDb) / range, 0.0, 1.0);
    return graph.bottom - static_cast<int>(std::lround(yT * (graph.bottom - graph.top)));
}

int unclampedGraphYFromDb(const RECT& graph, double valueDb, double minDb, double maxDb) {
    const double range = std::max(maxDb - minDb, 1e-6);
    const double yT = (valueDb - minDb) / range;
    return graph.bottom - static_cast<int>(std::lround(yT * (graph.bottom - graph.top)));
}

double responseGraphFrequencyAtT(double xT) {
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

double frequencyFromGraphX(const RECT& graph, int x) {
    const int width = std::max(static_cast<int>(graph.right - graph.left), 1);
    const double xT =
        static_cast<double>(clampValue(static_cast<int>(x - graph.left), 0, width)) / static_cast<double>(width);
    return clampValue(responseGraphFrequencyAtT(xT), kMinFrequencyHz, kMaxFrequencyHz);
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

std::vector<double> buildFrequencyTickValues(const RECT& graph) {
    std::vector<double> candidates;
    for (double decade = 10.0; decade <= 1000.0; decade *= 10.0) {
        for (int multiplier = 1; multiplier <= 9; ++multiplier) {
            candidates.push_back(decade * static_cast<double>(multiplier));
        }
    }
    for (double frequencyHz = 10000.0; frequencyHz <= kMaxFrequencyHz; frequencyHz += 1000.0) {
        candidates.push_back(frequencyHz);
    }

    std::vector<double> ticks;
    const int graphWidth = std::max(static_cast<int>(graph.right - graph.left), 1);
    const int minPixelSpacing = clampValue(graphWidth / 60, 8, 12);
    int lastPixel = std::numeric_limits<int>::min() / 2;
    for (const double candidate : candidates) {
        const int pixel = graphXFromFrequency(graph, candidate);
        if (ticks.empty() || isMajorFrequencyTick(candidate) || pixel - lastPixel >= minPixelSpacing ||
            candidate >= kMaxFrequencyHz) {
            ticks.push_back(candidate);
            lastPixel = pixel;
        }
    }

    if (ticks.empty() || ticks.back() < kMaxFrequencyHz) {
        ticks.push_back(kMaxFrequencyHz);
    }
    return ticks;
}

std::vector<AxisLabel> buildFrequencyLabels(HDC hdc, const RECT& graph, const std::vector<double>& tickValues) {
    std::vector<AxisLabel> candidates;
    candidates.reserve(tickValues.size());
    for (const double tickValue : tickValues) {
        AxisLabel label;
        label.value = tickValue;
        label.pixel = graphXFromFrequency(graph, tickValue);
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

std::vector<double> buildDbTickValues(double minDb, double maxDb, double stepDb) {
    std::vector<double> ticks;
    if (stepDb <= 0.0 || maxDb < minDb) {
        return ticks;
    }

    const double rangeDb = maxDb - minDb;
    const int steps = std::max(static_cast<int>(std::floor((rangeDb / stepDb) + 0.001)), 0);
    ticks.reserve(static_cast<size_t>(steps + 1));
    for (int index = steps; index >= 0; --index) {
        ticks.push_back(maxDb - (stepDb * static_cast<double>(index)));
    }
    return ticks;
}

GraphLayout buildGraphLayout(HDC hdc,
                             const RECT& rect,
                             const ResponseGraphData& data,
                             double extraVisibleRangeDb,
                             double verticalOffsetDb) {
    GraphLayout layout;

    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    layout.textHeight = std::max(static_cast<int>(metrics.tmHeight), 12);

    const double dataMinDb = data.minDb;
    double dataMaxDb = dataMinDb + 1.0;
    for (const auto& series : data.series) {
        for (const double value : series.values) {
            dataMaxDb = std::max(dataMaxDb, value);
        }
    }
    dataMaxDb = std::ceil(dataMaxDb);
    layout.baseAxisMaxDb = dataMaxDb;

    const int infoLineHeight = layout.textHeight + 6;
    const int estimatedGraphHeight = std::max(static_cast<int>(rect.bottom - rect.top) - (layout.textHeight + 22) -
                                                  infoLineHeight,
                                              80);
    const double fullVisibleRangeDb = std::max(layout.baseAxisMaxDb - dataMinDb, 1.0);
    layout.defaultVisibleRangeDb = fullVisibleRangeDb;
    layout.minVisibleRangeDb = kMinVisibleRangeDb;
    layout.currentVisibleRangeDb = std::max(layout.defaultVisibleRangeDb + extraVisibleRangeDb, layout.minVisibleRangeDb);
    layout.axisMaxDb = layout.baseAxisMaxDb + verticalOffsetDb;
    layout.axisMinDb = layout.axisMaxDb - layout.currentVisibleRangeDb;
    const double rawDbStep = layout.currentVisibleRangeDb * static_cast<double>(layout.textHeight + 10) /
                             static_cast<double>(std::max(estimatedGraphHeight, 1));
    layout.dbStep = nextNiceStep(rawDbStep);
    layout.yTickValues = buildDbTickValues(layout.axisMinDb, layout.axisMaxDb, layout.dbStep);

    int widestYLabel = 0;
    for (const double tickValue : layout.yTickValues) {
        widestYLabel = std::max(widestYLabel, measureTextWidth(hdc, formatDbTickLabel(tickValue)));
    }

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

void drawSeries(HDC hdc,
                const RECT& graph,
                double axisMinDb,
                double axisMaxDb,
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
        const double xT = responseGraphXT(frequencyAxisHz[i]);
        const int x = graph.left + static_cast<int>(xT * (graph.right - graph.left));
        const int y = unclampedGraphYFromDb(graph, values[i], axisMinDb, axisMaxDb);
        if (i == 0) {
            MoveToEx(hdc, x, y, nullptr);
        } else {
            LineTo(hdc, x, y);
        }
    }
    RestoreDC(hdc, savedDc);
}

}  // namespace

void ResponseGraph::registerWindowClass(HINSTANCE instance) {
    WNDCLASSW graphClass{};
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
    invalidate();
}

void ResponseGraph::setExtraVisibleRangeDb(double extraVisibleRangeDb) {
    if (std::abs(extraVisibleRangeDb - extraVisibleRangeDb_) < 0.001) {
        return;
    }

    extraVisibleRangeDb_ = extraVisibleRangeDb;
    invalidate();
}

void ResponseGraph::setVerticalOffsetDb(double verticalOffsetDb) {
    if (std::abs(verticalOffsetDb - verticalOffsetDb_) < 0.001) {
        return;
    }

    verticalOffsetDb_ = verticalOffsetDb;
    invalidate();
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

    if (message == WM_PAINT) {
        graph->onPaint();
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_MOUSEWHEEL) {
        if (graph->onMouseWheel(wParam, lParam)) {
            return 0;
        }
    }
    if (message == WM_MOUSEMOVE) {
        graph->onMouseMove(lParam);
        return 0;
    }
    if (message == WM_MOUSELEAVE) {
        graph->onMouseLeave();
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
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
    const GraphLayout layout = buildGraphLayout(hdc, rect, data_, extraVisibleRangeDb_, verticalOffsetDb_);
    ReleaseDC(window_, hdc);

    rect.left = layout.graph.left;
    rect.top += 2;
    rect.right -= 8;
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

bool ResponseGraph::onMouseWheel(WPARAM wParam, LPARAM lParam) {
    if (window_ == nullptr) {
        return false;
    }

    HDC hdc = GetDC(window_);
    if (hdc == nullptr) {
        return false;
    }

    RECT rect{};
    GetClientRect(window_, &rect);
    const GraphLayout layout = buildGraphLayout(hdc, rect, data_, extraVisibleRangeDb_, verticalOffsetDb_);
    ReleaseDC(window_, hdc);

    POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(window_, &cursor);
    if (PtInRect(&layout.graph, cursor) == FALSE) {
        return false;
    }

    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    const int wheelSteps = delta / WHEEL_DELTA;
    if (wheelSteps == 0) {
        return true;
    }

    if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) {
        const double cursorDb = dbFromGraphY(layout.graph, cursor.y, layout.axisMinDb, layout.axisMaxDb);
        const double cursorT =
            (cursorDb - layout.axisMinDb) / std::max(layout.currentVisibleRangeDb, layout.minVisibleRangeDb);
        const double nextVisibleRangeDb = std::max(layout.currentVisibleRangeDb *
                                                       std::pow(kWheelZoomScalePerStep, static_cast<double>(wheelSteps)),
                                                   layout.minVisibleRangeDb);
        const double nextAxisMinDb = cursorDb - (cursorT * nextVisibleRangeDb);
        const double nextAxisMaxDb = nextAxisMinDb + nextVisibleRangeDb;
        const double nextExtraVisibleRangeDb = nextVisibleRangeDb - layout.defaultVisibleRangeDb;
        const double nextVerticalOffsetDb = nextAxisMaxDb - layout.baseAxisMaxDb;
        if (std::abs(nextExtraVisibleRangeDb - extraVisibleRangeDb_) < 0.001 &&
            std::abs(nextVerticalOffsetDb - verticalOffsetDb_) < 0.001) {
            return true;
        }

        extraVisibleRangeDb_ = nextExtraVisibleRangeDb;
        verticalOffsetDb_ = nextVerticalOffsetDb;
    } else {
        const double nextVerticalOffsetDb = verticalOffsetDb_ + (layout.dbStep * static_cast<double>(wheelSteps));
        if (std::abs(nextVerticalOffsetDb - verticalOffsetDb_) < 0.001) {
            return true;
        }

        verticalOffsetDb_ = nextVerticalOffsetDb;
    }

    invalidate();
    notifyZoomChanged();
    return true;
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
    const GraphLayout layout = buildGraphLayout(hdc, rect, data_, extraVisibleRangeDb_, verticalOffsetDb_);
    ReleaseDC(window_, hdc);

    const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const bool insideGraph = PtInRect(&layout.graph, position) != FALSE;
    const bool changed = hover_.active != insideGraph || hover_.position.x != position.x || hover_.position.y != position.y;
    hover_.active = insideGraph;
    hover_.position = position;
    if (changed) {
        invalidateInfoLine();
    }
}

void ResponseGraph::onMouseLeave() {
    const bool hadHover = hover_.active || hover_.tracking;
    hover_.active = false;
    hover_.tracking = false;
    if (hadHover) {
        invalidateInfoLine();
    }
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
    const GraphLayout layout = buildGraphLayout(hdc, rect, data_, extraVisibleRangeDb_, verticalOffsetDb_);
    const RECT& graph = layout.graph;
    const std::vector<double> xTickValues = buildFrequencyTickValues(graph);
    const std::vector<AxisLabel> xLabels = buildFrequencyLabels(hdc, graph, xTickValues);

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

    if (hover_.active) {
        const double frequencyHz = frequencyFromGraphX(graph, hover_.position.x);
        const double amplitudeDb = dbFromGraphY(graph, hover_.position.y, layout.axisMinDb, layout.axisMaxDb);
        const double wavelengthMeters = 343.0 / std::max(frequencyHz, 1e-6);
        const std::wstring infoText = formatHoverAmplitude(amplitudeDb) + L" @ " + formatHoverFrequency(frequencyHz) +
                                      L"    " + formatHoverWavelength(wavelengthMeters) + L"    " +
                                      formatHoverQuarterWavelength(wavelengthMeters);
        RECT infoRect{graph.left, rect.top + 2, rect.right - 8, graph.top - 4};
        SetTextColor(hdc, ui_theme::kAccent);
        DrawTextW(hdc, infoText.c_str(), -1, &infoRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SetDCPenColor(hdc, ui_theme::kBorder);
    Rectangle(hdc, graph.left, graph.top, graph.right, graph.bottom);

    for (const double tickHz : xTickValues) {
        const int x = graphXFromFrequency(graph, tickHz);
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
        for (const auto& series : data_.series) {
            drawSeries(hdc, graph, layout.axisMinDb, layout.axisMaxDb, data_.frequencyAxisHz, series.values, series.color);
        }
    }

    SetTextColor(hdc, ui_theme::kMuted);
    for (const AxisLabel& label : xLabels) {
        RECT textRect{label.left, graph.bottom + 6, label.right + 2, rect.bottom};
        DrawTextW(hdc, label.text.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    SetTextColor(hdc, ui_theme::kMuted);
    for (const double tickValue : layout.yTickValues) {
        const int y = graphYFromDb(graph, tickValue, layout.axisMinDb, layout.axisMaxDb);
        RECT labelRect{rect.left + 4, y - layout.textHeight / 2 - 2, graph.left - 6, y + layout.textHeight / 2 + 2};
        const std::wstring label = formatDbTickLabel(tickValue);
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    EndPaint(window_, &paint);
}

}  // namespace wolfie::ui
