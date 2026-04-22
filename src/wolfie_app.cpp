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
constexpr int kButtonStopMeasurement = 3012;

constexpr wchar_t kMainClassName[] = L"WolfieMainWindow";
constexpr wchar_t kGraphClassName[] = L"WolfieGraphWindow";
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
constexpr int kWorkspaceSectionHeight = 72;

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
    const size_t segmentFrames = leadInFrames + sweepFrames;
    const size_t pointCount = clampValue<size_t>(sweepFrames / 256, 128, 1024);
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
        const size_t begin = (i * sweepFrames) / pointCount;
        const size_t end = std::max(begin + 1, ((i + 1) * sweepFrames) / pointCount);
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
    if (frequencyHz >= 1000.0) {
        const double khz = frequencyHz / 1000.0;
        out.precision(khz >= 10.0 ? 0 : 1);
        out << khz << L" kHz";
    } else {
        out.precision(frequencyHz >= 100.0 ? 0 : 1);
        out << frequencyHz << L" Hz";
    }
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
    currentFrequencyHz_ = 0.0;
    currentAmplitudeDb_ = -90.0;
    peakAmplitudeDb_ = -90.0;
    lastErrorMessage_.clear();

    const int sampleRate = std::max(8000, workspace.audio.sampleRate);
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
    return true;
}

void MeasurementEngine::cancel() {
    running_ = false;
    finished_ = false;
    progress_ = 0.0;
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
    progress_ = clampValue(static_cast<double>(elapsedMs) / static_cast<double>(durationMs_), 0.0, 1.0);

    const size_t elapsedFrames = std::min(
        runtime_->totalFrames,
        static_cast<size_t>((elapsedMs * static_cast<uint64_t>(runtime_->sampleRate)) / 1000ULL));
    const size_t rightSegmentStart = runtime_->segmentFrames;
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
    if (!playbackDone && progress_ < 1.0) {
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
    currentFrequencyHz_ = snapshot_.measurement.endFrequencyHz;
    result_ = buildMeasurementResultFromCapture(runtime_->capturedSamples,
                                                runtime_->playedSweep,
                                                runtime_->leadInFrames,
                                                runtime_->sampleRate,
                                                snapshot_.measurement);
    cleanupRuntime();
}

WolfieApp::WolfieApp(HINSTANCE instance) : instance_(instance) {
    workspace_.measurement.endFrequencyHz = workspace_.audio.sampleRate / 2.0;
}

int WolfieApp::run() {
    INITCOMMONCONTROLSEX initCommonControls{};
    initCommonControls.dwSize = sizeof(initCommonControls);
    initCommonControls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
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
        app->onCommand(LOWORD(wParam));
        return 0;
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
    case WM_ERASEBKGND:
        return 1;
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
                if (!result.frequencyAxisHz.empty()) {
                    const double startHz = std::max(1.0, app->workspace_.measurement.startFrequencyHz);
                    const double endHz = std::max(startHz + 1.0, app->workspace_.measurement.endFrequencyHz);
                    const double logStart = std::log10(startHz);
                    const double logEnd = std::log10(endHz);
                    const double logRange = std::max(0.001, logEnd - logStart);

                    auto drawSeries = [&](const std::vector<double>& values, COLORREF color) {
                        SetDCPenColor(hdc, color);
                        const double minDb = -12.0;
                        const double maxDb = 12.0;
                        for (size_t i = 0; i < values.size(); ++i) {
                            const double frequency = clampValue(result.frequencyAxisHz[i], startHz, endHz);
                            const double xT = values.size() == 1 ? 0.0 : (std::log10(frequency) - logStart) / logRange;
                            const double yT = clampValue((values[i] - minDb) / (maxDb - minDb), 0.0, 1.0);
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
                RECT label1{graph.left - 8, graph.bottom + 6, graph.left + 60, graph.bottom + 24};
                RECT label2{graph.right - 84, graph.bottom + 6, graph.right + 8, graph.bottom + 24};
                RECT label3{graph.left - 40, graph.top - 8, graph.left + 10, graph.top + 12};
                RECT label4{graph.left - 40, graph.bottom - 8, graph.left + 10, graph.bottom + 12};
                const std::wstring startLabel = formatFrequencyLabel(std::max(1.0, app->workspace_.measurement.startFrequencyHz));
                const std::wstring endLabel = formatFrequencyLabel(std::max(app->workspace_.measurement.startFrequencyHz + 1.0,
                                                                            app->workspace_.measurement.endFrequencyHz));
                DrawTextW(hdc, startLabel.c_str(), -1, &label1, DT_LEFT);
                DrawTextW(hdc, endLabel.c_str(), -1, &label2, DT_RIGHT);
                DrawTextW(hdc, L"+12 dB", -1, &label3, DT_RIGHT);
                DrawTextW(hdc, L"-12 dB", -1, &label4, DT_RIGHT);
            }
        }

        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK WolfieApp::SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
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
        CreateWindowW(L"STATIC", L"Sample rate", WS_CHILD | WS_VISIBLE, 20, 164, 140, 20, window, nullptr, nullptr, nullptr);
        HWND sampleRate = CreateWindowW(L"COMBOBOX", nullptr,
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                        170, 160, 120, 200, window, reinterpret_cast<HMENU>(5), nullptr, nullptr);
        SendMessageW(sampleRate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"44.1 kHz"));
        SendMessageW(sampleRate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"48 kHz"));
        SendMessageW(sampleRate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"96 kHz"));
        switch (app->workspace_.audio.sampleRate) {
        case 48000:
            SendMessageW(sampleRate, CB_SETCURSEL, 1, 0);
            break;
        case 96000:
            SendMessageW(sampleRate, CB_SETCURSEL, 2, 0);
            break;
        case 44100:
        default:
            SendMessageW(sampleRate, CB_SETCURSEL, 0, 0);
            break;
        }
        CreateWindowW(L"BUTTON", L"Open ASIO Control Panel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 208, 180, 28, window,
                      reinterpret_cast<HMENU>(7), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 250, 208, 80, 28, window,
                      reinterpret_cast<HMENU>(8), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 340, 208, 80, 28, window,
                      reinterpret_cast<HMENU>(9), nullptr, nullptr);

        SetPropW(window, L"driver", driver);
        SetPropW(window, L"mic", mic);
        SetPropW(window, L"left", left);
        SetPropW(window, L"right", right);
        SetPropW(window, L"sampleRate", sampleRate);
        return 0;
    }
    case WM_COMMAND: {
        auto* app = reinterpret_cast<WolfieApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (LOWORD(wParam) == 8) {
            const std::wstring driverText = getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"driver")));
            if (driverText != kNoAsioDrivers) {
                app->workspace_.audio.driver = toUtf8(driverText);
            }
            app->workspace_.audio.micInputChannel = std::stoi(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"mic"))));
            app->workspace_.audio.leftOutputChannel = std::stoi(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"left"))));
            app->workspace_.audio.rightOutputChannel = std::stoi(getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"right"))));
            const int sampleRateIndex = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(GetPropW(window, L"sampleRate")), CB_GETCURSEL, 0, 0));
            app->workspace_.audio.sampleRate = sampleRateIndex == 1 ? 48000 : sampleRateIndex == 2 ? 96000 : 44100;
            app->workspace_.measurement.endFrequencyHz = app->workspace_.audio.sampleRate / 2.0;
            app->populateControlsFromState();
            app->saveWorkspaceFiles();
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == 7) {
            const std::wstring driverText = getWindowText(reinterpret_cast<HWND>(GetPropW(window, L"driver")));
            if (const auto error = openAsioControlPanel(window, driverText)) {
                MessageBoxW(window, error->c_str(), L"ASIO Control Panel", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (LOWORD(wParam) == 9) {
            DestroyWindow(window);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
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

    WNDCLASSW settingsClass{};
    settingsClass.lpfnWndProc = SettingsWindowProc;
    settingsClass.hInstance = instance_;
    settingsClass.lpszClassName = kSettingsClassName;
    settingsClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = CreateSolidBrush(kPanelBackground);
    RegisterClassW(&settingsClass);

    mainWindow_ = CreateWindowExW(
        0, kMainClassName, L"Wolfie", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1400, 920, nullptr, nullptr, instance_, this);

    createMenus();
    createLayout();
    populateControlsFromState();
    refreshWorkspaceLabels();
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

void WolfieApp::createSectionChrome(HWND parent, HWND& section, const wchar_t* title) {
    section = CreateWindowW(L"BUTTON", title, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 100, 100, parent, nullptr, instance_, nullptr);
}

void WolfieApp::createLayout() {
    createSectionChrome(mainWindow_, controls_.sectionWorkspace, L" Workspace");
    createSectionChrome(mainWindow_, controls_.sectionMeasurement, L" Measurement");

    controls_.workspacePath = CreateWindowW(L"STATIC", L"No workspace selected", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionWorkspace, nullptr, instance_, nullptr);

    controls_.labelFadeIn = CreateWindowW(L"STATIC", L"Fade-in [s]", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.labelFadeOut = CreateWindowW(L"STATIC", L"Fade-out [s]", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.labelDuration = CreateWindowW(L"STATIC", L"Sweep duration [s]", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.labelStartFrequency = CreateWindowW(L"STATIC", L"Start frequency [Hz]", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.labelEndFrequency = CreateWindowW(L"STATIC", L"End frequency [Hz]", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.labelTargetLength = CreateWindowW(L"STATIC", L"Target length [samples]", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.labelLeadIn = CreateWindowW(L"STATIC", L"Lead-in [samples]", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);

    controls_.editFadeIn = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.sectionMeasurement,
                                           reinterpret_cast<HMENU>(kEditFadeIn), instance_, nullptr);
    controls_.editFadeOut = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.sectionMeasurement,
                                            reinterpret_cast<HMENU>(kEditFadeOut), instance_, nullptr);
    controls_.editDuration = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.sectionMeasurement,
                                             reinterpret_cast<HMENU>(kEditDuration), instance_, nullptr);
    controls_.editStartFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.sectionMeasurement,
                                                   reinterpret_cast<HMENU>(kEditStartFrequency), instance_, nullptr);
    controls_.editEndFrequency = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.sectionMeasurement,
                                                 reinterpret_cast<HMENU>(kEditEndFrequency), instance_, nullptr);
    controls_.editTargetLength = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.sectionMeasurement,
                                                 reinterpret_cast<HMENU>(kEditTargetLength), instance_, nullptr);
    controls_.editLeadIn = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, controls_.sectionMeasurement,
                                           reinterpret_cast<HMENU>(kEditLeadIn), instance_, nullptr);
    controls_.buttonMeasure = CreateWindowW(L"BUTTON", L"Start Measurement", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, mainWindow_,
                                            reinterpret_cast<HMENU>(kButtonMeasure), instance_, nullptr);
    controls_.buttonStopMeasurement = CreateWindowW(L"BUTTON", L"Stop Measurement", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, mainWindow_,
                                                    reinterpret_cast<HMENU>(kButtonStopMeasurement), instance_, nullptr);

    controls_.statusText = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.progressText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.currentFrequency = CreateWindowW(L"STATIC", L"Current frequency: 0 Hz", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.currentAmplitude = CreateWindowW(L"STATIC", L"Current amplitude: -90 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.peakAmplitude = CreateWindowW(L"STATIC", L"Peak amplitude: -90 dB", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.progressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_, nullptr);
    controls_.resultGraph = CreateWindowExW(0, kGraphClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, controls_.sectionMeasurement, nullptr, instance_,
                                            reinterpret_cast<void*>(static_cast<INT_PTR>(GraphKind::Response)));

    SendMessageW(controls_.progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    layoutMainWindow();
}

void WolfieApp::layoutMainWindow() {
    const RECT bounds = clientRect(mainWindow_);
    const int width = std::max(320L, bounds.right - (2 * kContentMargin));
    const int workspaceTop = kContentMargin;
    const int measurementTop = workspaceTop + kWorkspaceSectionHeight + kSectionSpacing;
    const int measurementHeight = std::max(420L, bounds.bottom - measurementTop - kContentMargin);

    MoveWindow(controls_.sectionWorkspace, kContentMargin, workspaceTop, width, kWorkspaceSectionHeight, TRUE);
    MoveWindow(controls_.sectionMeasurement, kContentMargin, measurementTop, width, measurementHeight, TRUE);
    layoutContent();
}

void WolfieApp::layoutContent() {
    RECT workspaceRect = clientRect(controls_.sectionWorkspace);
    const int workspaceInnerWidth = std::max(240L, workspaceRect.right - 32);
    MoveWindow(controls_.workspacePath, 18, 30, workspaceInnerWidth - 36, 20, TRUE);

    RECT measurementRect = clientRect(controls_.sectionMeasurement);
    RECT measurementBounds{};
    GetWindowRect(controls_.sectionMeasurement, &measurementBounds);
    MapWindowPoints(nullptr, mainWindow_, reinterpret_cast<LPPOINT>(&measurementBounds), 2);
    const int innerWidth = std::max(480L, measurementRect.right - 32);
    const int innerHeight = std::max(360L, measurementRect.bottom - 32);
    const int sectionLeft = 24;
    const int sectionTop = 34;
    const bool stackStatus = innerWidth < 980;
    const int formWidth = stackStatus ? innerWidth - 48 : std::max(560, innerWidth - 360);
    const int actionLeft = stackStatus ? sectionLeft : sectionLeft + formWidth + 24;
    const int actionWidth = stackStatus ? innerWidth - 48 : std::max(240, innerWidth - actionLeft - 24);
    const bool singleFormColumn = formWidth < 620;

    auto placeField = [&](HWND label, HWND edit, int left, int top, int width) {
        const int editWidth = std::max(90, std::min(160, width - kLabelWidth - 12));
        MoveWindow(label, left, top + 4, kLabelWidth, 20, TRUE);
        MoveWindow(edit, left + kLabelWidth, top, editWidth, kControlHeight, TRUE);
    };

    int formBottom = sectionTop;
    if (singleFormColumn) {
        const int fieldWidth = formWidth - 24;
        const int rowGap = 32;
        placeField(controls_.labelFadeIn, controls_.editFadeIn, sectionLeft, sectionTop, fieldWidth);
        placeField(controls_.labelFadeOut, controls_.editFadeOut, sectionLeft, sectionTop + rowGap, fieldWidth);
        placeField(controls_.labelDuration, controls_.editDuration, sectionLeft, sectionTop + rowGap * 2, fieldWidth);
        placeField(controls_.labelStartFrequency, controls_.editStartFrequency, sectionLeft, sectionTop + rowGap * 3, fieldWidth);
        placeField(controls_.labelEndFrequency, controls_.editEndFrequency, sectionLeft, sectionTop + rowGap * 4, fieldWidth);
        placeField(controls_.labelTargetLength, controls_.editTargetLength, sectionLeft, sectionTop + rowGap * 5, fieldWidth);
        placeField(controls_.labelLeadIn, controls_.editLeadIn, sectionLeft, sectionTop + rowGap * 6, fieldWidth);
        formBottom = sectionTop + rowGap * 6 + kControlHeight;
    } else {
        const int columnGap = 28;
        const int columnWidth = (formWidth - columnGap) / 2;
        const int leftColumn = sectionLeft;
        const int rightColumn = sectionLeft + columnWidth + columnGap;
        const int rowGap = 34;
        placeField(controls_.labelFadeIn, controls_.editFadeIn, leftColumn, sectionTop, columnWidth);
        placeField(controls_.labelFadeOut, controls_.editFadeOut, leftColumn, sectionTop + rowGap, columnWidth);
        placeField(controls_.labelDuration, controls_.editDuration, leftColumn, sectionTop + rowGap * 2, columnWidth);
        placeField(controls_.labelStartFrequency, controls_.editStartFrequency, rightColumn, sectionTop, columnWidth);
        placeField(controls_.labelEndFrequency, controls_.editEndFrequency, rightColumn, sectionTop + rowGap, columnWidth);
        placeField(controls_.labelTargetLength, controls_.editTargetLength, rightColumn, sectionTop + rowGap * 2, columnWidth);
        placeField(controls_.labelLeadIn, controls_.editLeadIn, rightColumn, sectionTop + rowGap * 3, columnWidth);
        formBottom = sectionTop + rowGap * 3 + kControlHeight;
    }

    int actionTop = stackStatus ? formBottom + 20 : sectionTop;
    const bool stackButtons = actionWidth < 300;
    const int buttonLeft = measurementBounds.left + actionLeft;
    const int buttonTop = measurementBounds.top + actionTop;
    if (stackButtons) {
        MoveWindow(controls_.buttonMeasure, buttonLeft, buttonTop, actionWidth, 30, TRUE);
        MoveWindow(controls_.buttonStopMeasurement, buttonLeft, buttonTop + 38, actionWidth, 30, TRUE);
        actionTop += 76;
    } else {
        const int measureWidth = 150;
        const int stopWidth = std::max(140, actionWidth - measureWidth - 10);
        MoveWindow(controls_.buttonMeasure, buttonLeft, buttonTop, measureWidth, 30, TRUE);
        MoveWindow(controls_.buttonStopMeasurement, buttonLeft + measureWidth + 10, buttonTop, stopWidth, 30, TRUE);
        actionTop += 42;
    }

    MoveWindow(controls_.statusText, actionLeft, actionTop, actionWidth, 20, TRUE);
    MoveWindow(controls_.progressBar, actionLeft, actionTop + 28, actionWidth, 18, TRUE);
    MoveWindow(controls_.progressText, actionLeft, actionTop + 52, actionWidth, 18, TRUE);
    MoveWindow(controls_.currentFrequency, actionLeft, actionTop + 80, actionWidth, 20, TRUE);
    MoveWindow(controls_.currentAmplitude, actionLeft, actionTop + 108, actionWidth, 20, TRUE);
    MoveWindow(controls_.peakAmplitude, actionLeft, actionTop + 136, actionWidth, 20, TRUE);
    const int actionBottom = actionTop + 156;

    const int graphTop = std::max(formBottom, actionBottom) + 20;
    const int graphHeight = std::max(200, innerHeight - graphTop - 12);
    MoveWindow(controls_.resultGraph, sectionLeft, graphTop, innerWidth - 48, graphHeight, TRUE);
}

void WolfieApp::showSettingsWindow() {
    CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClassName, L"Measurement Settings",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, 460, 320, mainWindow_, nullptr, instance_, this);
}

void WolfieApp::populateControlsFromState() {
    setWindowText(controls_.editFadeIn, formatWideDouble(workspace_.measurement.fadeInSeconds));
    setWindowText(controls_.editFadeOut, formatWideDouble(workspace_.measurement.fadeOutSeconds));
    setWindowText(controls_.editDuration, formatWideDouble(workspace_.measurement.durationSeconds, 1));
    setWindowText(controls_.editStartFrequency, formatWideDouble(workspace_.measurement.startFrequencyHz, 1));
    setWindowText(controls_.editEndFrequency, formatWideDouble(workspace_.measurement.endFrequencyHz, 1));
    setWindowText(controls_.editTargetLength, formatWideDouble(workspace_.measurement.targetLengthSamples, 0));
    setWindowText(controls_.editLeadIn, formatWideDouble(workspace_.measurement.leadInSamples, 0));
}

void WolfieApp::syncStateFromControls() {
    workspace_.measurement.fadeInSeconds = std::stod(getWindowText(controls_.editFadeIn));
    workspace_.measurement.fadeOutSeconds = std::stod(getWindowText(controls_.editFadeOut));
    workspace_.measurement.durationSeconds = std::stod(getWindowText(controls_.editDuration));
    workspace_.measurement.startFrequencyHz = std::stod(getWindowText(controls_.editStartFrequency));
    workspace_.measurement.endFrequencyHz = std::stod(getWindowText(controls_.editEndFrequency));
    workspace_.measurement.targetLengthSamples = std::stoi(getWindowText(controls_.editTargetLength));
    workspace_.measurement.leadInSamples = std::stoi(getWindowText(controls_.editLeadIn));
}

void WolfieApp::refreshWorkspaceLabels() {
    const std::wstring pathLabel = workspace_.rootPath.empty()
        ? L"No workspace selected"
        : L"Workspace: " + workspace_.rootPath.wstring();
    setWindowText(controls_.workspacePath, pathLabel);
}

void WolfieApp::refreshMeasurementStatus() {
    const int progress = static_cast<int>(measurementEngine_.progress() * 1000.0);
    SendMessageW(controls_.progressBar, PBM_SETPOS, progress, 0);
    setWindowText(controls_.progressText, std::to_wstring(progress / 10) + L"%");
    setWindowText(controls_.currentFrequency, L"Current frequency: " + formatWideDouble(measurementEngine_.currentFrequencyHz(), 0) + L" Hz");
    setWindowText(controls_.currentAmplitude, L"Current amplitude: " + formatWideDouble(measurementEngine_.currentAmplitudeDb(), 1) + L" dB");
    setWindowText(controls_.peakAmplitude, L"Peak amplitude: " + formatWideDouble(measurementEngine_.peakAmplitudeDb(), 1) + L" dB");
    EnableWindow(controls_.buttonMeasure, !measurementEngine_.running());
    EnableWindow(controls_.buttonStopMeasurement, measurementEngine_.running());

    if (measurementEngine_.running()) {
        setWindowText(controls_.statusText, L"Measurement running");
    } else if (!measurementEngine_.lastError().empty()) {
        setWindowText(controls_.statusText, std::wstring(measurementEngine_.lastError()));
    } else if (!workspace_.result.frequencyAxisHz.empty()) {
        setWindowText(controls_.statusText, L"Measurement complete");
    } else {
        setWindowText(controls_.statusText, L"Ready to measure");
    }

    invalidateGraphs();
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
        startMeasurement();
        return;
    case kButtonStopMeasurement:
        stopMeasurement();
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
    workspace_.measurement.endFrequencyHz = workspace_.audio.sampleRate / 2.0;
    populateControlsFromState();
    refreshWorkspaceLabels();
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
    refreshWorkspaceLabels();
    refreshMeasurementStatus();
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
    refreshWorkspaceLabels();
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
    workspace_.measurement.endFrequencyHz = workspace_.audio.sampleRate / 2.0;

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
            workspace_.audio.sampleRate = static_cast<int>(*value);
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
        if (const auto value = findJsonNumber(*content, "endFrequencyHz")) {
            workspace_.measurement.endFrequencyHz = *value;
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

    if (const auto response = readTextFile(path / "measurement" / "response.csv")) {
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
                  << "    \"sampleRate\": " << workspace_.audio.sampleRate << ",\n"
                  << "    \"outputVolumeDb\": " << workspace_.audio.outputVolumeDb << "\n"
                  << "  },\n"
                  << "  \"measurement\": {\n"
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
    SetTimer(mainWindow_, kMeasurementTimerId, 50, nullptr);
    refreshMeasurementStatus();
}

void WolfieApp::stopMeasurement() {
    if (!measurementEngine_.running()) {
        return;
    }

    measurementEngine_.cancel();
    KillTimer(mainWindow_, kMeasurementTimerId);
    refreshMeasurementStatus();
}

void WolfieApp::finalizeMeasurement() {
    workspace_.result = measurementEngine_.result();
    saveWorkspaceFiles();
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
