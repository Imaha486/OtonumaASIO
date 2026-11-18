// asio_driver.cpp - OtonumaASIO の IASIO 実装と COM まわり
#include "../common/bs_common.h"
#include "../common/asio_abi.h"
#include "../engine/engine.h"
#include <olectl.h>
#include <cstring>
#include <cstdio>
#include <new>

HMODULE g_hModule = nullptr;
static LONG g_objects = 0, g_locks = 0;

class BslAsio : public IASIO {
public:
    BslAsio() { InterlockedIncrement(&g_objects); }
    virtual ~BslAsio() { delete engine_; InterlockedDecrement(&g_objects); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_OtonumaASIO)) {
            *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // IASIO
    ASIOBool init(void* sysHandle) override {
        if (!engine_) engine_ = new (std::nothrow) Engine();
        if (!engine_) { set_err(L"メモリ不足"); return ASIOFalse; }
        std::wstring err;
        if (!engine_->init(sysHandle, err)) { set_err(err); return ASIOFalse; }
        inited_ = true;
        return ASIOTrue;
    }
    void getDriverName(char* name) override { strcpy(name, BSL_DRIVER_NAME); }
    long getDriverVersion() override { return BSL_DRIVER_VERSION; }
    void getErrorMessage(char* s) override { strncpy(s, errMsg_, 123); s[123] = 0; }

    ASIOError start() override {
        if (!ok()) return ASE_NotPresent;
        std::wstring err;
        if (!engine_->start(err)) { set_err(err); return ASE_HWMalfunction; }
        return ASE_OK;
    }
    ASIOError stop() override {
        if (!ok()) return ASE_NotPresent;
        engine_->stop();
        return ASE_OK;
    }
    ASIOError getChannels(long* in, long* out) override {
        if (!ok()) return ASE_NotPresent;
        if (in) *in = 0;
        if (out) *out = engine_->output_channels();
        return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override {
        if (!ok()) return ASE_NotPresent;
        engine_->host_latencies(in, out);
        return ASE_OK;
    }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        if (!ok()) return ASE_NotPresent;
        engine_->buffer_sizes(mn, mx, pref, gran);
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        if (!ok()) return ASE_NotPresent;
        return engine_->can_rate(r) ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override {
        if (!ok()) return ASE_NotPresent;
        if (r) *r = engine_->rate();
        return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (!ok()) return ASE_NotPresent;
        if (r <= 0) return ASE_OK;   // 0 は外部クロック指定。対応しないが無視する
        return engine_->set_rate(r) ? ASE_OK : ASE_NoClock;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override {
        if (!c || !n) return ASE_InvalidParameter;
        if (*n < 1) return ASE_InvalidParameter;
        memset(c, 0, sizeof *c);
        c->index = 0; c->associatedChannel = -1; c->associatedGroup = -1; c->isCurrentSource = ASIOTrue;
        strcpy(c->name, "Internal");
        *n = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* pos, ASIOTimeStamp* ts) override {
        if (!ok()) return ASE_NotPresent;
        uint64_t p = 0, t = 0;
        engine_->sample_position(p, t);
        if (pos) asio_set_u64(*pos, p);
        if (ts) asio_set_u64(*ts, t);
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!ok()) return ASE_NotPresent;
        if (!info || info->isInput || info->channel < 0 || info->channel >= engine_->output_channels()) return ASE_InvalidParameter;
        info->isActive = engine_->prepared() ? ASIOTrue : ASIOFalse;
        info->channelGroup = 0;
        info->type = engine_->is_dsd() ? ASIOSTDSDInt8MSB1 : ASIOSTInt32LSB;
        snprintf(info->name, sizeof info->name, "%s", info->channel == 0 ? "Out L" : "Out R");
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long n, long size, ASIOCallbacks* cb) override {
        if (!ok()) return ASE_NotPresent;
        std::wstring err;
        if (!engine_->prepare(size, infos, n, cb, err)) { set_err(err); return ASE_InvalidMode; }
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        if (!ok()) return ASE_NotPresent;
        engine_->dispose();
        return ASE_OK;
    }
    ASIOError controlPanel() override {
        if (!engine_) return ASE_NotPresent;
        engine_->open_panel();
        return ASE_OK;
    }
    ASIOError future(long sel, void* opt) override {
        if (!ok()) return ASE_NotPresent;
        switch (sel) {
        case kAsioCanTimeInfo: return ASE_SUCCESS;
        case kAsioCanDoIoFormat: {
            ASIOIoFormat* f = (ASIOIoFormat*)opt;
            if (!f) return ASE_InvalidParameter;
            return (f->FormatType == kASIOPCMFormat || f->FormatType == kASIODSDFormat) ? ASE_SUCCESS : ASE_NotPresent;
        }
        case kAsioSetIoFormat: {
            ASIOIoFormat* f = (ASIOIoFormat*)opt;
            if (!f) return ASE_InvalidParameter;
            if (f->FormatType != kASIOPCMFormat && f->FormatType != kASIODSDFormat) return ASE_NotPresent;
            return engine_->set_dsd(f->FormatType == kASIODSDFormat) ? ASE_SUCCESS : ASE_InvalidMode;
        }
        case kAsioGetIoFormat: {
            ASIOIoFormat* f = (ASIOIoFormat*)opt;
            if (!f) return ASE_InvalidParameter;
            f->FormatType = engine_->is_dsd() ? kASIODSDFormat : kASIOPCMFormat;
            return ASE_SUCCESS;
        }
        }
        return ASE_InvalidParameter;
    }
    ASIOError outputReady() override {
        if (!ok()) return ASE_NotPresent;
        return engine_->output_ready() ? ASE_OK : ASE_NotPresent;
    }

private:
    bool ok() const { return inited_ && engine_; }
    void set_err(const std::wstring& w) {
        // ASIO のエラーメッセージは char。ホストの ANSI コードページに合わせる
        int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, errMsg_, sizeof errMsg_ - 1, nullptr, nullptr);
        if (n <= 0) errMsg_[0] = 0;
        errMsg_[sizeof errMsg_ - 1] = 0;
    }
    LONG ref_ = 1;
    Engine* engine_ = nullptr;
    bool inited_ = false;
    char errMsg_[124] = {0};
};

// ---------------- クラスファクトリ ----------------
class BslFactory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        BslAsio* a = new (std::nothrow) BslAsio();
        if (!a) return E_OUTOFMEMORY;
        HRESULT hr = a->QueryInterface(riid, ppv);
        a->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_locks); else InterlockedDecrement(&g_locks);
        return S_OK;
    }
};
static BslFactory g_factory;

extern "C" {

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hModule = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!IsEqualCLSID(rclsid, CLSID_OtonumaASIO)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow(void) { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

static LONG set_str(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    LONG r = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

STDAPI DllRegisterServer(void) {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    std::wstring clsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + BSL_CLSID_STR_W;
    if (set_str(HKEY_LOCAL_MACHINE, clsidKey, nullptr, BSL_DRIVER_NAME_W) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    set_str(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, path);
    set_str(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + BSL_DRIVER_NAME_W;
    if (set_str(HKEY_LOCAL_MACHINE, asioKey, L"CLSID", BSL_CLSID_STR_W) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    set_str(HKEY_LOCAL_MACHINE, asioKey, L"Description", BSL_DRIVER_NAME_W);
    return S_OK;
}

STDAPI DllUnregisterServer(void) {
    std::wstring clsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + BSL_CLSID_STR_W;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, (clsidKey + L"\\InprocServer32").c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, clsidKey.c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + BSL_DRIVER_NAME_W).c_str());
    return S_OK;
}

} // extern "C"
