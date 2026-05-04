#include "ui/alignment_page.h"

#include <algorithm>
#include <cmath>

#include "core/text_utils.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

std::wstring formatMovementText(const measurement::SweetSpotAlignmentView& view) {
    if (!view.available) {
        return L"--";
    }
    if (view.suggestedDirection == measurement::SweetSpotMoveDirection::None) {
        return L"Centered";
    }

    return formatWideDouble(view.suggestedMoveCm, 2) +
           (view.suggestedDirection == measurement::SweetSpotMoveDirection::Left ? L" cm left" : L" cm right");
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
                                   L"Run a narrow high-band burst loop to center the microphone on the stereo centerline.",
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

    constexpr const wchar_t* kMetricLabels[4] = {
        L"L-R Delay",
        L"Path Delta",
        L"Move",
        L"Confidence",
    };
    for (int index = 0; index < 4; ++index) {
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
    const int metricsGap = 16;
    const int metricWidth = (innerWidth - (metricsGap * 3)) / 4;
    for (int index = 0; index < 4; ++index) {
        const int left = contentLeft + (index * (metricWidth + metricsGap));
        MoveWindow(controls_.metrics[index].label, left, metricsTop, metricWidth, 18, TRUE);
        MoveWindow(controls_.metrics[index].value, left, metricsTop + 22, metricWidth, 22, TRUE);
    }

    const int graphTop = metricsTop + 64;
    MoveWindow(controls_.graphTitle, contentLeft, graphTop, innerWidth, 18, TRUE);
    pulseGraph_.layout(RECT{contentLeft,
                            graphTop + 24,
                            contentLeft + innerWidth,
                            contentTop + innerHeight});
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
    SetWindowTextW(controls_.metrics[2].value, formatMovementText(view_).c_str());
    SetWindowTextW(controls_.metrics[3].value,
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
