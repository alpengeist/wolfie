#include "ui/measurement_page.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include <commctrl.h>

#include "core/text_utils.h"
#include "measurement/response_analyzer.h"
#include "measurement/sweep_generator.h"
#include "ui/ui_theme.h"

namespace wolfie::ui {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

constexpr double kMutedOutputVolumeDb = -100.0;

std::wstring formatBool(bool value) {
    return value ? L"Yes" : L"No";
}

std::wstring formatSamples(int sampleCount) {
    return std::to_wstring(sampleCount) + L" samples";
}

std::wstring formatSamplesAndMs(int sampleCount, int sampleRate) {
    std::wstring text = formatSamples(sampleCount);
    if (sampleRate > 0) {
        const double milliseconds = static_cast<double>(sampleCount) * 1000.0 / static_cast<double>(sampleRate);
        text += L" (" + formatWideDouble(milliseconds, 2) + L" ms)";
    }
    return text;
}

std::wstring formatPathValue(const std::filesystem::path& path) {
    return path.empty() ? L"-" : path.wstring();
}

}  // namespace

void MeasurementPage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = CreateSolidBrush(ui_theme::kBackground);
    RegisterClassW(&pageClass);
}

const wchar_t* MeasurementPage::pageWindowClassName() {
    return kPageClassName;
}

void MeasurementPage::create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    window_ = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr, instance, nullptr);
    createControls();
}

void MeasurementPage::createControls() {
    controls_.labelFadeIn = CreateWindowW(L"STATIC", L"Fade-In", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelFadeOut = CreateWindowW(L"STATIC", L"Fade-Out", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelDuration = CreateWindowW(L"STATIC", L"Sweep Time", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelStartFrequency = CreateWindowW(L"STATIC", L"Sweep Start", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelEndFrequency = CreateWindowW(L"STATIC", L"Sweep End", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelTargetLength = CreateWindowW(L"STATIC", L"Target Length", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelLeadIn = CreateWindowW(L"STATIC", L"Lead-In", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelSampleRate = CreateWindowW(L"STATIC", L"Sample Rate", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitFadeIn = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitFadeOut = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitDuration = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitStartFrequency = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitEndFrequency = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitTargetLength = CreateWindowW(L"STATIC", L"samples", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitLeadIn = CreateWindowW(L"STATIC", L"samples", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editFadeIn = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditFadeIn), instance_, nullptr);
    controls_.editFadeOut = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditFadeOut), instance_, nullptr);
    controls_.editDuration = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditDuration), instance_, nullptr);
    controls_.editStartFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditStartFrequency), instance_, nullptr);
    controls_.editEndFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditEndFrequency), instance_, nullptr);
    controls_.editTargetLength = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditTargetLength), instance_, nullptr);
    controls_.editLeadIn = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditLeadIn), instance_, nullptr);
    controls_.comboSampleRate = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kComboMeasurementSampleRate), instance_, nullptr);
    controls_.labelOutputVolume = CreateWindowW(L"STATIC", L"Output level", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.outputVolumeValue = CreateWindowW(L"STATIC", L"-30 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.outputVolumeSlider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.outputVolumeMuteLabel = CreateWindowW(L"STATIC", L"Mute", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.outputVolumeMaxLabel = CreateWindowW(L"STATIC", L"0 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.buttonMeasure = CreateWindowW(L"BUTTON", L"START", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonMeasure), instance_, nullptr);
    controls_.leftChannelLabel = CreateWindowW(L"STATIC", L"Left", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.leftProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.leftProgressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightChannelLabel = CreateWindowW(L"STATIC", L"Right", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightProgressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.currentFrequency = CreateWindowW(L"STATIC", L"Freq 0 Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.currentAmplitude = CreateWindowW(L"STATIC", L"Amp -90 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.peakAmplitude = CreateWindowW(L"STATIC", L"Peak -90 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.metadataLabel = CreateWindowW(L"STATIC", L"Measurement Metadata", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.metadataTable = CreateWindowExW(WS_EX_CLIENTEDGE,
                                              WC_LISTVIEWW,
                                              nullptr,
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                              0,
                                              0,
                                              0,
                                              0,
                                              window_,
                                              reinterpret_cast<HMENU>(kMetadataTable),
                                              instance_,
                                              nullptr);

    responseGraph_.create(window_, instance_, kResponseGraph);

    const DWORD centeredStaticStyle = SS_CENTER | WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(controls_.labelFadeIn, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelFadeOut, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelDuration, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelStartFrequency, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelEndFrequency, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelTargetLength, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelLeadIn, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelSampleRate, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.unitFadeIn, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.unitFadeOut, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.unitDuration, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.unitStartFrequency, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.unitEndFrequency, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.unitTargetLength, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.unitLeadIn, GWL_STYLE, centeredStaticStyle);
    SendMessageW(controls_.editEndFrequency, EM_SETREADONLY, TRUE, 0);

    SendMessageW(controls_.leftProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    SendMessageW(controls_.rightProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    SendMessageW(controls_.outputVolumeSlider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.outputVolumeSlider, TBM_SETRANGEMAX, FALSE, kOutputVolumeSliderMax);
    SendMessageW(controls_.outputVolumeSlider, TBM_SETTICFREQ, 10, 0);
    ListView_SetExtendedListViewStyle(controls_.metadataTable,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(L"Section");
    column.cx = 132;
    ListView_InsertColumn(controls_.metadataTable, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"Metric");
    column.cx = 220;
    column.iSubItem = 1;
    ListView_InsertColumn(controls_.metadataTable, 1, &column);
    column.pszText = const_cast<LPWSTR>(L"Value");
    column.cx = 560;
    column.iSubItem = 2;
    ListView_InsertColumn(controls_.metadataTable, 2, &column);
    populateMeasurementSampleRateCombo(controls_.comboSampleRate);
}

void MeasurementPage::layout() {
    RECT measurementRect{};
    GetClientRect(window_, &measurementRect);
    const int contentLeft = 20;
    const int contentTop = 20;
    const int innerWidth = std::max(480L, measurementRect.right - (contentLeft * 2));
    const int innerHeight = std::max(360L, measurementRect.bottom - (contentTop * 2));
    constexpr int kLabelWidthSmall = 86;
    constexpr int kLabelWidthMedium = 100;
    constexpr int kLabelWidthLarge = 104;
    constexpr int kValueWidthTiny = 56;
    constexpr int kValueWidthSmall = 62;
    constexpr int kValueWidthMedium = 74;
    constexpr int kValueWidthCombo = 96;
    constexpr int kFieldGap = 48;
    constexpr int kLabelTopOffset = 2;
    constexpr int kFieldTopOffset = 22;
    constexpr int kUnitHeight = 16;
    constexpr int kUnitTopOffset = 52;
    constexpr int kButtonWidth = 184;
    constexpr int kProgressLabelWidth = 42;
    constexpr int kProgressBarWidth = 180;
    constexpr int kProgressTextWidth = 44;
    constexpr int kMetricWidth = 90;
    constexpr int kMetricGap = 10;
    constexpr int kSliderWidth = 220;
    constexpr int kSliderValueWidth = 56;

    auto placeCenteredFieldWithUnit = [&](HWND label, HWND edit, HWND unit, int left, int top, int labelWidth, int editWidth, int unitWidth) {
        const int labelLeft = left + ((editWidth - labelWidth) / 2);
        const int unitLeft = left + ((editWidth - unitWidth) / 2);
        MoveWindow(label, labelLeft, top + kLabelTopOffset, labelWidth, 18, TRUE);
        MoveWindow(edit, left, top + kFieldTopOffset, editWidth, 26, TRUE);
        MoveWindow(unit, unitLeft, top + kUnitTopOffset, unitWidth, kUnitHeight, TRUE);
    };

    auto placeCenteredComboField = [&](HWND label, HWND combo, int left, int top, int labelWidth, int comboWidth) {
        const int labelLeft = left + ((comboWidth - labelWidth) / 2);
        MoveWindow(label, labelLeft, top + kLabelTopOffset, labelWidth, 18, TRUE);
        MoveWindow(combo, left, top + kFieldTopOffset, comboWidth, 220, TRUE);
    };

    const int paramsTop = contentTop;
    int left = contentLeft;
    placeCenteredFieldWithUnit(controls_.labelFadeIn, controls_.editFadeIn, controls_.unitFadeIn, left, paramsTop, kLabelWidthSmall, kValueWidthTiny, 32);
    left += kValueWidthTiny + kFieldGap;
    placeCenteredFieldWithUnit(controls_.labelFadeOut, controls_.editFadeOut, controls_.unitFadeOut, left, paramsTop, kLabelWidthSmall, kValueWidthTiny, 32);
    left += kValueWidthTiny + kFieldGap;
    placeCenteredFieldWithUnit(controls_.labelDuration, controls_.editDuration, controls_.unitDuration, left, paramsTop, kLabelWidthMedium, kValueWidthSmall, 32);
    left += kValueWidthSmall + kFieldGap;
    placeCenteredFieldWithUnit(controls_.labelStartFrequency, controls_.editStartFrequency, controls_.unitStartFrequency, left, paramsTop, kLabelWidthLarge, kValueWidthTiny, 24);
    left += kValueWidthTiny + kFieldGap;
    placeCenteredFieldWithUnit(controls_.labelEndFrequency, controls_.editEndFrequency, controls_.unitEndFrequency, left, paramsTop, kLabelWidthLarge, kValueWidthMedium, 24);
    left += kValueWidthMedium + kFieldGap;
    placeCenteredFieldWithUnit(controls_.labelTargetLength, controls_.editTargetLength, controls_.unitTargetLength, left, paramsTop, kLabelWidthLarge, kValueWidthMedium, 54);
    left += kValueWidthMedium + kFieldGap;
    placeCenteredFieldWithUnit(controls_.labelLeadIn, controls_.editLeadIn, controls_.unitLeadIn, left, paramsTop, kLabelWidthMedium, kValueWidthSmall, 54);
    left += kValueWidthSmall + kFieldGap;
    placeCenteredComboField(controls_.labelSampleRate, controls_.comboSampleRate, left, paramsTop, kLabelWidthMedium, kValueWidthCombo);

    const int volumeTop = paramsTop + 82;
    MoveWindow(controls_.labelOutputVolume, contentLeft, volumeTop + 5, 90, 20, TRUE);
    MoveWindow(controls_.outputVolumeValue, contentLeft + 100, volumeTop + 5, kSliderValueWidth, 20, TRUE);
    MoveWindow(controls_.outputVolumeSlider, contentLeft + 100 + kSliderValueWidth + 12, volumeTop, kSliderWidth, 32, TRUE);
    MoveWindow(controls_.outputVolumeMuteLabel, contentLeft + 100 + kSliderValueWidth + 12, volumeTop + 32, 40, 18, TRUE);
    MoveWindow(controls_.outputVolumeMaxLabel, contentLeft + 100 + kSliderValueWidth + 12 + kSliderWidth - 40, volumeTop + 32, 40, 18, TRUE);

    const int metricsTop = volumeTop + 66;
    const int dataRowTop = metricsTop;
    const int progressRowTop = dataRowTop + 30;
    const int buttonTop = dataRowTop - 4;
    const int buttonHeight = (progressRowTop + 20) - buttonTop;
    MoveWindow(controls_.buttonMeasure, contentLeft, buttonTop, kButtonWidth, buttonHeight, TRUE);
    int metricLeft = contentLeft + kButtonWidth + kMetricGap;
    MoveWindow(controls_.currentFrequency, metricLeft, dataRowTop + 4, kMetricWidth, 20, TRUE);
    metricLeft += kMetricWidth + kMetricGap;
    MoveWindow(controls_.currentAmplitude, metricLeft, dataRowTop + 4, kMetricWidth, 20, TRUE);
    metricLeft += kMetricWidth + kMetricGap;
    MoveWindow(controls_.peakAmplitude, metricLeft, dataRowTop + 4, kMetricWidth, 20, TRUE);

    metricLeft = contentLeft + kButtonWidth + kMetricGap;
    MoveWindow(controls_.leftChannelLabel, metricLeft, progressRowTop + 2, kProgressLabelWidth, 18, TRUE);
    MoveWindow(controls_.leftProgressBar, metricLeft + kProgressLabelWidth + 8, progressRowTop + 4, kProgressBarWidth, 16, TRUE);
    MoveWindow(controls_.leftProgressText, metricLeft + kProgressLabelWidth + 8 + kProgressBarWidth + 8, progressRowTop, kProgressTextWidth, 20, TRUE);
    metricLeft += kProgressLabelWidth + 8 + kProgressBarWidth + 8 + kProgressTextWidth + kMetricGap;
    MoveWindow(controls_.rightChannelLabel, metricLeft, progressRowTop + 2, kProgressLabelWidth, 18, TRUE);
    MoveWindow(controls_.rightProgressBar, metricLeft + kProgressLabelWidth + 8, progressRowTop + 4, kProgressBarWidth, 16, TRUE);
    MoveWindow(controls_.rightProgressText, metricLeft + kProgressLabelWidth + 8 + kProgressBarWidth + 8, progressRowTop, kProgressTextWidth, 20, TRUE);

    const int graphTop = progressRowTop + 48;
    const int availableBottom = contentTop + innerHeight;
    const int metadataLabelTop = std::max(graphTop + 220, graphTop + ((availableBottom - graphTop) * 11 / 20));
    const int graphBottom = std::max(graphTop + 200, metadataLabelTop - 28);
    const RECT graphBounds{contentLeft, graphTop, contentLeft + innerWidth, graphBottom};
    responseGraph_.layout(graphBounds);
    MoveWindow(controls_.metadataLabel, contentLeft, metadataLabelTop, innerWidth, 20, TRUE);
    MoveWindow(controls_.metadataTable,
               contentLeft,
               metadataLabelTop + 24,
               innerWidth,
               std::max(120, availableBottom - (metadataLabelTop + 24)),
               TRUE);
    const int sectionWidth = std::clamp(innerWidth / 7, 120, 180);
    const int metricWidth = std::clamp(innerWidth / 4, 180, 260);
    const int valueWidth = std::max(240, innerWidth - sectionWidth - metricWidth - 28);
    ListView_SetColumnWidth(controls_.metadataTable, 0, sectionWidth);
    ListView_SetColumnWidth(controls_.metadataTable, 1, metricWidth);
    ListView_SetColumnWidth(controls_.metadataTable, 2, valueWidth);
}

void MeasurementPage::setVisible(bool visible) const {
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
}

void MeasurementPage::populate(const WorkspaceState& workspace) {
    setWindowTextValue(controls_.editFadeIn, formatWideDouble(workspace.measurement.fadeInSeconds));
    setWindowTextValue(controls_.editFadeOut, formatWideDouble(workspace.measurement.fadeOutSeconds));
    setWindowTextValue(controls_.editDuration, formatWideDouble(workspace.measurement.durationSeconds, 0));
    setWindowTextValue(controls_.editStartFrequency, formatWideDouble(workspace.measurement.startFrequencyHz, 0));
    setWindowTextValue(controls_.editEndFrequency, formatWideDouble(workspace.measurement.endFrequencyHz, 0));
    setWindowTextValue(controls_.editTargetLength, formatWideDouble(workspace.measurement.targetLengthSamples, 0));
    setWindowTextValue(controls_.editLeadIn, formatWideDouble(workspace.measurement.leadInSamples, 0));
    SendMessageW(controls_.comboSampleRate, CB_SETCURSEL, comboIndexFromMeasurementSampleRate(workspace.measurement.sampleRate), 0);
    setWindowTextValue(controls_.outputVolumeValue, formatOutputVolumeLabel(workspace.audio.outputVolumeDb));
    SendMessageW(controls_.outputVolumeSlider, TBM_SETPOS, TRUE, outputVolumeDbToSliderPosition(workspace.audio.outputVolumeDb));
    responseGraph_.setExtraVisibleRangeDb(workspace.ui.measurementGraphExtraRangeDb);
    responseGraph_.setVerticalOffsetDb(workspace.ui.measurementGraphVerticalOffsetDb);
    setMeasurementResult(workspace.result);
}

void MeasurementPage::syncToWorkspace(WorkspaceState& workspace) const {
    workspace.measurement.sampleRate = measurementSampleRateFromComboIndex(static_cast<int>(SendMessageW(controls_.comboSampleRate, CB_GETCURSEL, 0, 0)));
    workspace.measurement.fadeInSeconds = std::stod(getWindowTextValue(controls_.editFadeIn));
    workspace.measurement.fadeOutSeconds = std::stod(getWindowTextValue(controls_.editFadeOut));
    workspace.measurement.durationSeconds = std::stod(getWindowTextValue(controls_.editDuration));
    workspace.measurement.startFrequencyHz = std::stod(getWindowTextValue(controls_.editStartFrequency));
    workspace.measurement.targetLengthSamples = std::stoi(getWindowTextValue(controls_.editTargetLength));
    workspace.measurement.leadInSamples = std::stoi(getWindowTextValue(controls_.editLeadIn));
    workspace.ui.measurementGraphExtraRangeDb = responseGraph_.extraVisibleRangeDb();
    workspace.ui.measurementGraphVerticalOffsetDb = responseGraph_.verticalOffsetDb();
    measurement::syncDerivedMeasurementSettings(workspace.measurement);
}

void MeasurementPage::setMeasurementResult(const MeasurementResult& result) {
    responseGraph_.setData(buildGraphData(result));
    populateMetadataTable(result);
}

void MeasurementPage::refreshStatus(const MeasurementStatus& status, bool hasResult) {
    int leftProgress = 0;
    int rightProgress = 0;
    const int currentProgress = static_cast<int>(status.progress * 1000.0);
    if (status.running) {
        if (status.currentChannel == MeasurementChannel::Right) {
            leftProgress = 1000;
            rightProgress = currentProgress;
        } else {
            leftProgress = currentProgress;
        }
    } else if (status.finished) {
        leftProgress = 1000;
        rightProgress = 1000;
    }

    SendMessageW(controls_.leftProgressBar, PBM_SETPOS, leftProgress, 0);
    SendMessageW(controls_.rightProgressBar, PBM_SETPOS, rightProgress, 0);
    setWindowTextValue(controls_.leftProgressText, std::to_wstring(leftProgress / 10) + L"%");
    setWindowTextValue(controls_.rightProgressText, std::to_wstring(rightProgress / 10) + L"%");
    setWindowTextValue(controls_.currentFrequency, L"Freq " + formatWideDouble(status.currentFrequencyHz, 0) + L" Hz");
    setWindowTextValue(controls_.currentAmplitude, L"Amp " + formatWideDouble(status.currentAmplitudeDb, 1) + L" dB");
    setWindowTextValue(controls_.peakAmplitude, L"Peak " + formatWideDouble(status.peakAmplitudeDb, 1) + L" dB");
    InvalidateRect(controls_.buttonMeasure, nullptr, TRUE);

    (void)hasResult;
}

void MeasurementPage::invalidateGraph() const {
    responseGraph_.invalidate();
}

bool MeasurementPage::handleDrawItem(const DRAWITEMSTRUCT* draw, bool measurementRunning) const {
    if (draw == nullptr || draw->CtlID != kButtonMeasure) {
        return false;
    }

    HDC hdc = draw->hDC;
    RECT rect = draw->rcItem;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const COLORREF fill = measurementRunning ? ui_theme::kRed : ui_theme::kGreen;
    const COLORREF fillPressed = measurementRunning ? RGB(156, 53, 53) : RGB(37, 118, 68);
    const COLORREF border = measurementRunning ? RGB(132, 42, 42) : RGB(29, 95, 55);

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
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT textRect = rect;
    if (pressed) {
        OffsetRect(&textRect, 0, 1);
    }
    DrawTextW(hdc, measurementRunning ? L"STOP" : L"START", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (focused) {
        RECT focusRect = rect;
        InflateRect(&focusRect, -4, -4);
        DrawFocusRect(hdc, &focusRect);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(buttonFont);
    return true;
}

bool MeasurementPage::handleCommand(WORD commandId,
                                    WORD notificationCode,
                                    WorkspaceState& workspace,
                                    bool& measurePressed,
                                    bool& sampleRateChanged,
                                    bool& graphZoomChanged) {
    if (commandId == kButtonMeasure) {
        measurePressed = true;
        return true;
    }

    if (commandId == kResponseGraph && notificationCode == ResponseGraph::kZoomChangedNotification) {
        workspace.ui.measurementGraphExtraRangeDb = responseGraph_.extraVisibleRangeDb();
        workspace.ui.measurementGraphVerticalOffsetDb = responseGraph_.verticalOffsetDb();
        graphZoomChanged = true;
        return true;
    }

    if (commandId == kComboMeasurementSampleRate && notificationCode == CBN_SELCHANGE) {
        try {
            syncToWorkspace(workspace);
        } catch (...) {
        }
        workspace.measurement.sampleRate = measurementSampleRateFromComboIndex(static_cast<int>(SendMessageW(controls_.comboSampleRate, CB_GETCURSEL, 0, 0)));
        measurement::syncDerivedMeasurementSettings(workspace.measurement);
        populate(workspace);
        sampleRateChanged = true;
        return true;
    }

    return false;
}

bool MeasurementPage::handleHScroll(HWND source, WorkspaceState& workspace) {
    if (source != controls_.outputVolumeSlider) {
        return false;
    }

    workspace.audio.outputVolumeDb = sliderPositionToOutputVolumeDb(static_cast<int>(SendMessageW(controls_.outputVolumeSlider, TBM_GETPOS, 0, 0)));
    setWindowTextValue(controls_.outputVolumeValue, formatOutputVolumeLabel(workspace.audio.outputVolumeDb));
    return true;
}

LRESULT CALLBACK MeasurementPage::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static HBRUSH pageBackgroundBrush = CreateSolidBrush(ui_theme::kBackground);
    switch (message) {
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_DRAWITEM: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int MeasurementPage::measurementSampleRateFromComboIndex(int index) {
    switch (index) {
    case 1:
        return 48000;
    case 2:
        return 96000;
    case 0:
    default:
        return 44100;
    }
}

int MeasurementPage::comboIndexFromMeasurementSampleRate(int sampleRate) {
    switch (sampleRate) {
    case 48000:
        return 1;
    case 96000:
        return 2;
    case 44100:
    default:
        return 0;
    }
}

void MeasurementPage::populateMeasurementSampleRateCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"44.1 kHz"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"48 kHz"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"96 kHz"));
}

double MeasurementPage::sliderPositionToOutputVolumeDb(int position) {
    const int clamped = clampValue(position, 0, kOutputVolumeSliderMax);
    return clamped == 0 ? kMutedOutputVolumeDb : static_cast<double>(clamped - kOutputVolumeSliderMax);
}

int MeasurementPage::outputVolumeDbToSliderPosition(double outputVolumeDb) {
    if (outputVolumeDb <= kMutedOutputVolumeDb) {
        return 0;
    }
    return clampValue(static_cast<int>(std::lround(outputVolumeDb + kOutputVolumeSliderMax)), 1, kOutputVolumeSliderMax);
}

std::wstring MeasurementPage::formatOutputVolumeLabel(double outputVolumeDb) {
    if (outputVolumeDb <= kMutedOutputVolumeDb) {
        return L"Mute";
    }
    return formatWideDouble(outputVolumeDb, 0) + L" dB";
}

std::wstring MeasurementPage::getWindowTextValue(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}

void MeasurementPage::setWindowTextValue(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

void MeasurementPage::setListViewText(HWND listView, int row, int column, const std::wstring& text) {
    LVITEMW item{};
    item.iItem = row;
    item.iSubItem = column;
    item.mask = LVIF_TEXT;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    if (column == 0) {
        ListView_InsertItem(listView, &item);
    } else {
        ListView_SetItem(listView, &item);
    }
}

ResponseGraphData MeasurementPage::buildGraphData(const MeasurementResult& result) const {
    ResponseGraphData data;
    const MeasurementValueSet* magnitudeResponse = result.preferredMagnitudeResponse();
    if (magnitudeResponse != nullptr && magnitudeResponse->valid()) {
        data.frequencyAxisHz = magnitudeResponse->xValues;
        data.series.push_back({L"Left", ui_theme::kGreen, magnitudeResponse->leftValues});
        data.series.push_back({L"Right", ui_theme::kRed, magnitudeResponse->rightValues});
    }
    return data;
}

std::vector<MeasurementPage::MetadataRow> MeasurementPage::buildMetadataRows(const MeasurementResult& result) const {
    std::vector<MetadataRow> rows;
    const MeasurementAnalysis& analysis = result.analysis;
    if (!result.hasAnyValues() &&
        analysis.measurementTimestampUtc.empty() &&
        analysis.artifacts.empty()) {
        rows.push_back({L"Result", L"Status", L"No measurement result saved."});
        return rows;
    }

    auto add = [&](std::wstring section, std::wstring metric, std::wstring value) {
        rows.push_back({std::move(section), std::move(metric), std::move(value)});
    };

    add(L"Result", L"Value Sets", std::to_wstring(result.valueSets.size()));
    if (const MeasurementValueSet* preferred = result.preferredMagnitudeResponse(); preferred != nullptr) {
        add(L"Result", L"Preferred Magnitude Key", toWide(preferred->key));
        add(L"Result", L"Preferred Magnitude Points", std::to_wstring(preferred->xValues.size()));
    } else {
        add(L"Result", L"Preferred Magnitude Key", L"-");
        add(L"Result", L"Preferred Magnitude Points", L"0");
    }

    add(L"Run", L"Analyzer Version", toWide(analysis.analyzerVersion));
    add(L"Run", L"Measured At (UTC)", toWide(analysis.measurementTimestampUtc.empty() ? std::string("-") : analysis.measurementTimestampUtc));
    add(L"Run", L"Backend", toWide(analysis.backendName.empty() ? std::string("-") : analysis.backendName));
    add(L"Run", L"Input Device", toWide(analysis.backendInputDevice.empty() ? std::string("-") : analysis.backendInputDevice));
    add(L"Run", L"Output Device", toWide(analysis.backendOutputDevice.empty() ? std::string("-") : analysis.backendOutputDevice));
    add(L"Run", L"Requested Driver", toWide(analysis.requestedDriver.empty() ? std::string("-") : analysis.requestedDriver));
    add(L"Run", L"Routing Honored", formatBool(analysis.routingSelectionHonored));
    add(L"Run", L"Routing Notes", toWide(analysis.routingNotes.empty() ? std::string("-") : analysis.routingNotes));

    add(L"Sweep", L"Sample Rate", std::to_wstring(analysis.sampleRate) + L" Hz");
    add(L"Sweep", L"Duration", formatWideDouble(analysis.sweepDurationSeconds, 1) + L" s");
    add(L"Sweep", L"Fade In", formatWideDouble(analysis.fadeInSeconds, 2) + L" s");
    add(L"Sweep", L"Fade Out", formatWideDouble(analysis.fadeOutSeconds, 2) + L" s");
    add(L"Sweep", L"Start Frequency", formatWideDouble(analysis.startFrequencyHz, 1) + L" Hz");
    add(L"Sweep", L"End Frequency", formatWideDouble(analysis.endFrequencyHz, 1) + L" Hz");
    add(L"Sweep", L"Target Length", formatSamples(analysis.targetLengthSamples));
    add(L"Sweep", L"Lead-In", formatSamples(analysis.leadInSamples));
    add(L"Sweep", L"Output Volume", formatWideDouble(analysis.outputVolumeDb, 1) + L" dB");
    add(L"Sweep", L"Played Sweep Samples", formatSamples(analysis.playedSweepSamples));

    add(L"Capture", L"Captured Samples", formatSamples(analysis.capturedSamples));
    add(L"Capture", L"Capture Clipping", formatBool(analysis.captureClippingDetected));
    add(L"Capture", L"Capture Too Quiet", formatBool(analysis.captureTooQuiet));
    add(L"Capture", L"Capture Peak", formatWideDouble(analysis.capturePeakDb, 1) + L" dB");
    add(L"Capture", L"Capture RMS", formatWideDouble(analysis.captureRmsDb, 1) + L" dB");
    add(L"Capture", L"Capture Noise Floor", formatWideDouble(analysis.captureNoiseFloorDb, 1) + L" dB");
    add(L"Capture", L"Alignment Search", formatSamplesAndMs(analysis.alignmentSearchSamples, analysis.sampleRate));
    add(L"Capture", L"Alignment Method", toWide(analysis.alignmentMethod.empty() ? std::string("-") : analysis.alignmentMethod));
    add(L"Capture", L"Window Type", toWide(analysis.windowType.empty() ? std::string("-") : analysis.windowType));
    add(L"Capture", L"Inverse Filter Length", formatSamples(analysis.inverseFilterLengthSamples));
    add(L"Capture", L"Inverse Filter Peak Index", std::to_wstring(analysis.inverseFilterPeakIndex));
    add(L"Capture", L"FFT Size", formatSamples(analysis.fftSize));
    add(L"Capture", L"Display Points", std::to_wstring(analysis.displayPointCount));

    auto addChannel = [&](const wchar_t* section, const MeasurementChannelMetrics& channel) {
        add(section, L"Available", formatBool(channel.available));
        add(section, L"Detected Latency", formatSamplesAndMs(channel.detectedLatencySamples, analysis.sampleRate));
        add(section, L"Onset Sample", std::to_wstring(channel.onsetSampleIndex));
        add(section, L"Onset Time", formatWideDouble(channel.onsetTimeSeconds * 1000.0, 2) + L" ms");
        add(section, L"Deconvolution Peak Index", std::to_wstring(channel.peakSampleIndex));
        add(section, L"Impulse Start Sample", std::to_wstring(channel.impulseStartSample));
        add(section, L"Impulse Length", formatSamples(channel.impulseLengthSamples));
        add(section, L"Pre-Roll", formatSamples(channel.preRollSamples));
        add(section, L"Window Start Sample", std::to_wstring(channel.analysisWindowStartSample));
        add(section, L"Window Length", formatSamples(channel.analysisWindowLengthSamples));
        add(section, L"Window Fade", formatSamples(channel.analysisWindowFadeSamples));
        add(section, L"Capture Peak", formatWideDouble(channel.capturePeakDb, 1) + L" dB");
        add(section, L"Capture RMS", formatWideDouble(channel.captureRmsDb, 1) + L" dB");
        add(section, L"Noise Floor", formatWideDouble(channel.noiseFloorDb, 1) + L" dB");
        add(section, L"Impulse Peak Amplitude", formatWideDouble(channel.impulsePeakAmplitude, 6));
        add(section, L"Impulse Peak", formatWideDouble(channel.impulsePeakDb, 1) + L" dB");
        add(section, L"Impulse RMS", formatWideDouble(channel.impulseRmsDb, 1) + L" dB");
        add(section, L"Impulse Peak/Noise", formatWideDouble(channel.impulsePeakToNoiseDb, 1) + L" dB");
    };

    addChannel(L"Left", analysis.left);
    addChannel(L"Right", analysis.right);

    for (const MeasurementArtifact& artifact : analysis.artifacts) {
        add(L"Artifacts", toWide(artifact.key), formatPathValue(artifact.path));
    }

    for (const MeasurementValueSet& valueSet : result.valueSets) {
        add(L"Series", toWide(valueSet.key), std::to_wstring(valueSet.xValues.size()) + L" pts");
    }

    return rows;
}

void MeasurementPage::populateMetadataTable(const MeasurementResult& result) const {
    if (controls_.metadataTable == nullptr) {
        return;
    }

    const std::vector<MetadataRow> rows = buildMetadataRows(result);
    ListView_DeleteAllItems(controls_.metadataTable);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        setListViewText(controls_.metadataTable, i, 0, rows[i].section);
        setListViewText(controls_.metadataTable, i, 1, rows[i].metric);
        setListViewText(controls_.metadataTable, i, 2, rows[i].value);
    }
}

}  // namespace wolfie::ui
