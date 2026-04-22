#include "ui/response_graph.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double responseGraphXT(double frequencyHz) {
    const double clamped = clampValue(frequencyHz, 10.0, 24000.0);
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

    return DefWindowProcW(window, message, wParam, lParam);
}

void ResponseGraph::onPaint() const {
    PAINTSTRUCT paint{};
    HDC hdc = BeginPaint(window_, &paint);

    RECT rect{};
    GetClientRect(window_, &rect);
    HBRUSH background = CreateSolidBrush(RGB(248, 250, 252));
    FillRect(hdc, &rect, background);
    DeleteObject(background);
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, GetStockObject(DC_PEN));
    SelectObject(hdc, GetStockObject(DC_BRUSH));

    RECT graph = rect;
    InflateRect(&graph, -48, -32);
    MoveToEx(hdc, graph.left, graph.top, nullptr);
    LineTo(hdc, graph.left, graph.bottom);
    LineTo(hdc, graph.right, graph.bottom);

    constexpr std::array<double, 5> kFrequencyTicks{10.0, 100.0, 1000.0, 10000.0, 20000.0};
    const double minDb = data_.minDb;
    double maxDb = minDb + 1.0;
    for (const auto& series : data_.series) {
        for (const double value : series.values) {
            maxDb = std::max(maxDb, value);
        }
    }
    maxDb = std::ceil(maxDb);

    SetDCPenColor(hdc, ui_theme::kBorder);
    for (const double tickHz : kFrequencyTicks) {
        const int x = graph.left + static_cast<int>(responseGraphXT(tickHz) * (graph.right - graph.left));
        MoveToEx(hdc, x, graph.top, nullptr);
        LineTo(hdc, x, graph.bottom);
    }

    const int zeroY = graph.bottom - static_cast<int>(clampValue((0.0 - minDb) / (maxDb - minDb), 0.0, 1.0) * (graph.bottom - graph.top));
    MoveToEx(hdc, graph.left, zeroY, nullptr);
    LineTo(hdc, graph.right, zeroY);

    if (!data_.frequencyAxisHz.empty()) {
        for (const auto& series : data_.series) {
            SetDCPenColor(hdc, series.color);
            for (size_t i = 0; i < series.values.size() && i < data_.frequencyAxisHz.size(); ++i) {
                const double xT = responseGraphXT(data_.frequencyAxisHz[i]);
                const double yT = clampValue((series.values[i] - minDb) / (maxDb - minDb), 0.0, 1.0);
                const int x = graph.left + static_cast<int>(xT * (graph.right - graph.left));
                const int y = graph.bottom - static_cast<int>(yT * (graph.bottom - graph.top));
                if (i == 0) {
                    MoveToEx(hdc, x, y, nullptr);
                } else {
                    LineTo(hdc, x, y);
                }
            }
        }
    }

    SetTextColor(hdc, ui_theme::kMuted);
    for (const double tickHz : kFrequencyTicks) {
        const int x = graph.left + static_cast<int>(responseGraphXT(tickHz) * (graph.right - graph.left));
        RECT label{x - 28, graph.bottom + 6, x + 28, graph.bottom + 24};
        UINT align = DT_CENTER;
        if (tickHz == kFrequencyTicks.front()) {
            label.left = graph.left - 4;
            label.right = graph.left + 40;
            align = DT_LEFT;
        } else if (tickHz == kFrequencyTicks.back()) {
            label.left = graph.right - 44;
            label.right = graph.right + 4;
            align = DT_RIGHT;
        }
        const std::wstring tickLabel = formatResponseTickLabel(tickHz);
        DrawTextW(hdc, tickLabel.c_str(), -1, &label, align);
    }

    RECT labelTop{graph.left - 56, graph.top - 8, graph.left - 4, graph.top + 12};
    RECT labelBottom{graph.left - 56, graph.bottom - 8, graph.left - 4, graph.bottom + 12};
    RECT labelZero{graph.left - 56, zeroY - 8, graph.left - 4, zeroY + 12};
    DrawTextW(hdc, (formatWideDouble(maxDb, 0) + L" dB").c_str(), -1, &labelTop, DT_RIGHT);
    DrawTextW(hdc, (formatWideDouble(minDb, 0) + L" dB").c_str(), -1, &labelBottom, DT_RIGHT);
    if (zeroY > graph.top + 12 && zeroY < graph.bottom - 12) {
        DrawTextW(hdc, L"0 dB", -1, &labelZero, DT_RIGHT);
    }

    EndPaint(window_, &paint);
}

}  // namespace wolfie::ui
