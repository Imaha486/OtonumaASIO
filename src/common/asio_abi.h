// asio_abi.h - Steinberg ASIO SDK に依存しない自前の IASIO / ASIO 型定義
// バイナリ互換 (ABI) だけを合わせている。32bit 版はメンバ関数が thiscall になる
// (MSVC / MinGW とも C++ の仮想関数の既定呼び出し規約が thiscall のため、そのままで一致する)。
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <objbase.h>

typedef long ASIOBool;
typedef long ASIOError;
typedef long ASIOSampleType;
typedef double ASIOSampleRate;

enum { ASIOFalse = 0, ASIOTrue = 1 };

struct ASIOSamples   { unsigned long hi, lo; };
struct ASIOTimeStamp { unsigned long hi, lo; };

enum {
    ASE_OK = 0,
    ASE_SUCCESS = 0x3f4847a0,
    ASE_NotPresent = -1000,
    ASE_HWMalfunction,
    ASE_InvalidParameter,
    ASE_InvalidMode,
    ASE_SPNotAdvancing,
    ASE_NoClock,
    ASE_NoMemory
};

enum {
    ASIOSTInt16MSB = 0, ASIOSTInt24MSB = 1, ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3, ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8, ASIOSTInt32MSB18 = 9, ASIOSTInt32MSB20 = 10, ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16, ASIOSTInt24LSB = 17, ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19, ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24, ASIOSTInt32LSB18 = 25, ASIOSTInt32LSB20 = 26, ASIOSTInt32LSB24 = 27,
    ASIOSTDSDInt8LSB1 = 32, ASIOSTDSDInt8MSB1 = 33, ASIOSTDSDInt8NER8 = 40
};

struct ASIODriverInfo {
    long asioVersion;
    long driverVersion;
    char name[32];
    char errorMessage[124];
    void* sysRef;
};

struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
};

struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
};

struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
};

enum {
    kSystemTimeValid = 1, kSamplePositionValid = 1 << 1, kSampleRateValid = 1 << 2,
    kSpeedValid = 1 << 3, kSampleRateChanged = 1 << 4, kClockSourceChanged = 1 << 5
};

struct AsioTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
};

struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
};

struct ASIOTime {
    long reserved[4];
    AsioTimeInfo timeInfo;
    ASIOTimeCode timeCode;
};

struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

enum {
    kAsioSelectorSupported = 1, kAsioEngineVersion, kAsioResetRequest, kAsioBufferSizeChange,
    kAsioResyncRequest, kAsioLatenciesChanged, kAsioSupportsTimeInfo, kAsioSupportsTimeCode,
    kAsioMMCCommand, kAsioSupportsInputMonitor, kAsioSupportsInputGain, kAsioSupportsInputMeter,
    kAsioSupportsOutputGain, kAsioSupportsOutputMeter, kAsioOverload
};

enum {
    kAsioEnableTimeCodeRead = 1, kAsioDisableTimeCodeRead, kAsioSetInputMonitor, kAsioTransport,
    kAsioSetInputGain, kAsioGetInputMeter, kAsioSetOutputGain, kAsioGetOutputMeter,
    kAsioCanInputMonitor, kAsioCanTimeInfo, kAsioCanTimeCode, kAsioCanTransport,
    kAsioCanInputGain, kAsioCanInputMeter, kAsioCanOutputGain, kAsioCanOutputMeter,
    kAsioOptionalOne,
    kAsioSetIoFormat = 0x23111961,
    kAsioGetIoFormat = 0x23111983,
    kAsioCanDoIoFormat = 0x23112004,
    kAsioCanReportOverload = 0x24042012,
    kAsioGetInternalBufferSamples = 0x25042012
};

typedef long ASIOIoFormatType;
enum { kASIOFormatInvalid = -1, kASIOPCMFormat = 0, kASIODSDFormat = 1 };

struct ASIOIoFormat {
    ASIOIoFormatType FormatType;
    char future[512 - sizeof(ASIOIoFormatType)];
};

// IASIO 本体。ASIO の慣例で CLSID をそのまま IID としても使う。
struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

inline unsigned long long asio_u64(const ASIOSamples& s) {
    return ((unsigned long long)s.hi << 32) | s.lo;
}
inline void asio_set_u64(ASIOSamples& s, unsigned long long v) {
    s.hi = (unsigned long)(v >> 32); s.lo = (unsigned long)(v & 0xffffffffULL);
}
inline void asio_set_u64(ASIOTimeStamp& s, unsigned long long v) {
    s.hi = (unsigned long)(v >> 32); s.lo = (unsigned long)(v & 0xffffffffULL);
}
