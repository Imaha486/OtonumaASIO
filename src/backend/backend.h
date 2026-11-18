// backend.h - 出力バックエンド共通インターフェース
#pragma once
#include "../common/bs_common.h"
#include <string>
#include <vector>
#include <stdint.h>

enum BsKind { BS_KIND_PCM = 0, BS_KIND_DSD_NATIVE = 1, BS_KIND_DOP = 2 };

// デバイスへ渡す形式 (リングの中身もこの形式のインターリーブ)
struct BsFormat {
    double deviceRate = 0;   // デバイスのレート (DoP は DSD/16、ネイティブ DSD は DSD/8 = 1バイト単位)
    int channels = 2;
    int containerBits = 32;  // 16 / 24(詰めた24bit) / 32 / 8(ネイティブ DSD, 1バイト=8サンプル)
    int validBits = 32;
    int kind = BS_KIND_PCM;
    uint32_t frame_bytes() const { return (uint32_t)(channels * containerBits / 8); }
};

struct BsOpenRequest {
    double hostRate = 44100;   // ホストのレート (DSD は DSD レート)
    bool dsd = false;
    int dsdMode = BS_DSD_AUTO;
    int channels = 2;
    const BsSettings* s = nullptr;
};

// エンジンがバックエンドに提供する口
class BsEngineIO {
public:
    virtual ~BsEngineIO() {}
    // 必ず frames 分を dst に書く (足りない分は無音)。実データのフレーム数を返す。
    virtual size_t pull(uint8_t* dst, size_t frames) = 0;
    // 出力スレッドの起床を記録 (計測用)
    virtual void wake_mark() = 0;
    // 出力スレッドの開始・周期ごと・終了時に呼ぶ (優先度・コア固定の反映)
    virtual void out_thread_begin() = 0;
    virtual void out_thread_poll() = 0;
    virtual void out_thread_end() = 0;
    // 現在の WaveRT 書き込み先行量 (ms)
    virtual int rt_lead_ms() = 0;
    // バックエンド側からの要求
    virtual void request_reopen() = 0;
    virtual void request_host_reset() = 0;
    virtual void report_error(const wchar_t* msg) = 0;
    // デバイス側のアンダーラン (WaveRT で書き込みが追い越された等)
    virtual void device_underrun() = 0;
};

struct BsDeviceInfo {
    std::wstring id;
    std::wstring name;
};

class BsBackend {
public:
    virtual ~BsBackend() {}
    virtual int type() const = 0;
    // デバイスを掴む (ASIO 中継ならベンダードライバを読み込む)
    virtual bool attach(const std::wstring& deviceId, void* sysHandle, std::wstring& err) = 0;
    virtual void detach() = 0;
    virtual bool can_rate(double hostRate, bool dsd, int dsdMode) = 0;
    // ストリームを作る。fmt を確定させる。
    virtual bool open(const BsOpenRequest& rq, BsEngineIO* io, BsFormat& fmt, std::wstring& err) = 0;
    virtual bool start(std::wstring& err) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    virtual uint32_t chunk_frames() const = 0;     // 1回に渡す量 (デバイスフレーム)
    virtual uint32_t latency_frames() const = 0;   // バックエンド内に溜まる量の目安
    virtual double effective_hz() const = 0;
    virtual int resolved_type() const { return type(); }
    virtual std::wstring device_name() const = 0;
    virtual std::wstring info() const { return std::wstring(); }
    virtual bool has_vendor_panel() const { return false; }
    virtual void vendor_panel() {}
    // ホストバッファの希望値をバックエンドが持つ場合 (ASIO 中継のネイティブ DSD など)
    virtual bool supports_native_dsd() const { return false; }
};

BsBackend* bs_create_backend(int outType);

// デバイス列挙 (パネルとも共有)
void bs_enum_devices(int outType, std::vector<BsDeviceInfo>& out);
void bs_enum_wasapi(std::vector<BsDeviceInfo>& out);
void bs_enum_ks(std::vector<BsDeviceInfo>& out);
void bs_enum_asio(std::vector<BsDeviceInfo>& out);

BsBackend* bs_new_null();
BsBackend* bs_new_wasapi();
BsBackend* bs_new_ks(int outType);
BsBackend* bs_new_asio();
BsBackend* bs_new_net();

// 指定 Hz に最も近い 1回の量 (フレーム)
inline uint32_t bs_chunk_for_hz(double rate, double hz) {
    double f = rate / (hz > 0 ? hz : 10.0);
    if (f < 16) f = 16;
    return (uint32_t)(f + 0.5);
}
