// bs_common.h - DLL・パネル・ツールで共有する定義
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include <string>

#define BSL_DRIVER_NAME      "OtonumaASIO"
#define BSL_DRIVER_NAME_W    L"OtonumaASIO"
#define BSL_DRIVER_VERSION   0x00010101   // 1.1.1
#define BSL_VERSION_STR_W    L"1.1.1"
#define BSL_CLSID_STR_W      L"{A62181B6-A0AC-45E6-9532-4EFC6C01C440}"
// {A62181B6-A0AC-45E6-9532-4EFC6C01C440}
static const GUID CLSID_OtonumaASIO =
    { 0xa62181b6, 0xa0ac, 0x45e6, { 0x95, 0x32, 0x4e, 0xfc, 0x6c, 0x01, 0xc4, 0x40 } };

#define BSL_TELEMETRY_NAME   L"Local\\OtonumaASIO.Telemetry"
#define BSL_PIPE_NAME        L"\\\\.\\pipe\\OtonumaASIO.Control"
#define BSL_ACTIVE_MUTEX     L"Global\\OtonumaASIO.Active"
#define BSL_ACTIVE_MUTEX_L   L"Local\\OtonumaASIO.Active"

// ---- 出力方式 ----
enum BsOutType {
    BS_OUT_ASIO = 0,      // ASIO 中継
    BS_OUT_WASAPI = 1,    // WASAPI 排他
    BS_OUT_KS_AUTO = 2,   // KS (パケット/WaveRT 自動選択)
    BS_OUT_KS_PACKET = 3, // KS パケット方式に固定
    BS_OUT_KS_RT = 4,     // KS WaveRT 方式に固定
    BS_OUT_NET = 5,       // BareStream ネットワーク (段階8で統合)
    BS_OUT_NULL = 6,      // 試験用 (音は出さない)
    BS_OUT_COUNT
};

enum BsDriveMode { BS_DRIVE_EVENT = 0, BS_DRIVE_TIMER = 1, BS_DRIVE_TIMER_SPIN = 2 };
enum BsPumpMode  { BS_PUMP_SYNC = 0, BS_PUMP_READAHEAD = 1 };
enum BsPriority  { BS_PRIO_MMCSS = 0, BS_PRIO_CRITICAL = 1, BS_PRIO_TIMECRITICAL = 2 };
enum BsTimerRes  { BS_TIMER_OS = 0, BS_TIMER_1MS = 1, BS_TIMER_05MS = 2 };
enum BsDsdMode   { BS_DSD_AUTO = 0, BS_DSD_NATIVE = 1, BS_DSD_DOP = 2 };

// 変更の反映区分
enum BsApplyClass { BS_APPLY_NOW = 0, BS_APPLY_REOPEN = 1, BS_APPLY_RESET = 2 };

struct BsSettings {
    int    outType        = BS_OUT_KS_AUTO;
    std::wstring devWasapi;               // エンドポイント ID
    std::wstring devKs;                   // デバイスインターフェースのパス
    std::wstring devAsio;                 // ASIO ドライバ名
    int    hostBuffer     = 1024;         // サンプル
    double pacerHz        = 10.0;         // 受け渡し周期
    double prefillSec     = 2.0;          // 先読み量
    int    pumpMode       = BS_PUMP_SYNC;
    int    driveMode      = BS_DRIVE_EVENT;
    int    ksQueue        = 4;
    int    rtLeadMs       = 20;
    int    outPriority    = BS_PRIO_CRITICAL;
    int    cpuCore        = -1;           // -1 = 自動
    int    timerRes       = BS_TIMER_OS;
    int    memLock        = 1;
    int    dsdMode        = BS_DSD_AUTO;
    int    startSilenceMs = 300;
    int    ksFallback     = 1;            // KS が開けないとき WASAPI 排他へ自動切替
    int    spinUs         = 50;           // タイマー＋スピンの最後のスピン時間
    int    asioFirstCh    = 0;            // ASIO 中継で使う先頭出力チャンネル
    int    drainOnStop    = 1;            // 停止時にリングに溜めた分を鳴らし切る
    int    dopMax         = 2;            // DoP で送る DSD の上限 0=DSD64 1=DSD128 2=DSD256 3=DSD512
};

// ---- 共有メモリ (計測) ----
#define BSL_TELEMETRY_MAGIC   0x4C534253u  // 'BSL'
#define BSL_TELEMETRY_VERSION 4
#define BSL_HIST_BINS 13
#define BSL_UNDERRUN_LOG 16

enum BsState { BS_ST_IDLE = 0, BS_ST_INIT = 1, BS_ST_PREPARED = 2, BS_ST_RUNNING = 3, BS_ST_ERROR = 4 };

struct BsTelemetry {
    uint32_t magic;
    uint32_t version;
    volatile LONG seq;           // 書き込み中は奇数 (seqlock)
    uint32_t pid;
    uint64_t updateCounter;
    int32_t  state;
    wchar_t  hostApp[64];

    double   sampleRate;         // ホストから見たレート (DSD 時は DSD レート)
    int32_t  isDsd;
    int32_t  dsdTransport;       // 0=PCM, 1=ネイティブ, 2=DoP
    int32_t  outType;            // 設定された出力方式
    int32_t  outResolved;        // 実際に使っている方式 (KS 自動の解決結果など)
    wchar_t  deviceName[128];
    wchar_t  backendInfo[160];
    int32_t  containerBits;
    int32_t  validBits;
    int32_t  channels;
    double   deviceRate;         // デバイスに渡しているレート (DoP なら DSD/16)

    double   pacerHzReq;
    double   pacerHzEff;
    uint32_t chunkFrames;
    uint32_t hostBuffer;
    int32_t  pumpMode;
    int32_t  driveMode;

    uint64_t ringCapacity;       // フレーム
    uint64_t ringFill;
    uint64_t ringTarget;
    uint64_t framesIn;           // 再生ソフトから受け取った量
    uint64_t framesOut;          // バックエンドへ渡した量
    double   latencyMs;

    uint32_t underruns;
    uint32_t underrunLogCount;
    uint64_t underrunTimes[BSL_UNDERRUN_LOG]; // FILETIME (UTC)

    uint32_t wakeCount;          // 統計に入っているサンプル数
    double   wakeExpectedUs;
    double   wakeMeanUs;
    double   wakeMedianUs;       // 起床間隔と期待値の差の絶対値の中央値
    double   wakeP99Us;
    double   wakeMaxUs;
    uint32_t hist[BSL_HIST_BINS];
    double   outCpuPercent;

    int32_t  bitMatch;           // 0=未判定 1=一致 2=不一致
    uint32_t bitMatchWindows;
    uint32_t bitMatchFails;
    int32_t  notBitPerfect;      // 下位ビットに0以外を検出

    int32_t  vendorPanel;        // ベンダーパネルを呼べる
    wchar_t  lastError[256];
    int32_t  hostFillMode;       // 再生ソフトの書き込み方: 0=後から(遅らせて取り込む) 1=その場 2=outputReady -1=停止中
};

// ヒストグラムのビン上限 (µs)。最後のビンはそれ以上。
static const double kBsHistEdges[BSL_HIST_BINS] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 1e30
};

// DoP で送れる DSD レートの上限 (Hz)
inline double bs_dop_max_rate(int dopMax) {
    return 2822400.0 * (double)(1 << (dopMax < 0 ? 0 : (dopMax > 3 ? 3 : dopMax)));
}

inline const wchar_t* bs_out_type_name(int t) {
    switch (t) {
    case BS_OUT_ASIO: return L"ASIO中継";
    case BS_OUT_WASAPI: return L"WASAPI排他";
    case BS_OUT_KS_AUTO: return L"KS（自動選択）";
    case BS_OUT_KS_PACKET: return L"KS パケット";
    case BS_OUT_KS_RT: return L"KS WaveRT";
    case BS_OUT_NET: return L"BareStreamネットワーク";
    case BS_OUT_NULL: return L"null（試験用）";
    }
    return L"?";
}
