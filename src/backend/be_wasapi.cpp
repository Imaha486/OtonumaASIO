// be_wasapi.cpp - WASAPI 排他モード出力 (イベント駆動 / タイマー駆動)
#include "backend_util.h"
#include "../engine/util.h"
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <vector>
#include <cstdio>

namespace {
const CLSID kCLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
const IID kIID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
const IID kIID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4c32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
const IID kIID_IAudioRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
struct BsPropKey { GUID fmtid; DWORD pid; };
const BsPropKey kPKEY_Device_FriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

#ifndef AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
#define AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED ((HRESULT)0x88890019L)
#endif
#ifndef AUDCLNT_E_DEVICE_IN_USE
#define AUDCLNT_E_DEVICE_IN_USE ((HRESULT)0x8889000AL)
#endif
#ifndef AUDCLNT_STREAMFLAGS_NOPERSIST
#define AUDCLNT_STREAMFLAGS_NOPERSIST 0x00080000
#endif

template <class T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct ComScope {
    bool ok;
    ComScope() { HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); ok = SUCCEEDED(hr); }
    ~ComScope() { if (ok) CoUninitialize(); }
};

std::wstring device_friendly_name(IMMDevice* dev) {
    std::wstring name;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(*(const PROPERTYKEY*)&kPKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            name = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return name;
}

IMMDeviceEnumerator* make_enumerator() {
    IMMDeviceEnumerator* e = nullptr;
    CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&e);
    return e;
}

class WasapiBackend : public BsBackend {
public:
    ~WasapiBackend() { stop(); close(); detach(); }
    int type() const override { return BS_OUT_WASAPI; }

    bool attach(const std::wstring& id, void*, std::wstring& err) override {
        detach();
        IMMDeviceEnumerator* en = make_enumerator();
        if (!en) { err = L"MMDeviceEnumerator を作れません"; return false; }
        HRESULT hr;
        if (id.empty()) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev_);
        else hr = en->GetDevice(id.c_str(), &dev_);
        en->Release();
        if (FAILED(hr) || !dev_) { err = bs_hr_str(L"出力デバイスが見つかりません", hr); dev_ = nullptr; return false; }
        name_ = device_friendly_name(dev_);
        return true;
    }
    void detach() override { safe_release(dev_); }

    bool can_rate(double r, bool dsd, int dsdMode) override {
        if (!dev_) return false;
        if (dsd && dsdMode == BS_DSD_NATIVE) return false;
        double rate = dsd ? r / 16.0 : r;
        IAudioClient* ac = nullptr;
        if (FAILED(dev_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac)) || !ac) return false;
        bool ok = false;
        for (auto& c : bs_pcm_candidates(dsd)) {
            BsWaveFormatExtensible w; bs_fill_wfx(w, rate, 2, c.container, c.valid);
            if (ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&w, nullptr) == S_OK) { ok = true; break; }
        }
        ac->Release();
        return ok;
    }

    bool open(const BsOpenRequest& rq, BsEngineIO* io, BsFormat& fmt, std::wstring& err) override {
        close();
        if (!dev_) { err = L"デバイスがありません"; return false; }
        double rate; bool dop;
        if (!bs_pcm_target(rq, rate, dop, err)) return false;
        io_ = io; s_ = *rq.s;
        eventMode_ = s_.driveMode == BS_DRIVE_EVENT;

        HRESULT hr = dev_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac_);
        if (FAILED(hr)) { err = bs_hr_str(L"IAudioClient を作れません", hr); return false; }

        // 形式: 32/32 → 24/32 → 24/24 → 16/16
        BsWaveFormatExtensible w; bool found = false;
        for (auto& c : bs_pcm_candidates(dop)) {
            bs_fill_wfx(w, rate, rq.channels, c.container, c.valid);
            if (ac_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&w, nullptr) == S_OK) {
                bs_set_fmt(fmt, rate, rq.channels, c.container, c.valid, dop);
                found = true; break;
            }
        }
        if (!found) { err = L"デバイスがこの形式・レートを排他モードで受け付けません"; close(); return false; }

        REFERENCE_TIME defP = 0, minP = 0;
        ac_->GetDevicePeriod(&defP, &minP);
        minPeriod_ = minP;
        uint32_t want = bs_chunk_for_hz(rate, s_.pacerHz);
        REFERENCE_TIME period = (REFERENCE_TIME)((double)want * 1e7 / rate + 0.5);
        if (period < minP) period = minP;
        if (period > 20000000) period = 20000000;   // 2 秒まで

        // 初期化の組み合わせ。ドライバによって受け付けるものが違うので順に試す。
        //   イベント駆動: バッファ長 = 周期 (WASAPI の決まり)
        //   タイマー駆動: (4周期, 周期) → (4周期, 既定周期) → (2周期, 周期) → 最後の手段として
        //                 イベント駆動と同じ形で初期化し、通知を使わずタイマーで書く
        struct Try { REFERENCE_TIME buf, per; bool evFlag; const wchar_t* name; };
        std::vector<Try> tries;
        if (eventMode_) tries.push_back(Try{ period, period, true, L"" });
        else {
            tries.push_back(Try{ period * 4, period, false, L"4周期" });
            tries.push_back(Try{ period * 4, 0, false, L"4周期/既定周期" });
            tries.push_back(Try{ period * 2, period, false, L"2周期" });
            tries.push_back(Try{ period, period, true, L"1周期(通知形式)" });
        }
        std::wstring errs;
        evInit_ = false;
        const wchar_t* used = L"";
        hr = E_FAIL;
        for (size_t t = 0; t < tries.size(); t++) {
            if (t > 0 || !ac_) {
                safe_release(ac_);
                hr = dev_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac_);
                if (FAILED(hr)) { err = bs_hr_str(L"IAudioClient を作れません", hr); return false; }
            }
            DWORD flags = AUDCLNT_STREAMFLAGS_NOPERSIST | (tries[t].evFlag ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0);
            REFERENCE_TIME bufDur = tries[t].buf, perArg = tries[t].per;
            hr = ac_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, bufDur, perArg, (WAVEFORMATEX*)&w, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                // 整列済みのフレーム数から長さを計算し直して作り直す
                UINT32 frames = 0;
                ac_->GetBufferSize(&frames);
                safe_release(ac_);
                hr = dev_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac_);
                if (FAILED(hr)) { err = bs_hr_str(L"IAudioClient を作り直せません", hr); return false; }
                REFERENCE_TIME aligned = (REFERENCE_TIME)(1e7 * frames / rate + 0.5);
                if (perArg == bufDur) perArg = aligned;
                bufDur = aligned;
                hr = ac_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, bufDur, perArg, (WAVEFORMATEX*)&w, nullptr);
            }
            if (SUCCEEDED(hr)) { evInit_ = tries[t].evFlag; used = tries[t].name; break; }
            if (hr == AUDCLNT_E_DEVICE_IN_USE) break;
            wchar_t b[64]; swprintf(b, 64, L"%ls0x%08lX", errs.empty() ? L"" : L", ", (unsigned long)hr);
            errs += b;
        }
        if (FAILED(hr)) {
            err = hr == AUDCLNT_E_DEVICE_IN_USE ? bs_hr_str(L"デバイスが他のアプリに使われています", hr)
                                                : L"排他モードで初期化できません (" + errs + L")";
            close(); return false;
        }
        UINT32 bf = 0;
        ac_->GetBufferSize(&bf);
        bufFrames_ = bf;
        if (evInit_) {
            // 通知形式で初期化したときはイベントの登録が必須。1回に書く量はバッファ全体。
            ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ac_->SetEventHandle(ev_);
            chunk_ = bufFrames_;
        } else {
            chunk_ = want;
            if (chunk_ > bufFrames_ / 2) chunk_ = bufFrames_ / 2;
            if (chunk_ == 0) chunk_ = 1;
        }
        hr = ac_->GetService(kIID_IAudioRenderClient, (void**)&rc_);
        if (FAILED(hr)) { err = bs_hr_str(L"IAudioRenderClient を取得できません", hr); close(); return false; }
        fmt_ = fmt;
        wchar_t b[160];
        swprintf(b, 160, L"WASAPI 排他 %ls%ls%ls / %d/%d bit / バッファ %u フレーム", eventMode_ ? L"イベント駆動" : L"タイマー駆動",
                 used[0] ? L" " : L"", used, fmt.validBits, fmt.containerBits, bufFrames_);
        info_ = b;
        return true;
    }

    bool start(std::wstring& err) override {
        if (!ac_ || th_) return th_ != nullptr;
        // 開始前に1バッファ分を埋めておく
        BYTE* p = nullptr;
        if (SUCCEEDED(rc_->GetBuffer(bufFrames_, &p))) {
            io_->pull(p, bufFrames_);
            rc_->ReleaseBuffer(bufFrames_, 0);
        }
        stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        startHr_ = S_OK;
        startedEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        th_ = CreateThread(nullptr, 0, s_run, this, 0, nullptr);
        WaitForSingleObject(startedEv_, 3000);
        CloseHandle(startedEv_); startedEv_ = nullptr;
        if (FAILED(startHr_)) { err = bs_hr_str(L"再生を開始できません", startHr_); stop(); return false; }
        return true;
    }

    void stop() override {
        if (!th_) return;
        SetEvent(stopEv_);
        WaitForSingleObject(th_, INFINITE);
        CloseHandle(th_); th_ = nullptr;
        CloseHandle(stopEv_); stopEv_ = nullptr;
        if (ac_) { ac_->Stop(); ac_->Reset(); }
    }

    void close() override {
        stop();
        safe_release(rc_);
        safe_release(ac_);
        if (ev_) { CloseHandle(ev_); ev_ = nullptr; }
    }

    uint32_t chunk_frames() const override { return chunk_; }
    uint32_t latency_frames() const override { return bufFrames_; }
    double effective_hz() const override { return chunk_ ? fmt_.deviceRate / chunk_ : 0; }
    std::wstring device_name() const override { return name_; }
    std::wstring info() const override { return info_; }

private:
    static DWORD WINAPI s_run(void* p) { ((WasapiBackend*)p)->run(); return 0; }
    void run() {
        ComScope com;
        io_->out_thread_begin();
        startHr_ = ac_->Start();
        SetEvent(startedEv_);
        if (FAILED(startHr_)) { io_->out_thread_end(); return; }
        if (eventMode_) {
            HANDLE hs[2] = { stopEv_, ev_ };
            for (;;) {
                DWORD r = WaitForMultipleObjects(2, hs, FALSE, 2000);
                if (r == WAIT_OBJECT_0) break;
                if (r == WAIT_TIMEOUT) { io_->report_error(L"WASAPI: デバイスからの通知が来ません"); continue; }
                io_->wake_mark();
                io_->out_thread_poll();
                BYTE* p = nullptr;
                if (SUCCEEDED(rc_->GetBuffer(bufFrames_, &p))) {
                    io_->pull(p, bufFrames_);
                    rc_->ReleaseBuffer(bufFrames_, 0);
                }
            }
        } else {
            BsPacer pacer;
            pacer.start(chunk_ / fmt_.deviceRate, s_.driveMode == BS_DRIVE_TIMER_SPIN, s_.spinUs);
            while (pacer.wait(stopEv_)) {
                io_->wake_mark();
                io_->out_thread_poll();
                UINT32 pad = 0;
                if (FAILED(ac_->GetCurrentPadding(&pad))) continue;
                UINT32 avail = bufFrames_ - pad;
                while (avail >= chunk_) {
                    BYTE* p = nullptr;
                    if (FAILED(rc_->GetBuffer(chunk_, &p))) break;
                    io_->pull(p, chunk_);
                    rc_->ReleaseBuffer(chunk_, 0);
                    avail -= chunk_;
                }
            }
        }
        io_->out_thread_end();
    }

    IMMDevice* dev_ = nullptr;
    IAudioClient* ac_ = nullptr;
    IAudioRenderClient* rc_ = nullptr;
    HANDLE ev_ = nullptr, th_ = nullptr, stopEv_ = nullptr, startedEv_ = nullptr;
    volatile HRESULT startHr_ = S_OK;
    BsEngineIO* io_ = nullptr;
    BsSettings s_;
    BsFormat fmt_;
    bool eventMode_ = true;
    bool evInit_ = false;     // 通知形式で初期化した (タイマー駆動の最後の手段でも true)
    UINT32 bufFrames_ = 0;
    uint32_t chunk_ = 0;
    REFERENCE_TIME minPeriod_ = 0;
    std::wstring name_, info_;
};
} // namespace

BsBackend* bs_new_wasapi() { return new WasapiBackend(); }

void bs_enum_wasapi(std::vector<BsDeviceInfo>& out) {
    ComScope com;
    IMMDeviceEnumerator* en = make_enumerator();
    if (!en) return;
    out.push_back(BsDeviceInfo{ L"", L"（既定の出力デバイス）" });
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) {
                out.push_back(BsDeviceInfo{ id, device_friendly_name(d) });
                CoTaskMemFree(id);
            }
            d->Release();
        }
        col->Release();
    }
    en->Release();
}
