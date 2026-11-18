// engine.h - リングバッファ・ポンプ・管理・計測・操作パイプをまとめたエンジン
#pragma once
#include "../common/bs_common.h"
#include "../common/asio_abi.h"
#include "../backend/backend.h"
#include "ring.h"
#include "util.h"
#include <atomic>
#include <functional>
#include <deque>
#include <vector>
#include <string>
#include <memory>

class Engine : public BsEngineIO {
public:
    Engine();
    ~Engine();

    // ---- ホスト(ASIO フロント)向け ----
    bool init(void* sysHandle, std::wstring& err);
    void shutdown();
    bool can_rate(double rate);
    bool set_rate(double rate);
    double rate() const { return rate_; }
    bool set_dsd(bool dsd);
    bool is_dsd() const { return dsd_; }
    void buffer_sizes(long* mn, long* mx, long* pref, long* gran);
    bool prepare(long bufferSize, ASIOBufferInfo* infos, long numCh, ASIOCallbacks* cb, std::wstring& err);
    void dispose();
    bool start(std::wstring& err);
    void stop();
    void sample_position(uint64_t& samples, uint64_t& timeNs);
    void latencies(long* inLat, long* outLat);
    void host_latencies(long* inLat, long* outLat);   // ホストからの問い合わせ (返した値を覚える)
    void open_panel();
    bool prepared() const { return prepared_; }
    bool running() const { return running_; }
    long output_channels() const { return 2; }
    bool output_ready();   // ホストが outputReady() を呼んだ

    // ---- BsEngineIO (バックエンド向け) ----
    size_t pull(uint8_t* dst, size_t frames) override;
    void wake_mark() override;
    void out_thread_begin() override;
    void out_thread_poll() override;
    void out_thread_end() override;
    int rt_lead_ms() override { return rtLeadMs_.load(std::memory_order_relaxed); }
    void request_reopen() override;
    void request_host_reset() override { resetPending_.store(true); }
    void report_error(const wchar_t* msg) override;
    void device_underrun() override;

private:
    // スレッド
    static DWORD WINAPI s_manager(void* p) { return ((Engine*)p)->manager_loop(); }
    static DWORD WINAPI s_pump(void* p) { return ((Engine*)p)->pump_loop(); }
    static DWORD WINAPI s_telemetry(void* p) { return ((Engine*)p)->telemetry_loop(); }
    static DWORD WINAPI s_pipe(void* p) { return ((Engine*)p)->pipe_loop(); }
    DWORD manager_loop();
    DWORD pump_loop();
    DWORD telemetry_loop();
    DWORD pipe_loop();

    // 管理スレッドで同期実行
    void run_sync(const std::function<void()>& fn);
    void run_async(const std::function<void()>& fn);

    // 管理スレッド上の処理
    bool m_attach(std::wstring& err);
    void m_detach();
    bool m_open_backend(std::wstring& err);
    bool m_start_stream(std::wstring& err, bool resetCounters = true);
    void m_stop_stream(bool drain = false);
    void m_reopen_output();
    void m_apply_timer_res(int mode);

    void apply_hot_settings();
    void recompute_target();
    void convert_host_buffer(int idx);
    void call_host(int idx);
    std::string handle_command(const std::string& cmd);
    void set_error(const std::wstring& e);
    void publish_telemetry();

    // 設定
    SRWLOCK settingsLock_ = SRWLOCK_INIT;
    BsSettings settings_;        // 動作中の設定
    BsSettings applied_;         // ストリームを開いた時点の設定
    void* sysHandle_ = nullptr;
    std::wstring hostApp_;

    // バックエンド
    std::unique_ptr<BsBackend> backend_;
    BsFormat fmt_;
    int resolvedType_ = -1;

    // ホスト
    double rate_ = 44100.0;
    bool dsd_ = false;
    bool prepared_ = false;
    bool running_ = false;
    long hostBufSize_ = 0;
    uint32_t hostDevFrames_ = 0;     // ホストバッファ1つ分のデバイスフレーム
    uint32_t hostPerDev_ = 1;        // デバイス1フレームあたりのホストサンプル
    std::vector<uint8_t> hostMem_;
    void* hostBuf_[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};  // [ch][idx]
    ASIOCallbacks cb_ = {};
    bool hostTimeInfo_ = false;
    bool hostSupportsReset_ = false;
    bool hostSupportsResync_ = false;
    bool hostSupportsLat_ = false;
    ASIOTime timeInfo_ = {};

    // リング
    BsRing ring_;
    std::vector<uint8_t> scratch_;
    uint8_t silenceFrame_[64] = {0};
    uint32_t frameBytes_ = 8;
    int dopMarkerByte_ = -1;     // DoP: マーカーを書くバイト位置 (サンプル内)
    uint32_t sampleBytes_ = 4;
    uint8_t dopMarker_ = 0x05;
    uint64_t syncTarget_ = 0, prefillFrames_ = 0;
    std::atomic<uint64_t> target_{0};

    // スレッド・イベント
    HANDLE hManager_ = nullptr, hPump_ = nullptr, hTelemetry_ = nullptr, hPipe_ = nullptr;
    DWORD managerTid_ = 0;
    HANDLE evQuit_ = nullptr;        // 全体終了
    HANDLE evPumpStop_ = nullptr;
    HANDLE evConsumed_ = nullptr;
    HANDLE evTask_ = nullptr;
    HANDLE evTailDone_ = nullptr;
    HANDLE evOutReady_ = nullptr;
    std::atomic<bool> hostUsesOR_{false};
    std::atomic<bool> drainReq_{false};     // 停止時に最後のホストバッファも取り込む
    std::atomic<int> hostFillMode_{0};      // 0=遅らせて取り込む 1=その場で取り込む 2=outputReady
    uint32_t host_buffer_crc(int idx);
    std::atomic<long> hostLatReported_{-1};
    void maybe_notify_latency();   // ホストが outputReady で埋め終わりを知らせてくる
    CRITICAL_SECTION taskLock_;
    struct Task { std::function<void()> fn; HANDLE done; };
    std::deque<Task> tasks_;
    HANDLE activeMutex_ = nullptr;
    bool comInit_ = false;

    // リアルタイム側の状態
    std::atomic<bool> playing_{false};
    std::atomic<bool> flushReq_{false};
    std::atomic<bool> tail_{false};
    std::atomic<int64_t> tailRemain_{0};
    std::atomic<int64_t> silenceRemain_{0};
    std::atomic<uint64_t> framesIn_{0}, framesOut_{0}, pulledTotal_{0}, padTotal_{0};
    std::atomic<uint32_t> underruns_{0};
    std::atomic<bool> resetPending_{false}, resyncPending_{false}, latChangedPending_{false};
    std::atomic<bool> notBitPerfect_{false};
    std::atomic<int> rtLeadMs_{20};
    std::atomic<int> prio_{BS_PRIO_CRITICAL}, core_{-1};
    std::atomic<uint32_t> tuneGen_{0};
    int64_t lastResyncQpc_ = 0;
    SpscQueue<int64_t, 16384> wakeQ_;
    SpscQueue<int64_t, 64> underrunQ_;
    CrcWindow crcIn_, crcOut_;
    std::atomic<HANDLE> outThread_{nullptr};
    std::atomic<bool> outThreadNew_{false};

    // 出力スレッド側のスレッドローカル相当
    BsThreadTuning tuning_;
    uint32_t tuneSeen_ = 0;

    // 計測
    HANDLE hMap_ = nullptr;
    BsTelemetry* tel_ = nullptr;
    std::wstring lastError_;
    SRWLOCK errLock_ = SRWLOCK_INIT;
    int timerResApplied_ = 0;
    int64_t qpcBase_ = 0;
    uint64_t ftBase_ = 0;

    // バックエンド情報のキャッシュ (他スレッドから backend_ を触らないため)
    void m_cache_backend();
    std::atomic<uint32_t> cLat_{0}, cChunk_{0};
    std::atomic<uint64_t> cHzMilli_{0};
    std::atomic<int> cResolved_{-1};
    std::atomic<bool> cVendor_{false};
    std::wstring cDevName_, cInfo_;   // errLock_ で保護
    bool outStarted_ = false;
};
