// be_asio.cpp - ASIO 中継 (ベンダー ASIO ドライバをホストとして開いて流す)
// ASIO のコールバックはユーザー引数を持たないため、中継先はプロセスに 1 つだけ。
#include "backend_util.h"
#include "../common/asio_abi.h"
#include "../engine/util.h"
#include <vector>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <cstdio>
#include <cstring>

namespace {
class AsioRelay;
std::atomic<AsioRelay*> g_relay{nullptr};

bool clsid_from_string(const wchar_t* s, CLSID& out) { return SUCCEEDED(CLSIDFromString((LPOLESTR)s, &out)); }

bool read_driver_clsid(const std::wstring& name, CLSID& out) {
    HKEY k;
    std::wstring path = L"SOFTWARE\\ASIO\\" + name;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t v[64] = {0}; DWORD sz = sizeof(v) - 2, type = 0;
    bool ok = RegQueryValueExW(k, L"CLSID", nullptr, &type, (BYTE*)v, &sz) == ERROR_SUCCESS && type == REG_SZ && clsid_from_string(v, out);
    RegCloseKey(k);
    return ok;
}

class AsioRelay : public BsBackend {
public:
    ~AsioRelay() { stop(); close(); detach(); }
    int type() const override { return BS_OUT_ASIO; }

    bool attach(const std::wstring& id, void* sysHandle, std::wstring& err) override {
        detach();
        std::wstring name = id;
        if (name.empty()) {
            std::vector<BsDeviceInfo> v; bs_enum_asio(v);
            for (auto& d : v) if (!d.id.empty()) { name = d.id; break; }
        }
        if (name.empty()) { err = L"ASIO ドライバが見つかりません"; return false; }
        CLSID clsid;
        if (!read_driver_clsid(name, clsid)) { err = L"ASIO ドライバの登録が読めません: " + name; return false; }
        if (IsEqualCLSID(clsid, CLSID_OtonumaASIO)) { err = L"自分自身は中継先にできません"; return false; }
        AsioRelay* expected = nullptr;
        if (!g_relay.compare_exchange_strong(expected, this)) { err = L"ASIO 中継はプロセスに 1 つだけです"; return false; }
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&drv_);
        if (FAILED(hr) || !drv_) { g_relay = nullptr; drv_ = nullptr; err = bs_hr_str(L"ASIO ドライバを読み込めません", hr); return false; }
        if (!drv_->init(sysHandle ? sysHandle : GetDesktopWindow())) {
            char m[128] = {0}; drv_->getErrorMessage(m);
            err = L"ASIO ドライバの初期化に失敗: " + std::wstring(m, m + strlen(m));
            drv_->Release(); drv_ = nullptr; g_relay = nullptr;
            return false;
        }
        char dn[64] = {0}; drv_->getDriverName(dn);
        name_ = std::wstring(dn, dn + strlen(dn));
        if (name_.empty()) name_ = name;
        long in = 0, out = 0; drv_->getChannels(&in, &out);
        outCh_ = out;
        ASIOIoFormat f = {}; f.FormatType = kASIODSDFormat;
        nativeDsd_ = drv_->future(kAsioCanDoIoFormat, &f) == ASE_SUCCESS;
        return true;
    }

    void detach() override {
        if (drv_) { drv_->Release(); drv_ = nullptr; }
        AsioRelay* me = this;
        g_relay.compare_exchange_strong(me, nullptr);
    }

    bool supports_native_dsd() const override { return nativeDsd_; }
    bool has_vendor_panel() const override { return drv_ != nullptr; }
    void vendor_panel() override { if (drv_) drv_->controlPanel(); }

    bool can_rate(double r, bool dsd, int dsdMode) override {
        if (!drv_ || created_) return created_ && r == curRate_;
        if (dsd) {
            if (dsdMode != BS_DSD_DOP && nativeDsd_) {
                bool ok = set_io(kASIODSDFormat) && drv_->canSampleRate(r) == ASE_OK;
                set_io(kASIOPCMFormat);
                if (ok || dsdMode == BS_DSD_NATIVE) return ok;
            }
            if (dsdMode == BS_DSD_NATIVE) return false;
            return drv_->canSampleRate(r / 16.0) == ASE_OK;
        }
        return drv_->canSampleRate(r) == ASE_OK;
    }

    bool open(const BsOpenRequest& rq, BsEngineIO* io, BsFormat& fmt, std::wstring& err) override {
        close();
        if (!drv_) { err = L"ASIO ドライバが読み込まれていません"; return false; }
        io_ = io; s_ = *rq.s;
        int first = s_.asioFirstCh;
        if (first + rq.channels > outCh_) first = 0;
        if (rq.channels > outCh_) { err = L"出力チャンネルが足りません"; return false; }

        kind_ = BS_KIND_PCM;
        double vendorRate = rq.hostRate;
        if (rq.dsd) {
            bool native = nativeDsd_ && rq.dsdMode != BS_DSD_DOP;
            if (rq.dsdMode == BS_DSD_NATIVE && !nativeDsd_) { err = L"このドライバはネイティブ DSD に対応していません"; return false; }
            if (native && set_io(kASIODSDFormat) && drv_->setSampleRate(rq.hostRate) == ASE_OK) {
                kind_ = BS_KIND_DSD_NATIVE;
            } else {
                if (rq.dsdMode == BS_DSD_NATIVE) { err = L"ネイティブ DSD でこのレートに設定できません"; return false; }
                if (rq.hostRate > bs_dop_max_rate(s_.dopMax) + 1.0) { err = L"DoP で送れる上限を超えた DSD レートです"; return false; }
                set_io(kASIOPCMFormat);
                kind_ = BS_KIND_DOP;
                vendorRate = rq.hostRate / 16.0;
            }
        } else {
            set_io(kASIOPCMFormat);
        }
        if (kind_ != BS_KIND_DSD_NATIVE) {
            ASIOSampleRate cur = 0; drv_->getSampleRate(&cur);
            if (cur != vendorRate && drv_->setSampleRate(vendorRate) != ASE_OK) { err = L"ASIO ドライバがこのレートに設定できません"; return false; }
        }
        curRate_ = rq.hostRate;

        // バッファサイズ: 受け渡し周期 (Hz) から最も近い許可値
        long mn = 0, mx = 0, pref = 0, gran = 0;
        drv_->getBufferSize(&mn, &mx, &pref, &gran);
        double want = vendorRate / s_.pacerHz;
        long bs = choose_size(want, mn, mx, pref, gran);
        if (kind_ == BS_KIND_DSD_NATIVE && bs % 8) bs += 8 - bs % 8;

        // 出力チャンネルの型
        ASIOChannelInfo ci = {}; ci.channel = first; ci.isInput = ASIOFalse;
        if (drv_->getChannelInfo(&ci) != ASE_OK) { err = L"チャンネル情報を取得できません"; return false; }
        type_ = ci.type;
        int container = 32, valid = 32;
        switch (type_) {
        case ASIOSTInt32LSB: container = 32; valid = 32; break;
        case ASIOSTInt32LSB24: container = 32; valid = 24; break;
        case ASIOSTInt24LSB: container = 24; valid = 24; break;
        case ASIOSTInt16LSB: container = 16; valid = 16; break;
        case ASIOSTFloat32LSB: container = 32; valid = 24; break;   // 24bit までは float で正確に表せる
        case ASIOSTDSDInt8MSB1: container = 8; valid = 8; break;
        default: err = L"このドライバのサンプル形式には対応していません"; return false;
        }
        if (kind_ == BS_KIND_DSD_NATIVE && type_ != ASIOSTDSDInt8MSB1) { err = L"DSD の形式が想定外です"; return false; }
        if (kind_ == BS_KIND_DOP) {
            if (valid < 24) { err = L"DoP には 24bit 以上が必要です"; return false; }
            if (container == 32) valid = 24;
        }
        if (kind_ == BS_KIND_PCM && type_ == ASIOSTDSDInt8MSB1) { err = L"PCM なのに DSD の形式になっています"; return false; }

        ASIOBufferInfo bi[2] = {};
        for (int c = 0; c < rq.channels; c++) { bi[c].isInput = ASIOFalse; bi[c].channelNum = first + c; }
        static ASIOCallbacks cbs = { s_bufferSwitch, s_rateChanged, s_message, s_bufferSwitchTI };
        ASIOError e = drv_->createBuffers(bi, rq.channels, bs, &cbs);
        if (e != ASE_OK) { err = L"ASIO バッファを作れません"; return false; }
        created_ = true;
        for (int c = 0; c < rq.channels; c++) { bufs_[c][0] = bi[c].buffers[0]; bufs_[c][1] = bi[c].buffers[1]; }
        nch_ = rq.channels;
        vendorBuf_ = bs;
        outputReady_ = drv_->outputReady() == ASE_OK;

        double devRate = kind_ == BS_KIND_DSD_NATIVE ? rq.hostRate / 8.0 : vendorRate;
        fmt.deviceRate = devRate; fmt.channels = rq.channels;
        fmt.containerBits = container; fmt.validBits = valid; fmt.kind = kind_;
        fmt_ = fmt;
        chunk_ = kind_ == BS_KIND_DSD_NATIVE ? (uint32_t)(bs / 8) : (uint32_t)bs;
        scratch_.assign((size_t)chunk_ * fmt.frame_bytes(), 0);
        long il = 0, ol = 0;
        drv_->getLatencies(&il, &ol);
        latency_ = kind_ == BS_KIND_DSD_NATIVE ? (uint32_t)(ol / 8) : (uint32_t)ol;
        if (latency_ < chunk_) latency_ = chunk_;
        wchar_t b[160];
        swprintf(b, 160, L"ASIO 中継 / %ls / バッファ %ld / 型 %ld%ls", kind_ == BS_KIND_DSD_NATIVE ? L"ネイティブ DSD" : (kind_ == BS_KIND_DOP ? L"DoP" : L"PCM"),
                 bs, (long)type_, type_ == ASIOSTFloat32LSB ? L" (float)" : L"");
        info_ = b;
        return true;
    }

    bool start(std::wstring& err) override {
        if (!created_) return false;
        if (running_) return true;
        running_ = true;
        if (drv_->start() != ASE_OK) { running_ = false; err = L"ASIO ドライバを開始できません"; return false; }
        return true;
    }
    void stop() override {
        if (running_ && drv_) drv_->stop();
        running_ = false;
    }
    void close() override {
        stop();
        if (created_ && drv_) drv_->disposeBuffers();
        created_ = false;
    }
    uint32_t chunk_frames() const override { return chunk_; }
    uint32_t latency_frames() const override { return latency_; }
    double effective_hz() const override { return chunk_ ? fmt_.deviceRate / chunk_ : 0; }
    std::wstring device_name() const override { return name_; }
    std::wstring info() const override { return info_; }

private:
    bool set_io(long t) {
        ASIOIoFormat f = {}; f.FormatType = t;
        ASIOError e = drv_->future(kAsioSetIoFormat, &f);
        return e == ASE_SUCCESS || (t == kASIOPCMFormat && (e == ASE_NotPresent || e == ASE_InvalidParameter));
    }

    static long choose_size(double want, long mn, long mx, long pref, long gran) {
        if (mn <= 0 || mx < mn) return pref > 0 ? pref : 1024;
        if (gran == 0) return pref;
        long best = pref; double bd = 1e300;
        if (gran == -1) {
            for (long v = 1; v <= mx && v > 0; v <<= 1) {
                if (v < mn) continue;
                double d = std::fabs(std::log(v / want));
                if (d < bd) { bd = d; best = v; }
            }
        } else {
            long k = (long)((want - mn) / gran + 0.5);
            if (k < 0) k = 0;
            best = mn + k * gran;
            if (best > mx) best = mx;
        }
        return best;
    }

    void on_switch(long idx) {
        if (!running_ || !io_) return;
        io_->wake_mark();
        io_->pull(scratch_.data(), chunk_);
        const uint32_t fb = fmt_.frame_bytes();
        const uint32_t sb = (uint32_t)fmt_.containerBits / 8;
        for (int c = 0; c < nch_; c++) {
            uint8_t* dst = (uint8_t*)bufs_[c][idx & 1];
            const uint8_t* src = scratch_.data() + c * sb;
            if (!dst) continue;
            switch (type_) {
            case ASIOSTInt32LSB:
                for (uint32_t f = 0; f < chunk_; f++) memcpy(dst + f * 4, src + f * fb, 4);
                break;
            case ASIOSTInt32LSB24:   // 32bit の下位 24bit に右詰め
                for (uint32_t f = 0; f < chunk_; f++) { int32_t v; memcpy(&v, src + f * fb, 4); v >>= 8; memcpy(dst + f * 4, &v, 4); }
                break;
            case ASIOSTInt24LSB:
                for (uint32_t f = 0; f < chunk_; f++) memcpy(dst + f * 3, src + f * fb, 3);
                break;
            case ASIOSTInt16LSB:
                for (uint32_t f = 0; f < chunk_; f++) memcpy(dst + f * 2, src + f * fb, 2);
                break;
            case ASIOSTFloat32LSB:
                for (uint32_t f = 0; f < chunk_; f++) { int32_t v; memcpy(&v, src + f * fb, 4); float x = (float)((double)v / 2147483648.0); memcpy(dst + f * 4, &x, 4); }
                break;
            case ASIOSTDSDInt8MSB1:
                for (uint32_t f = 0; f < chunk_; f++) dst[f] = src[f * fb];
                break;
            }
        }
        if (outputReady_) drv_->outputReady();
    }

    static void s_bufferSwitch(long idx, ASIOBool) { AsioRelay* r = g_relay.load(); if (r) r->on_switch(idx); }
    static ASIOTime* s_bufferSwitchTI(ASIOTime* t, long idx, ASIOBool) { AsioRelay* r = g_relay.load(); if (r) r->on_switch(idx); return t; }
    static void s_rateChanged(ASIOSampleRate) { AsioRelay* r = g_relay.load(); if (r && r->io_) r->io_->request_host_reset(); }
    static long s_message(long sel, long value, void*, double*) {
        AsioRelay* r = g_relay.load();
        switch (sel) {
        case kAsioSelectorSupported:
            return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
                   value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo || value == kAsioBufferSizeChange;
        case kAsioEngineVersion: return 2;
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
            if (r && r->io_) r->io_->request_reopen();
            return 1;
        case kAsioResyncRequest: return 1;
        case kAsioLatenciesChanged: return 1;
        case kAsioSupportsTimeInfo: return 1;
        }
        return 0;
    }

    IASIO* drv_ = nullptr;
    std::wstring name_, info_;
    long outCh_ = 0;
    bool nativeDsd_ = false;
    bool created_ = false;
    volatile bool running_ = false;
    bool outputReady_ = false;
    void* bufs_[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};
    int nch_ = 2;
    long vendorBuf_ = 0;
    long type_ = ASIOSTInt32LSB;
    int kind_ = BS_KIND_PCM;
    double curRate_ = 0;
    uint32_t chunk_ = 0, latency_ = 0;
    std::vector<uint8_t> scratch_;
    BsEngineIO* io_ = nullptr;
    BsSettings s_;
    BsFormat fmt_;
};
} // namespace

BsBackend* bs_new_asio() { return new AsioRelay(); }

void bs_enum_asio(std::vector<BsDeviceInfo>& out) {
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < 256; i++) {
        wchar_t name[256]; DWORD nl = 256;
        if (RegEnumKeyExW(root, i, name, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t clsid[64] = {0}; DWORD sz = sizeof(clsid) - 2, type = 0;
        bool ok = RegQueryValueExW(k, L"CLSID", nullptr, &type, (BYTE*)clsid, &sz) == ERROR_SUCCESS;
        wchar_t desc[256] = {0}; DWORD dsz = sizeof(desc) - 2;
        RegQueryValueExW(k, L"Description", nullptr, nullptr, (BYTE*)desc, &dsz);
        RegCloseKey(k);
        if (!ok || _wcsicmp(clsid, BSL_CLSID_STR_W) == 0) continue;   // 自分自身は除外
        out.push_back(BsDeviceInfo{ name, desc[0] ? desc : name });
    }
    RegCloseKey(root);
}
