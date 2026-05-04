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
constexpr int kGraphHeight = 340;
constexpr int kSampleDeltaHeight = 42;
constexpr int kSampleDeltaTopGap = 10;
constexpr int kRunButtonWidth = 184;
constexpr int kRunButtonHeight = 32;

void drawActionButton(const DRAWITEMSTRUCT& draw,
                      bool enabled,
                      bool activeRun,
                      const wchar_t* idleText) {
    HDC hdc = draw.hDC;
    RECT rect = draw.rcItem;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const COLORREF fill = !enabled ? RGB(186, 192, 200)
                                   : (activeRun ? ui_theme::kRed : ui_theme::kGreen);
    const COLORREF fillPressed = !enabled ? fill
                                          : (activeRun ? RGB(156, 53, 53) : RGB(37, 118, 68));
    const COLORREF border = !enabled ? RGB(156, 163, 172)
                                     : (activeRun ? RGB(132, 42, 42) : RGB(29, 95, 55));

    HBRUSH brush = CreateSolidBrush(pressed ? fillPressed : fill);
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
    baseFont.lfHeight = -18;
    HFONT buttonFont = CreateFontIndirectW(&baseFont);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, buttonFont));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, enabled ? RGB(255, 255, 255) : RGB(245, 247, 250));
    RECT textRect = rect;
    if (pressed) {
        OffsetRect(&textRect, 0, 1);
    }
    DrawTextW(hdc,
              activeRun ? L"STOP" : idleText,
              -1,
              &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (focused) {
        RECT focusRect = rect;
        InflateRect(&focusRect, -4, -4);
        DrawFocusRect(hdc, &focusRect);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(buttonFont);
}

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
    controls_.buttonRun = CreateWindowW(L"BUTTON",
                                        L"START",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
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
    controls_.confidenceLabel = CreateWindowW(L"STATIC", L"Confidence", WS_CHILD | WS_VISIBLE,
                                              0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.confidenceValue = CreateWindowW(L"STATIC", L"--", WS_CHILD | WS_VISIBLE,
                                              0, 0, 0, 0, window_, nullptr, instance_, nullptr);

    controls_.graphTitle = CreateWindowW(L"STATIC",
                                         L"Move the mic until curves align and sample difference is 0",
                                         WS_CHILD | WS_VISIBLE | SS_CENTER,
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
    const int topRowTop = contentTop;
    const int confidenceWidth = 120;
    const int graphTitleTop = contentTop + 72;
    const int graphTitleHeight = 34;

    MoveWindow(controls_.buttonRun, contentLeft, topRowTop + 2, kRunButtonWidth, kRunButtonHeight, TRUE);
    MoveWindow(controls_.confidenceLabel,
               contentLeft + innerWidth - confidenceWidth,
               topRowTop,
               confidenceWidth,
               18,
               TRUE);
    MoveWindow(controls_.confidenceValue,
               contentLeft + innerWidth - confidenceWidth,
               topRowTop + 22,
               confidenceWidth,
               22,
               TRUE);
    MoveWindow(controls_.status, contentLeft, contentTop + 46, innerWidth, 18, TRUE);
    MoveWindow(controls_.graphTitle, contentLeft, graphTitleTop, innerWidth, graphTitleHeight, TRUE);

    const int arrowTop = graphTitleTop + graphTitleHeight + 12;
    const int availableArrowHeight = std::max(140, innerHeight - (arrowTop - contentTop) - kSampleDeltaHeight - kSampleDeltaTopGap - 16);
    const int arrowHeight = std::min(kGraphHeight, availableArrowHeight);
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
    sampleDeltaBounds_ = RECT{graphBounds.left,
                              graphBounds.bottom + kSampleDeltaTopGap,
                              graphBounds.right,
                              graphBounds.bottom + kSampleDeltaTopGap + kSampleDeltaHeight};

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

bool AlignmentPage::handleDrawItem(const DRAWITEMSTRUCT* draw) const {
    if (draw == nullptr || draw->CtlID != kButtonRun) {
        return false;
    }

    const bool enabled = IsWindowEnabled(draw->hwndItem) != FALSE;
    const bool activeRun = alignmentRunActive_ && status_.running;
    drawActionButton(*draw, enabled, activeRun, L"START");
    return true;
}

PlotGraphData AlignmentPage::buildPulseGraphData() const {
    PlotGraphData data;
    data.xAxisMode = PlotGraphXAxisMode::Linear;
    data.yAxisMode = PlotGraphYAxisMode::Auto;
    data.xUnit = L"ms";
    data.yUnit = L"";
    data.fixedYRange = true;
    data.minY = 0.0;
    data.maxY = 1.05;

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

    SetWindowTextW(controls_.confidenceValue,
                   view_.available
                       ? formatMetricValue(view_.confidenceDb, L" dB", 1).c_str()
                       : L"--");

    const bool canRun = !status_.running || alignmentRunActive_;
    EnableWindow(controls_.buttonRun, (hasWorkspace_ && canRun) ? TRUE : FALSE);
    RedrawWindow(controls_.status, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    RedrawWindow(controls_.buttonRun, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    InvalidateRect(window_, &leftArrowBounds_, TRUE);
    InvalidateRect(window_, &rightArrowBounds_, TRUE);
    InvalidateRect(window_, &sampleDeltaBounds_, TRUE);
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
    paintSampleDelta(hdc);
}

std::wstring AlignmentPage::sampleDeltaReadoutText() const {
    if (!view_.available) {
        return L"--";
    }

    std::wstring text = std::to_wstring(view_.delayMismatchSamples);
    if (view_.delayMismatchSamples > 0) {
        text = L"+" + text;
    }
    text += L" samples";
    return text;
}

void AlignmentPage::paintSampleDelta(HDC hdc) const {
    if ((sampleDeltaBounds_.right - sampleDeltaBounds_.left) <= 0 ||
        (sampleDeltaBounds_.bottom - sampleDeltaBounds_.top) <= 0) {
        return;
    }

    const std::wstring text = sampleDeltaReadoutText();
    const int boundsHeight = sampleDeltaBounds_.bottom - sampleDeltaBounds_.top;
    LOGFONTW font{};
    font.lfHeight = -std::max(18, static_cast<int>(std::lround(boundsHeight * 0.62)));
    font.lfWeight = FW_SEMIBOLD;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    HFONT fontHandle = CreateFontIndirectW(&font);
    HGDIOBJ previousFont = SelectObject(hdc, fontHandle);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, view_.available ? ui_theme::kText : ui_theme::kMuted);
    DrawTextW(hdc,
              text.c_str(),
              static_cast<int>(text.size()),
              const_cast<RECT*>(&sampleDeltaBounds_),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, previousFont);
    DeleteObject(fontHandle);
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
    case WM_NOTIFY:
    case WM_DRAWITEM: {
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
