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
constexpr double kLevelMeterMinDb = -90.0;
constexpr double kLevelMeterMaxDb = 0.0;

std::wstring staleReferenceReason(const AudioSettings& audio,
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

std::wstring describeReferenceInterface(const MeasurementAnalysis& analysis) {
    if (analysis.requestedBackend == "asio") {
        const std::string driver = !analysis.requestedDriver.empty() ? analysis.requestedDriver : analysis.backendName;
        if (!driver.empty()) {
            return L"ASIO driver '" + toWide(driver) + L"'";
        }
        return L"ASIO";
    }

    if (analysis.requestedBackend == "windows") {
        const bool hasInput = !analysis.requestedWindowsInputDeviceName.empty();
        const bool hasOutput = !analysis.requestedWindowsOutputDeviceName.empty();
        if (hasInput && hasOutput) {
            return L"Windows input '" + toWide(analysis.requestedWindowsInputDeviceName) +
                   L"' and output '" + toWide(analysis.requestedWindowsOutputDeviceName) + L"'";
        }
        if (hasInput) {
            return L"Windows input '" + toWide(analysis.requestedWindowsInputDeviceName) + L"'";
        }
        if (hasOutput) {
            return L"Windows output '" + toWide(analysis.requestedWindowsOutputDeviceName) + L"'";
        }
        return L"Windows audio";
    }

    if (analysis.requestedBackend == "simulation") {
        if (!analysis.requestedDriver.empty()) {
            return L"simulation '" + toWide(analysis.requestedDriver) + L"'";
        }
        return L"simulation";
    }

    if (!analysis.requestedDriver.empty()) {
        return toWide(analysis.requestedDriver);
    }
    if (!analysis.requestedBackend.empty()) {
        return toWide(analysis.requestedBackend);
    }
    if (!analysis.backendName.empty()) {
        return toWide(analysis.backendName);
    }
    return L"saved audio settings";
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

void drawLegendFrame(const DRAWITEMSTRUCT& draw) {
    FillRect(draw.hDC, &draw.rcItem, ui_theme::backgroundBrush());

    const int savedDc = SaveDC(draw.hDC);
    SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
    HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
    SelectObject(draw.hDC, borderPen);
    Rectangle(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right, draw.rcItem.bottom);
    RestoreDC(draw.hDC, savedDc);
    DeleteObject(borderPen);
}

void drawLegendLineSample(const DRAWITEMSTRUCT& draw, COLORREF color, bool dashed) {
    FillRect(draw.hDC, &draw.rcItem, ui_theme::backgroundBrush());

    const int savedDc = SaveDC(draw.hDC);
    HPEN pen = CreatePen(dashed ? PS_DASH : PS_SOLID, 1, color);
    SelectObject(draw.hDC, pen);
    const int centerY = (draw.rcItem.top + draw.rcItem.bottom) / 2;
    MoveToEx(draw.hDC, draw.rcItem.left, centerY, nullptr);
    LineTo(draw.hDC, draw.rcItem.right, centerY);
    RestoreDC(draw.hDC, savedDc);
    DeleteObject(pen);
}

HFONT createUiFont(int pixelHeight, LONG weight) {
    LOGFONTW font{};
    HFONT guiFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    GetObjectW(guiFont, sizeof(font), &font);
    font.lfHeight = -pixelHeight;
    font.lfWeight = weight;
    return CreateFontIndirectW(&font);
}

void drawCenteredText(HDC hdc, RECT rect, const std::wstring& text, COLORREF color, int maxPixelHeight, LONG weight) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);

    const int minPixelHeight = 12;
    for (int pixelHeight = maxPixelHeight; pixelHeight >= minPixelHeight; --pixelHeight) {
        HFONT font = createUiFont(pixelHeight, weight);
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, font));
        RECT textRect = rect;
        DrawTextW(hdc, text.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
        const bool fits = (textRect.right - textRect.left) <= (rect.right - rect.left) &&
                          (textRect.bottom - textRect.top) <= (rect.bottom - rect.top);
        SelectObject(hdc, oldFont);
        if (fits) {
            oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, font));
            DrawTextW(hdc, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
            DeleteObject(font);
            return;
        }
        DeleteObject(font);
    }

    HFONT fallbackFont = createUiFont(minPixelHeight, weight);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, fallbackFont));
    DrawTextW(hdc, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
    DeleteObject(fallbackFont);
}

int meterXFromDb(const RECT& rect, double dbValue) {
    const double clampedDb = clampValue(dbValue, kLevelMeterMinDb, kLevelMeterMaxDb);
    const double ratio = (clampedDb - kLevelMeterMinDb) / (kLevelMeterMaxDb - kLevelMeterMinDb);
    return rect.left + static_cast<int>(std::lround(ratio * static_cast<double>(rect.right - rect.left)));
}

void drawFrequencyDisplay(const DRAWITEMSTRUCT& draw, double currentFrequencyHz) {
    FillRect(draw.hDC, &draw.rcItem, ui_theme::backgroundBrush());
    RECT textRect = draw.rcItem;
    InflateRect(&textRect, -4, -2);
    const std::wstring text = std::to_wstring(std::max(0, static_cast<int>(std::lround(currentFrequencyHz))));
    drawCenteredText(draw.hDC,
                     textRect,
                     text,
                     ui_theme::kText,
                     std::max(18, static_cast<int>(textRect.bottom - textRect.top - 4)),
                     FW_BOLD);
}

void drawLevelMeter(const DRAWITEMSTRUCT& draw, double currentAmplitudeDb, double peakAmplitudeDb) {
    FillRect(draw.hDC, &draw.rcItem, ui_theme::backgroundBrush());

    RECT barRect = draw.rcItem;
    InflateRect(&barRect, -6, -8);
    barRect.bottom -= 16;
    if ((barRect.right - barRect.left) < 24 || (barRect.bottom - barRect.top) < 12) {
        return;
    }

    HBRUSH trackBrush = CreateSolidBrush(RGB(236, 240, 245));
    FillRect(draw.hDC, &barRect, trackBrush);
    DeleteObject(trackBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, ui_theme::kBorder);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(draw.hDC, borderPen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH)));
    Rectangle(draw.hDC, barRect.left, barRect.top, barRect.right, barRect.bottom);
    SelectObject(draw.hDC, oldBrush);
    SelectObject(draw.hDC, oldPen);
    DeleteObject(borderPen);

    RECT fillRect = barRect;
    fillRect.left += 1;
    fillRect.top += 1;
    fillRect.bottom -= 1;
    fillRect.right = std::max(fillRect.left, static_cast<LONG>(meterXFromDb(barRect, currentAmplitudeDb) - 1));
    const int fillWidth = fillRect.right - fillRect.left;
    if (fillWidth > 0) {
        for (int x = 0; x < fillWidth; ++x) {
            const double ratio = static_cast<double>(x) / std::max(1, fillWidth - 1);
            const COLORREF startColor = ui_theme::blendColor(ui_theme::kGreen, ui_theme::kGold, ratio * 1.2);
            const COLORREF barColor = ui_theme::blendColor(startColor, ui_theme::kRed, std::max(0.0, (ratio - 0.72) / 0.28));
            HPEN pen = CreatePen(PS_SOLID, 1, barColor);
            HPEN activePen = reinterpret_cast<HPEN>(SelectObject(draw.hDC, pen));
            MoveToEx(draw.hDC, fillRect.left + x, fillRect.top, nullptr);
            LineTo(draw.hDC, fillRect.left + x, fillRect.bottom);
            SelectObject(draw.hDC, activePen);
            DeleteObject(pen);
        }
    }

    const double scaleMarksDb[] = {-90.0, -60.0, -30.0, -12.0, 0.0};
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, ui_theme::kMuted);
    HFONT scaleFont = createUiFont(11, FW_NORMAL);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(draw.hDC, scaleFont));
    for (const double dbMark : scaleMarksDb) {
        const int x = meterXFromDb(barRect, dbMark);
        HPEN tickPen = CreatePen(PS_SOLID, 1, ui_theme::blendColor(ui_theme::kBorder, ui_theme::kMuted, 0.35));
        HPEN activePen = reinterpret_cast<HPEN>(SelectObject(draw.hDC, tickPen));
        MoveToEx(draw.hDC, x, barRect.top + 1, nullptr);
        LineTo(draw.hDC, x, barRect.bottom - 1);
        SelectObject(draw.hDC, activePen);
        DeleteObject(tickPen);

        RECT labelRect{x - 22, barRect.bottom + 2, x + 22, draw.rcItem.bottom};
        const std::wstring label = std::to_wstring(static_cast<int>(dbMark));
        DrawTextW(draw.hDC, label.c_str(), -1, &labelRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    SelectObject(draw.hDC, oldFont);
    DeleteObject(scaleFont);

    const int peakX = meterXFromDb(barRect, peakAmplitudeDb);
    HPEN peakPen = CreatePen(PS_SOLID, 2, RGB(120, 35, 35));
    oldPen = reinterpret_cast<HPEN>(SelectObject(draw.hDC, peakPen));
    MoveToEx(draw.hDC, peakX, barRect.top - 1, nullptr);
    LineTo(draw.hDC, peakX, barRect.bottom + 1);
    SelectObject(draw.hDC, oldPen);
    DeleteObject(peakPen);
}

}  // namespace

void MeasurementPage::registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = ui_theme::backgroundBrush();
    RegisterClassW(&pageClass);
}

const wchar_t* MeasurementPage::pageWindowClassName() {
    return kPageClassName;
}

void MeasurementPage::create(HWND parent, HINSTANCE instance) {
    instance_ = instance;
    window_ = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr, instance, this);
    createControls();
}

void MeasurementPage::createControls() {
    helpBubble_.create(window_, instance_);

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
    controls_.labelFadeIn = CreateWindowW(L"STATIC", L"Fade-In", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelFadeOut = CreateWindowW(L"STATIC", L"Fade-Out", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelDuration = CreateWindowW(L"STATIC", L"Sweep Time", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelStartFrequency = CreateWindowW(L"STATIC", L"Sweep Start", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelEndFrequency = CreateWindowW(L"STATIC", L"Sweep End", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelTargetLength = CreateWindowW(L"STATIC", L"Target Length", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelLeadIn = CreateWindowW(L"STATIC", L"Lead-In", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.labelSampleRate = CreateWindowW(L"STATIC", L"Sample Rate", WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitFadeIn = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitFadeOut = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitDuration = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitStartFrequency = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitEndFrequency = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitTargetLength = CreateWindowW(L"STATIC", L"samples", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.unitLeadIn = CreateWindowW(L"STATIC", L"samples", WS_CHILD | WS_VISIBLE | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.editFadeIn = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditFadeIn), instance_, nullptr);
    controls_.editFadeOut = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditFadeOut), instance_, nullptr);
    controls_.editDuration = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditDuration), instance_, nullptr);
    controls_.editStartFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditStartFrequency), instance_, nullptr);
    controls_.editEndFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditEndFrequency), instance_, nullptr);
    controls_.editTargetLength = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditTargetLength), instance_, nullptr);
    controls_.editLeadIn = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kEditLeadIn), instance_, nullptr);
    controls_.comboSampleRate = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL | kHelpBubbleChildClipStyle, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kComboMeasurementSampleRate), instance_, nullptr);
    controls_.labelOutputVolume = CreateWindowW(L"STATIC", L"Output level", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.outputVolumeValue = CreateWindowW(L"STATIC", L"-30 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.outputVolumeSlider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.outputVolumeMuteLabel = CreateWindowW(L"STATIC", L"Mute", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.outputVolumeMaxLabel = CreateWindowW(L"STATIC", L"0 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.actionMetersFrame = CreateWindowExW(0,
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
    controls_.buttonRoomSimulation = CreateWindowW(L"BUTTON",
                                                   L"Sim",
                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   window_,
                                                   reinterpret_cast<HMENU>(kButtonRoomSimulation),
                                                   instance_,
                                                   nullptr);
    controls_.labelReferenceNote = CreateWindowW(L"STATIC",
                                                 L"Reference: none saved.",
                                                 WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 window_,
                                                 nullptr,
                                                 instance_,
                                                 nullptr);
    controls_.leftChannelLabel = CreateWindowW(L"STATIC", L"Left", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.leftProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.leftProgressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightChannelLabel = CreateWindowW(L"STATIC", L"Right", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.rightProgressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.frequencyDisplay = CreateWindowW(L"STATIC",
                                               L"",
                                               WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                               0,
                                               0,
                                               0,
                                               0,
                                               window_,
                                               reinterpret_cast<HMENU>(kFrequencyDisplay),
                                               instance_,
                                               nullptr);
    controls_.levelMeter = CreateWindowW(L"STATIC",
                                         L"",
                                         WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                         0,
                                         0,
                                         0,
                                         0,
                                         window_,
                                         reinterpret_cast<HMENU>(kLevelMeter),
                                         instance_,
                                         nullptr);
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
    controls_.labelWaterfallSource = CreateWindowW(L"STATIC", L"Source", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.comboWaterfallSource = CreateWindowW(L"COMBOBOX",
                                                   nullptr,
                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   window_,
                                                   reinterpret_cast<HMENU>(kComboWaterfallSource),
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
    controls_.labelWaterfallLowCutoff = CreateWindowW(L"STATIC", L"Low Cutoff", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    controls_.sliderWaterfallLowCutoff = CreateWindowExW(0,
                                                         TRACKBAR_CLASSW,
                                                         nullptr,
                                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS | TBS_HORZ,
                                                         0,
                                                         0,
                                                         0,
                                                         0,
                                                         window_,
                                                         reinterpret_cast<HMENU>(kSliderWaterfallLowCutoff),
                                                         instance_,
                                                         nullptr);
    controls_.valueWaterfallLowCutoff = CreateWindowW(L"STATIC", L"-72 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
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

    responseGraph_.create(window_, instance_, kResponseGraph);
    waterfallGraph_.create(window_, instance_);
    ShowWindow(responseGraph_.window(), SW_HIDE);
    ShowWindow(waterfallGraph_.window(), SW_HIDE);

    const DWORD centeredStaticStyle = SS_CENTER | WS_CHILD | WS_VISIBLE | SS_NOTIFY | kHelpBubbleChildClipStyle;
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
    SetWindowLongPtrW(controls_.labelWaterfallSource, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelWaterfallChannel, GWL_STYLE, centeredStaticStyle);
    SetWindowLongPtrW(controls_.labelWaterfallLowCutoff, GWL_STYLE, centeredStaticStyle);
    helpBubble_.registerLabel(controls_.labelFadeIn, L"Sets how long the sweep fades in at the start to avoid abrupt clicks.");
    helpBubble_.registerLabel(controls_.labelFadeOut, L"Sets how long the sweep fades out at the end so playback stops cleanly.");
    helpBubble_.registerLabel(controls_.labelDuration, L"Controls the total sweep duration used to measure the room response.");
    helpBubble_.registerLabel(controls_.labelStartFrequency, L"Sets the low end of the sweep range used during the measurement.");
    helpBubble_.registerLabel(controls_.labelEndFrequency, L"Sets the high end of the sweep range. This is fixed for the current workflow.");
    helpBubble_.registerLabel(controls_.labelTargetLength, L"Sets the generated sweep length in samples before fades are applied.");
    helpBubble_.registerLabel(controls_.labelLeadIn, L"Adds silent samples before the sweep so devices have time to settle before capture.");
    helpBubble_.registerLabel(controls_.labelSampleRate, L"Sets the sweep and capture sample rate used for the measurement run.");
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
    SendMessageW(controls_.sliderWaterfallLowCutoff, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(controls_.sliderWaterfallLowCutoff,
                 TBM_SETRANGEMAX,
                 FALSE,
                 kWaterfallLowCutoffMaxDb - kWaterfallLowCutoffMinDb);
    SendMessageW(controls_.sliderWaterfallLowCutoff, TBM_SETTICFREQ, 6, 0);
    populateMeasurementSampleRateCombo(controls_.comboSampleRate);
    populatePlotCombo(controls_.comboPlot);
    populateWaterfallSourceCombo(controls_.comboWaterfallSource);
    populateWaterfallChannelCombo(controls_.comboWaterfallChannel);
}

void MeasurementPage::layout() {
    helpBubble_.hide();
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
    constexpr int kWaterfallSourceLabelWidth = 72;
    constexpr int kWaterfallSourceComboWidth = 124;
    constexpr int kWaterfallChannelLabelWidth = 72;
    constexpr int kWaterfallChannelComboWidth = 110;
    constexpr int kWaterfallCutoffLabelWidth = 72;
    constexpr int kWaterfallCutoffSliderWidth = 160;
    constexpr int kWaterfallCutoffValueWidth = 56;
    constexpr int kLegendGap = 14;
    constexpr int kLegendWidth = 172;
    constexpr int kCalibrationLabelWidth = 104;
    constexpr int kCalibrationBrowseWidth = 74;
    constexpr int kCalibrationClearWidth = 58;
    constexpr int kCalibrationGap = 10;
    constexpr int kReferenceButtonWidth = 138;
    constexpr int kSimButtonWidth = 56;
    constexpr int kActionFramePaddingX = 12;
    constexpr int kActionGroupGap = 18;
    constexpr int kActionFrameHeight = 64;
    constexpr int kActionFrameBottomGap = 14;
    constexpr int kReferenceNoteHeight = 32;
    constexpr int kReferenceNoteBottomGap = 8;
    constexpr int kMinProgressBarWidth = 96;
    constexpr int kFrequencyDisplayMinWidth = 82;
    constexpr int kFrequencyDisplayMaxWidth = 118;
    constexpr int kMeterBlockMinWidth = 176;
    constexpr int kLevelMeterMinWidth = 150;

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
    const int buttonsBlockWidth = kButtonWidth + 10 + kReferenceButtonWidth + 8 + kSimButtonWidth;
    const int dataRowTop = metricsTop;
    const int progressRowTop = dataRowTop + 30;
    const int legacyButtonTop = dataRowTop - 4;
    const int legacyButtonHeight = (progressRowTop + 20) - legacyButtonTop;
    const int actionFrameTop = metricsTop - 4;
    const bool useGroupedActionFrame = innerWidth >= 860;
    const int graphControlsAnchorRight = contentLeft + buttonsBlockWidth;

    int metricLeft = 0;
    int graphControlsTop = progressRowTop + 30;
    if (useGroupedActionFrame) {
        const int actionFrameWidth = innerWidth;
        const int actionFrameBottom = actionFrameTop + kActionFrameHeight;
        const int buttonHeight = 32;
        const int buttonTop = actionFrameTop + ((kActionFrameHeight - buttonHeight) / 2);
        const int buttonLeft = contentLeft + kActionFramePaddingX;
        const int frequencyLeft = buttonLeft + buttonsBlockWidth + kActionGroupGap;
        const int frameInnerRight = contentLeft + actionFrameWidth - kActionFramePaddingX;
        const int meterBlockWidth = std::clamp((innerWidth - 770) / 2, kMeterBlockMinWidth, 228);
        const int frequencyWidth = std::clamp((innerWidth - 1020) / 2 + 96, kFrequencyDisplayMinWidth, kFrequencyDisplayMaxWidth);
        const int levelMeterLeft = frameInnerRight - std::max(kLevelMeterMinWidth,
                                                               frameInnerRight - (frequencyLeft + frequencyWidth + kActionGroupGap + meterBlockWidth + kActionGroupGap));
        const int metersLeft = levelMeterLeft - kActionGroupGap - meterBlockWidth;
        const int levelMeterWidth = frameInnerRight - levelMeterLeft;
        const int meterRowHeight = 20;
        const int meterRowsHeight = (meterRowHeight * 2) + 4;
        const int meterTop = actionFrameTop + ((kActionFrameHeight - meterRowsHeight) / 2);
        const int frequencyTop = actionFrameTop + 6;
        const int frequencyHeight = kActionFrameHeight - 12;

        MoveWindow(controls_.actionMetersFrame, contentLeft, actionFrameTop, actionFrameWidth, kActionFrameHeight, TRUE);
        MoveWindow(controls_.buttonMeasure, buttonLeft, buttonTop, kButtonWidth, buttonHeight, TRUE);
        MoveWindow(controls_.buttonMeasureReference,
                   buttonLeft + kButtonWidth + 10,
                   buttonTop,
                   kReferenceButtonWidth,
                   buttonHeight,
                   TRUE);
        MoveWindow(controls_.buttonRoomSimulation,
                   buttonLeft + kButtonWidth + 10 + kReferenceButtonWidth + 8,
                   buttonTop,
                   kSimButtonWidth,
                   buttonHeight,
                   TRUE);
        MoveWindow(controls_.frequencyDisplay, frequencyLeft, frequencyTop, frequencyWidth, frequencyHeight, TRUE);

        MoveWindow(controls_.leftChannelLabel, metersLeft, meterTop + 2, kProgressLabelWidth, 18, TRUE);
        MoveWindow(controls_.leftProgressBar,
                   metersLeft + kProgressLabelWidth + 8,
                   meterTop + 4,
                   meterBlockWidth - kProgressLabelWidth - 8 - kProgressTextWidth - 8,
                   16,
                   TRUE);
        MoveWindow(controls_.leftProgressText,
                   metersLeft + meterBlockWidth - kProgressTextWidth,
                   meterTop,
                   kProgressTextWidth,
                   20,
                   TRUE);
        MoveWindow(controls_.rightChannelLabel, metersLeft, meterTop + meterRowHeight + 6, kProgressLabelWidth, 18, TRUE);
        MoveWindow(controls_.rightProgressBar,
                   metersLeft + kProgressLabelWidth + 8,
                   meterTop + meterRowHeight + 8,
                   meterBlockWidth - kProgressLabelWidth - 8 - kProgressTextWidth - 8,
                   16,
                   TRUE);
        MoveWindow(controls_.rightProgressText,
                   metersLeft + meterBlockWidth - kProgressTextWidth,
                   meterTop + meterRowHeight + 4,
                   kProgressTextWidth,
                   20,
                   TRUE);
        MoveWindow(controls_.levelMeter,
                   levelMeterLeft,
                   actionFrameTop + 6,
                   levelMeterWidth,
                   kActionFrameHeight - 12,
                   TRUE);

        graphControlsTop = actionFrameBottom + kActionFrameBottomGap;
    } else {
        ShowWindow(controls_.actionMetersFrame, SW_HIDE);
        MoveWindow(controls_.buttonMeasure, contentLeft, legacyButtonTop, kButtonWidth, legacyButtonHeight, TRUE);
        MoveWindow(controls_.buttonMeasureReference,
                   contentLeft + kButtonWidth + 10,
                   legacyButtonTop,
                   kReferenceButtonWidth,
                   legacyButtonHeight,
                   TRUE);
        MoveWindow(controls_.buttonRoomSimulation,
                   contentLeft + kButtonWidth + 10 + kReferenceButtonWidth + 8,
                   legacyButtonTop,
                   kSimButtonWidth,
                   legacyButtonHeight,
                   TRUE);
        metricLeft = contentLeft + buttonsBlockWidth + kMetricGap;
        MoveWindow(controls_.frequencyDisplay, metricLeft, dataRowTop + 1, 92, 28, TRUE);
        MoveWindow(controls_.levelMeter,
                   metricLeft + 92 + kMetricGap,
                   dataRowTop,
                   std::max(180, innerWidth - (metricLeft - contentLeft) - 92 - kMetricGap),
                   30,
                   TRUE);

        metricLeft = contentLeft + buttonsBlockWidth + kMetricGap;
        MoveWindow(controls_.leftChannelLabel, metricLeft, progressRowTop + 2, kProgressLabelWidth, 18, TRUE);
        MoveWindow(controls_.leftProgressBar, metricLeft + kProgressLabelWidth + 8, progressRowTop + 4, kProgressBarWidth, 16, TRUE);
        MoveWindow(controls_.leftProgressText, metricLeft + kProgressLabelWidth + 8 + kProgressBarWidth + 8, progressRowTop, kProgressTextWidth, 20, TRUE);
        metricLeft += kProgressLabelWidth + 8 + kProgressBarWidth + 8 + kProgressTextWidth + kMetricGap;
        MoveWindow(controls_.rightChannelLabel, metricLeft, progressRowTop + 2, kProgressLabelWidth, 18, TRUE);
        MoveWindow(controls_.rightProgressBar, metricLeft + kProgressLabelWidth + 8, progressRowTop + 4, kProgressBarWidth, 16, TRUE);
        MoveWindow(controls_.rightProgressText, metricLeft + kProgressLabelWidth + 8 + kProgressBarWidth + 8, progressRowTop, kProgressTextWidth, 20, TRUE);
    }
    if (useGroupedActionFrame) {
        ShowWindow(controls_.actionMetersFrame, SW_SHOW);
    }

    MoveWindow(controls_.labelReferenceNote, contentLeft, graphControlsTop, innerWidth, kReferenceNoteHeight, TRUE);
    graphControlsTop += kReferenceNoteHeight + kReferenceNoteBottomGap;

    const int controlsRight = contentLeft + innerWidth;
    const int plotGroupWidth = kWaterfallChannelLabelWidth + 10 + kGraphComboWidth;
    const int sourceGroupWidth = kWaterfallSourceLabelWidth + 10 + kWaterfallSourceComboWidth;
    const int channelGroupWidth = kWaterfallChannelLabelWidth + 10 + kWaterfallChannelComboWidth;
    const int cutoffGroupWidth = kWaterfallCutoffLabelWidth + 10 + kWaterfallCutoffSliderWidth + 10 + kWaterfallCutoffValueWidth;
    constexpr int kWaterfallControlsGap = 24;
    constexpr int kWaterfallControlsRowPitch = 34;
    int waterfallControlsRowTop = graphControlsTop;
    int waterfallControlsRight = contentLeft;
    auto beginWaterfallGroup = [&](int groupWidth) {
        int groupLeft = waterfallControlsRight == contentLeft ? contentLeft : waterfallControlsRight + kWaterfallControlsGap;
        if (groupLeft + groupWidth > controlsRight && waterfallControlsRight != contentLeft) {
            waterfallControlsRowTop += kWaterfallControlsRowPitch;
            waterfallControlsRight = contentLeft;
            groupLeft = contentLeft;
        }
        return groupLeft;
    };

    const int plotFieldLeft = beginWaterfallGroup(plotGroupWidth);
    const int plotComboLeft = plotFieldLeft + kWaterfallChannelLabelWidth + 10;
    MoveWindow(controls_.labelPlot, plotFieldLeft, waterfallControlsRowTop + 2, kWaterfallChannelLabelWidth, 18, TRUE);
    MoveWindow(controls_.comboPlot,
               plotComboLeft,
               waterfallControlsRowTop - 2,
               kGraphComboWidth,
               kGraphComboHeight,
               TRUE);
    waterfallControlsRight = plotFieldLeft + plotGroupWidth;

    const int waterfallSourceLeft = beginWaterfallGroup(sourceGroupWidth);
    MoveWindow(controls_.labelWaterfallSource,
               waterfallSourceLeft,
               waterfallControlsRowTop + 2,
               kWaterfallSourceLabelWidth,
               18,
               TRUE);
    MoveWindow(controls_.comboWaterfallSource,
               waterfallSourceLeft + kWaterfallSourceLabelWidth + 10,
               waterfallControlsRowTop,
               kWaterfallSourceComboWidth,
               kGraphComboHeight,
               TRUE);
    waterfallControlsRight = waterfallSourceLeft + sourceGroupWidth;

    const int waterfallChannelLeft = beginWaterfallGroup(channelGroupWidth);
    MoveWindow(controls_.labelWaterfallChannel,
               waterfallChannelLeft,
               waterfallControlsRowTop + 2,
               kWaterfallChannelLabelWidth,
               18,
               TRUE);
    MoveWindow(controls_.comboWaterfallChannel,
               waterfallChannelLeft + kWaterfallChannelLabelWidth + 10,
               waterfallControlsRowTop - 2,
               kWaterfallChannelComboWidth,
               kGraphComboHeight,
               TRUE);
    waterfallControlsRight = waterfallChannelLeft + channelGroupWidth;

    const int waterfallCutoffLeft = beginWaterfallGroup(cutoffGroupWidth);
    MoveWindow(controls_.labelWaterfallLowCutoff,
               waterfallCutoffLeft,
               waterfallControlsRowTop + 2,
               kWaterfallCutoffLabelWidth,
               18,
               TRUE);
    MoveWindow(controls_.sliderWaterfallLowCutoff,
               waterfallCutoffLeft + kWaterfallCutoffLabelWidth + 10,
               waterfallControlsRowTop - 2,
               kWaterfallCutoffSliderWidth,
               32,
               TRUE);
    MoveWindow(controls_.valueWaterfallLowCutoff,
               waterfallCutoffLeft + kWaterfallCutoffLabelWidth + 10 + kWaterfallCutoffSliderWidth + 10,
               waterfallControlsRowTop + 2,
               kWaterfallCutoffValueWidth,
               18,
               TRUE);
    const bool waterfallVisible =
        plotModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPlot, CB_GETCURSEL, 0, 0))) == "waterfall";
    const int graphTop = waterfallControlsRowTop + 56;
    const int availableBottom = contentTop + innerHeight;
    const int graphBottom = std::max(graphTop + 200, availableBottom);
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
    SendMessageW(controls_.comboWaterfallSource,
                 CB_SETCURSEL,
                 comboIndexFromWaterfallSource(workspace.ui.measurementWaterfallSource),
                 0);
    SendMessageW(controls_.comboWaterfallChannel,
                 CB_SETCURSEL,
                 comboIndexFromWaterfallChannel(workspace.ui.measurementWaterfallChannel),
                 0);
    SendMessageW(controls_.sliderWaterfallLowCutoff,
                 TBM_SETPOS,
                 TRUE,
                 sliderPositionFromWaterfallLowCutoffDb(workspace.ui.measurementWaterfallLowCutoffDb));
    setWindowTextValue(controls_.valueWaterfallLowCutoff,
                       formatWaterfallLowCutoffLabel(workspace.ui.measurementWaterfallLowCutoffDb));
    waterfallGraph_.setLowCutoffDb(workspace.ui.measurementWaterfallLowCutoffDb);
    showRoomLeft_ = workspace.ui.measurementShowRoomLeft;
    showRoomRight_ = workspace.ui.measurementShowRoomRight;
    showReference_ = workspace.ui.measurementShowReference;
    SendMessageW(controls_.checkboxShowRoomLeft, BM_SETCHECK, showRoomLeft_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowRoomRight, BM_SETCHECK, showRoomRight_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(controls_.checkboxShowReference, BM_SETCHECK, showReference_ ? BST_CHECKED : BST_UNCHECKED, 0);
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
    workspace.ui.measurementWaterfallSource = waterfallSourceFromComboIndex(
        static_cast<int>(SendMessageW(controls_.comboWaterfallSource, CB_GETCURSEL, 0, 0)));
    workspace.ui.measurementWaterfallChannel = waterfallChannelFromComboIndex(
        static_cast<int>(SendMessageW(controls_.comboWaterfallChannel, CB_GETCURSEL, 0, 0)));
    workspace.ui.measurementWaterfallLowCutoffDb =
        waterfallLowCutoffDbFromSliderPosition(static_cast<int>(SendMessageW(controls_.sliderWaterfallLowCutoff, TBM_GETPOS, 0, 0)));
    workspace.ui.measurementShowRoomLeft = SendMessageW(controls_.checkboxShowRoomLeft, BM_GETCHECK, 0, 0) == BST_CHECKED;
    workspace.ui.measurementShowRoomRight = SendMessageW(controls_.checkboxShowRoomRight, BM_GETCHECK, 0, 0) == BST_CHECKED;
    workspace.ui.measurementShowReference = SendMessageW(controls_.checkboxShowReference, BM_GETCHECK, 0, 0) == BST_CHECKED;
    measurement::syncDerivedMeasurementSettings(workspace.measurement);
}

void MeasurementPage::setWorkspaceView(const WorkspaceState& workspace) {
    audioSettings_ = workspace.audio;
    measurementSettings_ = workspace.measurement;
    result_ = workspace.result;
    referenceResult_ = workspace.referenceResult;
    filterResult_ = workspace.filterResult;
    refreshActionButtons();
    refreshReferenceNote();
    refreshPlots();
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
    InvalidateRect(controls_.frequencyDisplay, nullptr, TRUE);
    InvalidateRect(controls_.levelMeter, nullptr, TRUE);
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
    EnableWindow(controls_.comboWaterfallSource, enabled ? TRUE : FALSE);
    EnableWindow(controls_.comboWaterfallChannel, enabled ? TRUE : FALSE);
    EnableWindow(controls_.sliderWaterfallLowCutoff, enabled ? TRUE : FALSE);
    EnableWindow(controls_.checkboxShowRoomLeft, enabled ? TRUE : FALSE);
    EnableWindow(controls_.checkboxShowRoomRight, enabled ? TRUE : FALSE);
    EnableWindow(controls_.checkboxShowReference, enabled ? TRUE : FALSE);
    EnableWindow(controls_.buttonRoomSimulation, enabled ? TRUE : FALSE);
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

    if (draw->hwndItem == controls_.frequencyDisplay) {
        drawFrequencyDisplay(*draw, status_.currentFrequencyHz);
        return true;
    }
    if (draw->hwndItem == controls_.levelMeter) {
        drawLevelMeter(*draw, status_.currentAmplitudeDb, status_.peakAmplitudeDb);
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
                                    bool& roomSimulationPressed,
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

    if (commandId == kButtonRoomSimulation && notificationCode == BN_CLICKED) {
        roomSimulationPressed = true;
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

    if ((commandId == kComboPlot ||
         commandId == kComboWaterfallSource ||
         commandId == kComboWaterfallChannel) &&
        notificationCode == CBN_SELCHANGE) {
        if (commandId == kComboPlot) {
            updatePlotControlVisibility();
            layout();
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
    if (source == controls_.outputVolumeSlider) {
        workspace.audio.outputVolumeDb = sliderPositionToOutputVolumeDb(static_cast<int>(SendMessageW(controls_.outputVolumeSlider, TBM_GETPOS, 0, 0)));
        setWindowTextValue(controls_.outputVolumeValue, formatOutputVolumeLabel(workspace.audio.outputVolumeDb));
        return true;
    }

    if (source == controls_.sliderWaterfallLowCutoff) {
        workspace.ui.measurementWaterfallLowCutoffDb = waterfallLowCutoffDbFromSliderPosition(
            static_cast<int>(SendMessageW(controls_.sliderWaterfallLowCutoff, TBM_GETPOS, 0, 0)));
        setWindowTextValue(controls_.valueWaterfallLowCutoff,
                           formatWaterfallLowCutoffLabel(workspace.ui.measurementWaterfallLowCutoffDb));
        waterfallGraph_.setLowCutoffDb(workspace.ui.measurementWaterfallLowCutoffDb);
        return true;
    }

    return false;
}

LRESULT CALLBACK MeasurementPage::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* page = reinterpret_cast<MeasurementPage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_SIZE:
        if (page != nullptr) {
            page->helpBubble_.hide();
        }
        break;
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
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        LRESULT helpColorResult = 0;
        if (page != nullptr && page->helpBubble_.handleCtlColorStatic(hdc, reinterpret_cast<HWND>(lParam), helpColorResult)) {
            return helpColorResult;
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ui_theme::kText);
        return reinterpret_cast<INT_PTR>(ui_theme::backgroundBrush());
    }
    case WM_NCDESTROY:
        if (page != nullptr) {
            page->helpBubble_.destroy();
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        break;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
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

void MeasurementPage::populateWaterfallSourceCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Measured"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Expected"));
}

int MeasurementPage::comboIndexFromWaterfallSource(const std::string& source) {
    if (source == "expected") {
        return 1;
    }
    return 0;
}

std::string MeasurementPage::waterfallSourceFromComboIndex(int index) {
    return index == 1 ? "expected" : "measured";
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

double MeasurementPage::waterfallLowCutoffDbFromSliderPosition(int position) {
    const int clamped = clampValue(position, 0, kWaterfallLowCutoffMaxDb - kWaterfallLowCutoffMinDb);
    return static_cast<double>(kWaterfallLowCutoffMinDb + clamped);
}

int MeasurementPage::sliderPositionFromWaterfallLowCutoffDb(double lowCutoffDb) {
    const int rounded = static_cast<int>(std::lround(lowCutoffDb));
    return clampValue(rounded - kWaterfallLowCutoffMinDb, 0, kWaterfallLowCutoffMaxDb - kWaterfallLowCutoffMinDb);
}

std::wstring MeasurementPage::formatWaterfallLowCutoffLabel(double lowCutoffDb) {
    return formatWideDouble(lowCutoffDb, 0) + L" dB";
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

std::wstring MeasurementPage::buildReferenceNoteText() const {
    if (!referenceResult_.hasAnyValues()) {
        return L"Reference: none saved.";
    }

    const std::wstring interfaceText = describeReferenceInterface(referenceResult_.analysis);
    const std::wstring staleReason = staleReferenceReason(audioSettings_, measurementSettings_, referenceResult_);
    std::wstring text = L"Reference saved with " + interfaceText + L".";
    if (!staleReason.empty() && staleReason != L"missing") {
        text += L" Current measurement settings differ (" + staleReason + L").";
    }
    return text;
}

void MeasurementPage::refreshReferenceNote() const {
    setWindowTextValue(controls_.labelReferenceNote, buildReferenceNoteText());
}

void MeasurementPage::refreshPlots() {
    responseGraph_.setData(buildGraphData());
    waterfallGraph_.setLowCutoffDb(
        waterfallLowCutoffDbFromSliderPosition(static_cast<int>(SendMessageW(controls_.sliderWaterfallLowCutoff, TBM_GETPOS, 0, 0))));

    const std::string sourceSelection = waterfallSourceFromComboIndex(
        static_cast<int>(SendMessageW(controls_.comboWaterfallSource, CB_GETCURSEL, 0, 0)));
    const std::string channelSelection = waterfallChannelFromComboIndex(
        static_cast<int>(SendMessageW(controls_.comboWaterfallChannel, CB_GETCURSEL, 0, 0)));
    const MeasurementChannel channel =
        channelSelection == "right" ? MeasurementChannel::Right : MeasurementChannel::Left;
    const measurement::WaterfallPlotData waterfallData =
        sourceSelection == "expected"
            ? measurement::buildExpectedWaterfallPlotData(result_, filterResult_, channel)
            : measurement::buildWaterfallPlotData(result_, channel);
    waterfallGraph_.setData(waterfallData);

    updatePlotControlVisibility();
}

void MeasurementPage::updatePlotControlVisibility() const {
    const std::string plotMode =
        plotModeFromComboIndex(static_cast<int>(SendMessageW(controls_.comboPlot, CB_GETCURSEL, 0, 0)));
    const bool waterfallVisible = plotMode == "waterfall";
    ShowWindow(responseGraph_.window(), waterfallVisible ? SW_HIDE : SW_SHOW);
    ShowWindow(waterfallGraph_.window(), waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.labelWaterfallSource, waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.comboWaterfallSource, waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.labelWaterfallChannel, waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.comboWaterfallChannel, waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.labelWaterfallLowCutoff, waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.sliderWaterfallLowCutoff, waterfallVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.valueWaterfallLowCutoff, waterfallVisible ? SW_SHOW : SW_HIDE);
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
    EnableWindow(controls_.buttonRoomSimulation, busy ? FALSE : TRUE);
    InvalidateRect(controls_.buttonMeasure, nullptr, TRUE);
    InvalidateRect(controls_.buttonMeasureReference, nullptr, TRUE);
}

}  // namespace wolfie::ui
