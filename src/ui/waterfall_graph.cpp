#include "ui/waterfall_graph.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

constexpr double kMinFrequencyHz = 20.0;
constexpr double kMaxFrequencyHz = 20000.0;

double graphXT(double frequencyHz) {
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

int graphXFromFrequency(const RECT& graph, double frequencyHz) {
    return graph.left + static_cast<int>(std::lround(graphXT(frequencyHz) * (graph.right - graph.left)));
}

int graphYFromDb(const RECT& graph, double valueDb, double minDb, double maxDb) {
    const double range = std::max(maxDb - minDb, 1.0e-6);
    const double yT = clampValue((valueDb - minDb) / range, 0.0, 1.0);
    return graph.bottom - static_cast<int>(std::lround(yT * (graph.bottom - graph.top)));
}

COLORREF blendTowardWhite(COLORREF color, double factor) {
    factor = clampValue(factor, 0.0, 1.0);
    const auto blend = [factor](int component) {
        return static_cast<int>(std::lround((static_cast<double>(component) * (1.0 - factor)) + (255.0 * factor)));
    };
    return RGB(blend(GetRValue(color)), blend(GetGValue(color)), blend(GetBValue(color)));
}

COLORREF blendColors(COLORREF from, COLORREF to, double t) {
    t = clampValue(t, 0.0, 1.0);
    const auto blend = [t](int a, int b) {
        return static_cast<int>(std::lround((static_cast<double>(a) * (1.0 - t)) + (static_cast<double>(b) * t)));
    };
    return RGB(blend(GetRValue(from), GetRValue(to)),
               blend(GetGValue(from), GetGValue(to)),
               blend(GetBValue(from), GetBValue(to)));
}

std::wstring formatFrequencyLabel(double frequencyHz) {
    if (frequencyHz >= 1000.0) {
        return std::to_wstring(static_cast<int>(std::lround(frequencyHz / 1000.0))) + L"k";
    }
    return std::to_wstring(static_cast<int>(std::lround(frequencyHz)));
}

std::wstring formatDbLabel(double valueDb) {
    return formatWideDouble(valueDb, 0);
}

int measureTextWidth(HDC hdc, const std::wstring& text) {
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

std::vector<double> majorFrequencyTicks() {
    return {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0};
}

std::vector<double> yTicks(double minDb, double maxDb) {
    std::vector<double> ticks;
    for (double value = minDb; value <= maxDb + 0.001; value += 12.0) {
        ticks.push_back(value);
    }
    if (ticks.empty() || std::abs(ticks.back() - maxDb) > 0.5) {
        ticks.push_back(maxDb);
    }
    return ticks;
}

void drawAxisUnitLabels(HDC hdc,
                        const RECT& rect,
                        const RECT& graph,
                        int backRight,
                        int backBottom,
                        int textHeight) {
    SetTextColor(hdc, ui_theme::kMuted);
    const int yUnitWidth = measureTextWidth(hdc, L"dB") + 10;

    RECT yUnitRect{
        rect.left + 4,
        graph.top + 2,
        rect.left + yUnitWidth,
        graph.top + textHeight + 4,
    };
    DrawTextW(hdc, L"dB", -1, &yUnitRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    const int xTickBottom = graph.bottom + textHeight + 8;
    RECT xUnitRect{
        graph.left,
        xTickBottom,
        graph.right,
        rect.bottom - 2,
    };
    DrawTextW(hdc, L"Hz", -1, &xUnitRect, DT_CENTER | DT_TOP | DT_SINGLELINE);

    const int depthLabelCenterX = graph.right + ((backRight - graph.right) / 2);
    const int depthLabelCenterY = graph.bottom + ((backBottom - graph.bottom) / 2);
    RECT zUnitRect{
        depthLabelCenterX - 16,
        depthLabelCenterY - (textHeight / 2),
        depthLabelCenterX + 16,
        depthLabelCenterY + (textHeight / 2) + 2,
    };
    DrawTextW(hdc, L"ms", -1, &zUnitRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

}  // namespace

void WaterfallGraph::registerWindowClass(HINSTANCE instance) {
    WNDCLASSW graphClass{};
    graphClass.lpfnWndProc = WindowProc;
    graphClass.hInstance = instance;
    graphClass.lpszClassName = kWindowClassName;
    graphClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    graphClass.hbrBackground = CreateSolidBrush(RGB(248, 250, 252));
    RegisterClassW(&graphClass);
}

void WaterfallGraph::create(HWND parent, HINSTANCE instance, int controlId) {
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

void WaterfallGraph::setData(measurement::WaterfallPlotData data) {
    data_ = std::move(data);
    invalidate();
}

void WaterfallGraph::layout(const RECT& bounds) const {
    if (window_ != nullptr) {
        MoveWindow(window_, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, TRUE);
    }
}

void WaterfallGraph::invalidate() const {
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, TRUE);
    }
}

LRESULT CALLBACK WaterfallGraph::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    WaterfallGraph* graph = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        graph = reinterpret_cast<WaterfallGraph*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(graph));
        if (graph != nullptr) {
            graph->window_ = window;
        }
        return TRUE;
    }

    graph = reinterpret_cast<WaterfallGraph*>(GetWindowLongPtrW(window, GWLP_USERDATA));
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

    return DefWindowProcW(window, message, wParam, lParam);
}

void WaterfallGraph::onPaint() const {
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

    if (!data_.valid()) {
        SetTextColor(hdc, ui_theme::kMuted);
        DrawTextW(hdc,
                  L"No waterfall data available for this measurement.",
                  -1,
                  &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window_, &paint);
        return;
    }

    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    const int textHeight = std::max(static_cast<int>(metrics.tmHeight), 12);
    const int yUnitWidth = measureTextWidth(hdc, L"dB") + 10;
    const int depthCount = static_cast<int>(data_.slices.size()) - 1;
    const int elevation = clampValue<int>(static_cast<int>((rect.bottom - rect.top) / 5), 42, 86);
    RECT graph{
        rect.left + std::max(54, yUnitWidth + 54),
        rect.top + elevation + 22,
        rect.right - 54,
        rect.bottom - ((textHeight * 2) + 16)
    };

    if (graph.right - graph.left < 180 || graph.bottom - graph.top < 100) {
        SetTextColor(hdc, ui_theme::kMuted);
        DrawTextW(hdc, L"Resize to view waterfall.", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window_, &paint);
        return;
    }

    const std::vector<double> xTicks = majorFrequencyTicks();
    const std::vector<double> tickValues = yTicks(data_.minDb, data_.maxDb);
    constexpr double kYawDegrees = 20.0;
    const int depthDx =
        std::max(8, static_cast<int>(std::lround(static_cast<double>(elevation) * std::tan(kYawDegrees * 3.14159265358979323846 / 180.0))));
    const auto projectX = [&](int x, double depthT) {
        return x + static_cast<int>(std::lround(depthT * static_cast<double>(depthDx)));
    };
    const auto projectY = [&](int y, double depthT) {
        return y - static_cast<int>(std::lround(depthT * static_cast<double>(elevation)));
    };

    const int backLeft = projectX(graph.left, 1.0);
    const int backRight = projectX(graph.right, 1.0);
    const int backTop = projectY(graph.top, 1.0);
    const int backBottom = projectY(graph.bottom, 1.0);

    SetDCPenColor(hdc, blendTowardWhite(ui_theme::kBorder, 0.12));
    for (const double tick : tickValues) {
        const int yFront = graphYFromDb(graph, tick, data_.minDb, data_.maxDb);
        const int yBack = projectY(yFront, 1.0);
        MoveToEx(hdc, backLeft, yBack, nullptr);
        LineTo(hdc, backRight, yBack);
        MoveToEx(hdc, graph.left, yFront, nullptr);
        LineTo(hdc, backLeft, yBack);
        MoveToEx(hdc, graph.right, yFront, nullptr);
        LineTo(hdc, backRight, yBack);
    }
    for (const double frequencyHz : xTicks) {
        const int xFront = graphXFromFrequency(graph, frequencyHz);
        const int xBack = projectX(xFront, 1.0);
        MoveToEx(hdc, xBack, backTop, nullptr);
        LineTo(hdc, xBack, backBottom);
        MoveToEx(hdc, xFront, graph.bottom, nullptr);
        LineTo(hdc, xBack, backBottom);
    }
    SetDCPenColor(hdc, ui_theme::kBorder);
    Rectangle(hdc, backLeft, backTop, backRight, backBottom);
    MoveToEx(hdc, graph.left, graph.top, nullptr);
    LineTo(hdc, graph.left, graph.bottom);
    MoveToEx(hdc, graph.right, graph.top, nullptr);
    LineTo(hdc, graph.right, graph.bottom);
    MoveToEx(hdc, graph.left, graph.bottom, nullptr);
    LineTo(hdc, graph.right, graph.bottom);
    MoveToEx(hdc, graph.left, graph.bottom, nullptr);
    LineTo(hdc, backLeft, backBottom);
    MoveToEx(hdc, graph.right, graph.bottom, nullptr);
    LineTo(hdc, backRight, backBottom);

    const int savedDc = SaveDC(hdc);
    IntersectClipRect(hdc,
                      std::min<int>(static_cast<int>(graph.left), backLeft),
                      backTop,
                      std::max<int>(static_cast<int>(graph.right), backRight),
                      graph.bottom);
    SelectObject(hdc, GetStockObject(DC_BRUSH));

    for (int sliceIndex = 0; sliceIndex <= depthCount; ++sliceIndex) {
        const auto& slice = data_.slices[static_cast<size_t>(sliceIndex)];
        const double depthT =
            depthCount <= 0 ? 0.0 : (1.0 - (static_cast<double>(sliceIndex) / static_cast<double>(depthCount)));
        const double colorT =
            depthCount <= 0 ? 0.0 : static_cast<double>(sliceIndex) / static_cast<double>(depthCount);
        const COLORREF fillColor = blendColors(RGB(31, 124, 223), RGB(228, 92, 48), colorT);
        const COLORREF outlineColor = blendColors(fillColor, RGB(48, 48, 48), 0.32);
        SetDCBrushColor(hdc, fillColor);
        SetDCPenColor(hdc, outlineColor);

        std::vector<POINT> polygon;
        polygon.reserve((slice.valuesDb.size() * 2) + 2);
        std::vector<POINT> ridge;
        ridge.reserve(slice.valuesDb.size());

        for (size_t pointIndex = 0; pointIndex < data_.frequencyAxisHz.size() && pointIndex < slice.valuesDb.size(); ++pointIndex) {
            const int xFront = graphXFromFrequency(graph, data_.frequencyAxisHz[pointIndex]);
            const int yFront = graphYFromDb(graph, slice.valuesDb[pointIndex], data_.minDb, data_.maxDb);
            const int x = projectX(xFront, depthT);
            const int y = projectY(yFront, depthT);
            ridge.push_back(POINT{x, y});
            polygon.push_back(POINT{x, y});
        }

        for (size_t reverseIndex = ridge.size(); reverseIndex > 0; --reverseIndex) {
            const POINT ridgePoint = ridge[reverseIndex - 1];
            polygon.push_back(POINT{ridgePoint.x, projectY(graph.bottom, depthT)});
        }

        if (polygon.size() >= 3) {
            Polygon(hdc, polygon.data(), static_cast<int>(polygon.size()));
        }

        if (!ridge.empty()) {
            MoveToEx(hdc, ridge.front().x, ridge.front().y, nullptr);
            for (size_t pointIndex = 1; pointIndex < ridge.size(); ++pointIndex) {
                LineTo(hdc, ridge[pointIndex].x, ridge[pointIndex].y);
            }
        }
    }
    RestoreDC(hdc, savedDc);
    SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

    SetTextColor(hdc, ui_theme::kMuted);
    const int xTickTop = graph.bottom + 4;
    const int xTickBottom = xTickTop + textHeight + 4;
    for (const double tick : tickValues) {
        const int y = graphYFromDb(graph, tick, data_.minDb, data_.maxDb);
        RECT labelRect{
            rect.left + yUnitWidth + 6,
            y - (textHeight / 2),
            graph.left - 8,
            y + (textHeight / 2) + 2
        };
        const std::wstring label = formatDbLabel(tick);
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    for (const double frequencyHz : xTicks) {
        const std::wstring label = formatFrequencyLabel(frequencyHz);
        const int labelWidth = measureTextWidth(hdc, label);
        const int x = graphXFromFrequency(graph, frequencyHz) - (labelWidth / 2);
        RECT labelRect{x, xTickTop, x + labelWidth + 4, xTickBottom};
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

        const int xBack = projectX(graphXFromFrequency(graph, frequencyHz), 1.0) - (labelWidth / 2);
        RECT topLabelRect{xBack, rect.top + 2, xBack + labelWidth + 4, backTop - 4};
        DrawTextW(hdc, label.c_str(), -1, &topLabelRect, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    }

    drawAxisUnitLabels(hdc, rect, graph, backRight, backBottom, textHeight);

    EndPaint(window_, &paint);
}

}  // namespace wolfie::ui
