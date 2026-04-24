#pragma once

#include <optional>
#include <string>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>

namespace wolfie::audio {

using ASIOBool = long;
using ASIOSampleRate = double;
using ASIOError = long;
using ASIOSampleType = long;

struct ASIOSamples;
struct ASIOTimeStamp;
struct ASIOClockSource;
struct ASIOTime;

struct ASIOBufferInfo {
    ASIOBool isInput = 0;
    long channelNum = 0;
    void* buffers[2]{};
};

struct ASIOCallbacks {
    void (*bufferSwitch)(long index, ASIOBool processNow) = nullptr;
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long index, ASIOBool processNow) = nullptr;
    void (*sampleRateDidChange)(ASIOSampleRate sampleRate) = nullptr;
    long (*asioMessage)(long selector, long value, void* message, double* opt) = nullptr;
};

struct ASIOChannelInfo {
    long channel = 0;
    ASIOBool isInput = 0;
    ASIOBool isActive = 0;
    long channelGroup = 0;
    ASIOSampleType type = 0;
    char name[32]{};
};

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

enum AsioSampleType : ASIOSampleType {
    kAsioSampleInt16Msb = 0,
    kAsioSampleInt24Msb = 1,
    kAsioSampleInt32Msb = 2,
    kAsioSampleFloat32Msb = 3,
    kAsioSampleFloat64Msb = 4,
    kAsioSampleInt32Msb16 = 8,
    kAsioSampleInt32Msb18 = 9,
    kAsioSampleInt32Msb20 = 10,
    kAsioSampleInt32Msb24 = 11,
    kAsioSampleInt16Lsb = 16,
    kAsioSampleInt24Lsb = 17,
    kAsioSampleInt32Lsb = 18,
    kAsioSampleFloat32Lsb = 19,
    kAsioSampleFloat64Lsb = 20,
    kAsioSampleInt32Lsb16 = 24,
    kAsioSampleInt32Lsb18 = 25,
    kAsioSampleInt32Lsb20 = 26,
    kAsioSampleInt32Lsb24 = 27
};

struct DriverHandle {
    IASIO* driver = nullptr;
    bool shouldUninitialize = false;

    DriverHandle() = default;
    DriverHandle(const DriverHandle&) = delete;
    DriverHandle& operator=(const DriverHandle&) = delete;
    DriverHandle(DriverHandle&& other) noexcept;
    DriverHandle& operator=(DriverHandle&& other) noexcept;
    ~DriverHandle();
};

std::wstring formatHResultMessage(HRESULT hr);
std::wstring asioDriverMessage(IASIO* driver);
std::wstring asioChannelName(const ASIOChannelInfo& info, int channelNumber);
std::optional<std::wstring> openDriver(HWND parentWindow, std::wstring_view driverName, DriverHandle& handle);
bool isSupportedAsioSampleType(ASIOSampleType sampleType);
std::wstring asioSampleTypeName(ASIOSampleType sampleType);

}  // namespace wolfie::audio
