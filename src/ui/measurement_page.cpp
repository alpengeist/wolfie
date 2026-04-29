#include "ui/measurement_page.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <cwchar>

#include <commctrl.h>
#include <commdlg.h>

#include "core/text_utils.h"
#include "measurement/response_analyzer.h"
#include "measurement/sweep_generator.h"
#include "measurement/waterfall_builder.h"
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

std::optional<std::filesystem::path> pickCalibrationFile(HWND owner, const std::filesystem::path& initialPath) {
    std::wstring buffer(32768, L'\0');
    const std::wstring initialText = initialPath.wstring();
    if (!initialText.empty()) {
        initialText.copy(buffer.data(), std::min(initialText.size(), buffer.size() - 1));
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter =
        L"Calibration Files (*.txt;*.cal;*.csv)\0*.txt;*.cal;*.csv\0"
        L"All Files (*.*)\0*.*\0\0";
    dialog.lpstrTitle = L"Select Microphone Calibration File";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }

    buffer.resize(wcslen(buffer.c_str()));
    return std::filesystem::path(buffer);
}

bool hasValidMicrophoneCalibration(const AudioSettings& audio) {
    return audio.microphoneCalibrationFrequencyHz.size() >= 2 &&
           audio.microphoneCalibrationFrequencyHz.size() == audio.microphoneCalibrationCorrectionDb.size();
}

std::vector<double> buildMonoReferenceValues(const MeasurementValueSet& valueSet) {
    std::vector<double> values;
    const size_t count = std::min({valueSet.xValues.size(), valueSet.leftValues.size(), valueSet.rightValues.size()});
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        values.push_back((valueSet.leftValues[index] + valueSet.rightValues[index]) * 0.5);
    }
    return values;
}

double interpolateLinear(double x, double x0, double y0, double x1, double y1) {
    if (std::abs(x1 - x0) < 1.0e-9) {
        return y0;
    }
    const double t = (x - x0) / (x1 - x0);
    return y0 + (t * (y1 - y0));
}

std::vector<double> resampleLogFrequency(const std::vector<double>& sourceAxisHz,
                                         const std::vector<double>& sourceValues,
                                         const std::vector<double>& targetAxisHz) {
    std::vector<double> resampled;
    if (sourceAxisHz.empty() || sourceValues.size() != sourceAxisHz.size() || targetAxisHz.empty()) {
        return resampled;
    }

    resampled.reserve(targetAxisHz.size());
    for (const double frequencyHz : targetAxisHz) {
        if (frequencyHz <= sourceAxisHz.front()) {
            resampled.push_back(sourceValues.front());
            continue;
        }
        if (frequencyHz >= sourceAxisHz.back()) {
            resampled.push_back(sourceValues.back());
            continue;
        }

        const auto upper = std::lower_bound(sourceAxisHz.begin(), sourceAxisHz.end(), frequencyHz);
        if (upper == sourceAxisHz.begin()) {
            resampled.push_back(sourceValues.front());
            continue;
        }
        if (upper == sourceAxisHz.end()) {
            resampled.push_back(sourceValues.back());
            continue;
        }

        const size_t upperIndex = static_cast<size_t>(upper - sourceAxisHz.begin());
        const size_t lowerIndex = upperIndex - 1;
        const double x = std::log10(std::max(frequencyHz, 1.0));
        const double x0 = std::log10(std::max(sourceAxisHz[lowerIndex], 1.0));
        const double x1 = std::log10(std::max(sourceAxisHz[upperIndex], 1.0));
        resampled.push_back(interpolateLinear(x, x0, sourceValues[lowerIndex], x1, sourceValues[upperIndex]));
    }
    return resampled;
}

std::wstring referenceStaleReason(const AudioSettings& audio,
                                  const MeasurementSettings& measurement,
                                  const MeasurementResult& referenceResult) {
    if (!referenceResult.hasAnyValues()) {
        return L"missing";
    }
    if (!audio.loopbackEnabled) {
        return L"loopback off";
    }

    const MeasurementAnalysis& analysis = referenceResult.analysis;
    if (analysis.sampleRate > 0 && analysis.sampleRate != measurement.sampleRate) {
        return L"sample rate changed";
    }
    if (!analysis.requestedBackend.empty() && analysis.requestedBackend != audio.backend) {
        return L"backend changed";
    }
    if (audio.backend == "asio") {
        if (!analysis.requestedDriver.empty() && analysis.requestedDriver != audio.driver) {
            return L"driver changed";
        }
    } else {
        if (!analysis.requestedWindowsInputDeviceId.empty() &&
            analysis.requestedWindowsInputDeviceId != audio.windowsInputDeviceId) {
            return L"input device changed";
        }
        if (!analysis.requestedWindowsOutputDeviceId.empty() &&
            analysis.requestedWindowsOutputDeviceId != audio.windowsOutputDeviceId) {
            return L"output device changed";
        }
    }
    if (analysis.requestedMicInputChannel > 0 && analysis.requestedMicInputChannel != audio.loopbackInputChannel) {
        return L"loopback input changed";
    }
    if (analysis.requestedLeftOutputChannel > 0 && analysis.requestedLeftOutputChannel != audio.leftOutputChannel) {
        return L"left output changed";
    }
    if (analysis.requestedRightOutputChannel > 0 && analysis.requestedRightOutputChannel != audio.rightOutputChannel) {
        return L"right output changed";
    }
    return {};
}

void drawDisclosureTriangle(HDC hdc, const RECT& rect, bool collapsed, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    POINT points[3]{};
    if (collapsed) {
        points[0] = {centerX - 3, centerY - 5};
        points[1] = {centerX + 4, centerY};
        points[2] = {centerX - 3, centerY + 5};
    } else {
        points[0] = {centerX - 5, centerY - 2};
        points[1] = {centerX + 5, centerY - 2};
        points[2] = {centerX, centerY + 4};
    }

    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HBRUSH brush = CreateSolidBrush(color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, brush));
    Polygon(hdc, points, 3);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void drawLegendFrame(const DRAWITEMSTRUCT& draw) {
    HBRUSH backgroundBrush = CreateSolidBrush(ui_theme::kBackground);
    FillRect(draw.hDC, &draw.rcItem, backgroundBrush);
    DeleteObject(backgroundBrush);

    const int savedDc = SaveDC(draw.hDC);
    SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
    HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
    SelectObject(draw.hDC, borderPen);
    Rectangle(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right, draw.rcItem.bottom);
    RestoreDC(draw.hDC, savedDc);
    DeleteObject(borderPen);
}

void drawLegendLineSample(const DRAWITEMSTRUCT& draw, COLORREF color, bool dashed) {
    HBRUSH backgroundBrush = CreateSolidBrush(ui_theme::kBackground);
    FillRect(draw.hDC, &draw.rcItem, backgroundBrush);
    DeleteObject(backgroundBrush);

    const int savedDc = SaveDC(draw.hDC);
    HPEN pen = CreatePen(dashed ? PS_DASH : PS_SOLID, 1, color);
    SelectObject(draw.hDC, pen);
    const int centerY = (draw.rcItem.top + draw.rcItem.bottom) / 2;
    MoveToEx(draw.hDC, draw.rcItem.left, centerY, nullptr);
    LineTo(draw.hDC, draw.rcItem.right, centerY);
    RestoreDC(draw.hDC, savedDc);
    DeleteObject(pen);
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
    controls_.labelMicCalibration = CreateWindowW(L"STATIC", L"Mic calibration", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editMicCalibrationPath = CreateWindowExW(WS_EX_CLIENTEDGE,
                                                       L"EDIT",
                                                       L"",
                                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                                       0,
                                                       0,
                                                       0,
                                                       0,
                                                       window_,
                                                       nullptr,
                                                       instance_,
                                                       nullptr);
    controls_.buttonMicCalibrationBrowse = CreateWindowW(L"BUTTON",
                                                         L"Browse...",
                                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                         0,
                                                         0,
                                                         0,
                                                         0,
                                                         window_,
                                                         reinterpret_cast<HMENU>(kButtonMicCalibrationBrowse),
                                                         instance_,
                                                         nullptr);
    controls_.buttonMicCalibrationClear = CreateWindowW(L"BUTTON",
                                                         L"Clear",
                                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                        0,
                                                        0,
                                                        0,
                                                        0,
                                                        window_,
                                                         reinterpret_cast<HMENU>(kButtonMicCalibrationClear),
                                                         instance_,
                                                         nullptr);
    controls_.calibrationActivityLabel = CreateWindowW(L"STATIC",
                                                       L"Reanalysis in progress",
                                                       WS_CHILD,
                                                       0,
                                                       0,
                                                       0,
                                                       0,
                                                       window_,
                                                       nullptr,
                                                       instance_,
                                                       nullptr);
    controls_.calibrationActivityBar = CreateWindowExW(0,
                                                       PROGRESS_CLASSW,
                                                       nullptr,
                                                       WS_CHILD | WS_VISIBLE,
                                                       0,
                                                       0,
                                                       0,
                                                       0,
                                                       window_,
                                                       nullptr,
                                                       instance_,
                                                       nullptr);
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
    controls_.buttonMeasureReference = CreateWindowW(L"BUTTON",
                                                     L"REFERENCE",
                                                     WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     window_,
                                                     reinterpret_cast<HMENU>(kButtonMeasureReference),
                                                     instance_,
                                                     nullptr);
    controls_.leftChannelLabel = CreateWindowW(L"STATIC", L"Left", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.leftProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.leftProgressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightChannelLabel = CreateWindowW(L"STATIC", L"Right", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightProgressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.currentFrequency = CreateWindowW(L"STATIC", L"Freq 0 Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.currentAmplitude = CreateWindowW(L"STATIC", L"Amp -90 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.peakAmplitude = CreateWindowW(L"STATIC", L"Peak -90 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelPlot = CreateWindowW(L"STATIC", L"Plot", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboPlot = CreateWindowW(L"COMBOBOX",
                                        nullptr,
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                        0,
                                        0,
                                        0,
                                        0,
                                        window_,
                                        reinterpret_cast<HMENU>(kComboPlot),
                                        instance_,
                                        nullptr);
    controls_.labelWaterfallChannel = CreateWindowW(L"STATIC", L"Channel", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboWaterfallChannel = CreateWindowW(L"COMBOBOX",
                                                    nullptr,
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    window_,
                                                    reinterpret_cast<HMENU>(kComboWaterfallChannel),
                                                    instance_,
                                                    nullptr);
    controls_.infoStatusFrame = CreateWindowExW(0,
                                              L"STATIC",
                                              L"",
                                              WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME,
                                              0,
                                              0,
                                              0,
                                              0,
                                              window_,
                                              nullptr,
                                              instance_,
                                              nullptr);
    controls_.referenceStatus = CreateWindowW(L"STATIC", L"Reference: missing", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.micCompStatus = CreateWindowW(L"STATIC", L"Mic compensation: none", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.responseLegendFrame = CreateWindowW(L"STATIC",
                                                  L"",
                                                  WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  window_,
                                                  nullptr,
                                                  instance_,
                                                  nullptr);
    controls_.checkboxShowRoomLeft = CreateWindowW(L"BUTTON",
                                                   L"",
                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   window_,
                                                   reinterpret_cast<HMENU>(kCheckboxShowRoomLeft),
                                                   instance_,
                                                   nullptr);
    controls_.lineRoomLeft = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelRoomLeft = CreateWindowW(L"STATIC", L"Room Left", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowRoomRight = CreateWindowW(L"BUTTON",
                                                    L"",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    window_,
                                                    reinterpret_cast<HMENU>(kCheckboxShowRoomRight),
                                                    instance_,
                                                    nullptr);
    controls_.lineRoomRight = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelRoomRight = CreateWindowW(L"STATIC", L"Room Right", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.checkboxShowReference = CreateWindowW(L"BUTTON",
                                                    L"",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    window_,
                                                    reinterpret_cast<HMENU>(kCheckboxShowReference),
                                                    instance_,
                                                    nullptr);
    controls_.lineReference = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelReference = CreateWindowW(L"STATIC", L"Reference", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.metadataLabel = CreateWindowW(L"STATIC", L"Measurement Metadata", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.metadataToggle = CreateWindowW(L"BUTTON",
                                             L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                             0,
                                             0,
                                             0,
                                             0,
                                             window_,
                                             reinterpret_cast<HMENU>(kButtonMetadataToggle),
                                             instance_,
                                             nullptr);
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
    waterfallGraph_.create(window_, instance_);
    ShowWindow(responseGraph_.window(), SW_HIDE);
    ShowWindow(waterfallGraph_.window(), SW_HIDE);

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
    SetWindowLongPtrW(controls_.labelPlot, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelWaterfallChannel, GWL_STYLE, centeredStaticStyle);
    SendMessageW(controls_.editEndFrequency, EM_SETREADONLY, TRUE, 0);
    SendMessageW(controls_.checkboxShowRoomLeft, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(controls_.checkboxShowRoomRight, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(controls_.checkboxShowReference, BM_SETCHECK, BST_CHECKED, 0);

    SendMessageW(controls_.leftProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    SendMessageW(controls_.rightProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    SendMessageW(controls_.calibrationActivityBar, PBM_SETRANGE32, 0, 100);
    ShowWindow(controls_.calibrationActivityLabel, SW_HIDE);
    ShowWindow(controls_.calibrationActivityBar, SW_HIDE);
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
    populatePlotCombo(controls_.comboPlot);
    populateWaterfallChannelCombo(controls_.comboWaterfallChannel);
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
    constexpr int kMetricWidth = 118;
    constexpr int kMetricGap = 10;
    constexpr int kSliderWidth = 220;
    constexpr int kSliderValueWidth = 56;
    constexpr int kGraphComboWidth = 150;
    constexpr int kGraphComboHeight = 220;
    constexpr int kLegendGap = 14;
    constexpr int kLegendWidth = 172;
    constexpr int kInfoBoxHeight = 48;
    constexpr int kCalibrationLabelWidth = 104;
    constexpr int kCalibrationBrowseWidth = 74;
    constexpr int kCalibrationClearWidth = 58;
    constexpr int kCalibrationGap = 10;

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

    const int calibrationTop = contentTop;
    const int calibrationFieldLeft = contentLeft + kCalibrationLabelWidth + 12;
    const int calibrationButtonsWidth = kCalibrationBrowseWidth + kCalibrationGap + kCalibrationClearWidth;
    const int calibrationFieldWidth = std::max(180, innerWidth - kCalibrationLabelWidth - 12 - calibrationButtonsWidth - (kCalibrationGap * 2));
    MoveWindow(controls_.labelMicCalibration, contentLeft, calibrationTop + 4, kCalibrationLabelWidth, 20, TRUE);
    MoveWindow(controls_.editMicCalibrationPath, calibrationFieldLeft, calibrationTop, calibrationFieldWidth, 24, TRUE);
    MoveWindow(controls_.buttonMicCalibrationBrowse,
               calibrationFieldLeft + calibrationFieldWidth + kCalibrationGap,
               calibrationTop - 2,
               kCalibrationBrowseWidth,
               28,
               TRUE);
    MoveWindow(controls_.buttonMicCalibrationClear,
               calibrationFieldLeft + calibrationFieldWidth + kCalibrationGap + kCalibrationBrowseWidth + kCalibrationGap,
               calibrationTop - 2,
               kCalibrationClearWidth,
               28,
               TRUE);
    MoveWindow(controls_.calibrationActivityLabel, calibrationFieldLeft, calibrationTop + 32, 260, 18, TRUE);
    MoveWindow(controls_.calibrationActivityBar, calibrationFieldLeft, calibrationTop + 52, calibrationFieldWidth + calibrationButtonsWidth + kCalibrationGap, 16, TRUE);

    const int paramsTop = calibrationTop + 74;
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
    constexpr int kReferenceButtonWidth = 138;
    MoveWindow(controls_.buttonMeasure, contentLeft, buttonTop, kButtonWidth, buttonHeight, TRUE);
    MoveWindow(controls_.buttonMeasureReference,
               contentLeft + kButtonWidth + 10,
               buttonTop,
               kReferenceButtonWidth,
               buttonHeight,
               TRUE);
    int metricLeft = contentLeft + kButtonWidth + 10 + kReferenceButtonWidth + kMetricGap;
    MoveWindow(controls_.currentFrequency, metricLeft, dataRowTop + 4, kMetricWidth, 20, TRUE);
    metricLeft += kMetricWidth + kMetricGap;
    MoveWindow(controls_.currentAmplitude, metricLeft, dataRowTop + 4, kMetricWidth, 20, TRUE);
    metricLeft += kMetricWidth + kMetricGap;
    MoveWindow(controls_.peakAmplitude, metricLeft, dataRowTop + 4, kMetricWidth, 20, TRUE);

    metricLeft = contentLeft + kButtonWidth + 10 + kReferenceButtonWidth + kMetricGap;
    MoveWindow(controls_.leftChannelLabel, metricLeft, progressRowTop + 2, kProgressLabelWidth, 18, TRUE);
    MoveWindow(controls_.leftProgressBar, metricLeft + kProgressLabelWidth + 8, progressRowTop + 4, kProgressBarWidth, 16, TRUE);
    MoveWindow(controls_.leftProgressText, metricLeft + kProgressLabelWidth + 8 + kProgressBarWidth + 8, progressRowTop, kProgressTextWidth, 20, TRUE);
    metricLeft += kProgressLabelWidth + 8 + kProgressBarWidth + 8 + kProgressTextWidth + kMetricGap;
    MoveWindow(controls_.rightChannelLabel, metricLeft, progressRowTop + 2, kProgressLabelWidth, 18, TRUE);
    MoveWindow(controls_.rightProgressBar, metricLeft + kProgressLabelWidth + 8, progressRowTop + 4, kProgressBarWidth, 16, TRUE);
    MoveWindow(controls_.rightProgressText, metricLeft + kProgressLabelWidth + 8 + kProgressBarWidth + 8, progressRowTop, kProgressTextWidth, 20, TRUE);

    const int graphControlsTop = progressRowTop + 30;
    MoveWindow(controls_.labelPlot, contentLeft, graphControlsTop + 2, 44, 18, TRUE);
    MoveWindow(controls_.comboPlot, contentLeft + 52, graphControlsTop - 2, kGraphComboWidth, kGraphComboHeight, TRUE);
    MoveWindow(controls_.labelWaterfallChannel, contentLeft + 220, graphControlsTop + 2, 56, 18, TRUE);
    MoveWindow(controls_.comboWaterfallChannel,
               contentLeft + 286,
               graphControlsTop - 2,
               110,
               kGraphComboHeight,
               TRUE);
    const int infoBoxWidth = std::clamp(innerWidth / 4, 260, 340);
    const int infoBoxLeft = contentLeft + innerWidth - infoBoxWidth;
    const int infoBoxTop = paramsTop;
    MoveWindow(controls_.infoStatusFrame, infoBoxLeft, infoBoxTop, infoBoxWidth, kInfoBoxHeight, TRUE);
    MoveWindow(controls_.referenceStatus, infoBoxLeft + 14, infoBoxTop + 10, infoBoxWidth - 28, 16, TRUE);
    MoveWindow(controls_.micCompStatus, infoBoxLeft + 14, infoBoxTop + 26, infoBoxWidth - 28, 16, TRUE);

    const int graphTop = graphControlsTop + 56;
    const int availableBottom = contentTop + innerHeight;
    const int metadataSectionHeight = metadataCollapsed_ ? 26 : std::max(150, (availableBottom - graphTop) * 9 / 20);
    const int metadataLabelTop = availableBottom - metadataSectionHeight;
    const int graphBottom = std::max(graphTop + 200, metadataLabelTop - 10);
    const bool waterfallVisible =
        plotModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPlot, CB_GETCURSEL, 0, 0))) == "waterfall";
    const int legendLeft = contentLeft + innerWidth - kLegendWidth;
    const int graphRight = waterfallVisible ? (contentLeft + innerWidth) : (legendLeft - kLegendGap);
    const RECT graphBounds{contentLeft, graphTop, graphRight, graphBottom};
    responseGraph_.layout(graphBounds);
    waterfallGraph_.layout(graphBounds);
    const int legendHeight = std::max(116, graphBottom - graphTop);
    MoveWindow(controls_.responseLegendFrame, legendLeft, graphTop, kLegendWidth, legendHeight, TRUE);
    const int checkboxLeft = legendLeft + 14;
    const int lineLeft = checkboxLeft + 26;
    const int labelLeft = lineLeft + 34;
    const int rowTop = graphTop + 14;
    const int rowStep = 28;
    MoveWindow(controls_.checkboxShowRoomLeft, checkboxLeft, rowTop, 18, 20, TRUE);
    MoveWindow(controls_.lineRoomLeft, lineLeft, rowTop + 9, 24, 4, TRUE);
    MoveWindow(controls_.labelRoomLeft, labelLeft, rowTop, kLegendWidth - (labelLeft - legendLeft) - 12, 20, TRUE);
    MoveWindow(controls_.checkboxShowRoomRight, checkboxLeft, rowTop + rowStep, 18, 20, TRUE);
    MoveWindow(controls_.lineRoomRight, lineLeft, rowTop + rowStep + 9, 24, 4, TRUE);
    MoveWindow(controls_.labelRoomRight,
               labelLeft,
               rowTop + rowStep,
               kLegendWidth - (labelLeft - legendLeft) - 12,
               20,
               TRUE);
    MoveWindow(controls_.checkboxShowReference, checkboxLeft, rowTop + (rowStep * 2), 18, 20, TRUE);
    MoveWindow(controls_.lineReference, lineLeft, rowTop + (rowStep * 2) + 9, 24, 4, TRUE);
    MoveWindow(controls_.labelReference,
               labelLeft,
               rowTop + (rowStep * 2),
               kLegendWidth - (labelLeft - legendLeft) - 12,
               20,
               TRUE);
    constexpr int kMetadataToggleSize = 24;
    MoveWindow(controls_.metadataLabel, contentLeft, metadataLabelTop, std::max(160, innerWidth - 36), 20, TRUE);
    MoveWindow(controls_.metadataToggle,
               contentLeft + innerWidth - kMetadataToggleSize,
               metadataLabelTop - 2,
               kMetadataToggleSize,
               kMetadataToggleSize,
               TRUE);
    MoveWindow(controls_.metadataTable,
               contentLeft,
               metadataLabelTop + 24,
               innerWidth,
               metadataCollapsed_ ? 0 : std::max(120, availableBottom - (metadataLabelTop + 24)),
               TRUE);
    const int sectionWidth = std::clamp(innerWidth / 7, 120, 180);
    const int metricWidth = std::clamp(innerWidth / 4, 180, 260);
    const int valueWidth = std::max(240, innerWidth - sectionWidth - metricWidth - 28);
    ListView_SetColumnWidth(controls_.metadataTable, 0, sectionWidth);
    ListView_SetColumnWidth(controls_.metadataTable, 1, metricWidth);
    ListView_SetColumnWidth(controls_.metadataTable, 2, valueWidth);
    updateMetadataVisibility();
    if (window_ != nullptr) {
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

void MeasurementPage::setVisible(bool visible) const {
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
}

void MeasurementPage::populate(const WorkspaceState& workspace) {
    setWindowTextValue(controls_.editMicCalibrationPath, workspace.audio.microphoneCalibrationPath.wstring());
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
    responseGraph_.setVisibleFrequencyRange(workspace.ui.measurementGraphHasCustomFrequencyRange,
                                            workspace.ui.measurementGraphVisibleMinFrequencyHz,
                                            workspace.ui.measurementGraphVisibleMaxFrequencyHz);
    SendMessageW(controls_.comboPlot, CB_SETCURSEL, comboIndexFromPlotMode(workspace.ui.measurementPlotMode), 0);
    SendMessageW(controls_.comboWaterfallChannel,
                 CB_SETCURSEL,
                 comboIndexFromWaterfallChannel(workspace.ui.measurementWaterfallChannel),
                 0);
    showRoomLeft_ = workspace.ui.measurementShowRoomLeft;
    showRoomRight_ = workspace.ui.measurementShowRoomRight;
    showReference_ = workspace.ui.measurementShowReference;
    SendMessageW(controls_.checkboxShowRoomLeft, BM_SETCHECK, showRoomLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowRoomRight, BM_SETCHECK, showRoomRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowReference, BM_SETCHECK, showReference_ ? BST_CHECKED : BST_UNCHECKED, 0);
    metadataCollapsed_ = workspace.ui.measurementMetadataCollapsed;
    updateMetadataVisibility();
    setWorkspaceView(workspace);
}

void MeasurementPage::syncToWorkspace(WorkspaceState& workspace) const {
    workspace.audio.microphoneCalibrationPath = std::filesystem::path(getWindowTextValue(controls_.editMicCalibrationPath));
    workspace.measurement.sampleRate = measurementSampleRateFromComboIndex(static_cast<int>(SendMessageW(controls_.comboSampleRate, CB_GETCURSEL, 0, 0)));
    workspace.measurement.fadeInSeconds = std::stod(getWindowTextValue(controls_.editFadeIn));
    workspace.measurement.fadeOutSeconds = std::stod(getWindowTextValue(controls_.editFadeOut));
    workspace.measurement.durationSeconds = std::stod(getWindowTextValue(controls_.editDuration));
    workspace.measurement.startFrequencyHz = std::stod(getWindowTextValue(controls_.editStartFrequency));
    workspace.measurement.targetLengthSamples = std::stoi(getWindowTextValue(controls_.editTargetLength));
    workspace.measurement.leadInSamples = std::stoi(getWindowTextValue(controls_.editLeadIn));
    workspace.ui.measurementGraphExtraRangeDb = 0.0;
    workspace.ui.measurementGraphVerticalOffsetDb = 0.0;
    workspace.ui.measurementGraphHasCustomFrequencyRange = responseGraph_.hasCustomVisibleFrequencyRange();
    workspace.ui.measurementGraphVisibleMinFrequencyHz = responseGraph_.visibleMinFrequencyHz();
    workspace.ui.measurementGraphVisibleMaxFrequencyHz = responseGraph_.visibleMaxFrequencyHz();
    workspace.ui.measurementPlotMode =
        plotModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPlot, CB_GETCURSEL, 0, 0)));
    workspace.ui.measurementWaterfallChannel = waterfallChannelFromComboIndex(
        static_cast<int>(SendMessageW(controls_.comboWaterfallChannel, CB_GETCURSEL, 0, 0)));
    workspace.ui.measurementShowRoomLeft = SendMessageW(controls_.checkboxShowRoomLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    workspace.ui.measurementShowRoomRight = SendMessageW(controls_.checkboxShowRoomRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    workspace.ui.measurementShowReference = SendMessageW(controls_.checkboxShowReference, BM_GETCHECK, 0, 0) == BST_CHECKED;
    workspace.ui.measurementMetadataCollapsed = metadataCollapsed_;
    measurement::syncDerivedMeasurementSettings(workspace.measurement);
}

void MeasurementPage::setWorkspaceView(const WorkspaceState& workspace) {
    audioSettings_ = workspace.audio;
    measurementSettings_ = workspace.measurement;
    result_ = workspace.result;
    referenceResult_ = workspace.referenceResult;
    refreshReferenceStatusLabels();
    refreshActionButtons();
    refreshPlots();
    populateMetadataTable(result_);
}

void MeasurementPage::setCalibrationRefreshInProgress(bool inProgress,
                                                      int currentStep,
                                                      int totalSteps,
                                                      const std::wstring& statusText) {
    calibrationRefreshInProgress_ = inProgress;
    if (!inProgress) {
        ShowWindow(controls_.calibrationActivityLabel, SW_HIDE);
        ShowWindow(controls_.calibrationActivityBar, SW_HIDE);
        SetWindowTextW(controls_.calibrationActivityLabel, L"");
        SendMessageW(controls_.calibrationActivityBar, PBM_SETPOS, 0, 0);
        setInteractiveControlsEnabled(true);
        refreshActionButtons();
        return;
    }

    const int safeTotalSteps = std::max(1, totalSteps);
    const int safeCurrentStep = std::clamp(currentStep, 0, safeTotalSteps);
    std::wstring text = statusText;
    if (text.empty()) {
        text = L"Working...";
    }
    SetWindowTextW(controls_.calibrationActivityLabel,
                   (L"Reanalysis " + std::to_wstring(safeCurrentStep) + L"/" + std::to_wstring(safeTotalSteps) + L": " + text).c_str());
    SendMessageW(controls_.calibrationActivityBar, PBM_SETRANGE32, 0, safeTotalSteps);
    SendMessageW(controls_.calibrationActivityBar, PBM_SETPOS, safeCurrentStep, 0);
    ShowWindow(controls_.calibrationActivityLabel, SW_SHOW);
    ShowWindow(controls_.calibrationActivityBar, SW_SHOW);
    setInteractiveControlsEnabled(false);
    refreshActionButtons();
}

void MeasurementPage::refreshStatus(const MeasurementStatus& status, bool hasResult) {
    status_ = status;
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
    InvalidateRect(controls_.buttonMeasureReference, nullptr, TRUE);
    refreshActionButtons();

    (void)hasResult;
}

void MeasurementPage::setInteractiveControlsEnabled(bool enabled) const {
    EnableWindow(controls_.editFadeIn, enabled ? TRUE : FALSE);
    EnableWindow(controls_.editFadeOut, enabled ? TRUE : FALSE);
    EnableWindow(controls_.editDuration, enabled ? TRUE : FALSE);
    EnableWindow(controls_.editStartFrequency, enabled ? TRUE : FALSE);
    EnableWindow(controls_.editTargetLength, enabled ? TRUE : FALSE);
    EnableWindow(controls_.editLeadIn, enabled ? TRUE : FALSE);
    EnableWindow(controls_.comboSampleRate, enabled ? TRUE : FALSE);
    EnableWindow(controls_.outputVolumeSlider, enabled ? TRUE : FALSE);
    EnableWindow(controls_.comboPlot, enabled ? TRUE : FALSE);
    EnableWindow(controls_.comboWaterfallChannel, enabled ? TRUE : FALSE);
    EnableWindow(controls_.checkboxShowRoomLeft, enabled ? TRUE : FALSE);
    EnableWindow(controls_.checkboxShowRoomRight, enabled ? TRUE : FALSE);
    EnableWindow(controls_.checkboxShowReference, enabled ? TRUE : FALSE);
    EnableWindow(controls_.metadataToggle, enabled ? TRUE : FALSE);
    EnableWindow(responseGraph_.window(), enabled ? TRUE : FALSE);
    EnableWindow(waterfallGraph_.window(), enabled ? TRUE : FALSE);
}

void MeasurementPage::invalidateGraph() const {
    responseGraph_.invalidate();
    waterfallGraph_.invalidate();
}

bool MeasurementPage::handleDrawItem(const DRAWITEMSTRUCT* draw) const {
    if (draw == nullptr) {
        return false;
    }

    if (draw->hwndItem == controls_.responseLegendFrame) {
        drawLegendFrame(*draw);
        return true;
    }

    if (draw->hwndItem == controls_.lineRoomLeft) {
        drawLegendLineSample(*draw, ui_theme::kGreen, false);
        return true;
    }
    if (draw->hwndItem == controls_.lineRoomRight) {
        drawLegendLineSample(*draw, ui_theme::kRed, false);
        return true;
    }
    if (draw->hwndItem == controls_.lineReference) {
        drawLegendLineSample(*draw, ui_theme::kAccent, false);
        return true;
    }

    if (draw->CtlID == kButtonMetadataToggle) {
        HDC hdc = draw->hDC;
        RECT rect = draw->rcItem;
        const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
        const bool focused = (draw->itemState & ODS_FOCUS) != 0;
        const bool hot = (draw->itemState & ODS_HOTLIGHT) != 0;
        const COLORREF fill = pressed ? RGB(221, 228, 236) : (hot ? RGB(233, 239, 245) : ui_theme::kPanelBackground);
        const COLORREF border = pressed ? RGB(177, 187, 199) : ui_theme::kBorder;

        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(hdc, &rect, brush);
        DeleteObject(brush);

        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
        HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
        Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        drawDisclosureTriangle(hdc, rect, metadataCollapsed_, ui_theme::kMuted);

        if (focused) {
            RECT focusRect = rect;
            InflateRect(&focusRect, -3, -3);
            DrawFocusRect(hdc, &focusRect);
        }
        return true;
    }

    if (draw->CtlID != kButtonMeasure && draw->CtlID != kButtonMeasureReference) {
        return false;
    }

    HDC hdc = draw->hDC;
    RECT rect = draw->rcItem;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool enabled = IsWindowEnabled(draw->hwndItem) != FALSE;
    const bool isReferenceButton = draw->CtlID == kButtonMeasureReference;
    const bool activeRun = status_.running && (status_.runMode == (isReferenceButton ? MeasurementRunMode::Reference
                                                                                      : MeasurementRunMode::Room));
    const bool inactiveDuringOtherRun = status_.running && !activeRun;
    const COLORREF fill = !enabled ? RGB(186, 192, 200)
                                   : (inactiveDuringOtherRun ? RGB(162, 170, 180)
                                                             : (activeRun ? ui_theme::kRed
                                                                          : (isReferenceButton ? ui_theme::kAccent
                                                                                               : ui_theme::kGreen)));
    const COLORREF fillPressed = !enabled ? fill
                                          : (activeRun ? RGB(156, 53, 53)
                                                       : (isReferenceButton ? RGB(35, 90, 149) : RGB(37, 118, 68)));
    const COLORREF border = !enabled ? RGB(156, 163, 172)
                                     : (inactiveDuringOtherRun ? RGB(138, 146, 156)
                                                               : (activeRun ? RGB(132, 42, 42)
                                                                            : (isReferenceButton ? RGB(29, 76, 126)
                                                                                                 : RGB(29, 95, 55))));

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
    const wchar_t* buttonText = isReferenceButton
                                    ? (activeRun ? L"STOP" : L"REFERENCE")
                                    : (activeRun ? L"STOP" : L"START");
    DrawTextW(hdc, buttonText, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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
                                    bool& roomMeasurePressed,
                                    bool& referenceMeasurePressed,
                                    bool& microphoneCalibrationChanged,
                                    bool& sampleRateChanged,
                                    bool& graphZoomChanged,
                                    bool& plotSelectionChanged) {
    if (commandId == kButtonMeasure) {
        roomMeasurePressed = true;
        return true;
    }

    if (commandId == kButtonMeasureReference) {
        referenceMeasurePressed = true;
        return true;
    }

    if (commandId == kButtonMicCalibrationBrowse && notificationCode == BN_CLICKED) {
        if (const auto selected = pickCalibrationFile(window_, workspace.audio.microphoneCalibrationPath)) {
            if (*selected != workspace.audio.microphoneCalibrationPath) {
                workspace.audio.microphoneCalibrationPath = *selected;
                workspace.audio.microphoneCalibrationFrequencyHz.clear();
                workspace.audio.microphoneCalibrationCorrectionDb.clear();
                setWindowTextValue(controls_.editMicCalibrationPath, selected->wstring());
                microphoneCalibrationChanged = true;
            }
        }
        return true;
    }

    if (commandId == kButtonMicCalibrationClear && notificationCode == BN_CLICKED) {
        if (!workspace.audio.microphoneCalibrationPath.empty() ||
            !workspace.audio.microphoneCalibrationFrequencyHz.empty() ||
            !workspace.audio.microphoneCalibrationCorrectionDb.empty()) {
            workspace.audio.microphoneCalibrationPath.clear();
            workspace.audio.microphoneCalibrationFrequencyHz.clear();
            workspace.audio.microphoneCalibrationCorrectionDb.clear();
            setWindowTextValue(controls_.editMicCalibrationPath, L"");
            microphoneCalibrationChanged = true;
        }
        return true;
    }

    if (commandId == kResponseGraph && notificationCode == ResponseGraph::kZoomChangedNotification) {
        workspace.ui.measurementGraphExtraRangeDb = 0.0;
        workspace.ui.measurementGraphVerticalOffsetDb = 0.0;
        workspace.ui.measurementGraphHasCustomFrequencyRange = responseGraph_.hasCustomVisibleFrequencyRange();
        workspace.ui.measurementGraphVisibleMinFrequencyHz = responseGraph_.visibleMinFrequencyHz();
        workspace.ui.measurementGraphVisibleMaxFrequencyHz = responseGraph_.visibleMaxFrequencyHz();
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

    if (commandId == kButtonMetadataToggle && notificationCode == BN_CLICKED) {
        metadataCollapsed_ = !metadataCollapsed_;
        updateMetadataVisibility();
        layout();
        syncToWorkspace(workspace);
        plotSelectionChanged = true;
        return true;
    }

    if ((commandId == kComboPlot || commandId == kComboWaterfallChannel) && notificationCode == CBN_SELCHANGE) {
        if (commandId == kComboPlot) {
            updatePlotControlVisibility();
        }
        syncToWorkspace(workspace);
        refreshPlots();
        plotSelectionChanged = true;
        return true;
    }

    if ((commandId == kCheckboxShowRoomLeft ||
         commandId == kCheckboxShowRoomRight ||
         commandId == kCheckboxShowReference) &&
        notificationCode == BN_CLICKED) {
        showRoomLeft_ = SendMessageW(controls_.checkboxShowRoomLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showRoomRight_ = SendMessageW(controls_.checkboxShowRoomRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showReference_ = SendMessageW(controls_.checkboxShowReference, BM_GETCHECK, 0, 0) == BST_CHECKED;
        syncToWorkspace(workspace);
        refreshPlots();
        plotSelectionChanged = true;
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

void MeasurementPage::populatePlotCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Magnitude"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Waterfall"));
}

int MeasurementPage::comboIndexFromPlotMode(const std::string& plotMode) {
    if (plotMode == "waterfall") {
        return 1;
    }
    return 0;
}

std::string MeasurementPage::plotModeFromComboIndex(int index) {
    return index == 1 ? "waterfall" : "magnitude";
}

void MeasurementPage::populateWaterfallChannelCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Left"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Right"));
}

int MeasurementPage::comboIndexFromWaterfallChannel(const std::string& channel) {
    if (channel == "right") {
        return 1;
    }
    return 0;
}

std::string MeasurementPage::waterfallChannelFromComboIndex(int index) {
    return index == 1 ? "right" : "left";
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

std::wstring MeasurementPage::referenceStatusText(const AudioSettings& audio,
                                                  const MeasurementSettings& measurement,
                                                  const MeasurementResult& referenceResult) {
    const std::wstring staleReason = referenceStaleReason(audio, measurement, referenceResult);
    if (staleReason == L"missing") {
        return L"Reference: missing";
    }

    std::wstring text = L"Reference: ";
    if (staleReason.empty()) {
        text += L"current";
    } else {
        text += L"stale (" + staleReason + L")";
    }

    if (!referenceResult.analysis.measurementTimestampUtc.empty()) {
        text += L" - " + toWide(referenceResult.analysis.measurementTimestampUtc);
    }
    return text;
}

std::wstring MeasurementPage::microphoneCompStatusText(const AudioSettings& audio) {
    if (audio.microphoneCalibrationPath.empty()) {
        return L"Mic calibration: none";
    }
    if (!hasValidMicrophoneCalibration(audio)) {
        return L"Mic calibration: invalid";
    }
    return L"Mic calibration: loaded";
}

ResponseGraphData MeasurementPage::buildGraphData() const {
    ResponseGraphData data;
    const MeasurementValueSet* magnitudeResponse = result_.magnitudeResponse();
    if (magnitudeResponse != nullptr && magnitudeResponse->valid() &&
        (showRoomLeft_ || showRoomRight_)) {
        data.frequencyAxisHz = magnitudeResponse->xValues;
        if (showRoomLeft_) {
            data.series.push_back({L"Room Left", ui_theme::kGreen, false, magnitudeResponse->leftValues});
        }
        if (showRoomRight_) {
            data.series.push_back({L"Room Right", ui_theme::kRed, false, magnitudeResponse->rightValues});
        }
    }

    const MeasurementValueSet* referenceResponse = referenceResult_.magnitudeResponse();
    if (showReference_ && referenceResponse != nullptr && referenceResponse->valid()) {
        if (data.frequencyAxisHz.empty()) {
            data.frequencyAxisHz = referenceResponse->xValues;
        }
        std::vector<double> referenceValues = buildMonoReferenceValues(*referenceResponse);
        if (!referenceValues.empty()) {
            if (referenceResponse->xValues != data.frequencyAxisHz) {
                referenceValues = resampleLogFrequency(referenceResponse->xValues, referenceValues, data.frequencyAxisHz);
            }
        }
        if (!referenceValues.empty()) {
            data.series.push_back({L"Reference", ui_theme::kAccent, false, std::move(referenceValues)});
        }
    }
    return data;
}

void MeasurementPage::refreshPlots() {
    responseGraph_.setData(buildGraphData());

    const std::string channelSelection = waterfallChannelFromComboIndex(
        static_cast<int>(SendMessageW(controls_.comboWaterfallChannel, CB_GETCURSEL, 0, 0)));
    const MeasurementChannel channel =
        channelSelection == "right" ? MeasurementChannel::Right : MeasurementChannel::Left;
    waterfallGraph_.setData(measurement::buildWaterfallPlotData(result_, channel));

    updatePlotControlVisibility();
}

void MeasurementPage::updatePlotControlVisibility() const {
    const std::string plotMode =
        plotModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPlot, CB_GETCURSEL, 0, 0)));
    const bool waterfallVisible = plotMode == "waterfall";
    ShowWindow(responseGraph_.window(), waterfallVisible ? SW_HIDE : SW_SHOW);
    ShowWindow(waterfallGraph_.window(), waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.labelWaterfallChannel, waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.comboWaterfallChannel, waterfallVisible ? SW_SHOW : SW_HIDE);
    updateLegendVisibility();
}

void MeasurementPage::updateLegendVisibility() const {
    const std::string plotMode =
        plotModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPlot, CB_GETCURSEL, 0, 0)));
    const bool showLegend = plotMode != "waterfall";
    const bool hasReference = referenceResult_.magnitudeResponse() != nullptr;
    ShowWindow(controls_.responseLegendFrame, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.checkboxShowRoomLeft, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.lineRoomLeft, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.labelRoomLeft, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.checkboxShowRoomRight, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.lineRoomRight, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.labelRoomRight, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.checkboxShowReference, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.lineReference, showLegend ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.labelReference, showLegend ? SW_SHOW : SW_HIDE);
    EnableWindow(controls_.checkboxShowReference, hasReference ? TRUE : FALSE);
    EnableWindow(controls_.lineReference, hasReference ? TRUE : FALSE);
    EnableWindow(controls_.labelReference, hasReference ? TRUE : FALSE);
}

void MeasurementPage::updateMetadataVisibility() const {
    if (controls_.metadataToggle != nullptr) {
        SetWindowTextW(controls_.metadataToggle, metadataCollapsed_ ? L"Expand metadata" : L"Collapse metadata");
        InvalidateRect(controls_.metadataToggle, nullptr, TRUE);
    }
    if (controls_.metadataTable != nullptr) {
        ShowWindow(controls_.metadataTable, metadataCollapsed_ ? SW_HIDE : SW_SHOW);
    }
}

void MeasurementPage::refreshReferenceStatusLabels() const {
    setWindowTextValue(controls_.referenceStatus, referenceStatusText(audioSettings_, measurementSettings_, referenceResult_));
    setWindowTextValue(controls_.micCompStatus, microphoneCompStatusText(audioSettings_));
}

void MeasurementPage::refreshActionButtons() const {
    const bool running = status_.running;
    const bool busy = running || calibrationRefreshInProgress_;
    EnableWindow(controls_.buttonMicCalibrationBrowse, busy ? FALSE : TRUE);
    EnableWindow(controls_.buttonMicCalibrationClear, busy ? FALSE : TRUE);
    EnableWindow(controls_.buttonMeasure,
                 calibrationRefreshInProgress_ ? FALSE
                                               : (((running && status_.runMode == MeasurementRunMode::Room) || !running) ? TRUE : FALSE));
    EnableWindow(controls_.buttonMeasureReference,
                 (!calibrationRefreshInProgress_ &&
                  audioSettings_.loopbackEnabled &&
                  ((running && status_.runMode == MeasurementRunMode::Reference) || !running))
                     ? TRUE
                     : FALSE);
    InvalidateRect(controls_.buttonMeasure, nullptr, TRUE);
    InvalidateRect(controls_.buttonMeasureReference, nullptr, TRUE);
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
    if (const MeasurementValueSet* magnitude = result.magnitudeResponse(); magnitude != nullptr) {
        add(L"Result", L"Magnitude Response Key", toWide(magnitude->key));
        add(L"Result", L"Magnitude Response Points", std::to_wstring(magnitude->xValues.size()));
    } else {
        add(L"Result", L"Magnitude Response Key", L"-");
        add(L"Result", L"Magnitude Response Points", L"0");
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
