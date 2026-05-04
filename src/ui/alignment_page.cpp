#include "ui/alignment_page.h"

#include <algorithm>
#include <cmath>

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

constexpr int kGraphGap = 18;
constexpr int kArrowMinWidth = 96;
constexpr int kArrowMaxWidth = 180;
constexpr int kGraphMinWidth = 220;
constexpr int kGraphMaxWidth = 420;
constexpr int kGraphHeight = 170;

COLORREF directionArrowColor(const measurement::SweetSpotAlignmentView& view, bool pointLeft) {
    if (!view.available) {
        return ui_theme::blendColor(ui_theme::kMuted, ui_theme::backgroundColor(), 0.55);
    }
    if (view.suggestedDirection == measurement::SweetSpotMoveDirection::None) {
        return ui_theme::kGreen;
    }

    const bool active = (view.suggestedDirection == measurement::SweetSpotMoveDirection::Left) == pointLeft;
    if (active) {
        return ui_theme::kAccent;
    }
    return ui_theme::blendColor(ui_theme::kMuted, ui_theme::backgroundColor(), 0.55);
}

std::wstring formatWarningText(const measurement::SweetSpotAlignmentView& view,
                               const MeasurementStatus& status,
                               bool alignmentRunActive,
                               bool hasWorkspace) {
    if (!hasWorkspace) {
        return L"Open or create a workspace first.";
    }
    if (status.running && !alignmentRunActive) {
        return L"Another measurement is in progress.";
    }
    if (!view.available) {
        return L"";
    }
    if (view.captureClippingDetected) {
        return L"Reduce output level. Clipping was detected.";
    }
    if (view.captureTooQuiet) {
        return L"Signal is low. Raise output level or mic gain.";
    }
    return L"";
}

}  // namespace

void AlignmentPage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = ui_theme::backgroundBrush();
    RegisterClassW(&pageClass);
}

const wchar_t* AlignmentPage::pageWindowClassName() {
    return kPageClassName;
}

void AlignmentPage::create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    window_ = CreateWindowExW(0,
                              kPageClassName,
                              nullptr,
                              WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
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

void AlignmentPage::createControls() {
    controls_.note = CreateWindowW(L"STATIC",
                                   L"Run a short burst loop to center the microphone on the stereo centerline.",
                                   WS_CHILD | WS_VISIBLE,
                                   0,
                                   0,
                                   0,
                                   0,
                                   window_,
                                   nullptr,
                                   instance_,
                                   nullptr);
    controls_.buttonRun = CreateWindowW(L"BUTTON",
                                        L"Start Loop",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        0,
                                        0,
                                        0,
                                        0,
                                        window_,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonRun)),
                                        instance_,
                                        nullptr);
    controls_.status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                     0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    constexpr const wchar_t* kMetricLabels[] = {
        L"L-R Delay",
        L"Path Delta",
        L"Confidence",
    };
    for (int index = 0; index < kMetricCount; ++index) {
        controls_.metrics[index].label =
            CreateWindowW(L"STATIC", kMetricLabels[index], WS_CHILD | WS_VISIBLE,
                          0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        controls_.metrics[index].value =
            CreateWindowW(L"STATIC", L"--", WS_CHILD | WS_VISIBLE,
                          0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    }

    controls_.graphTitle = CreateWindowW(L"STATIC", L"Direct Arrival Overlay", WS_CHILD | WS_VISIBLE,
                                         0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    pulseGraph_.create(window_, instance_, kPulseGraph);
    pulseGraph_.setHoverCrosshairEnabled(true);
}

void AlignmentPage::layout() {
    RECT pageRect{};
    GetClientRect(window_, &pageRect);
    const int contentLeft = 20;
    const int contentTop = 20;
    const int innerWidth = std::max(520L, pageRect.right - (contentLeft * 2));
    const int innerHeight = std::max(420L, pageRect.bottom - (contentTop * 2));

    const int buttonWidth = 180;
    MoveWindow(controls_.note, contentLeft, contentTop, innerWidth - buttonWidth - 16, 36, TRUE);
    MoveWindow(controls_.buttonRun, contentLeft + innerWidth - buttonWidth, contentTop, buttonWidth, 28, TRUE);
    MoveWindow(controls_.status, contentLeft, contentTop + 42, innerWidth, 18, TRUE);

    const int metricsTop = contentTop + 76;
    const int metricsGap = 18;
    const int metricWidth = (innerWidth - (metricsGap * (kMetricCount - 1))) / kMetricCount;
    for (int index = 0; index < kMetricCount; ++index) {
        const int left = contentLeft + (index * (metricWidth + metricsGap));
        MoveWindow(controls_.metrics[index].label, left, metricsTop, metricWidth, 18, TRUE);
        MoveWindow(controls_.metrics[index].value, left, metricsTop + 22, metricWidth, 22, TRUE);
    }

    const int arrowTop = metricsTop + 74;
    const int arrowHeight = std::min(kGraphHeight, std::max(140, innerHeight - (arrowTop - contentTop) - 16));
    const int arrowWidth = std::clamp(innerWidth / 5, kArrowMinWidth, kArrowMaxWidth);
    const int graphWidth = std::clamp(innerWidth - (arrowWidth * 2) - (kGraphGap * 2),
                                      kGraphMinWidth,
                                      kGraphMaxWidth);
    const int rowWidth = (arrowWidth * 2) + (kGraphGap * 2) + graphWidth;
    const int rowLeft = contentLeft + std::max(0, (innerWidth - rowWidth) / 2);
    const RECT graphBounds{
        rowLeft + arrowWidth + kGraphGap,
        arrowTop,
        rowLeft + arrowWidth + kGraphGap + graphWidth,
        arrowTop + arrowHeight};

    leftArrowBounds_ = RECT{rowLeft, arrowTop, rowLeft + arrowWidth, arrowTop + arrowHeight};
    rightArrowBounds_ = RECT{graphBounds.right + kGraphGap,
                             arrowTop,
                             graphBounds.right + kGraphGap + arrowWidth,
                             arrowTop + arrowHeight};

    MoveWindow(controls_.graphTitle, graphBounds.left, arrowTop - 24, graphWidth, 18, TRUE);
    pulseGraph_.layout(graphBounds);
    InvalidateRect(window_, nullptr, TRUE);
}

void AlignmentPage::setVisible(bool visible) const {
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
}

void AlignmentPage::populate(const measurement::SweetSpotAlignmentView& view) {
    view_ = view;
    pulseGraph_.setData(buildPulseGraphData());
    refreshPresentation();
}

void AlignmentPage::refreshStatus(const MeasurementStatus& status, bool alignmentRunActive, bool hasWorkspace) {
    status_ = status;
    alignmentRunActive_ = alignmentRunActive;
    hasWorkspace_ = hasWorkspace;
    refreshPresentation();
}

bool AlignmentPage::handleCommand(WORD commandId, WORD notificationCode, bool& startStopPressed) {
    if (commandId == kButtonRun && notificationCode == BN_CLICKED) {
        startStopPressed = true;
        return true;
    }
    return false;
}

PlotGraphData AlignmentPage::buildPulseGraphData() const {
    PlotGraphData data;
    data.xAxisMode = PlotGraphXAxisMode::Linear;
    data.yAxisMode = PlotGraphYAxisMode::SymmetricAroundZero;
    data.xUnit = L"ms";
    data.yUnit = L"";
    data.fixedYRange = true;
    data.minY = -1.1;
    data.maxY = 1.1;

    if (!view_.available || view_.timeAxisMs.empty()) {
        data.xValues = {0.0, 1.0};
        data.series.push_back({L"Center", ui_theme::kGray, {0.0, 0.0}, PlotGraphLineStyle::Dash});
        return data;
    }

    data.xValues = view_.timeAxisMs;
    data.series.push_back({L"Center", ui_theme::kGray, std::vector<double>(data.xValues.size(), 0.0), PlotGraphLineStyle::Dash});
    data.series.push_back({L"Left", ui_theme::kGreen, view_.leftImpulse, PlotGraphLineStyle::Solid});
    data.series.push_back({L"Right", ui_theme::kRed, view_.rightImpulse, PlotGraphLineStyle::Solid});
    return data;
}

void AlignmentPage::refreshPresentation() const {
    SetWindowTextW(controls_.status,
                   formatWarningText(view_, status_, alignmentRunActive_, hasWorkspace_).c_str());

    SetWindowTextW(controls_.metrics[0].value,
                   view_.available
                       ? formatMetricValue(view_.delayMismatchMs, L" ms", 3, true).c_str()
                       : L"--");
    SetWindowTextW(controls_.metrics[1].value,
                   view_.available
                       ? formatMetricValue(view_.pathMismatchCm, L" cm", 2, true).c_str()
                       : L"--");
    SetWindowTextW(controls_.metrics[2].value,
                   view_.available
                       ? formatMetricValue(view_.confidenceDb, L" dB", 1).c_str()
                       : L"--");

    std::wstring buttonText = L"Start Loop";
    if (alignmentRunActive_ && status_.running) {
        buttonText = L"Stop Loop";
    }
    SetWindowTextW(controls_.buttonRun, buttonText.c_str());

    const bool canRun = !status_.running || alignmentRunActive_;
    EnableWindow(controls_.buttonRun, (hasWorkspace_ && canRun) ? TRUE : FALSE);
    RedrawWindow(controls_.status, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    RedrawWindow(controls_.buttonRun, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    InvalidateRect(window_, &leftArrowBounds_, TRUE);
    InvalidateRect(window_, &rightArrowBounds_, TRUE);
}

std::wstring AlignmentPage::formatMetricValue(double value,
                                              const wchar_t* unit,
                                              int decimals,
                                              bool signedValue) {
    if (!std::isfinite(value)) {
        return L"--";
    }
    std::wstring text = formatWideDouble(value, decimals);
    if (signedValue && value > 0.0) {
        text = L"+" + text;
    }
    if (unit != nullptr) {
        text += unit;
    }
    return text;
}

void AlignmentPage::paint(HDC hdc) const {
    paintDirectionArrow(hdc, leftArrowBounds_, true, directionArrowColor(view_, true));
    paintDirectionArrow(hdc, rightArrowBounds_, false, directionArrowColor(view_, false));
}

void AlignmentPage::paintDirectionArrow(HDC hdc,
                                        const RECT& bounds,
                                        bool pointLeft,
                                        COLORREF fillColor) const {
    if ((bounds.right - bounds.left) < 24 || (bounds.bottom - bounds.top) < 24) {
        return;
    }

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int paddingX = std::max(8, width / 10);
    const int paddingY = std::max(10, height / 8);
    const int shaftHalfThickness = std::max(9, height / 10);
    const int headWidth = std::max(28, width / 3);
    const int left = bounds.left + paddingX;
    const int right = bounds.right - paddingX;
    const int top = bounds.top + paddingY;
    const int bottom = bounds.bottom - paddingY;
    const int centerY = bounds.top + (height / 2);

    POINT points[7]{};
    if (pointLeft) {
        points[0] = POINT{left, centerY};
        points[1] = POINT{left + headWidth, top};
        points[2] = POINT{left + headWidth, centerY - shaftHalfThickness};
        points[3] = POINT{right, centerY - shaftHalfThickness};
        points[4] = POINT{right, centerY + shaftHalfThickness};
        points[5] = POINT{left + headWidth, centerY + shaftHalfThickness};
        points[6] = POINT{left + headWidth, bottom};
    } else {
        points[0] = POINT{right, centerY};
        points[1] = POINT{right - headWidth, top};
        points[2] = POINT{right - headWidth, centerY - shaftHalfThickness};
        points[3] = POINT{left, centerY - shaftHalfThickness};
        points[4] = POINT{left, centerY + shaftHalfThickness};
        points[5] = POINT{right - headWidth, centerY + shaftHalfThickness};
        points[6] = POINT{right - headWidth, bottom};
    }

    HBRUSH brush = CreateSolidBrush(fillColor);
    HPEN pen = CreatePen(PS_SOLID, 1, ui_theme::blendColor(fillColor, ui_theme::kText, 0.22));
    HGDIOBJ previousBrush = SelectObject(hdc, brush);
    HGDIOBJ previousPen = SelectObject(hdc, pen);
    SetBkMode(hdc, TRANSPARENT);
    Polygon(hdc, points, 7);
    SelectObject(hdc, previousPen);
    SelectObject(hdc, previousBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

LRESULT CALLBACK AlignmentPage::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AlignmentPage* page = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        page = reinterpret_cast<AlignmentPage*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(page));
    } else {
        page = reinterpret_cast<AlignmentPage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    switch (message) {
    case WM_PAINT:
        if (page != nullptr) {
            PAINTSTRUCT paint{};
            HDC hdc = BeginPaint(window, &paint);
            FillRect(hdc, &paint.rcPaint, ui_theme::backgroundBrush());
            page->paint(hdc);
            EndPaint(window, &paint);
            return 0;
        }
        break;
    case WM_COMMAND:
    case WM_NOTIFY: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace wolfie::ui
