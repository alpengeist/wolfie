#include "wolfie_app.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <commctrl.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

namespace wolfie {

namespace {

constexpr int kMenuFileNew = 1001;
constexpr int kMenuFileOpen = 1002;
constexpr int kMenuFileSave = 1003;
constexpr int kMenuFileSaveAs = 1004;
constexpr int kMenuFileSettings = 1005;
constexpr int kMenuFileRecentBase = 1100;

constexpr int kEditFadeIn = 3004;
constexpr int kEditFadeOut = 3005;
constexpr int kEditDuration = 3006;
constexpr int kEditStartFrequency = 3007;
constexpr int kEditEndFrequency = 3008;
constexpr int kEditTargetLength = 3009;
constexpr int kEditLeadIn = 3010;
constexpr int kButtonMeasure = 3011;
constexpr int kComboMeasurementSampleRate = 3012;
constexpr int kTabMain = 3013;

constexpr wchar_t kMainClassName[] = L"WolfieMainWindow";
constexpr wchar_t kGraphClassName[] = L"WolfieGraphWindow";
constexpr wchar_t kPageClassName[] = L"WolfiePageWindow";
constexpr wchar_t kSettingsClassName[] = L"WolfieSettingsWindow";
constexpr wchar_t kNoAsioDrivers[] = L"(No ASIO drivers found)";

constexpr COLORREF kBackground = RGB(241, 244, 248);
constexpr COLORREF kPanelBackground = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(218, 224, 231);
constexpr COLORREF kAccent = RGB(44, 110, 182);
constexpr COLORREF kGreen = RGB(46, 143, 82);
constexpr COLORREF kRed = RGB(190, 69, 69);
constexpr COLORREF kText = RGB(45, 52, 61);
constexpr COLORREF kMuted = RGB(109, 118, 130);
constexpr double kMutedOutputVolumeDb = -100.0;
constexpr int kOutputVolumeSliderMax = 61;

using ASIOBool = long;
using ASIOSampleRate = double;
using ASIOError = long;

struct ASIOSamples;
struct ASIOTimeStamp;
struct ASIOClockSource;
struct ASIOChannelInfo;
struct ASIOBufferInfo;
struct ASIOCallbacks;

struct IASIO : public IUnknown {
    virtual ASIOBool STDMETHODCALLTYPE init(void* sysHandle) = 0;
    virtual void STDMETHODCALLTYPE getDriverName(char* name) = 0;
    virtual long STDMETHODCALLTYPE getDriverVersion() = 0;
    virtual void STDMETHODCALLTYPE getErrorMessage(char* string) = 0;
    virtual ASIOError STDMETHODCALLTYPE start() = 0;
    virtual ASIOError STDMETHODCALLTYPE stop() = 0;
    virtual ASIOError STDMETHODCALLTYPE getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError STDMETHODCALLTYPE getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError STDMETHODCALLTYPE getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError STDMETHODCALLTYPE canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError STDMETHODCALLTYPE getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError STDMETHODCALLTYPE setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError STDMETHODCALLTYPE getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError STDMETHODCALLTYPE setClockSource(long reference) = 0;
    virtual ASIOError STDMETHODCALLTYPE getSamplePosition(ASIOSamples* samplePosition, ASIOTimeStamp* timeStamp) = 0;
    virtual ASIOError STDMETHODCALLTYPE getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError STDMETHODCALLTYPE createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError STDMETHODCALLTYPE disposeBuffers() = 0;
    virtual ASIOError STDMETHODCALLTYPE controlPanel() = 0;
    virtual ASIOError STDMETHODCALLTYPE future(long selector, void* opt) = 0;
    virtual ASIOError STDMETHODCALLTYPE outputReady() = 0;
};

std::string escapeJson(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::optional<std::string> findJsonString(const std::string& source, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const size_t keyPos = source.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t colonPos = source.find(':', keyPos + pattern.size());
    const size_t firstQuote = source.find('"', colonPos + 1);
    if (colonPos == std::string::npos || firstQuote == std::string::npos) {
        return std::nullopt;
    }

    std::string value;
    for (size_t cursor = firstQuote + 1; cursor < source.size(); ++cursor) {
        const char ch = source[cursor];
        if (ch == '\\' && cursor + 1 < source.size()) {
            value.push_back(source[cursor + 1]);
            ++cursor;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

std::optional<double> findJsonNumber(const std::string& source, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const size_t keyPos = source.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t colonPos = source.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t valueStart = source.find_first_of("-0123456789", colonPos + 1);
    const size_t valueEnd = source.find_first_not_of("0123456789+-.eE", valueStart);
    if (valueStart == std::string::npos) {
        return std::nullopt;
    }

    return std::stod(source.substr(valueStart, valueEnd - valueStart));
}

std::vector<std::string> findJsonStringArray(const std::string& source, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const size_t keyPos = source.find(pattern);
    if (keyPos == std::string::npos) {
        return {};
    }

    const size_t arrayStart = source.find('[', keyPos + pattern.size());
    const size_t arrayEnd = source.find(']', arrayStart + 1);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos) {
        return {};
    }

    std::vector<std::string> values;
    size_t cursor = arrayStart + 1;
    while (cursor < arrayEnd) {
        const size_t quoteStart = source.find('"', cursor);
        if (quoteStart == std::string::npos || quoteStart >= arrayEnd) {
            break;
        }
        std::string value;
        for (size_t i = quoteStart + 1; i < arrayEnd; ++i) {
            const char ch = source[i];
            if (ch == '\\' && i + 1 < arrayEnd) {
                value.push_back(source[i + 1]);
                ++i;
                continue;
            }
            if (ch == '"') {
                values.push_back(value);
                cursor = i + 1;
                break;
            }
            value.push_back(ch);
        }
        ++cursor;
    }
    return values;
}

bool writeTextFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << content;
    return static_cast<bool>(out);
}

std::optional<std::string> readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

uint64_t tickMillis() {
    return GetTickCount64();
}

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double defaultSweepEndFrequencyHz(int sampleRate) {
    return sampleRate == 44100 ? 22050.0 : 24000.0;
}

void syncDerivedMeasurementSettings(MeasurementSettings& settings) {
    settings.endFrequencyHz = defaultSweepEndFrequencyHz(settings.sampleRate);
}

int measurementSampleRateFromComboIndex(int index) {
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

int comboIndexFromMeasurementSampleRate(int sampleRate) {
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

void populateMeasurementSampleRateCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"44.1 kHz"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"48 kHz"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"96 kHz"));
}

std::vector<float> generateSweepSamples(const MeasurementSettings& settings, int sampleRate) {
    const int totalSamples = std::max(1, static_cast<int>(std::round(settings.durationSeconds * sampleRate)));
    const double duration = std::max(0.1, settings.durationSeconds);
    const double startHz = std::max(1.0, settings.startFrequencyHz);
    const double nyquist = std::max(2.0, static_cast<double>(sampleRate) * 0.5);
    const double endHz = clampValue(settings.endFrequencyHz, startHz, nyquist);
    const double logSpan = std::log(endHz / startHz);

    std::vector<float> samples(totalSamples, 0.0f);
    constexpr double twoPi = 2.0 * 3.14159265358979323846;
    const double growth = logSpan > 1.0e-9 ? duration / logSpan : 0.0;
    const double phaseScale = logSpan > 1.0e-9 ? twoPi * startHz * growth : 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(sampleRate);
        const double phase = logSpan > 1.0e-9
            ? phaseScale * (std::exp(t / growth) - 1.0)
            : twoPi * startHz * t;
        double envelope = 1.0;
        if (t < settings.fadeInSeconds) {
            envelope = t / std::max(0.01, settings.fadeInSeconds);
        } else if (t > duration - settings.fadeOutSeconds) {
            envelope = std::max(0.0, (duration - t) / std::max(0.01, settings.fadeOutSeconds));
        }
        samples[i] = static_cast<float>(std::sin(phase) * envelope);
    }

    float peak = 0.0f;
    for (const float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    if (peak > 0.0f) {
        for (float& sample : samples) {
            sample /= peak;
        }
    }
    return samples;
}

std::vector<float> scaleSweepSamples(const std::vector<float>& samples, double volumeDb) {
    if (volumeDb <= kMutedOutputVolumeDb) {
        return std::vector<float>(samples.size(), 0.0f);
    }
    const double gain = clampValue(std::pow(10.0, volumeDb / 20.0), 0.0, 1.0);
    std::vector<float> scaled(samples.size(), 0.0f);
    for (size_t i = 0; i < samples.size(); ++i) {
        scaled[i] = static_cast<float>(samples[i] * gain);
    }
    return scaled;
}

int16_t floatToPcm16(float sample) {
    const float clamped = clampValue(sample, -1.0f, 1.0f);
    return static_cast<int16_t>(std::round(clamped * 32767.0f));
}

std::vector<int16_t> buildStereoSweepPcm(const std::vector<float>& sweepSamples, int leadInSamples) {
    const size_t safeLeadIn = static_cast<size_t>(std::max(0, leadInSamples));
    const size_t segmentFrames = safeLeadIn + sweepSamples.size();
    const size_t totalFrames = segmentFrames * 2;
    std::vector<int16_t> pcm(totalFrames * 2, 0);

    for (size_t i = 0; i < sweepSamples.size(); ++i) {
        const size_t leftFrame = safeLeadIn + i;
        const size_t rightFrame = segmentFrames + safeLeadIn + i;
        pcm[leftFrame * 2] = floatToPcm16(sweepSamples[i]);
        pcm[rightFrame * 2 + 1] = floatToPcm16(sweepSamples[i]);
    }

    return pcm;
}

bool writeStereoWaveFile(const std::filesystem::path& path, const std::vector<int16_t>& interleavedSamples, int sampleRate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const uint16_t channels = 2;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t dataSize = static_cast<uint32_t>(interleavedSamples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36 + dataSize;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    const uint32_t fmtSize = 16;
    const uint16_t formatTag = 1;
    out.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    out.write(reinterpret_cast<const char*>(&formatTag), sizeof(formatTag));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));

    for (const int16_t pcm : interleavedSamples) {
        out.write(reinterpret_cast<const char*>(&pcm), sizeof(pcm));
    }

    return static_cast<bool>(out);
}

double rmsFromPcm16(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0;
    }

    long double energy = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const long double normalized = static_cast<long double>(samples[i]) / 32768.0L;
        energy += normalized * normalized;
    }
    return std::sqrt(static_cast<double>(energy / static_cast<long double>(count)));
}

double rmsFromFloat(const float* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0;
    }

    long double energy = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const long double normalized = static_cast<long double>(samples[i]);
        energy += normalized * normalized;
    }
    return std::sqrt(static_cast<double>(energy / static_cast<long double>(count)));
}

double amplitudeToDb(double amplitude) {
    if (amplitude <= 1.0e-6) {
        return -90.0;
    }
    return clampValue(20.0 * std::log10(amplitude), -90.0, 24.0);
}

std::wstring formatMmError(MMRESULT result, std::wstring_view operation) {
    std::array<wchar_t, MAXERRORLENGTH> buffer{};
    const MMRESULT textResult = waveOutGetErrorTextW(result, buffer.data(), static_cast<UINT>(buffer.size()));
    std::wstring message = operation.empty() ? L"Audio operation failed" : std::wstring(operation);
    message += L": ";
    if (textResult == MMSYSERR_NOERROR) {
        message += buffer.data();
    } else {
        message += L"Unknown multimedia error";
    }
    return message;
}

double sweepFrequencyAtSample(const MeasurementSettings& settings, int sampleRate, size_t sampleIndex, size_t totalSamples) {
    if (totalSamples == 0) {
        return std::max(1.0, settings.startFrequencyHz);
    }

    const double startHz = std::max(1.0, settings.startFrequencyHz);
    const double nyquist = std::max(2.0, static_cast<double>(sampleRate) * 0.5);
    const double endHz = clampValue(settings.endFrequencyHz, startHz, nyquist);
    const double position = clampValue(static_cast<double>(sampleIndex) / static_cast<double>(totalSamples), 0.0, 1.0);
    if (endHz <= startHz + 1.0e-9) {
        return startHz;
    }
    return startHz * std::pow(endHz / startHz, position);
}

void smoothResponse(std::vector<double>& series) {
    if (series.size() < 3) {
        return;
    }

    std::vector<double> smoothed(series.size(), 0.0);
    for (int pass = 0; pass < 2; ++pass) {
        smoothed.front() = series.front();
        smoothed.back() = series.back();
        for (size_t i = 1; i + 1 < series.size(); ++i) {
            smoothed[i] = (series[i - 1] + (series[i] * 2.0) + series[i + 1]) * 0.25;
        }
        series.swap(smoothed);
    }
}

MeasurementResult buildMeasurementResultFromCapture(const std::vector<int16_t>& capturedSamples,
                                                    const std::vector<float>& playedSweep,
                                                    size_t leadInFrames,
                                                    int sampleRate,
                                                    const MeasurementSettings& settings) {
    MeasurementResult result;
    if (playedSweep.empty()) {
        return result;
    }

    const size_t sweepFrames = playedSweep.size();
    const size_t fadeInFrames = clampValue<size_t>(static_cast<size_t>(std::round(std::max(0.0, settings.fadeInSeconds) * sampleRate)),
                                                   0,
                                                   sweepFrames);
    const size_t fadeOutFrames = clampValue<size_t>(static_cast<size_t>(std::round(std::max(0.0, settings.fadeOutSeconds) * sampleRate)),
                                                    0,
                                                    sweepFrames - fadeInFrames);
    const size_t analysisBegin = fadeInFrames;
    const size_t analysisEnd = sweepFrames - fadeOutFrames;
    if (analysisEnd <= analysisBegin) {
        return result;
    }

    const size_t analysisFrames = analysisEnd - analysisBegin;
    const size_t segmentFrames = leadInFrames + sweepFrames;
    // Ignore the faded sweep edges when generating the response plot because
    // those regions are intentionally attenuated and skew the measured ratio.
    const size_t pointCount = std::min<size_t>(analysisFrames, clampValue<size_t>(analysisFrames / 256, 128, 1024));
    result.frequencyAxisHz.reserve(pointCount);
    result.leftChannelDb.reserve(pointCount);
    result.rightChannelDb.reserve(pointCount);

    auto channelDb = [&](size_t segmentOffset, size_t begin, size_t end) {
        if (end <= begin) {
            return -90.0;
        }
        const size_t captureStart = segmentOffset + leadInFrames + begin;
        const size_t captureEnd = std::min(segmentOffset + leadInFrames + end, capturedSamples.size());
        if (captureEnd <= captureStart) {
            return -90.0;
        }
        const double measured = rmsFromPcm16(capturedSamples.data() + captureStart, captureEnd - captureStart);
        const double reference = rmsFromFloat(playedSweep.data() + begin, end - begin);
        return amplitudeToDb(measured / std::max(reference, 1.0e-6));
    };

    for (size_t i = 0; i < pointCount; ++i) {
        const size_t begin = analysisBegin + ((i * analysisFrames) / pointCount);
        const size_t end = std::max(begin + 1, analysisBegin + (((i + 1) * analysisFrames) / pointCount));
        const size_t center = begin + ((end - begin) / 2);

        result.frequencyAxisHz.push_back(sweepFrequencyAtSample(settings, sampleRate, center, sweepFrames));
        result.leftChannelDb.push_back(channelDb(0, begin, end));
        result.rightChannelDb.push_back(channelDb(segmentFrames, begin, end));
    }

    smoothResponse(result.leftChannelDb);
    smoothResponse(result.rightChannelDb);
    return result;
}

void setWindowText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

std::wstring getWindowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}

std::wstring formatFrequencyLabel(double frequencyHz) {
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(0);
    out << std::round(std::max(0.0, frequencyHz)) << L" Hz";
    return out.str();
}

int outputVolumeDbToSliderPosition(double volumeDb) {
    if (volumeDb <= kMutedOutputVolumeDb) {
        return 0;
    }
    return clampValue(static_cast<int>(std::lround(volumeDb + 61.0)), 1, kOutputVolumeSliderMax);
}

double sliderPositionToOutputVolumeDb(int position) {
    if (position <= 0) {
        return kMutedOutputVolumeDb;
    }
    return static_cast<double>(clampValue(position, 1, kOutputVolumeSliderMax) - 61);
}

std::wstring formatOutputVolumeLabel(double volumeDb) {
    if (volumeDb <= kMutedOutputVolumeDb) {
        return L"Mute";
    }
    if (volumeDb >= 0.0) {
        return L"0 dB";
    }
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(0);
    out << volumeDb << L" dB";
    return out.str();
}

double responseGraphXT(double frequencyHz) {
    constexpr double kMinFrequency = 10.0;
    constexpr double kMaxFrequency = 20000.0;
    const double clamped = clampValue(frequencyHz, kMinFrequency, kMaxFrequency);
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
        std::wostringstream out;
        out << static_cast<int>(std::lround(frequencyHz / 1000.0)) << L"k";
        return out.str();
    }
    std::wostringstream out;
    out << static_cast<int>(std::lround(frequencyHz));
    return out.str();
}

std::wstring trimLineBreaks(std::wstring value) {
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n')) {
        value.pop_back();
    }
    return value;
}

std::wstring formatHResultMessage(HRESULT hr) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = length > 0 && buffer != nullptr ? trimLineBreaks(std::wstring(buffer, length)) : L"Unknown COM error";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    std::wostringstream formatted;
    formatted << message << L" (0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << L")";
    return formatted.str();
}

std::optional<std::wstring> findAsioDriverClsid(std::wstring_view driverName) {
    constexpr std::array registryRoots{
        L"SOFTWARE\\ASIO\\",
        L"SOFTWARE\\WOW6432Node\\ASIO\\"
    };

    for (const auto* registryRoot : registryRoots) {
        const std::wstring keyPath = std::wstring(registryRoot) + std::wstring(driverName);
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keyPath.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
            bytes < sizeof(wchar_t)) {
            continue;
        }

        std::wstring clsid(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keyPath.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid.data(), &bytes) == ERROR_SUCCESS) {
            if (!clsid.empty() && clsid.back() == L'\0') {
                clsid.pop_back();
            }
            if (!clsid.empty()) {
                return clsid;
            }
        }
    }

    return std::nullopt;
}

std::wstring asioDriverMessage(IASIO* driver) {
    std::array<char, 512> buffer{};
    driver->getErrorMessage(buffer.data());
    if (buffer.front() == '\0') {
        return {};
    }

    const int wideLength = MultiByteToWideChar(CP_ACP, 0, buffer.data(), -1, nullptr, 0);
    if (wideLength <= 1) {
        return {};
    }

    std::wstring message(wideLength, L'\0');
    MultiByteToWideChar(CP_ACP, 0, buffer.data(), -1, message.data(), wideLength);
    if (!message.empty() && message.back() == L'\0') {
        message.pop_back();
    }
    return message;
}

std::optional<std::wstring> openAsioControlPanel(HWND parentWindow, std::wstring_view driverName) {
    if (driverName.empty() || driverName == kNoAsioDrivers) {
        return std::wstring(L"Select an installed ASIO driver first.");
    }

    const auto clsidText = findAsioDriverClsid(driverName);
    if (!clsidText) {
        return std::wstring(L"Could not locate the selected ASIO driver in the registry.");
    }

    CLSID clsid{};
    const HRESULT clsidHr = CLSIDFromString(const_cast<wchar_t*>(clsidText->c_str()), &clsid);
    if (FAILED(clsidHr)) {
        return std::wstring(L"The selected ASIO driver has an invalid CLSID: ") + formatHResultMessage(clsidHr);
    }

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
        return std::wstring(L"COM initialization failed while opening the ASIO control panel: ") + formatHResultMessage(initHr);
    }

    auto cleanupCom = [&]() {
        if (shouldUninitialize) {
            CoUninitialize();
        }
    };

    void* rawDriver = nullptr;
    const HRESULT createHr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &rawDriver);
    if (FAILED(createHr) || rawDriver == nullptr) {
        cleanupCom();
        return std::wstring(L"Failed to create the selected ASIO driver: ") + formatHResultMessage(createHr);
    }

    auto* driver = static_cast<IASIO*>(rawDriver);
    const ASIOBool initialized = driver->init(parentWindow != nullptr ? parentWindow : GetDesktopWindow());
    if (!initialized) {
        std::wstring message = asioDriverMessage(driver);
        driver->Release();
        cleanupCom();
        if (message.empty()) {
            message = L"The selected ASIO driver rejected initialization.";
        }
        return message;
    }

    const ASIOError result = driver->controlPanel();
    std::wstring message;
    if (result != 0) {
        message = asioDriverMessage(driver);
        if (message.empty()) {
            std::wostringstream fallback;
            fallback << L"The selected ASIO driver returned error " << result << L" while opening its control panel.";
            message = fallback.str();
        }
    }

    driver->Release();
    cleanupCom();
    if (!message.empty()) {
        return message;
    }
    return std::nullopt;
}

std::vector<std::wstring> enumerateAsioDrivers() {
    std::vector<std::wstring> drivers;

    auto collect = [&](HKEY root, const wchar_t* subKey) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return;
        }

        DWORD index = 0;
        while (true) {
            std::array<wchar_t, 256> name{};
            DWORD nameLength = static_cast<DWORD>(name.size());
            const LONG result = RegEnumKeyExW(
                key,
                index,
                name.data(),
                &nameLength,
                nullptr,
                nullptr,
                nullptr,
                nullptr);

            if (result == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (result == ERROR_SUCCESS && nameLength > 0) {
                drivers.emplace_back(name.data(), nameLength);
            }
            ++index;
        }

        RegCloseKey(key);
    };

    collect(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO");
    collect(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\ASIO");

    std::sort(drivers.begin(), drivers.end());
    drivers.erase(std::unique(drivers.begin(), drivers.end()), drivers.end());
    return drivers;
}

void selectComboBoxString(HWND combo, const std::wstring& value) {
    const LRESULT index = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value.c_str()));
    if (index != CB_ERR) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    } else if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
}

}  // namespace

struct MeasurementEngine::Runtime {
    struct CaptureBuffer {
        WAVEHDR header{};
        std::vector<int16_t> samples;
    };

    HWAVEOUT waveOut = nullptr;
    HWAVEIN waveIn = nullptr;
    std::vector<float> playedSweep;
    std::vector<int16_t> playbackPcm;
    WAVEHDR playbackHeader{};
    std::vector<CaptureBuffer> captureBuffers;
    std::vector<int16_t> capturedSamples;
    size_t leadInFrames = 0;
    size_t sweepFrames = 0;
    size_t segmentFrames = 0;
    size_t totalFrames = 0;
    int sampleRate = 44100;
    bool inputActive = false;

    void processCapture(double& currentAmplitudeDb, double& peakAmplitudeDb) {
        if (waveIn == nullptr) {
            return;
        }

        for (auto& buffer : captureBuffers) {
            if ((buffer.header.dwFlags & WHDR_DONE) == 0) {
                continue;
            }

            const size_t sampleCount = buffer.header.dwBytesRecorded / sizeof(int16_t);
            if (sampleCount > 0) {
                capturedSamples.insert(capturedSamples.end(), buffer.samples.begin(), buffer.samples.begin() + sampleCount);
                currentAmplitudeDb = amplitudeToDb(rmsFromPcm16(buffer.samples.data(), sampleCount));
                peakAmplitudeDb = std::max(peakAmplitudeDb, currentAmplitudeDb);
            }

            buffer.header.dwBytesRecorded = 0;
            buffer.header.dwFlags &= ~WHDR_DONE;
            if (inputActive) {
                waveInAddBuffer(waveIn, &buffer.header, sizeof(buffer.header));
            }
        }
    }

    void close() {
        inputActive = false;

        if (waveIn != nullptr) {
            waveInStop(waveIn);
            waveInReset(waveIn);
            for (auto& buffer : captureBuffers) {
                if ((buffer.header.dwFlags & WHDR_PREPARED) != 0) {
                    waveInUnprepareHeader(waveIn, &buffer.header, sizeof(buffer.header));
                }
            }
            waveInClose(waveIn);
            waveIn = nullptr;
        }

        if (waveOut != nullptr) {
            waveOutReset(waveOut);
            if ((playbackHeader.dwFlags & WHDR_PREPARED) != 0) {
                waveOutUnprepareHeader(waveOut, &playbackHeader, sizeof(playbackHeader));
            }
            waveOutClose(waveOut);
            waveOut = nullptr;
        }
    }
};

MeasurementEngine::MeasurementEngine() = default;

MeasurementEngine::~MeasurementEngine() {
    cleanupRuntime();
}

void MeasurementEngine::cleanupRuntime() {
    if (runtime_) {
        runtime_->close();
        runtime_.reset();
    }
}

bool MeasurementEngine::start(const WorkspaceState& workspace) {
    cleanupRuntime();

    snapshot_ = workspace;
    result_ = {};
    running_ = false;
    finished_ = false;
    progress_ = 0.0;
    currentChannel_ = MeasurementChannel::None;
    currentFrequencyHz_ = 0.0;
    currentAmplitudeDb_ = -90.0;
    peakAmplitudeDb_ = -90.0;
    lastErrorMessage_.clear();

    const int sampleRate = std::max(8000, workspace.measurement.sampleRate);
    auto runtime = std::make_unique<Runtime>();
    runtime->sampleRate = sampleRate;
    runtime->leadInFrames = static_cast<size_t>(std::max(0, workspace.measurement.leadInSamples));
    runtime->playedSweep = scaleSweepSamples(generateSweepSamples(workspace.measurement, sampleRate), workspace.audio.outputVolumeDb);
    runtime->sweepFrames = runtime->playedSweep.size();
    runtime->segmentFrames = runtime->leadInFrames + runtime->sweepFrames;
    runtime->totalFrames = runtime->segmentFrames * 2;
    runtime->playbackPcm = buildStereoSweepPcm(runtime->playedSweep, workspace.measurement.leadInSamples);

    const std::filesystem::path measurementDir = workspace.rootPath / "measurement";
    std::filesystem::create_directories(measurementDir);
    generatedSweepPath_ = measurementDir / "logsweep.wav";
    writeStereoWaveFile(generatedSweepPath_, runtime->playbackPcm, sampleRate);

    WAVEFORMATEX outputFormat{};
    outputFormat.wFormatTag = WAVE_FORMAT_PCM;
    outputFormat.nChannels = 2;
    outputFormat.nSamplesPerSec = sampleRate;
    outputFormat.wBitsPerSample = 16;
    outputFormat.nBlockAlign = outputFormat.nChannels * outputFormat.wBitsPerSample / 8;
    outputFormat.nAvgBytesPerSec = outputFormat.nSamplesPerSec * outputFormat.nBlockAlign;

    MMRESULT mmResult = waveOutOpen(&runtime->waveOut, WAVE_MAPPER, &outputFormat, 0, 0, CALLBACK_NULL);
    if (mmResult != MMSYSERR_NOERROR) {
        lastErrorMessage_ = formatMmError(mmResult, L"Could not open the speaker output");
        runtime->close();
        return false;
    }

    runtime->playbackHeader.lpData = reinterpret_cast<LPSTR>(runtime->playbackPcm.data());
    runtime->playbackHeader.dwBufferLength = static_cast<DWORD>(runtime->playbackPcm.size() * sizeof(int16_t));
    mmResult = waveOutPrepareHeader(runtime->waveOut, &runtime->playbackHeader, sizeof(runtime->playbackHeader));
    if (mmResult != MMSYSERR_NOERROR) {
        lastErrorMessage_ = formatMmError(mmResult, L"Could not prepare the speaker buffer");
        runtime->close();
        return false;
    }

    WAVEFORMATEX inputFormat{};
    inputFormat.wFormatTag = WAVE_FORMAT_PCM;
    inputFormat.nChannels = 1;
    inputFormat.nSamplesPerSec = sampleRate;
    inputFormat.wBitsPerSample = 16;
    inputFormat.nBlockAlign = inputFormat.nChannels * inputFormat.wBitsPerSample / 8;
    inputFormat.nAvgBytesPerSec = inputFormat.nSamplesPerSec * inputFormat.nBlockAlign;

    mmResult = waveInOpen(&runtime->waveIn, WAVE_MAPPER, &inputFormat, 0, 0, CALLBACK_NULL);
    if (mmResult != MMSYSERR_NOERROR) {
        lastErrorMessage_ = formatMmError(mmResult, L"Could not open the microphone input");
        runtime->close();
        return false;
    }

    const size_t captureBufferFrames = clampValue<size_t>(static_cast<size_t>(sampleRate / 20), 1024, 8192);
    runtime->captureBuffers.resize(6);
    runtime->capturedSamples.reserve(runtime->totalFrames + (captureBufferFrames * runtime->captureBuffers.size()));
    for (auto& buffer : runtime->captureBuffers) {
        buffer.samples.assign(captureBufferFrames, 0);
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
        buffer.header.dwBufferLength = static_cast<DWORD>(buffer.samples.size() * sizeof(int16_t));
        mmResult = waveInPrepareHeader(runtime->waveIn, &buffer.header, sizeof(buffer.header));
        if (mmResult != MMSYSERR_NOERROR) {
            lastErrorMessage_ = formatMmError(mmResult, L"Could not prepare the microphone buffer");
            runtime->close();
            return false;
        }
        mmResult = waveInAddBuffer(runtime->waveIn, &buffer.header, sizeof(buffer.header));
        if (mmResult != MMSYSERR_NOERROR) {
            lastErrorMessage_ = formatMmError(mmResult, L"Could not queue the microphone buffer");
            runtime->close();
            return false;
        }
    }

    mmResult = waveInStart(runtime->waveIn);
    if (mmResult != MMSYSERR_NOERROR) {
        lastErrorMessage_ = formatMmError(mmResult, L"Could not start the microphone capture");
        runtime->close();
        return false;
    }
    runtime->inputActive = true;

    mmResult = waveOutWrite(runtime->waveOut, &runtime->playbackHeader, sizeof(runtime->playbackHeader));
    if (mmResult != MMSYSERR_NOERROR) {
        lastErrorMessage_ = formatMmError(mmResult, L"Could not start the sweep playback");
        runtime->close();
        return false;
    }

    durationMs_ = static_cast<uint64_t>(
        std::ceil((static_cast<double>(runtime->totalFrames) * 1000.0) / static_cast<double>(sampleRate)));
    startTickMs_ = tickMillis();
    runtime_ = std::move(runtime);
    running_ = true;
    currentChannel_ = MeasurementChannel::Left;
    return true;
}

void MeasurementEngine::cancel() {
    running_ = false;
    finished_ = false;
    progress_ = 0.0;
    currentChannel_ = MeasurementChannel::None;
    currentFrequencyHz_ = 0.0;
    currentAmplitudeDb_ = -90.0;
    peakAmplitudeDb_ = -90.0;
    cleanupRuntime();
}

void MeasurementEngine::tick() {
    if (!running_ || !runtime_) {
        return;
    }

    runtime_->processCapture(currentAmplitudeDb_, peakAmplitudeDb_);

    const uint64_t elapsedMs = tickMillis() - startTickMs_;
    const size_t elapsedFrames = std::min(
        runtime_->totalFrames,
        static_cast<size_t>((elapsedMs * static_cast<uint64_t>(runtime_->sampleRate)) / 1000ULL));
    const size_t leftSweepStart = runtime_->leadInFrames;
    const size_t rightSegmentStart = runtime_->segmentFrames;
    const size_t rightSweepStart = rightSegmentStart + runtime_->leadInFrames;
    const double sweepFrames = std::max(1.0, static_cast<double>(runtime_->sweepFrames));
    if (elapsedFrames < rightSegmentStart) {
        currentChannel_ = MeasurementChannel::Left;
        if (elapsedFrames <= leftSweepStart) {
            progress_ = 0.0;
        } else {
            progress_ = clampValue(static_cast<double>(elapsedFrames - leftSweepStart) / sweepFrames, 0.0, 1.0);
        }
    } else {
        currentChannel_ = MeasurementChannel::Right;
        if (elapsedFrames <= rightSweepStart) {
            progress_ = 0.0;
        } else {
            progress_ = clampValue(static_cast<double>(elapsedFrames - rightSweepStart) / sweepFrames, 0.0, 1.0);
        }
    }
    if (elapsedFrames < runtime_->leadInFrames) {
        currentFrequencyHz_ = snapshot_.measurement.startFrequencyHz;
    } else if (elapsedFrames < runtime_->segmentFrames) {
        currentFrequencyHz_ = sweepFrequencyAtSample(snapshot_.measurement,
                                                     runtime_->sampleRate,
                                                     elapsedFrames - runtime_->leadInFrames,
                                                     runtime_->sweepFrames);
    } else if (elapsedFrames < rightSegmentStart + runtime_->leadInFrames) {
        currentFrequencyHz_ = snapshot_.measurement.startFrequencyHz;
    } else {
        const size_t rightSweepFrame = std::min(runtime_->sweepFrames, elapsedFrames - rightSegmentStart - runtime_->leadInFrames);
        currentFrequencyHz_ = sweepFrequencyAtSample(snapshot_.measurement,
                                                     runtime_->sampleRate,
                                                     rightSweepFrame,
                                                     runtime_->sweepFrames);
    }

    const bool playbackDone = (runtime_->playbackHeader.dwFlags & WHDR_DONE) != 0;
    if (!playbackDone && elapsedMs < durationMs_) {
        return;
    }

    runtime_->inputActive = false;
    if (runtime_->waveIn != nullptr) {
        waveInStop(runtime_->waveIn);
        waveInReset(runtime_->waveIn);
    }
    runtime_->processCapture(currentAmplitudeDb_, peakAmplitudeDb_);

    running_ = false;
    finished_ = true;
    progress_ = 1.0;
    currentChannel_ = MeasurementChannel::None;
    currentFrequencyHz_ = snapshot_.measurement.endFrequencyHz;
    result_ = buildMeasurementResultFromCapture(runtime_->capturedSamples,
                                                runtime_->playedSweep,
                                                runtime_->leadInFrames,
                                                runtime_->sampleRate,
                                                snapshot_.measurement);
    cleanupRuntime();
}

WolfieApp::WolfieApp(HINSTANCE instance) : instance_(instance) {}

int WolfieApp::run() {
    INITCOMMONCONTROLSEX initCommonControls{};
    initCommonControls.dwSize = sizeof(initCommonControls);
    initCommonControls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&initCommonControls);

    loadAppState();
    createMainWindow();
    loadLastWorkspaceIfPossible();

    ACCEL accelerator{};
    accelerator.fVirt = FCONTROL | FVIRTKEY;
    accelerator.key = 'S';
    accelerator.cmd = kMenuFileSave;
    acceleratorTable_ = CreateAcceleratorTableW(&accelerator, 1);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(mainWindow_, acceleratorTable_, &message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK WolfieApp::MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    WolfieApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<WolfieApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<WolfieApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (!app) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == kComboMeasurementSampleRate && HIWORD(wParam) != CBN_SELCHANGE) {
            return 0;
        }
        app->onCommand(LOWORD(wParam));
        return 0;
    case WM_NOTIFY:
        app->onNotify(lParam);
        return 0;
    case WM_HSCROLL:
        app->onHScroll(lParam);
        return 0;
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (draw != nullptr && draw->CtlID == kButtonMeasure) {
            HDC hdc = draw->hDC;
            RECT rect = draw->rcItem;
            const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
            const bool focused = (draw->itemState & ODS_FOCUS) != 0;
            const bool running = app->measurementEngine_.running();
            const COLORREF fill = running ? kRed : kGreen;
            const COLORREF fillPressed = running ? RGB(156, 53, 53) : RGB(37, 118, 68);
            const COLORREF border = running ? RGB(132, 42, 42) : RGB(29, 95, 55);

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
            DrawTextW(hdc, running ? L"STOP" : L"START", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            if (focused) {
                RECT focusRect = rect;
                InflateRect(&focusRect, -4, -4);
                DrawFocusRect(hdc, &focusRect);
            }

            SelectObject(hdc, oldFont);
            DeleteObject(buttonFont);
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'S' || wParam == 's')) {
            app->saveWorkspace(false);
            return 0;
        }
        break;
    case WM_TIMER:
        app->onTimer(wParam);
        return 0;
    case WM_SIZE:
        app->onResize();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 1120;
        info->ptMinTrackSize.y = 760;
        return 0;
    }
    case WM_DESTROY:
        if (app->acceleratorTable_) {
            DestroyAcceleratorTable(app->acceleratorTable_);
            app->acceleratorTable_ = nullptr;
        }
        app->saveAppState();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK WolfieApp::GraphWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        HBRUSH background = CreateSolidBrush(RGB(248, 250, 252));
        FillRect(hdc, &rect, background);
        DeleteObject(background);
        Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);

        auto* app = reinterpret_cast<WolfieApp*>(GetWindowLongPtrW(GetAncestor(window, GA_ROOT), GWLP_USERDATA));
        const auto kind = static_cast<GraphKind>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (app) {
            SetBkMode(hdc, TRANSPARENT);
            SelectObject(hdc, GetStockObject(DC_PEN));
            SelectObject(hdc, GetStockObject(DC_BRUSH));

            if (kind == GraphKind::Response) {
                RECT graph = rect;
                InflateRect(&graph, -48, -32);
                MoveToEx(hdc, graph.left, graph.top, nullptr);
                LineTo(hdc, graph.left, graph.bottom);
                LineTo(hdc, graph.right, graph.bottom);

                const MeasurementResult& result = app->workspace_.result;
                constexpr std::array<double, 5> kFrequencyTicks{10.0, 100.0, 1000.0, 10000.0, 20000.0};
                constexpr double kMinDb = -30.0;
                double maxDb = kMinDb + 1.0;
                for (const double value : result.leftChannelDb) {
                    maxDb = std::max(maxDb, value);
                }
                for (const double value : result.rightChannelDb) {
                    maxDb = std::max(maxDb, value);
                }
                maxDb = std::ceil(maxDb);

                SetDCPenColor(hdc, kBorder);
                for (const double tickHz : kFrequencyTicks) {
                    const int x = graph.left + static_cast<int>(responseGraphXT(tickHz) * (graph.right - graph.left));
                    MoveToEx(hdc, x, graph.top, nullptr);
                    LineTo(hdc, x, graph.bottom);
                }
                const int zeroY = graph.bottom - static_cast<int>(clampValue((0.0 - kMinDb) / (maxDb - kMinDb), 0.0, 1.0) * (graph.bottom - graph.top));
                MoveToEx(hdc, graph.left, zeroY, nullptr);
                LineTo(hdc, graph.right, zeroY);

                if (!result.frequencyAxisHz.empty()) {
                    auto drawSeries = [&](const std::vector<double>& values, COLORREF color) {
                        SetDCPenColor(hdc, color);
                        for (size_t i = 0; i < values.size(); ++i) {
                            const double xT = responseGraphXT(result.frequencyAxisHz[i]);
                            const double yT = clampValue((values[i] - kMinDb) / (maxDb - kMinDb), 0.0, 1.0);
                            const int x = graph.left + static_cast<int>(xT * (graph.right - graph.left));
                            const int y = graph.bottom - static_cast<int>(yT * (graph.bottom - graph.top));
                            if (i == 0) {
                                MoveToEx(hdc, x, y, nullptr);
                            } else {
                                LineTo(hdc, x, y);
                            }
                        }
                    };

                    drawSeries(result.leftChannelDb, kGreen);
                    drawSeries(result.rightChannelDb, kRed);
                }

                SetTextColor(hdc, kMuted);
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
                DrawTextW(hdc, L"-30 dB", -1, &labelBottom, DT_RIGHT);
                if (zeroY > graph.top + 12 && zeroY < graph.bottom - 12) {
                    DrawTextW(hdc, L"0 dB", -1, &labelZero, DT_RIGHT);
                }
            }
        }

        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK WolfieApp::PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static HBRUSH pageBackgroundBrush = CreateSolidBrush(kBackground);
    switch (message) {
    case WM_COMMAND:
    case WM_HSCROLL: {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr) {
            return SendMessageW(root, message, wParam, lParam);
        }
        return 0;
    }
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
        SetTextColor(hdc, kText);
        return reinterpret_cast<INT_PTR>(pageBackgroundBrush);
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK WolfieApp::SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static HBRUSH settingsBackgroundBrush = CreateSolidBrush(kBackground);
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE: {
        auto* app = reinterpret_cast<WolfieApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        const auto drivers = enumerateAsioDrivers();
        CreateWindowW(L"STATIC", L"ASIO driver", WS_CHILD | WS_VISIBLE, 20, 20, 140, 20, window, nullptr, nullptr, nullptr);
        HWND driver = CreateWindowW(L"COMBOBOX", nullptr,
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                    170, 16, 240, 240, window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
        for (const auto& driverName : drivers) {
            SendMessageW(driver, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(driverName.c_str()));
        }
        if (drivers.empty()) {
            SendMessageW(driver, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kNoAsioDrivers));
        }
        selectComboBoxString(driver, toWide(app->workspace_.audio.driver));
        CreateWindowW(L"STATIC", L"Mic input channel", WS_CHILD | WS_VISIBLE, 20, 56, 140, 20, window, nullptr, nullptr, nullptr);
        HWND mic = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", formatWideDouble(app->workspace_.audio.micInputChannel, 0).c_str(),
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 170, 52, 80, 24, window,
                                    reinterpret_cast<HMENU>(2), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Left output channel", WS_CHILD | WS_VISIBLE, 20, 92, 140, 20, window, nullptr, nullptr, nullptr);
        HWND left = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", formatWideDouble(app->workspace_.audio.leftOutputChannel, 0).c_str(),
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 170, 88, 80, 24, window,
                                    reinterpret_cast<HMENU>(3), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Right output channel", WS_CHILD | WS_VISIBLE, 20, 128, 140, 20, window, nullptr, nullptr, nullptr);
        HWND right = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", formatWideDouble(app->workspace_.audio.rightOutputChannel, 0).c_str(),
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 170, 124, 80, 24, window,
                                     reinterpret_cast<HMENU>(4), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open ASIO Control Panel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 164, 180, 28, window,
                      reinterpret_cast<HMENU>(7), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 340, 164, 80, 28, window,
                      reinterpret_cast<HMENU>(8), nullptr, nullptr);

        SetPropW(window, L"driver", driver);
        SetPropW(window, L"mic", mic);
        SetPropW(window, L"left", left);
        SetPropW(window, L"right", right);
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(settingsBackgroundBrush);
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kText);
        return reinterpret_cast<INT_PTR>(settingsBackgroundBrush);
    }
    case WM_COMMAND: {
        auto* app = reinterpret_cast<WolfieApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        auto autosaveSettings = [&]() {
            const std::wstring driverText = getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"driver")));
            if (driverText != kNoAsioDrivers) {
                app->workspace_.audio.driver = toUtf8(driverText);
            }

            auto tryParseInt = [](const std::wstring& text, int& value) {
                try {
                    size_t cursor = 0;
                    const int parsed = std::stoi(text, &cursor);
                    if (cursor != text.size()) {
                        return false;
                    }
                    value = parsed;
                    return true;
                } catch (...) {
                    return false;
                }
            };

            int parsedValue = 0;
            if (tryParseInt(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"mic"))), parsedValue)) {
                app->workspace_.audio.micInputChannel = parsedValue;
            }
            if (tryParseInt(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"left"))), parsedValue)) {
                app->workspace_.audio.leftOutputChannel = parsedValue;
            }
            if (tryParseInt(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"right"))), parsedValue)) {
                app->workspace_.audio.rightOutputChannel = parsedValue;
            }

            app->populateControlsFromState();
            app->saveWorkspaceFiles();
            app->refreshMeasurementStatus();
        };

        const WORD commandId = LOWORD(wParam);
        const WORD notificationCode = HIWORD(wParam);
        if (commandId == 1 && notificationCode == CBN_SELCHANGE) {
            autosaveSettings();
            return 0;
        }
        if ((commandId == 2 || commandId == 3 || commandId == 4) && notificationCode == EN_KILLFOCUS) {
            autosaveSettings();
            return 0;
        }
        if (commandId == 7) {
            const std::wstring driverText = getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"driver")));
            if (const auto error = openAsioControlPanel(window, driverText)) {
                MessageBoxW(window, error->c_str(), L"ASIO Control Panel", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (commandId == 8) {
            autosaveSettings();
            DestroyWindow(window);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (auto* app = reinterpret_cast<WolfieApp*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
            const std::wstring driverText = getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"driver")));
            if (driverText != kNoAsioDrivers) {
                app->workspace_.audio.driver = toUtf8(driverText);
            }

            auto tryParseInt = [](const std::wstring& text, int& value) {
                try {
                    size_t cursor = 0;
                    const int parsed = std::stoi(text, &cursor);
                    if (cursor != text.size()) {
                        return false;
                    }
                    value = parsed;
                    return true;
                } catch (...) {
                    return false;
                }
            };

            int parsedValue = 0;
            if (tryParseInt(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"mic"))), parsedValue)) {
                app->workspace_.audio.micInputChannel = parsedValue;
            }
            if (tryParseInt(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"left"))), parsedValue)) {
                app->workspace_.audio.leftOutputChannel = parsedValue;
            }
            if (tryParseInt(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"right"))), parsedValue)) {
                app->workspace_.audio.rightOutputChannel = parsedValue;
            }
            app->populateControlsFromState();
            app->saveWorkspaceFiles();
            app->refreshMeasurementStatus();
        }
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return 0;
}

void WolfieApp::createMainWindow() {
    WNDCLASSW mainClass{};
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = instance_;
    mainClass.lpszClassName = kMainClassName;
    mainClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mainClass.hbrBackground = CreateSolidBrush(kBackground);
    RegisterClassW(&mainClass);

    WNDCLASSW graphClass{};
    graphClass.lpfnWndProc = GraphWindowProc;
    graphClass.hInstance = instance_;
    graphClass.lpszClassName = kGraphClassName;
    graphClass.hCursor = LoadCursor(nullptr, IDC_CROSS);
    graphClass.hbrBackground = CreateSolidBrush(RGB(248, 250, 252));
    RegisterClassW(&graphClass);

    WNDCLASSW pageClass{};
    pageClass.lpfnWndProc = PageWindowProc;
    pageClass.hInstance = instance_;
    pageClass.lpszClassName = kPageClassName;
    pageClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pageClass.hbrBackground = CreateSolidBrush(kBackground);
    RegisterClassW(&pageClass);

    WNDCLASSW settingsClass{};
    settingsClass.lpfnWndProc = SettingsWindowProc;
    settingsClass.hInstance = instance_;
    settingsClass.lpszClassName = kSettingsClassName;
    settingsClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = CreateSolidBrush(kBackground);
    RegisterClassW(&settingsClass);

    mainWindow_ = CreateWindowExW(
        0, kMainClassName, L"Wolfie", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1400, 920, nullptr, nullptr, instance_, this);

    createMenus();
    createLayout();
    populateControlsFromState();
    refreshWindowTitle();
    refreshMeasurementStatus();
}

void WolfieApp::createMenus() {
    HMENU menuBar = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();

    AppendMenuW(fileMenu, MF_STRING, kMenuFileNew, L"&New...");
    AppendMenuW(fileMenu, MF_STRING, kMenuFileOpen, L"&Open...");
    HMENU recentMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(recentMenu), L"&Recent");
    AppendMenuW(fileMenu, MF_STRING, kMenuFileSave, L"&Save\tCtrl+S");
    AppendMenuW(fileMenu, MF_STRING, kMenuFileSaveAs, L"Save &As...");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, kMenuFileSettings, L"&Settings");

    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");
    SetMenu(mainWindow_, menuBar);
    refreshRecentMenu();
}

void WolfieApp::createLayout() {
    controls_.tabControl = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
                                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                                           0, 0, 0, 0, mainWindow_, reinterpret_cast<HMENU>(kTabMain), instance_, nullptr);

    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<LPWSTR>(L"Measurement");
    TabCtrl_InsertItem(controls_.tabControl, 0, &item);
    item.pszText = const_cast<LPWSTR>(L"Align Mic");
    TabCtrl_InsertItem(controls_.tabControl, 1, &item);
    item.pszText = const_cast<LPWSTR>(L"Target Curve");
    TabCtrl_InsertItem(controls_.tabControl, 2, &item);
    item.pszText = const_cast<LPWSTR>(L"Filters");
    TabCtrl_InsertItem(controls_.tabControl, 3, &item);
    item.pszText = const_cast<LPWSTR>(L"Export");
    TabCtrl_InsertItem(controls_.tabControl, 4, &item);

    controls_.pageMeasurement = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                                0, 0, 0, 0, controls_.tabControl, nullptr, instance_, nullptr);
    controls_.pageAlignment = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                              0, 0, 0, 0, controls_.tabControl, nullptr, instance_, nullptr);
    controls_.pageTargetCurve = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                                0, 0, 0, 0, controls_.tabControl, nullptr, instance_, nullptr);
    controls_.pageFilters = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                            0, 0, 0, 0, controls_.tabControl, nullptr, instance_, nullptr);
    controls_.pageExport = CreateWindowExW(0, kPageClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                           0, 0, 0, 0, controls_.tabControl, nullptr, instance_, nullptr);

    controls_.placeholderAlignment = CreateWindowW(L"STATIC", L"Microphone alignment will live here.", WS_CHILD | SS_CENTER,
                                                   0, 0, 0, 0, controls_.pageAlignment, nullptr, instance_, nullptr);
    controls_.placeholderTargetCurve = CreateWindowW(L"STATIC", L"Target curve design is not implemented yet.", WS_CHILD | SS_CENTER,
                                                     0, 0, 0, 0, controls_.pageTargetCurve, nullptr, instance_, nullptr);
    controls_.placeholderFilters = CreateWindowW(L"STATIC", L"Filter design and simulation will live here.", WS_CHILD | SS_CENTER,
                                                 0, 0, 0, 0, controls_.pageFilters, nullptr, instance_, nullptr);
    controls_.placeholderExport = CreateWindowW(L"STATIC", L"ROON export will live here.", WS_CHILD | SS_CENTER,
                                                0, 0, 0, 0, controls_.pageExport, nullptr, instance_, nullptr);

    controls_.labelFadeIn = CreateWindowW(L"STATIC", L"Fade-In", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.labelFadeOut = CreateWindowW(L"STATIC", L"Fade-Out", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.labelDuration = CreateWindowW(L"STATIC", L"Sweep Time", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.labelStartFrequency = CreateWindowW(L"STATIC", L"Sweep Start", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.labelEndFrequency = CreateWindowW(L"STATIC", L"Sweep End", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.labelTargetLength = CreateWindowW(L"STATIC", L"Target Length", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.labelLeadIn = CreateWindowW(L"STATIC", L"Lead-In", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.labelSampleRate = CreateWindowW(L"STATIC", L"Sample Rate", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.unitFadeIn = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.unitFadeOut = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.unitDuration = CreateWindowW(L"STATIC", L"sec", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.unitStartFrequency = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.unitEndFrequency = CreateWindowW(L"STATIC", L"Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.unitTargetLength = CreateWindowW(L"STATIC", L"samples", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.unitLeadIn = CreateWindowW(L"STATIC", L"samples", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);

    controls_.editFadeIn = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.pageMeasurement,
                                           reinterpret_cast<HMENU>(kEditFadeIn), instance_, nullptr);
    controls_.editFadeOut = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.pageMeasurement,
                                            reinterpret_cast<HMENU>(kEditFadeOut), instance_, nullptr);
    controls_.editDuration = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.pageMeasurement,
                                             reinterpret_cast<HMENU>(kEditDuration), instance_, nullptr);
    controls_.editStartFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.pageMeasurement,
                                                   reinterpret_cast<HMENU>(kEditStartFrequency), instance_, nullptr);
    controls_.editEndFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.pageMeasurement,
                                                 reinterpret_cast<HMENU>(kEditEndFrequency), instance_, nullptr);
    controls_.editTargetLength = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.pageMeasurement,
                                                 reinterpret_cast<HMENU>(kEditTargetLength), instance_, nullptr);
    controls_.editLeadIn = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.pageMeasurement,
                                           reinterpret_cast<HMENU>(kEditLeadIn), instance_, nullptr);
    controls_.comboSampleRate = CreateWindowW(L"COMBOBOX", nullptr,
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                              0, 0, 0, 0, controls_.pageMeasurement,
                                              reinterpret_cast<HMENU>(kComboMeasurementSampleRate), instance_, nullptr);
    controls_.labelOutputVolume = CreateWindowW(L"STATIC", L"Output level", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.outputVolumeValue = CreateWindowW(L"STATIC", L"-30 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.outputVolumeSlider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
                                                   WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                                                   0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.outputVolumeMuteLabel = CreateWindowW(L"STATIC", L"Mute", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.outputVolumeMaxLabel = CreateWindowW(L"STATIC", L"0 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.buttonMeasure = CreateWindowW(L"BUTTON", L"START", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW, 0, 0, 0, 0, controls_.pageMeasurement,
                                            reinterpret_cast<HMENU>(kButtonMeasure), instance_, nullptr);
    controls_.leftChannelLabel = CreateWindowW(L"STATIC", L"Left", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.leftProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.leftProgressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.rightChannelLabel = CreateWindowW(L"STATIC", L"Right", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.rightProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.rightProgressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);

    controls_.statusText = CreateWindowW(L"STATIC", L"Ready", WS_CHILD, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.currentFrequency = CreateWindowW(L"STATIC", L"Freq 0 Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.currentAmplitude = CreateWindowW(L"STATIC", L"Amp -90 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.peakAmplitude = CreateWindowW(L"STATIC", L"Peak -90 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_, nullptr);
    controls_.resultGraph = CreateWindowExW(0, kGraphClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.pageMeasurement, nullptr, instance_,
                                            reinterpret_cast<void*>(static_cast<INT_PTR>(GraphKind::Response)));

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
    populateMeasurementSampleRateCombo(controls_.comboSampleRate);
    updateVisibleTab();
    layoutMainWindow();
}

void WolfieApp::layoutMainWindow() {
    const RECT bounds = clientRect(mainWindow_);
    const int width = std::max(320L, bounds.right - (2 * kContentMargin));
    const int height = std::max(360L, bounds.bottom - (2 * kContentMargin));
    MoveWindow(controls_.tabControl, kContentMargin, kContentMargin, width, height, TRUE);

    RECT tabRect{};
    GetClientRect(controls_.tabControl, &tabRect);
    TabCtrl_AdjustRect(controls_.tabControl, FALSE, &tabRect);
    const int pageWidth = std::max(320L, tabRect.right - tabRect.left);
    const int pageHeight = std::max(240L, tabRect.bottom - tabRect.top);
    MoveWindow(controls_.pageMeasurement, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(controls_.pageAlignment, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(controls_.pageTargetCurve, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(controls_.pageFilters, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    MoveWindow(controls_.pageExport, tabRect.left, tabRect.top, pageWidth, pageHeight, TRUE);
    layoutContent();
}

void WolfieApp::layoutContent() {
    RECT alignmentRect = clientRect(controls_.pageAlignment);
    MoveWindow(controls_.placeholderAlignment, 24, 32, std::max(200L, alignmentRect.right - 48), 24, TRUE);
    RECT targetRect = clientRect(controls_.pageTargetCurve);
    MoveWindow(controls_.placeholderTargetCurve, 24, 32, std::max(200L, targetRect.right - 48), 24, TRUE);
    RECT filtersRect = clientRect(controls_.pageFilters);
    MoveWindow(controls_.placeholderFilters, 24, 32, std::max(200L, filtersRect.right - 48), 24, TRUE);
    RECT exportRect = clientRect(controls_.pageExport);
    MoveWindow(controls_.placeholderExport, 24, 32, std::max(200L, exportRect.right - 48), 24, TRUE);

    RECT measurementRect = clientRect(controls_.pageMeasurement);
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
    constexpr int kValueWidthLarge = 82;
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
    constexpr int kMetricWidth = 150;
    constexpr int kMetricGap = 16;
    constexpr int kSliderWidth = 220;
    constexpr int kSliderValueWidth = 56;

    auto placeCenteredField = [&](HWND label, HWND edit, int left, int top, int labelWidth, int editWidth) {
        const int labelLeft = left + ((editWidth - labelWidth) / 2);
        MoveWindow(label, labelLeft, top + kLabelTopOffset, labelWidth, 18, TRUE);
        MoveWindow(edit, left, top + kFieldTopOffset, editWidth, 26, TRUE);
    };

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
        // For dropdown combos the window height defines the popup list height as well.
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
    MoveWindow(controls_.statusText, contentLeft, dataRowTop + 4, 0, 0, TRUE);

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

    const int graphTop = progressRowTop + 40;
    const int graphHeight = std::max(200, innerHeight - graphTop - 12);
    MoveWindow(controls_.resultGraph, contentLeft, graphTop, innerWidth, graphHeight, TRUE);
}

void WolfieApp::showSettingsWindow() {
    CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClassName, L"Measurement Settings",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, 460, 246, mainWindow_, nullptr, instance_, this);
}

void WolfieApp::populateControlsFromState() {
    syncDerivedMeasurementSettings(workspace_.measurement);
    setWindowText(controls_.editFadeIn, formatWideDouble(workspace_.measurement.fadeInSeconds));
    setWindowText(controls_.editFadeOut, formatWideDouble(workspace_.measurement.fadeOutSeconds));
    setWindowText(controls_.editDuration, formatWideDouble(workspace_.measurement.durationSeconds, 0));
    setWindowText(controls_.editStartFrequency, formatWideDouble(workspace_.measurement.startFrequencyHz, 0));
    setWindowText(controls_.editEndFrequency, formatWideDouble(workspace_.measurement.endFrequencyHz, 0));
    setWindowText(controls_.editTargetLength, formatWideDouble(workspace_.measurement.targetLengthSamples, 0));
    setWindowText(controls_.editLeadIn, formatWideDouble(workspace_.measurement.leadInSamples, 0));
    if (controls_.comboSampleRate != nullptr) {
        SendMessageW(controls_.comboSampleRate, CB_SETCURSEL, comboIndexFromMeasurementSampleRate(workspace_.measurement.sampleRate), 0);
    }
    if (controls_.outputVolumeValue != nullptr) {
        setWindowText(controls_.outputVolumeValue, formatOutputVolumeLabel(workspace_.audio.outputVolumeDb));
    }
    if (controls_.outputVolumeSlider != nullptr) {
        SendMessageW(controls_.outputVolumeSlider, TBM_SETPOS, TRUE, outputVolumeDbToSliderPosition(workspace_.audio.outputVolumeDb));
    }
}

void WolfieApp::syncStateFromControls() {
    workspace_.measurement.sampleRate = measurementSampleRateFromComboIndex(
        static_cast<int>(SendMessageW(controls_.comboSampleRate, CB_GETCURSEL, 0, 0)));
    workspace_.measurement.fadeInSeconds = std::stod(getWindowText(controls_.editFadeIn));
    workspace_.measurement.fadeOutSeconds = std::stod(getWindowText(controls_.editFadeOut));
    workspace_.measurement.durationSeconds = std::stod(getWindowText(controls_.editDuration));
    workspace_.measurement.startFrequencyHz = std::stod(getWindowText(controls_.editStartFrequency));
    workspace_.measurement.targetLengthSamples = std::stoi(getWindowText(controls_.editTargetLength));
    workspace_.measurement.leadInSamples = std::stoi(getWindowText(controls_.editLeadIn));
    syncDerivedMeasurementSettings(workspace_.measurement);
}

void WolfieApp::refreshWindowTitle() {
    std::wstring title = L"Wolfie";
    if (!workspace_.rootPath.empty()) {
        title += L" - " + workspace_.rootPath.filename().wstring();
    }
    SetWindowTextW(mainWindow_, title.c_str());
}

void WolfieApp::refreshMeasurementStatus() {
    int leftProgress = 0;
    int rightProgress = 0;
    const int currentProgress = static_cast<int>(measurementEngine_.progress() * 1000.0);
    if (measurementEngine_.running()) {
        if (measurementEngine_.currentChannel() == MeasurementChannel::Right) {
            leftProgress = 1000;
            rightProgress = currentProgress;
        } else {
            leftProgress = currentProgress;
        }
    } else if (measurementEngine_.finished()) {
        leftProgress = 1000;
        rightProgress = 1000;
    }

    SendMessageW(controls_.leftProgressBar, PBM_SETPOS, leftProgress, 0);
    SendMessageW(controls_.rightProgressBar, PBM_SETPOS, rightProgress, 0);
    setWindowText(controls_.leftProgressText, std::to_wstring(leftProgress / 10) + L"%");
    setWindowText(controls_.rightProgressText, std::to_wstring(rightProgress / 10) + L"%");
    setWindowText(controls_.currentFrequency, L"Freq " + formatWideDouble(measurementEngine_.currentFrequencyHz(), 0) + L" Hz");
    setWindowText(controls_.currentAmplitude, L"Amp " + formatWideDouble(measurementEngine_.currentAmplitudeDb(), 1) + L" dB");
    setWindowText(controls_.peakAmplitude, L"Peak " + formatWideDouble(measurementEngine_.peakAmplitudeDb(), 1) + L" dB");
    InvalidateRect(controls_.buttonMeasure, nullptr, TRUE);
    EnableWindow(controls_.buttonMeasure, TRUE);

    if (measurementEngine_.running()) {
        setWindowText(controls_.statusText, measurementEngine_.currentChannel() == MeasurementChannel::Right
            ? L"measuring RIGHT"
            : L"measuring LEFT");
    } else if (!measurementEngine_.lastError().empty()) {
        setWindowText(controls_.statusText, std::wstring(measurementEngine_.lastError()));
    } else if (!workspace_.result.frequencyAxisHz.empty()) {
        setWindowText(controls_.statusText, L"Measurement complete");
    } else {
        setWindowText(controls_.statusText, L"Ready to measure");
    }

}

void WolfieApp::refreshRecentMenu() {
    HMENU menuBar = GetMenu(mainWindow_);
    HMENU fileMenu = GetSubMenu(menuBar, 0);
    HMENU recentMenu = GetSubMenu(fileMenu, 2);
    while (GetMenuItemCount(recentMenu) > 0) {
        DeleteMenu(recentMenu, 0, MF_BYPOSITION);
    }

    if (appState_.recentWorkspaces.empty()) {
        AppendMenuW(recentMenu, MF_GRAYED | MF_STRING, kMenuFileRecentBase, L"(none)");
        return;
    }

    for (size_t i = 0; i < appState_.recentWorkspaces.size() && i < 8; ++i) {
        AppendMenuW(recentMenu, MF_STRING, kMenuFileRecentBase + static_cast<UINT>(i),
                    appState_.recentWorkspaces[i].wstring().c_str());
    }
}

void WolfieApp::onCommand(WORD commandId) {
    switch (commandId) {
    case kMenuFileNew:
        newWorkspace();
        return;
    case kMenuFileOpen:
        openWorkspace();
        return;
    case kMenuFileSave:
        saveWorkspace(false);
        return;
    case kMenuFileSaveAs:
        saveWorkspace(true);
        return;
    case kMenuFileSettings:
        showSettingsWindow();
        return;
    case kButtonMeasure:
        if (measurementEngine_.running()) {
            stopMeasurement();
        } else {
            startMeasurement();
        }
        return;
    case kComboMeasurementSampleRate:
        try {
            syncStateFromControls();
        } catch (...) {
        }
        workspace_.measurement.sampleRate = measurementSampleRateFromComboIndex(
            static_cast<int>(SendMessageW(controls_.comboSampleRate, CB_GETCURSEL, 0, 0)));
        syncDerivedMeasurementSettings(workspace_.measurement);
        populateControlsFromState();
        saveWorkspaceFiles();
        refreshMeasurementStatus();
        return;
    default:
        if (commandId >= kMenuFileRecentBase && commandId < kMenuFileRecentBase + 8) {
            const size_t index = commandId - kMenuFileRecentBase;
            if (index < appState_.recentWorkspaces.size()) {
                openWorkspace(appState_.recentWorkspaces[index]);
            }
        }
        return;
    }
}

void WolfieApp::onHScroll(LPARAM lParam) {
    if (reinterpret_cast<HWND>(lParam) != controls_.outputVolumeSlider) {
        return;
    }

    workspace_.audio.outputVolumeDb = sliderPositionToOutputVolumeDb(
        static_cast<int>(SendMessageW(controls_.outputVolumeSlider, TBM_GETPOS, 0, 0)));
    setWindowText(controls_.outputVolumeValue, formatOutputVolumeLabel(workspace_.audio.outputVolumeDb));
    saveWorkspaceFiles();
}

void WolfieApp::onNotify(LPARAM lParam) {
    const auto* header = reinterpret_cast<const NMHDR*>(lParam);
    if (!header || header->hwndFrom != controls_.tabControl) {
        return;
    }
    if (header->code == TCN_SELCHANGE) {
        updateVisibleTab();
        layoutContent();
    }
}

void WolfieApp::onTimer(UINT_PTR timerId) {
    if (timerId != kMeasurementTimerId) {
        return;
    }

    measurementEngine_.tick();
    refreshMeasurementStatus();
    if (measurementEngine_.finished()) {
        finalizeMeasurement();
        KillTimer(mainWindow_, kMeasurementTimerId);
    }
}

void WolfieApp::onResize() {
    layoutMainWindow();
    RedrawWindow(mainWindow_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void WolfieApp::updateVisibleTab() {
    const int selected = controls_.tabControl == nullptr ? 0 : TabCtrl_GetCurSel(controls_.tabControl);
    ShowWindow(controls_.pageMeasurement, selected == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.pageAlignment, selected == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.pageTargetCurve, selected == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.pageFilters, selected == 3 ? SW_SHOW : SW_HIDE);
    ShowWindow(controls_.pageExport, selected == 4 ? SW_SHOW : SW_HIDE);
}

void WolfieApp::invalidateGraphs() {
    if (controls_.resultGraph != nullptr) {
        InvalidateRect(controls_.resultGraph, nullptr, TRUE);
    }
}

void WolfieApp::newWorkspace() {
    auto path = pickFolder(true);
    if (!path) {
        return;
    }

    workspace_ = {};
    workspace_.rootPath = *path;
    syncDerivedMeasurementSettings(workspace_.measurement);
    populateControlsFromState();
    refreshWindowTitle();
    invalidateGraphs();
    saveWorkspaceFiles();
    touchRecentWorkspace(*path);
}

void WolfieApp::openWorkspace() {
    auto path = pickFolder(false);
    if (path) {
        openWorkspace(*path);
    }
}

void WolfieApp::openWorkspace(const std::filesystem::path& path) {
    loadWorkspace(path);
    touchRecentWorkspace(path);
    populateControlsFromState();
    refreshWindowTitle();
    refreshMeasurementStatus();
    invalidateGraphs();
}

void WolfieApp::saveWorkspace(bool saveAs) {
    if (saveAs || workspace_.rootPath.empty()) {
        auto path = pickFolder(true);
        if (!path) {
            return;
        }
        workspace_.rootPath = *path;
        touchRecentWorkspace(*path);
    }

    syncStateFromControls();
    saveWorkspaceFiles();
    refreshWindowTitle();
}

void WolfieApp::loadLastWorkspaceIfPossible() {
    if (!appState_.lastWorkspace.empty() && std::filesystem::exists(appState_.lastWorkspace / "workspace.json")) {
        openWorkspace(appState_.lastWorkspace);
    }
}

void WolfieApp::touchRecentWorkspace(const std::filesystem::path& path) {
    appState_.lastWorkspace = path;
    appState_.recentWorkspaces.erase(std::remove(appState_.recentWorkspaces.begin(), appState_.recentWorkspaces.end(), path),
                                     appState_.recentWorkspaces.end());
    appState_.recentWorkspaces.insert(appState_.recentWorkspaces.begin(), path);
    if (appState_.recentWorkspaces.size() > 8) {
        appState_.recentWorkspaces.resize(8);
    }
    refreshRecentMenu();
    saveAppState();
}

std::optional<std::filesystem::path> WolfieApp::pickFolder(bool createIfMissing) const {
    std::optional<std::filesystem::path> selectedPath;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (dialog->Show(mainWindow_) == S_OK) {
            IShellItem* item = nullptr;
            if (dialog->GetResult(&item) == S_OK) {
                PWSTR pathBuffer = nullptr;
                if (item->GetDisplayName(SIGDN_FILESYSPATH, &pathBuffer) == S_OK) {
                    selectedPath = std::filesystem::path(pathBuffer);
                    CoTaskMemFree(pathBuffer);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }

    if (selectedPath && createIfMissing) {
        std::filesystem::create_directories(*selectedPath);
    }
    return selectedPath;
}

std::filesystem::path WolfieApp::appStatePath() const {
    return std::filesystem::current_path() / "wolfie-app-state.json";
}

void WolfieApp::loadAppState() {
    const auto content = readTextFile(appStatePath());
    if (!content) {
        return;
    }

    if (const auto lastWorkspace = findJsonString(*content, "lastWorkspace")) {
        appState_.lastWorkspace = std::filesystem::path(*lastWorkspace);
    }

    for (const auto& recent : findJsonStringArray(*content, "recentWorkspaces")) {
        appState_.recentWorkspaces.emplace_back(recent);
    }
}

void WolfieApp::saveAppState() const {
    std::ostringstream out;
    out << "{\n"
        << "  \"lastWorkspace\": \"" << escapeJson(appState_.lastWorkspace.generic_string()) << "\",\n"
        << "  \"recentWorkspaces\": [";
    for (size_t i = 0; i < appState_.recentWorkspaces.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "\"" << escapeJson(appState_.recentWorkspaces[i].generic_string()) << "\"";
    }
    out << "]\n}\n";
    writeTextFile(appStatePath(), out.str());
}

void WolfieApp::loadWorkspace(const std::filesystem::path& path) {
    workspace_ = {};
    workspace_.rootPath = path;
    syncDerivedMeasurementSettings(workspace_.measurement);

    if (const auto content = readTextFile(path / "workspace.json")) {
        if (const auto driver = findJsonString(*content, "driver")) {
            workspace_.audio.driver = *driver;
        }
        if (const auto value = findJsonNumber(*content, "micInputChannel")) {
            workspace_.audio.micInputChannel = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "leftOutputChannel")) {
            workspace_.audio.leftOutputChannel = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "rightOutputChannel")) {
            workspace_.audio.rightOutputChannel = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "sampleRate")) {
            workspace_.measurement.sampleRate = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "outputVolumeDb")) {
            workspace_.audio.outputVolumeDb = *value;
        }
        if (const auto value = findJsonNumber(*content, "fadeInSeconds")) {
            workspace_.measurement.fadeInSeconds = *value;
        }
        if (const auto value = findJsonNumber(*content, "fadeOutSeconds")) {
            workspace_.measurement.fadeOutSeconds = *value;
        }
        if (const auto value = findJsonNumber(*content, "durationSeconds")) {
            workspace_.measurement.durationSeconds = *value;
        }
        if (const auto value = findJsonNumber(*content, "startFrequencyHz")) {
            workspace_.measurement.startFrequencyHz = *value;
        }
        if (const auto value = findJsonNumber(*content, "targetLengthSamples")) {
            workspace_.measurement.targetLengthSamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "leadInSamples")) {
            workspace_.measurement.leadInSamples = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "measurementSectionHeight")) {
            workspace_.ui.measurementSectionHeight = static_cast<int>(*value);
        }
        if (const auto value = findJsonNumber(*content, "resultSectionHeight")) {
            workspace_.ui.resultSectionHeight = static_cast<int>(*value);
        }
    }

    syncDerivedMeasurementSettings(workspace_.measurement);

    loadMeasurementResultFile();
}

void WolfieApp::loadMeasurementResultFile() {
    workspace_.result = {};
    if (workspace_.rootPath.empty()) {
        return;
    }

    if (const auto response = readTextFile(workspace_.rootPath / "measurement" / "response.csv")) {
        std::istringstream in(*response);
        std::string line;
        while (std::getline(in, line)) {
            if (line.starts_with("frequency")) {
                continue;
            }
            std::istringstream row(line);
            std::string cell;
            std::array<double, 3> values{};
            int index = 0;
            while (std::getline(row, cell, ',') && index < 3) {
                values[index++] = std::stod(cell);
            }
            if (index == 3) {
                workspace_.result.frequencyAxisHz.push_back(values[0]);
                workspace_.result.leftChannelDb.push_back(values[1]);
                workspace_.result.rightChannelDb.push_back(values[2]);
            }
        }
    }
}

void WolfieApp::saveWorkspaceFiles() const {
    if (workspace_.rootPath.empty()) {
        return;
    }

    std::filesystem::create_directories(workspace_.rootPath / "measurement");
    std::ostringstream workspaceJson;
    workspaceJson << "{\n"
                  << "  \"audio\": {\n"
                  << "    \"driver\": \"" << escapeJson(workspace_.audio.driver) << "\",\n"
                  << "    \"micInputChannel\": " << workspace_.audio.micInputChannel << ",\n"
                  << "    \"leftOutputChannel\": " << workspace_.audio.leftOutputChannel << ",\n"
                  << "    \"rightOutputChannel\": " << workspace_.audio.rightOutputChannel << ",\n"
                  << "    \"outputVolumeDb\": " << workspace_.audio.outputVolumeDb << "\n"
                  << "  },\n"
                  << "  \"measurement\": {\n"
                  << "    \"sampleRate\": " << workspace_.measurement.sampleRate << ",\n"
                  << "    \"fadeInSeconds\": " << workspace_.measurement.fadeInSeconds << ",\n"
                  << "    \"fadeOutSeconds\": " << workspace_.measurement.fadeOutSeconds << ",\n"
                  << "    \"durationSeconds\": " << workspace_.measurement.durationSeconds << ",\n"
                  << "    \"startFrequencyHz\": " << workspace_.measurement.startFrequencyHz << ",\n"
                  << "    \"endFrequencyHz\": " << workspace_.measurement.endFrequencyHz << ",\n"
                  << "    \"targetLengthSamples\": " << workspace_.measurement.targetLengthSamples << ",\n"
                  << "    \"leadInSamples\": " << workspace_.measurement.leadInSamples << "\n"
                  << "  },\n"
                  << "  \"ui\": {\n"
                  << "    \"measurementSectionHeight\": " << workspace_.ui.measurementSectionHeight << ",\n"
                  << "    \"resultSectionHeight\": " << workspace_.ui.resultSectionHeight << "\n"
                  << "  }\n"
                  << "}\n";
    writeTextFile(workspace_.rootPath / "workspace.json", workspaceJson.str());

    std::ostringstream uiJson;
    uiJson << "{\n"
           << "  \"measurementSectionHeight\": " << workspace_.ui.measurementSectionHeight << ",\n"
           << "  \"resultSectionHeight\": " << workspace_.ui.resultSectionHeight << "\n"
           << "}\n";
    writeTextFile(workspace_.rootPath / "ui.json", uiJson.str());
}

void WolfieApp::saveMeasurementResultFile() const {
    if (workspace_.rootPath.empty()) {
        return;
    }

    std::filesystem::create_directories(workspace_.rootPath / "measurement");
    std::ostringstream responseCsv;
    responseCsv << "frequency,left,right\n";
    for (size_t i = 0; i < workspace_.result.frequencyAxisHz.size(); ++i) {
        responseCsv << workspace_.result.frequencyAxisHz[i] << ','
                    << workspace_.result.leftChannelDb[i] << ','
                    << workspace_.result.rightChannelDb[i] << '\n';
    }
    writeTextFile(workspace_.rootPath / "measurement" / "response.csv", responseCsv.str());
}

void WolfieApp::startMeasurement() {
    if (workspace_.rootPath.empty()) {
        MessageBoxW(mainWindow_, L"Create or open a workspace first.", L"Wolfie", MB_OK | MB_ICONWARNING);
        return;
    }

    syncStateFromControls();
    if (!measurementEngine_.start(workspace_)) {
        refreshMeasurementStatus();
        if (!measurementEngine_.lastError().empty()) {
            MessageBoxW(mainWindow_, std::wstring(measurementEngine_.lastError()).c_str(), L"Wolfie", MB_OK | MB_ICONERROR);
        }
        return;
    }

    workspace_.result = {};
    invalidateGraphs();
    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::stopMeasurement() {
    if (!measurementEngine_.running()) {
        return;
    }

    measurementEngine_.cancel();
    KillTimer(mainWindow_, kMeasurementTimerId);
    loadMeasurementResultFile();
    invalidateGraphs();
    refreshMeasurementStatus();
}

void WolfieApp::finalizeMeasurement() {
    workspace_.result = measurementEngine_.result();
    saveMeasurementResultFile();
    saveWorkspaceFiles();
    invalidateGraphs();
    refreshMeasurementStatus();
}

RECT WolfieApp::clientRect(HWND window) const {
    RECT rect{};
    GetClientRect(window, &rect);
    return rect;
}

std::wstring WolfieApp::toWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

std::string WolfieApp::toUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length, nullptr, nullptr);
    return output;
}

std::string WolfieApp::formatDouble(double value, int decimals) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(decimals);
    out << value;
    return out.str();
}

std::wstring WolfieApp::formatWideDouble(double value, int decimals) {
    return toWide(formatDouble(value, decimals));
}

}  // namespace wolfie
