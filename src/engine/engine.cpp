// engine.cpp - OtonumaASIO のエンジン本体
#include "engine.h"
#include "../common/settings.h"
#include <shellapi.h>
#include <mmsystem.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cwchar>

extern HMODULE g_hModule;

namespace {
typedef LONG (NTAPI *PFN_NtSetTimerResolution)(ULONG, BOOLEAN, PULONG);

uint64_t filetime_now() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

void wcopy(wchar_t* dst, size_t n, const std::wstring& s) {
    size_t k = s.size() < n - 1 ? s.size() : n - 1;
    memcpy(dst, s.data(), k * sizeof(wchar_t)); dst[k] = 0;
}
} // namespace

Engine::Engine() {
    InitializeCriticalSection(&taskLock_);
    evQuit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    evPumpStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    evConsumed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    evTask_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    evTailDone_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    evOutReady_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    qpcBase_ = bs_qpc();
    ftBase_ = filetime_now();

    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* b = wcsrchr(path, L'\\');
    hostApp_ = b ? b + 1 : path;
}

Engine::~Engine() {
    shutdown();
    CloseHandle(evQuit_); CloseHandle(evPumpStop_); CloseHandle(evConsumed_);
    CloseHandle(evTask_); CloseHandle(evTailDone_); CloseHandle(evOutReady_);
    DeleteCriticalSection(&taskLock_);
}

// ======================= 初期化・終了 =======================

bool Engine::init(void* sysHandle, std::wstring& err) {
    if (hManager_) return true;   // 2回目の init は何もしない
    sysHandle_ = sysHandle;

    // 同時に動けるのは 1 つだけ
    activeMutex_ = CreateMutexW(nullptr, FALSE, BSL_ACTIVE_MUTEX);
    if (!activeMutex_) activeMutex_ = CreateMutexW(nullptr, FALSE, BSL_ACTIVE_MUTEX_L);
    if (activeMutex_ && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(activeMutex_); activeMutex_ = nullptr;
        err = L"別の再生ソフトが OtonumaASIO を使用中です";
        return false;
    }

    BsSettings s;
    bs_load_settings(s);
    AcquireSRWLockExclusive(&settingsLock_);
    settings_ = s; applied_ = s;
    ReleaseSRWLockExclusive(&settingsLock_);

    // 共有メモリ (計測)
    hMap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(BsTelemetry), BSL_TELEMETRY_NAME);
    if (hMap_) {
        tel_ = (BsTelemetry*)MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BsTelemetry));
        if (tel_) {
            memset(tel_, 0, sizeof(BsTelemetry));
            tel_->magic = BSL_TELEMETRY_MAGIC; tel_->version = BSL_TELEMETRY_VERSION;
            tel_->pid = GetCurrentProcessId();
        }
    }

    ResetEvent(evQuit_);
    hManager_ = CreateThread(nullptr, 0, s_manager, this, 0, &managerTid_);
    hTelemetry_ = CreateThread(nullptr, 0, s_telemetry, this, 0, nullptr);
    hPipe_ = CreateThread(nullptr, 0, s_pipe, this, 0, nullptr);

    apply_hot_settings();
    bool ok = false;
    run_sync([&] { ok = m_attach(err); });
    if (!ok) set_error(err);
    return ok;
}

void Engine::shutdown() {
    if (!hManager_) return;
    if (running_) stop();
    if (prepared_) dispose();
    run_sync([&] { m_detach(); m_apply_timer_res(0); });
    SetEvent(evQuit_);
    HANDLE hs[3] = { hManager_, hTelemetry_, hPipe_ };
    for (HANDLE h : hs) if (h) { WaitForSingleObject(h, 3000); CloseHandle(h); }
    hManager_ = hTelemetry_ = hPipe_ = nullptr;
    if (hPump_) { WaitForSingleObject(hPump_, 3000); CloseHandle(hPump_); hPump_ = nullptr; }
    if (tel_) { tel_->state = BS_ST_IDLE; UnmapViewOfFile(tel_); tel_ = nullptr; }
    if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
    HANDLE ot = outThread_.exchange(nullptr);
    if (ot) CloseHandle(ot);
    if (activeMutex_) { CloseHandle(activeMutex_); activeMutex_ = nullptr; }
}

// ======================= 管理スレッド =======================

DWORD Engine::manager_loop() {
    comInit_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    for (;;) {
        HANDLE hs[2] = { evQuit_, evTask_ };
        DWORD r = MsgWaitForMultipleObjects(2, hs, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) {
            // 終了前に残ったタスクを片付ける
            EnterCriticalSection(&taskLock_);
            while (!tasks_.empty()) {
                Task t = tasks_.front(); tasks_.pop_front();
                LeaveCriticalSection(&taskLock_);
                t.fn();
                if (t.done) SetEvent(t.done);
                EnterCriticalSection(&taskLock_);
            }
            LeaveCriticalSection(&taskLock_);
            break;
        }
        // ベンダー ASIO ドライバの隠しウインドウ用にメッセージを回す
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        for (;;) {
            EnterCriticalSection(&taskLock_);
            if (tasks_.empty()) { LeaveCriticalSection(&taskLock_); break; }
            Task t = tasks_.front(); tasks_.pop_front();
            LeaveCriticalSection(&taskLock_);
            t.fn();
            if (t.done) SetEvent(t.done);
        }
    }
    if (comInit_) CoUninitialize();
    return 0;
}

void Engine::run_sync(const std::function<void()>& fn) {
    if (!hManager_ || GetCurrentThreadId() == managerTid_) { fn(); return; }
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EnterCriticalSection(&taskLock_);
    tasks_.push_back(Task{ fn, done });
    LeaveCriticalSection(&taskLock_);
    SetEvent(evTask_);
    // 管理スレッドがモーダルダイアログ中でもメッセージを回しながら待つ
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &done, FALSE, INFINITE, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0) break;
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE | PM_QS_SENDMESSAGE)) {}
    }
    CloseHandle(done);
}

void Engine::run_async(const std::function<void()>& fn) {
    if (!hManager_) return;
    EnterCriticalSection(&taskLock_);
    tasks_.push_back(Task{ fn, nullptr });
    LeaveCriticalSection(&taskLock_);
    SetEvent(evTask_);
}

bool Engine::m_attach(std::wstring& err) {
    m_detach();
    BsSettings s;
    AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
    backend_.reset(bs_create_backend(s.outType));
    if (!backend_) { err = L"出力方式を作れません"; return false; }
    if (backend_->attach(bs_settings_device(s), sysHandle_, err)) { m_cache_backend(); return true; }
    {
        bool isKs = s.outType == BS_OUT_KS_AUTO || s.outType == BS_OUT_KS_PACKET || s.outType == BS_OUT_KS_RT;
        if (isKs && s.ksFallback) {
            std::wstring e2;
            backend_.reset(bs_new_wasapi());
            if (backend_->attach(s.devWasapi, sysHandle_, e2)) {
                set_error(L"KS のデバイスを開けないため WASAPI 排他に切り替えました: " + err);
                err.clear();
                m_cache_backend();
                return true;
            }
        }
        backend_.reset();
        m_cache_backend();
        return false;
    }
}

void Engine::m_detach() {
    if (backend_) { backend_->stop(); backend_->close(); backend_->detach(); backend_.reset(); }
    m_cache_backend();
}

void Engine::m_cache_backend() {
    BsBackend* b = backend_.get();
    cLat_ = b ? b->latency_frames() : 0;
    cChunk_ = b ? b->chunk_frames() : 0;
    cHzMilli_ = b ? (uint64_t)(b->effective_hz() * 1000.0 + 0.5) : 0;
    cResolved_ = b ? b->resolved_type() : -1;
    cVendor_ = b ? b->has_vendor_panel() : false;
    AcquireSRWLockExclusive(&errLock_);
    cDevName_ = b ? b->device_name() : std::wstring();
    cInfo_ = b ? b->info() : std::wstring();
    ReleaseSRWLockExclusive(&errLock_);
}

bool Engine::m_open_backend(std::wstring& err) {
    BsSettings s;
    AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
    applied_ = s;
    if (!backend_ && !m_attach(err)) return false;
    BsOpenRequest rq;
    rq.hostRate = rate_; rq.dsd = dsd_; rq.dsdMode = s.dsdMode; rq.channels = 2; rq.s = &applied_;
    if (backend_->open(rq, this, fmt_, err)) { m_cache_backend(); return true; }

    int t = backend_->type();
    bool isKs = t == BS_OUT_KS_AUTO || t == BS_OUT_KS_PACKET || t == BS_OUT_KS_RT;
    if (isKs && s.ksFallback) {
        std::wstring e2;
        std::wstring ksErr = err;
        backend_->detach();
        backend_.reset(bs_new_wasapi());
        if (backend_->attach(s.devWasapi, sysHandle_, e2) && backend_->open(rq, this, fmt_, e2)) {
            set_error(L"KS で開けないため WASAPI 排他に切り替えました: " + ksErr);
            err.clear();
            m_cache_backend();
            return true;
        }
        err = ksErr + L" / WASAPI: " + e2;
        backend_.reset();
    }
    m_cache_backend();
    return false;
}

// ======================= ホスト向け API =======================

bool Engine::can_rate(double rate) {
    bool ok = false;
    run_sync([&] {
        if (!backend_) { std::wstring e; if (!m_attach(e)) return; }
        BsSettings s;
        AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
        if (dsd_) {
            // DoP で送ることになるのに上限を超えるレートは断る (DAC が解釈できず雑音になるため)
            bool native = backend_->supports_native_dsd() && s.dsdMode != BS_DSD_DOP;
            if (!native && rate > bs_dop_max_rate(s.dopMax) + 1.0) { ok = false; return; }
        }
        ok = backend_->can_rate(rate, dsd_, s.dsdMode);
    });
    return ok;
}

bool Engine::set_rate(double rate) {
    if (rate == rate_) return true;
    if (!can_rate(rate)) return false;
    bool wasRunning = running_;
    if (wasRunning) stop();
    rate_ = rate;
    if (wasRunning) { std::wstring e; start(e); }
    return true;
}

bool Engine::set_dsd(bool dsd) {
    if (dsd == dsd_) return true;
    if (running_) return false;
    dsd_ = dsd;
    // DSD 切替時はレートを妥当な既定値へ
    if (dsd_ && rate_ < 2822400.0) rate_ = 2822400.0;
    if (!dsd_ && rate_ >= 2822400.0) rate_ = 44100.0;
    return true;
}

void Engine::buffer_sizes(long* mn, long* mx, long* pref, long* gran) {
    BsSettings s;
    AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
    if (!dsd_) {
        if (mn) *mn = 32;
        if (mx) *mx = 32768;
        if (pref) *pref = s.hostBuffer;
        if (gran) *gran = -1;      // 2 のべき乗
        return;
    }
    // DSD はビット単位 (16 ビットで DoP 1 フレーム)。DSD256 で 32 フレームのような極端に短い
    // バッファになると毎秒何万回も再生ソフトを呼ぶことになり破綻するので、
    // 時間で見て「ホスト側バッファ (ただし最低 1024) ÷ 176.4 kHz」以上 (DSD のレートによらず約 5.8 ms 以上) にする。
    long mult = rate_ >= 2822400.0 ? (long)(rate_ / 2822400.0 + 0.5) : 1;   // DSD64 = 1, DSD256 = 4
    long base = s.hostBuffer < 1024 ? 1024 : s.hostBuffer;
    long p = base * 16 * mult;
    long lo = 1024 * 16 * mult;
    if (p > 32768L * 16 * 8) p = 32768L * 16 * 8;
    if (mn) *mn = lo;
    if (mx) *mx = p > 32768L * 16 * mult ? p : 32768L * 16 * mult;
    if (pref) *pref = p;
    if (gran) *gran = -1;
}

bool Engine::prepare(long bufferSize, ASIOBufferInfo* infos, long numCh, ASIOCallbacks* cb, std::wstring& err) {
    if (prepared_) dispose();
    if (!cb || bufferSize <= 0) { err = L"引数が不正です"; return false; }
    hostUsesOR_ = false;   // createBuffers の後でホストが outputReady() を試すので、ここで初期化
    hostLatReported_ = -1;

    // 種別・デバイスの変更 (リセット要求の結果) をここで反映
    BsSettings s;
    AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
    bool reattach = !backend_ || s.outType != applied_.outType || bs_settings_device(s) != bs_settings_device(applied_);
    if (reattach) {
        bool ok = false;
        run_sync([&] { ok = m_attach(err); });
        if (!ok) return false;
    }
    applied_ = s;

    if (dsd_ && (bufferSize % 16) != 0) { err = L"DSD ではバッファサイズを 16 の倍数にしてください"; return false; }
    size_t perBuf = dsd_ ? (size_t)bufferSize / 8 : (size_t)bufferSize * 4;
    hostMem_.assign(perBuf * 4 + 64, 0);
    uint8_t dsdSil = 0x69;
    for (int c = 0; c < 2; c++) for (int i = 0; i < 2; i++) {
        hostBuf_[c][i] = nullptr;
        if (dsd_) memset(&hostMem_[(size_t)(c * 2 + i) * perBuf], dsdSil, perBuf);
    }
    for (long i = 0; i < numCh; i++) {
        if (infos[i].isInput) { err = L"入力チャンネルはありません"; return false; }
        long ch = infos[i].channelNum;
        if (ch < 0 || ch > 1) { err = L"出力チャンネル番号が範囲外です"; return false; }
        hostBuf_[ch][0] = &hostMem_[(size_t)(ch * 2 + 0) * perBuf];
        hostBuf_[ch][1] = &hostMem_[(size_t)(ch * 2 + 1) * perBuf];
        infos[i].buffers[0] = hostBuf_[ch][0];
        infos[i].buffers[1] = hostBuf_[ch][1];
    }
    hostBufSize_ = bufferSize;
    cb_ = *cb;
    hostTimeInfo_ = cb_.asioMessage && cb_.bufferSwitchTimeInfo &&
                    cb_.asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) &&
                    cb_.asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr);
    hostSupportsReset_ = cb_.asioMessage && cb_.asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr);
    hostSupportsResync_ = cb_.asioMessage && cb_.asioMessage(kAsioSelectorSupported, kAsioResyncRequest, nullptr, nullptr);
    hostSupportsLat_ = cb_.asioMessage && cb_.asioMessage(kAsioSelectorSupported, kAsioLatenciesChanged, nullptr, nullptr);
    prepared_ = true;
    return true;
}

void Engine::dispose() {
    if (running_) stop();
    bool pumpGone = true;
    if (hPump_) {
        pumpGone = WaitForSingleObject(hPump_, 5000) == WAIT_OBJECT_0;
        if (pumpGone) { CloseHandle(hPump_); hPump_ = nullptr; }
    }
    prepared_ = false;
    if (!pumpGone) { set_error(L"再生ソフトのコールバックが戻らないため、バッファの解放を見送りました"); return; }
    memset(hostBuf_, 0, sizeof hostBuf_);
    hostMem_.clear(); hostMem_.shrink_to_fit();
    ring_.free_mem();
    cb_ = ASIOCallbacks{};
}

bool Engine::start(std::wstring& err) {
    if (!prepared_) { err = L"バッファが作られていません"; return false; }
    if (running_) return true;
    bool ok = false;
    run_sync([&] { ok = m_start_stream(err); });
    if (!ok) set_error(err);
    return ok;
}

void Engine::stop() {
    if (!running_) return;
    playing_.store(false);
    {
        BsSettings s;
        AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
        drainReq_.store(s.drainOnStop != 0);
    }
    SetEvent(evPumpStop_);       // 待っている開始処理も止める
    run_sync([&] { m_stop_stream(true); });
}

bool Engine::m_start_stream(std::wstring& err, bool resetCounters) {
    if (hPump_) { WaitForSingleObject(hPump_, 5000); CloseHandle(hPump_); hPump_ = nullptr; }
    if (!m_open_backend(err)) return false;

    hostPerDev_ = fmt_.kind == BS_KIND_DOP ? 16 : (fmt_.kind == BS_KIND_DSD_NATIVE ? 8 : 1);
    hostDevFrames_ = (uint32_t)(hostBufSize_ / (long)hostPerDev_);
    frameBytes_ = fmt_.frame_bytes();
    sampleBytes_ = (uint32_t)(fmt_.containerBits / 8);
    if (frameBytes_ == 0 || frameBytes_ > sizeof silenceFrame_ || hostDevFrames_ == 0) {
        err = L"形式が不正です"; backend_->close(); return false;
    }
    dopMarkerByte_ = fmt_.kind == BS_KIND_DOP ? (int)sampleBytes_ - 1 : -1;
    memset(silenceFrame_, 0, sizeof silenceFrame_);
    if (fmt_.kind == BS_KIND_DSD_NATIVE) memset(silenceFrame_, 0x69, frameBytes_);
    if (fmt_.kind == BS_KIND_DOP) {
        for (int c = 0; c < fmt_.channels; c++) {
            uint8_t* p = silenceFrame_ + c * sampleBytes_;
            p[sampleBytes_ - 2] = 0x69; p[sampleBytes_ - 3] = 0x69;  // 下位 16bit (マーカーは pull で付ける)
        }
    }

    uint32_t chunk = backend_->chunk_frames();
    syncTarget_ = std::max<uint64_t>((uint64_t)hostDevFrames_ * 2, (uint64_t)chunk * 2);
    prefillFrames_ = (uint64_t)(applied_.prefillSec * fmt_.deviceRate);
    uint64_t cap = std::max(syncTarget_, prefillFrames_) + hostDevFrames_ + (uint64_t)chunk * 2 + 64;
    bool locked = false;
    if (!ring_.alloc(cap, frameBytes_, applied_.memLock != 0, &locked)) {
        err = L"リングバッファを確保できません"; backend_->close(); return false;
    }
    if (applied_.memLock && !locked) set_error(L"メモリ固定 (VirtualLock) に失敗しました。そのまま続行します");
    scratch_.assign((size_t)hostDevFrames_ * frameBytes_, 0);
    recompute_target();

    // 計測の初期化
    if (resetCounters) { framesIn_ = 0; framesOut_ = 0; pulledTotal_ = 0; padTotal_ = 0; underruns_ = 0; }
    notBitPerfect_ = false; flushReq_ = false; tail_ = false;
    crcIn_.reset((uint64_t)fmt_.deviceRate, frameBytes_);
    crcOut_.reset((uint64_t)fmt_.deviceRate, frameBytes_);
    silenceRemain_ = (int64_t)(applied_.startSilenceMs * fmt_.deviceRate / 1000.0);
    dopMarker_ = 0x05;
    m_apply_timer_res(applied_.timerRes);

    ResetEvent(evPumpStop_);
    running_ = true;
    hPump_ = CreateThread(nullptr, 0, s_pump, this, 0, nullptr);

    // 先読みが溜まってから出力を開始する (ホストを待たせないよう非同期)
    run_async([this] {
        uint64_t need = std::min<uint64_t>(target_.load(), std::max<uint64_t>(syncTarget_, (uint64_t)fmt_.deviceRate));
        int64_t t0 = bs_qpc();
        while (running_ && WaitForSingleObject(evPumpStop_, 0) != WAIT_OBJECT_0) {
            if (ring_.fill() >= need) break;
            if (bs_qpc_to_us(bs_qpc() - t0) > 5e6) break;
            MsgWaitForMultipleObjects(1, &evPumpStop_, FALSE, 2, QS_ALLINPUT);
            MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (!running_ || WaitForSingleObject(evPumpStop_, 0) == WAIT_OBJECT_0) return;
        playing_ = true;
        std::wstring e;
        if (!backend_ || !backend_->start(e)) { set_error(L"出力を開始できません: " + e); playing_ = false; }
        else outStarted_ = true;
        m_cache_backend();
        // 開始時には遅延変更を通知しない (再生ソフトが開き直して無限ループになるため)
    });
    return true;
}

void Engine::m_stop_stream(bool drain) {
    if (!running_) return;
    SetEvent(evPumpStop_);
    if (hPump_) {
        // ホストのコールバック内で止まっている可能性があるので待ちすぎない
        if (WaitForSingleObject(hPump_, 2000) == WAIT_OBJECT_0) { CloseHandle(hPump_); hPump_ = nullptr; }
    }
    BsSettings s;
    AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
    if (drain && s.drainOnStop && backend_ && outStarted_ && fmt_.deviceRate > 0) {
        // 停止時にリングに溜めた分を鳴らし切る (曲の終わりやレート切替の直前が欠けないように)
        double maxSec = (double)ring_.fill() / fmt_.deviceRate + 0.5;
        int64_t t0 = bs_qpc();
        while (ring_.fill() > 0 && bs_qpc_to_us(bs_qpc() - t0) < maxSec * 1e6) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
            MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        // 出口 (ドライバ) の中に残っている分も鳴り終わるまで待つ
        DWORD tailMs = (DWORD)(cLat_.load() * 1000.0 / fmt_.deviceRate) + 5;
        if (tailMs > 1000) tailMs = 1000;
        int64_t t1 = bs_qpc();
        while (bs_qpc_to_us(bs_qpc() - t1) < tailMs * 1000.0) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
            MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
    }
    if (backend_) {
        if (fmt_.kind != BS_KIND_PCM && outStarted_) {
            // DSD は無音パターンを数十 ms 流してから止める (ポップ防止)
            ResetEvent(evTailDone_);
            tailRemain_ = (int64_t)(fmt_.deviceRate * 0.06);
            flushReq_ = true;
            tail_ = true;
            WaitForSingleObject(evTailDone_, 300);
        }
        backend_->stop();
        backend_->close();
    }
    outStarted_ = false;
    tail_ = false;
    playing_ = false;
    running_ = false;
    m_cache_backend();
}

void Engine::m_reopen_output() {
    if (!running_ || !backend_) return;
    BsFormat old = fmt_;
    backend_->stop();
    backend_->close();
    std::wstring err;
    BsFormat nf;
    BsSettings s;
    AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
    applied_ = s;
    BsOpenRequest rq; rq.hostRate = rate_; rq.dsd = dsd_; rq.dsdMode = s.dsdMode; rq.s = &applied_;
    if (!backend_->open(rq, this, nf, err)) { set_error(L"出力を開き直せません: " + err); outStarted_ = false; resetPending_ = true; return; }
    if (nf.deviceRate != old.deviceRate || nf.frame_bytes() != old.frame_bytes() || nf.kind != old.kind) {
        backend_->close();
        set_error(L"開き直したら形式が変わったため再生ソフトに再初期化を要求します");
        resetPending_ = true; return;
    }
    fmt_ = nf;
    m_cache_backend();
    recompute_target();
    if (outStarted_) {
        if (!backend_->start(err)) { set_error(L"出力を再開できません: " + err); outStarted_ = false; }
    }
    maybe_notify_latency();
}

void Engine::request_reopen() {
    run_async([this] { m_reopen_output(); });
}

void Engine::m_apply_timer_res(int mode) {
    if (mode == timerResApplied_) return;
    static PFN_NtSetTimerResolution pSet = nullptr;
    if (!pSet) pSet = (PFN_NtSetTimerResolution)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution");
    ULONG cur = 0;
    // 戻す
    if (timerResApplied_ == BS_TIMER_1MS) timeEndPeriod(1);
    if (timerResApplied_ == BS_TIMER_05MS && pSet) pSet(5000, FALSE, &cur);
    // 設定
    if (mode == BS_TIMER_1MS) timeBeginPeriod(1);
    if (mode == BS_TIMER_05MS && pSet) pSet(5000, TRUE, &cur);
    timerResApplied_ = mode;
}

void Engine::sample_position(uint64_t& samples, uint64_t& timeNs) {
    uint64_t pulled = pulledTotal_.load(std::memory_order_acquire);
    uint64_t pad = padTotal_.load(std::memory_order_acquire);
    uint64_t real = framesOut_.load(std::memory_order_acquire);
    uint64_t lat = cLat_.load(std::memory_order_relaxed);
    uint64_t dev = pulled > lat ? pulled - lat : 0;
    uint64_t played = dev > pad ? dev - pad : 0;
    if (played > real) played = real;
    samples = played * hostPerDev_;
    timeNs = (uint64_t)((double)bs_qpc() * 1e9 / (double)bs_qpf());
}

void Engine::host_latencies(long* inLat, long* outLat) {
    long o = 0;
    latencies(inLat, &o);
    if (outLat) *outLat = o;
    hostLatReported_.store(o);
}

// 再生中に設定変更で遅延が実際に変わったときだけ、一度だけ通知する
void Engine::maybe_notify_latency() {
    if (!running_ || !outStarted_) return;
    long rep = hostLatReported_.load();
    if (rep < 0) return;
    long now = 0; latencies(nullptr, &now);
    long diff = now > rep ? now - rep : rep - now;
    if (diff > hostBufSize_) { hostLatReported_.store(now); latChangedPending_ = true; }
}

void Engine::latencies(long* inLat, long* outLat) {
    if (inLat) *inLat = 0;
    if (outLat) {
        uint64_t devLat = target_.load() + cLat_.load();
        *outLat = (long)(hostBufSize_ + devLat * hostPerDev_);
    }
}

void Engine::open_panel() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    wchar_t* b = wcsrchr(path, L'\\');
    if (b) *(b + 1) = 0;
    std::wstring exe = std::wstring(path) + L"OtonumaPanel.exe";
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, path, SW_SHOWNORMAL);
}

// ======================= 設定の即時反映 =======================

void Engine::recompute_target() {
    BsSettings s;
    AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
    if (!fmt_.deviceRate || !ring_.capacity()) return;
    uint32_t chunk = cChunk_.load();
    syncTarget_ = std::max<uint64_t>((uint64_t)hostDevFrames_ * 2, (uint64_t)chunk * 2);
    prefillFrames_ = (uint64_t)(s.prefillSec * fmt_.deviceRate);
    uint64_t t = s.pumpMode == BS_PUMP_READAHEAD ? std::max<uint64_t>(prefillFrames_, syncTarget_) : syncTarget_;
    uint64_t lim = ring_.capacity() > hostDevFrames_ ? ring_.capacity() - hostDevFrames_ : ring_.capacity();
    if (t > lim) {
        t = lim;
        // 先読み量が容量を超えたらストリームを作り直す (一瞬途切れる)
        if (s.pumpMode == BS_PUMP_READAHEAD && prefillFrames_ > lim && running_) {
            run_async([this] {
                if (!running_) return;
                std::wstring e;
                m_stop_stream();
                if (!m_start_stream(e, false)) set_error(L"先読み量の変更に失敗: " + e);
            });
        }
    }
    target_.store(t);
}

void Engine::apply_hot_settings() {
    BsSettings s;
    AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
    rtLeadMs_ = s.rtLeadMs;
    prio_ = s.outPriority;
    core_ = s.cpuCore;
    tuneGen_.fetch_add(1);
    recompute_target();
    int tr = s.timerRes;
    if (running_) run_async([this, tr] { m_apply_timer_res(tr); });
    maybe_notify_latency();
}

// ======================= ポンプスレッド =======================

void Engine::call_host(int idx) {
    if (hostTimeInfo_) {
        uint64_t pos, ns;
        sample_position(pos, ns);
        memset(&timeInfo_, 0, sizeof timeInfo_);
        timeInfo_.timeInfo.speed = 1.0;
        asio_set_u64(timeInfo_.timeInfo.samplePosition, pos);
        asio_set_u64(timeInfo_.timeInfo.systemTime, ns);
        timeInfo_.timeInfo.sampleRate = rate_;
        timeInfo_.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        cb_.bufferSwitchTimeInfo(&timeInfo_, idx, ASIOTrue);
    } else if (cb_.bufferSwitch) {
        cb_.bufferSwitch(idx, ASIOTrue);
    }
}

void Engine::convert_host_buffer(int idx) {
    const uint32_t frames = hostDevFrames_;
    const int nch = fmt_.channels;
    uint8_t* dst = scratch_.data();
    bool nbp = false;
    for (int c = 0; c < nch; c++) {
        const uint8_t* src = c < 2 ? (const uint8_t*)hostBuf_[c][idx] : nullptr;
        uint8_t* d = dst + c * sampleBytes_;
        if (!src) {
            for (uint32_t f = 0; f < frames; f++, d += frameBytes_) memcpy(d, silenceFrame_ + c * sampleBytes_, sampleBytes_);
            continue;
        }
        switch (fmt_.kind) {
        case BS_KIND_PCM: {
            const int32_t* s = (const int32_t*)src;
            if (fmt_.containerBits == 32) {
                uint32_t lowMask = fmt_.validBits >= 32 ? 0 : ((1u << (32 - fmt_.validBits)) - 1);
                for (uint32_t f = 0; f < frames; f++, d += frameBytes_) {
                    int32_t v = s[f];
                    if ((uint32_t)v & lowMask) { nbp = true; v &= ~(int32_t)lowMask; }
                    memcpy(d, &v, 4);
                }
            } else if (fmt_.containerBits == 24) {
                for (uint32_t f = 0; f < frames; f++, d += frameBytes_) {
                    uint32_t v = (uint32_t)s[f];
                    if (v & 0xFF) nbp = true;
                    d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)(v >> 16); d[2] = (uint8_t)(v >> 24);
                }
            } else { // 16
                for (uint32_t f = 0; f < frames; f++, d += frameBytes_) {
                    uint32_t v = (uint32_t)s[f];
                    if (v & 0xFFFF) nbp = true;
                    d[0] = (uint8_t)(v >> 16); d[1] = (uint8_t)(v >> 24);
                }
            }
            break;
        }
        case BS_KIND_DOP: {
            // 1 フレーム = DSD 16 サンプル (2 バイト)。先に来るバイトを上位側 (bit15..8) に入れる。
            for (uint32_t f = 0; f < frames; f++, d += frameBytes_) {
                uint8_t b0 = src[2 * f], b1 = src[2 * f + 1];
                if (sampleBytes_ == 4) { d[0] = 0; d[1] = b1; d[2] = b0; d[3] = 0; }
                else { d[0] = b1; d[1] = b0; d[2] = 0; }
            }
            break;
        }
        case BS_KIND_DSD_NATIVE: {
            for (uint32_t f = 0; f < frames; f++, d += frameBytes_) *d = src[f];
            break;
        }
        }
    }
    if (nbp) notBitPerfect_.store(true, std::memory_order_relaxed);
    crcIn_.feed(dst, frames);
    size_t done = 0;
    while (done < frames) {
        size_t w = ring_.write(dst + done * frameBytes_, frames - done);
        done += w;
        if (done < frames) {   // 起きないはずだが念のため待つ
            if (WaitForSingleObject(evConsumed_, 5) == WAIT_OBJECT_0) {}
            if (WaitForSingleObject(evPumpStop_, 0) == WAIT_OBJECT_0) break;
        }
    }
    framesIn_.fetch_add(done, std::memory_order_relaxed);
}

bool Engine::output_ready() {
    if (!prepared_) return true;
    hostUsesOR_.store(true);
    SetEvent(evOutReady_);
    return true;
}

// ASIO の決まりでは、ホストは bufferSwitch(idx) の後「次の bufferSwitch まで」に idx を埋めればよい。
// コールバックから戻った直後にコピーすると、別スレッドで後から埋めるホストでは古い中身を拾ってしまう。
//   - outputReady() で知らせてくるホスト: 知らせが来たらすぐ取り込む
//   - それ以外: 最初は「1 周期あけて、次の bufferSwitch の直前に取り込む」(規約どおり)。
//     戻った直後と取り込み時の中身が一致し続ける (= その場で埋めるホスト) と分かったら、
//     戻った直後に取り込み、先読みモードで実時間より速く溜められるようにする。
//     その後に中身の食い違いを見つけたら、また遅らせる方式に戻す。
uint32_t Engine::host_buffer_crc(int idx) {
    size_t per = dsd_ ? (size_t)hostBufSize_ / 8 : (size_t)hostBufSize_ * 4;
    uint32_t c = 0;
    for (int ch = 0; ch < 2; ch++) if (hostBuf_[ch][idx]) c = bs_crc32_update(c, hostBuf_[ch][idx], per);
    return c;
}

DWORD Engine::pump_loop() {
    DWORD idxAvrt = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idxAvrt);
    int idx = 0;
    int pending = -1;            // まだ取り込んでいないホストバッファ
    uint32_t pendingCrc = 0;
    int checkIdx = -1;           // その場取り込みモードで、あとで食い違いを確かめるバッファ
    uint32_t checkCrc = 0, lastAfterCrc = 0;
    bool immediate = false;      // その場で埋めるホストと判定済み
    bool lazySeen = false;
    int streak = 0;
    int64_t lastCall = 0;
    const double periodUs = fmt_.deviceRate > 0 ? hostDevFrames_ * 1e6 / fmt_.deviceRate : 10000.0;
    // 待つのは 3/4 周期 (ちょうど 1 周期だと供給が実時間ぎりぎりになり、揺らぎで不足する)
    const int64_t gap = (int64_t)(periodUs * 0.75 * (double)bs_qpf() / 1e6);
    const int64_t ms2 = bs_qpf() / 500;
    const DWORD orTimeoutMs = (DWORD)(periodUs * 2 / 1000.0) + 20;
    bool stop = false;
    while (!stop && WaitForSingleObject(evPumpStop_, 0) != WAIT_OBJECT_0) {
        if (resetPending_.exchange(false) && cb_.asioMessage && hostSupportsReset_)
            cb_.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        if (resyncPending_.exchange(false) && cb_.asioMessage && hostSupportsResync_)
            cb_.asioMessage(kAsioResyncRequest, 0, nullptr, nullptr);
        if (latChangedPending_.exchange(false) && cb_.asioMessage && hostSupportsLat_)
            cb_.asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr);

        uint64_t fill = ring_.fill();
        // 遅らせて取り込む間は、手元に 1 バッファ分多く持っておく
        uint64_t tgt = target_.load(std::memory_order_relaxed) + (immediate || hostUsesOR_.load() ? 0 : hostDevFrames_);
        if (fill < tgt && fill + hostDevFrames_ * 2 <= ring_.capacity()) {
            if (checkIdx >= 0) {
                if (host_buffer_crc(checkIdx) != checkCrc) { immediate = false; streak = 0; lazySeen = true; }
                checkIdx = -1;
            }
            if (pending >= 0) {
                // 規約どおり、前回の呼び出しから 1 周期は待ってから取り込む
                for (;;) {
                    int64_t rem = gap - (bs_qpc() - lastCall);
                    if (rem <= 0) break;
                    if (rem > ms2) {   // 2ms 以上あれば眠る、残りは細かく待つ
                        if (WaitForSingleObject(evPumpStop_, (DWORD)((rem - ms2) * 1000 / bs_qpf())) == WAIT_OBJECT_0) { stop = true; break; }
                    } else {
                        if (WaitForSingleObject(evPumpStop_, 0) == WAIT_OBJECT_0) { stop = true; break; }
                        SwitchToThread();
                    }
                }
                if (stop) break;
                uint32_t now = host_buffer_crc(pending);
                if (now == pendingCrc) {
                    if (pendingCrc != lastAfterCrc && ++streak >= 256 && !lazySeen) immediate = true;
                } else { streak = 0; lazySeen = true; }   // 一度でも後から埋めたら、この再生中は遅らせる方式に固定
                lastAfterCrc = pendingCrc;
                convert_host_buffer(pending);
                pending = -1;
            }
            ResetEvent(evOutReady_);
            call_host(idx);
            lastCall = bs_qpc();
            if (WaitForSingleObject(evPumpStop_, 0) == WAIT_OBJECT_0) break;
            if (hostUsesOR_.load()) {
                HANDLE hs[2] = { evPumpStop_, evOutReady_ };
                DWORD w = WaitForMultipleObjects(2, hs, FALSE, orTimeoutMs);
                if (w == WAIT_OBJECT_0) break;
                if (w == WAIT_OBJECT_0 + 1) convert_host_buffer(idx);
                else { hostUsesOR_ = false; pending = idx; pendingCrc = host_buffer_crc(idx); }
            } else if (immediate) {
                checkCrc = host_buffer_crc(idx);
                checkIdx = idx;
                convert_host_buffer(idx);
            } else {
                pending = idx;
                pendingCrc = host_buffer_crc(idx);
            }
            hostFillMode_ = hostUsesOR_.load() ? 2 : (immediate ? 1 : 0);
            idx ^= 1;
            continue;
        }
        HANDLE hs[2] = { evPumpStop_, evConsumed_ };
        WaitForMultipleObjects(2, hs, FALSE, 20);
    }
    // 鳴らし切るときは、最後に渡したバッファも取り込んでおく (止める時点では書き終わっている)
    if (pending >= 0 && drainReq_.exchange(false) && ring_.space() >= hostDevFrames_) {
        // 後から書き込む再生ソフトのために、普段と同じだけ待ってから取り込む
        while (bs_qpc() - lastCall < gap) SwitchToThread();
        convert_host_buffer(pending);
    }
    if (av) AvRevertMmThreadCharacteristics(av);
    return 0;
}

// ======================= 出力側 (リアルタイム) =======================

size_t Engine::pull(uint8_t* dst, size_t frames) {
    const uint32_t fb = frameBytes_;
    size_t pos = 0, real = 0;
    if (flushReq_.exchange(false, std::memory_order_acq_rel)) ring_.drop_all();
    bool tail = tail_.load(std::memory_order_acquire);

    int64_t sil = silenceRemain_.load(std::memory_order_relaxed);
    if (sil > 0) {
        size_t n = (size_t)std::min<int64_t>(sil, (int64_t)frames);
        for (size_t i = 0; i < n; i++) memcpy(dst + i * fb, silenceFrame_, fb);
        pos = n;
        silenceRemain_.store(sil - (int64_t)n, std::memory_order_relaxed);
    }
    if (!tail && pos < frames) {
        real = ring_.read(dst + pos * fb, frames - pos);
        if (real) crcOut_.feed(dst + pos * fb, real);
        pos += real;
    }
    if (pos < frames) {
        for (size_t i = pos; i < frames; i++) memcpy(dst + i * fb, silenceFrame_, fb);
        if (!tail && playing_.load(std::memory_order_relaxed)) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            int64_t now = bs_qpc();
            underrunQ_.push(now);
            // 再同期要求は送らない (再生ソフトによっては開き直してしまい、音が途切れ続ける)
        }
    }
    if (tail) {
        int64_t r = tailRemain_.fetch_sub((int64_t)frames) - (int64_t)frames;
        if (r <= 0) SetEvent(evTailDone_);
    }
    // DoP マーカー (0x05 / 0xFA を交互) をデータと無音の区別なく連続で付ける
    if (dopMarkerByte_ >= 0) {
        uint8_t m = dopMarker_;
        const int nch = fmt_.channels;
        for (size_t i = 0; i < frames; i++) {
            uint8_t* f = dst + i * fb;
            for (int c = 0; c < nch; c++) f[c * sampleBytes_ + dopMarkerByte_] = m;
            m = (m == 0x05) ? 0xFA : 0x05;
        }
        dopMarker_ = m;
    }
    framesOut_.fetch_add(real, std::memory_order_release);
    padTotal_.fetch_add(frames - real, std::memory_order_release);
    pulledTotal_.fetch_add(frames, std::memory_order_release);
    SetEvent(evConsumed_);
    return real;
}

void Engine::wake_mark() { wakeQ_.push(bs_qpc()); }

void Engine::out_thread_begin() {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    HANDLE old = outThread_.exchange(h);
    if (old) CloseHandle(old);
    wakeQ_.clear_from_consumer();
    tuning_.apply(prio_.load(), core_.load());
    tuneSeen_ = tuneGen_.load();
}

void Engine::out_thread_poll() {
    uint32_t g = tuneGen_.load(std::memory_order_relaxed);
    if (g != tuneSeen_) { tuneSeen_ = g; tuning_.apply(prio_.load(), core_.load()); }
}

void Engine::out_thread_end() { tuning_.revert(); }

void Engine::report_error(const wchar_t* msg) { set_error(msg ? msg : L""); }

void Engine::device_underrun() {
    if (!playing_.load(std::memory_order_relaxed)) return;
    underruns_.fetch_add(1, std::memory_order_relaxed);
    underrunQ_.push(bs_qpc());
}

void Engine::set_error(const std::wstring& e) {
    AcquireSRWLockExclusive(&errLock_);
    lastError_ = e;
    ReleaseSRWLockExclusive(&errLock_);
}

// ======================= 計測スレッド =======================

DWORD Engine::telemetry_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<double> win;          // 起床間隔のずれ (µs)
    std::vector<double> intervals;
    size_t winPos = 0;
    const size_t kWin = 4096;
    win.reserve(kWin); intervals.reserve(kWin);
    int64_t lastWake = 0;
    HANDLE myThread = nullptr;
    uint64_t lastCpu = 0; int64_t lastCpuQpc = bs_qpc();
    uint64_t underrunLog[BSL_UNDERRUN_LOG] = {0};
    uint32_t underrunLogCount = 0;
    std::vector<CrcWindowItem> pendIn, pendOut;
    uint32_t bmWindows = 0, bmFails = 0;
    double cpuPct = 0;

    while (WaitForSingleObject(evQuit_, 100) == WAIT_TIMEOUT) {
        // 出力スレッドのハンドルを引き取る
        HANDLE nh = outThread_.exchange(nullptr);
        if (nh) { if (myThread) CloseHandle(myThread); myThread = nh; lastCpu = 0; lastWake = 0; win.clear(); intervals.clear(); winPos = 0; }

        double expectedUs = 0;
        if (fmt_.deviceRate > 0 && running_) expectedUs = cChunk_.load() * 1e6 / fmt_.deviceRate;

        int64_t t;
        while (wakeQ_.pop(t)) {
            if (lastWake) {
                double iv = bs_qpc_to_us(t - lastWake);
                double dev = std::fabs(iv - expectedUs);
                if (win.size() < kWin) { win.push_back(dev); intervals.push_back(iv); }
                else { win[winPos] = dev; intervals[winPos] = iv; winPos = (winPos + 1) % kWin; }
            }
            lastWake = t;
        }
        while (underrunQ_.pop(t)) {
            uint64_t ft = ftBase_ + (uint64_t)((double)(t - qpcBase_) * 1e7 / (double)bs_qpf());
            underrunLog[underrunLogCount % BSL_UNDERRUN_LOG] = ft;
            underrunLogCount++;
        }
        // ビット一致 (1 秒窓の CRC を突き合わせる)
        CrcWindowItem it;
        while (crcIn_.q.pop(it)) pendIn.push_back(it);
        while (crcOut_.q.pop(it)) pendOut.push_back(it);
        for (size_t i = 0; i < pendOut.size();) {
            bool matched = false;
            for (size_t k = 0; k < pendIn.size(); k++) {
                if (pendIn[k].index == pendOut[i].index) {
                    bmWindows++;
                    if (pendIn[k].crc != pendOut[i].crc) bmFails++;
                    pendIn.erase(pendIn.begin() + k);
                    matched = true; break;
                }
            }
            if (matched) pendOut.erase(pendOut.begin() + i); else i++;
        }
        if (pendIn.size() > 256) pendIn.erase(pendIn.begin(), pendIn.begin() + (pendIn.size() - 256));
        if (pendOut.size() > 256) pendOut.erase(pendOut.begin(), pendOut.begin() + (pendOut.size() - 256));
        if (!running_) { pendIn.clear(); pendOut.clear(); }

        // CPU 使用率
        if (myThread) {
            FILETIME c, e, k, u;
            if (GetThreadTimes(myThread, &c, &e, &k, &u)) {
                uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
                int64_t now = bs_qpc();
                double wallUs = bs_qpc_to_us(now - lastCpuQpc);
                if (lastCpu && wallUs > 0) cpuPct = (double)(cpu - lastCpu) / 10.0 / wallUs * 100.0;
                lastCpu = cpu; lastCpuQpc = now;
            }
        }

        if (!tel_) continue;
        // 統計
        double med = 0, p99 = 0, mx = 0, mean = 0;
        uint32_t hist[BSL_HIST_BINS] = {0};
        if (!win.empty()) {
            std::vector<double> tmp(win);
            std::sort(tmp.begin(), tmp.end());
            med = tmp[tmp.size() / 2];
            p99 = tmp[std::min(tmp.size() - 1, (size_t)(tmp.size() * 0.99))];
            mx = tmp.back();
            for (double d : win) {
                int b = 0; while (b < BSL_HIST_BINS - 1 && d >= kBsHistEdges[b]) b++;
                hist[b]++;
            }
            double sum = 0; for (double v : intervals) sum += v; mean = sum / intervals.size();
        }
        std::wstring err, devName, info;
        AcquireSRWLockShared(&errLock_); err = lastError_; devName = cDevName_; info = cInfo_; ReleaseSRWLockShared(&errLock_);
        BsSettings s;
        AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);

        InterlockedIncrement(&tel_->seq);
        MemoryBarrier();
        BsTelemetry& T = *tel_;
        T.updateCounter++;
        T.pid = GetCurrentProcessId();
        T.state = running_ ? BS_ST_RUNNING : (prepared_ ? BS_ST_PREPARED : (cResolved_.load() >= 0 ? BS_ST_INIT : BS_ST_IDLE));
        wcopy(T.hostApp, 64, hostApp_);
        T.sampleRate = rate_;
        T.isDsd = dsd_;
        T.dsdTransport = running_ ? (fmt_.kind == BS_KIND_DOP ? 2 : (fmt_.kind == BS_KIND_DSD_NATIVE ? 1 : 0)) : 0;
        T.outType = s.outType;
        T.outResolved = cResolved_.load();
        wcopy(T.deviceName, 128, devName);
        wcopy(T.backendInfo, 160, info);
        T.containerBits = running_ ? fmt_.containerBits : 0;
        T.validBits = running_ ? fmt_.validBits : 0;
        T.channels = fmt_.channels;
        T.deviceRate = running_ ? fmt_.deviceRate : 0;
        T.pacerHzReq = s.pacerHz;
        T.pacerHzEff = running_ ? cHzMilli_.load() / 1000.0 : 0;
        T.chunkFrames = running_ ? cChunk_.load() : 0;
        T.hostBuffer = (uint32_t)hostBufSize_;
        T.pumpMode = s.pumpMode;
        T.driveMode = s.driveMode;
        T.ringCapacity = ring_.capacity();
        T.ringFill = running_ ? ring_.fill() : 0;
        T.ringTarget = target_.load();
        T.framesIn = framesIn_.load();
        T.framesOut = framesOut_.load();
        long ol = 0; latencies(nullptr, &ol);
        T.latencyMs = rate_ > 0 ? ol * 1000.0 / rate_ : 0;
        T.underruns = underruns_.load();
        T.underrunLogCount = underrunLogCount;
        memcpy(T.underrunTimes, underrunLog, sizeof underrunLog);
        T.wakeCount = (uint32_t)win.size();
        T.wakeExpectedUs = expectedUs;
        T.wakeMeanUs = mean;
        T.wakeMedianUs = med;
        T.wakeP99Us = p99;
        T.wakeMaxUs = mx;
        memcpy(T.hist, hist, sizeof hist);
        T.outCpuPercent = cpuPct;
        T.bitMatch = bmWindows ? (bmFails ? 2 : 1) : 0;
        T.bitMatchWindows = bmWindows;
        T.bitMatchFails = bmFails;
        T.notBitPerfect = notBitPerfect_.load() ? 1 : 0;
        T.vendorPanel = cVendor_.load();
        T.hostFillMode = running_ ? hostFillMode_.load() : -1;
        wcopy(T.lastError, 256, err);
        MemoryBarrier();
        InterlockedIncrement(&tel_->seq);

        if (!running_) { bmWindows = 0; bmFails = 0; }
    }
    if (myThread) CloseHandle(myThread);
    return 0;
}

// ======================= 操作パイプ =======================

std::string Engine::handle_command(const std::string& line) {
    std::string cmd = line, arg;
    size_t sp = line.find(' ');
    if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }

    if (cmd == "PING") {
        char b[96]; snprintf(b, sizeof b, "OK %lu", GetCurrentProcessId());
        return b;
    }
    if (cmd == "GET") {
        BsSettings s;
        AcquireSRWLockShared(&settingsLock_); s = settings_; ReleaseSRWLockShared(&settingsLock_);
        Json j = Json::object();
        j.set("settings", bs_settings_to_json(s));
        j.set("running", Json::boolean(running_));
        j.set("prepared", Json::boolean(prepared_));
        return "OK " + j.dump();
    }
    if (cmd == "SET" || cmd == "SETQ") {   // SETQ: 開き直しは起こさない (ABX で REOPEN と組み合わせる)
        bool quiet = cmd == "SETQ";
        bool ok = false;
        Json j = Json::parse(arg, &ok);
        if (!ok || j.t != Json::Obj) return "ERR json";
        AcquireSRWLockExclusive(&settingsLock_);
        BsSettings nw = settings_;
        bs_settings_from_json(j, nw);
        settings_ = nw;
        ReleaseSRWLockExclusive(&settingsLock_);
        int cls = bs_settings_diff_class(applied_, nw);
        apply_hot_settings();
        if (cls == BS_APPLY_RESET) {
            if (running_) resetPending_ = true;
            else if (prepared_ && cb_.asioMessage && hostSupportsReset_) {
                ASIOCallbacks cb = cb_;
                run_async([cb] { cb.asioMessage(kAsioResetRequest, 0, nullptr, nullptr); });
            } else if (!prepared_) {
                run_async([this] { std::wstring e; if (!m_attach(e)) set_error(e); else applied_ = settings_; });
            }
        } else if (cls == BS_APPLY_REOPEN && running_ && !quiet) {
            run_async([this] { m_reopen_output(); });
        }
        char b[32]; snprintf(b, sizeof b, "OK %d", cls);
        return b;
    }
    if (cmd == "REOPEN") {
        if (running_) run_sync([this] { m_reopen_output(); });
        return "OK";
    }
    if (cmd == "VENDORPANEL") {
        run_async([this] { if (backend_) backend_->vendor_panel(); });
        return "OK";
    }
    return "ERR unknown";
}

DWORD Engine::pipe_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<char> buf(65536);
    while (WaitForSingleObject(evQuit_, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateNamedPipeW(BSL_PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       1, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(evQuit_, 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        ResetEvent(ov.hEvent);
        BOOL c = ConnectNamedPipe(pipe, &ov);
        DWORD le = GetLastError();
        bool connected = c || le == ERROR_PIPE_CONNECTED;
        if (!connected && le == ERROR_IO_PENDING) {
            HANDLE hs[2] = { evQuit_, ov.hEvent };
            DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) { CancelIo(pipe); CloseHandle(pipe); break; }
            DWORD dummy; connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE) != 0;
        }
        while (connected) {
            ResetEvent(ov.hEvent);
            DWORD rd = 0;
            BOOL ok = ReadFile(pipe, buf.data(), (DWORD)buf.size() - 1, &rd, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                HANDLE hs[2] = { evQuit_, ov.hEvent };
                DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                if (r == WAIT_OBJECT_0) { CancelIo(pipe); connected = false; break; }
                ok = GetOverlappedResult(pipe, &ov, &rd, FALSE);
            }
            if (!ok || rd == 0) break;
            std::string req(buf.data(), rd);
            while (!req.empty() && (req.back() == '\n' || req.back() == '\r')) req.pop_back();
            std::string resp = handle_command(req);
            ResetEvent(ov.hEvent);
            DWORD wr = 0;
            ok = WriteFile(pipe, resp.data(), (DWORD)resp.size(), &wr, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                WaitForSingleObject(ov.hEvent, 2000);
                ok = GetOverlappedResult(pipe, &ov, &wr, FALSE);
            }
            if (!ok) break;
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    CloseHandle(ov.hEvent);
    return 0;
}
