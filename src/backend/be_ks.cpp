// be_ks.cpp - Kernel Streaming 出力 (パケット方式 / WaveRT 方式、自動判定)
// KS の構造体・GUID は SDK ヘッダの差を避けるため自前で定義している。
#include "backend_util.h"
#include "../engine/util.h"
#include <setupapi.h>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <cstdio>

namespace {
// ---------------- KS 定義 ----------------
const GUID kKSCATEGORY_AUDIO    = { 0x6994AD04, 0x93EF, 0x11D0, { 0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
const GUID kKSCATEGORY_RENDER   = { 0x65E8773E, 0x8F56, 0x11D0, { 0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
const GUID kKSCATEGORY_REALTIME = { 0xEB115FFC, 0x10C8, 0x4964, { 0x83, 0x1D, 0x6D, 0xCB, 0x02, 0xE6, 0xF2, 0x3F } };
const GUID kKSPROPSETID_Pin     = { 0x8C134960, 0x51AD, 0x11CF, { 0x87, 0x8A, 0x94, 0xF8, 0x01, 0xC1, 0x00, 0x00 } };
const GUID kKSPROPSETID_Connection = { 0x1D58C920, 0xAC9B, 0x11CF, { 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 } };
const GUID kKSPROPSETID_Audio   = { 0x45FFAAA0, 0x6E1B, 0x11D0, { 0xBC, 0xF2, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kKSPROPSETID_RtAudio = { 0xA855A48C, 0x2F78, 0x4729, { 0x90, 0x51, 0x19, 0x68, 0x74, 0x6B, 0x9E, 0xEF } };
const GUID kKSINTERFACESETID_Standard = { 0x1A8766A0, 0x62CE, 0x11CF, { 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 } };
const GUID kKSMEDIUMSETID_Standard    = { 0x4747B320, 0x62CE, 0x11CF, { 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 } };
const GUID kKSDATAFORMAT_TYPE_AUDIO   = { 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
const GUID kKSDATAFORMAT_SPECIFIER_WAVEFORMATEX = { 0x05589F81, 0xC356, 0x11CE, { 0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A } };
const GUID kGUID_NULL = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

enum { KSPROPERTY_TYPE_GET = 1, KSPROPERTY_TYPE_SET = 2 };
enum { KSPROPERTY_PIN_CTYPES = 1, KSPROPERTY_PIN_DATAFLOW = 2, KSPROPERTY_PIN_DATARANGES = 3,
       KSPROPERTY_PIN_COMMUNICATION = 7, KSPROPERTY_PIN_PROPOSEDATAFORMAT = 14 };
enum { KSPIN_DATAFLOW_IN = 1 };
enum { KSPIN_COMMUNICATION_SINK = 1, KSPIN_COMMUNICATION_BOTH = 3 };
enum { KSSTATE_STOP = 0, KSSTATE_ACQUIRE = 1, KSSTATE_PAUSE = 2, KSSTATE_RUN = 3 };
enum { KSPROPERTY_CONNECTION_STATE = 0 };
enum { KSPROPERTY_AUDIO_POSITION = 5 };
enum { KSPROPERTY_RTAUDIO_BUFFER = 1, KSPROPERTY_RTAUDIO_HWLATENCY = 2, KSPROPERTY_RTAUDIO_POSITIONREGISTER = 3,
       KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION = 5, KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT = 6,
       KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT = 7 };
enum { KSINTERFACE_STANDARD_STREAMING = 0, KSINTERFACE_STANDARD_LOOPED_STREAMING = 1 };
enum { KSDATARANGE_ATTRIBUTES = 2 };
const DWORD kIOCTL_KS_PROPERTY     = 0x002F0003;  // CTL_CODE(FILE_DEVICE_KS, 0, METHOD_NEITHER, FILE_ANY_ACCESS)
const DWORD kIOCTL_KS_WRITE_STREAM = 0x002F8013;  // CTL_CODE(FILE_DEVICE_KS, 4, METHOD_NEITHER, FILE_WRITE_ACCESS)

struct KsIdent { GUID Set; ULONG Id; ULONG Flags; };          // KSPROPERTY / KSPIN_INTERFACE / KSPIN_MEDIUM
struct KsPPin { KsIdent Property; ULONG PinId; ULONG Reserved; };
struct KsMultipleItem { ULONG Size; ULONG Count; };
struct KsDataFormat { ULONG FormatSize; ULONG Flags; ULONG SampleSize; ULONG Reserved; GUID MajorFormat; GUID SubFormat; GUID Specifier; };
struct KsDataRangeAudio { KsDataFormat DataRange; ULONG MaximumChannels; ULONG MinimumBitsPerSample; ULONG MaximumBitsPerSample;
                          ULONG MinimumSampleFrequency; ULONG MaximumSampleFrequency; };
struct KsPriority { ULONG PriorityClass; ULONG PrioritySubClass; };
struct KsPinConnect { KsIdent Interface; KsIdent Medium; ULONG PinId; HANDLE PinToHandle; KsPriority Priority; };
struct KsTime { LONGLONG Time; ULONG Numerator; ULONG Denominator; };
struct KsStreamHeader {
    ULONG Size; ULONG TypeSpecificFlags; KsTime PresentationTime; LONGLONG Duration;
    ULONG FrameExtent; ULONG DataUsed; PVOID Data; ULONG OptionsFlags;
#ifdef _WIN64
    ULONG Reserved;
#endif
};
struct KsAudioPosition { ULONGLONG PlayOffset; ULONGLONG WriteOffset; };
struct KsRtBufferProp { KsIdent Property; PVOID BaseAddress; ULONG RequestedBufferSize; };
struct KsRtBufferPropNotify { KsIdent Property; PVOID BaseAddress; ULONG RequestedBufferSize; ULONG NotificationCount; };
struct KsRtBuffer { PVOID BufferAddress; ULONG ActualBufferSize; BOOL CallMemoryBarrier; };
struct KsRtNotifyEventProp { KsIdent Property; HANDLE NotificationEvent; };
struct KsRtHwRegisterProp { KsIdent Property; PVOID BaseAddress; };
struct KsRtHwRegister { PVOID Register; ULONG Width; ULONGLONG Numerator; ULONGLONG Denominator; ULONG Accuracy; };
struct KsRtHwLatency { ULONG FifoSize; ULONG ChipsetDelay; ULONG CodecDelay; };

typedef DWORD (WINAPI *PFN_KsCreatePin)(HANDLE, KsPinConnect*, ACCESS_MASK, PHANDLE);

PFN_KsCreatePin ks_create_pin_fn() {
    static PFN_KsCreatePin fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"ksuser.dll");
        if (m) fn = (PFN_KsCreatePin)GetProcAddress(m, "KsCreatePin");
    }
    return fn;
}

// 同期 DeviceIoControl (ハンドルは OVERLAPPED で開いている)
DWORD ks_ioctl(HANDLE h, DWORD code, void* in, DWORD inSz, void* out, DWORD outSz, DWORD* ret) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD r = 0, err = ERROR_SUCCESS;
    BOOL ok = DeviceIoControl(h, code, in, inSz, out, outSz, &r, &ov);
    if (!ok) {
        err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            ok = GetOverlappedResult(h, &ov, &r, TRUE);
            err = ok ? ERROR_SUCCESS : GetLastError();
        }
    }
    CloseHandle(ov.hEvent);
    if (ret) *ret = r;
    return err;
}

bool ks_pin_ulong(HANDLE filter, ULONG pin, ULONG id, ULONG& out) {
    KsPPin p{ { kKSPROPSETID_Pin, id, KSPROPERTY_TYPE_GET }, pin, 0 };
    DWORD r = 0;
    return ks_ioctl(filter, kIOCTL_KS_PROPERTY, &p, sizeof p, &out, sizeof out, &r) == ERROR_SUCCESS;
}

bool ks_pin_data(HANDLE filter, ULONG pin, ULONG id, std::vector<uint8_t>& out) {
    KsPPin p{ { kKSPROPSETID_Pin, id, KSPROPERTY_TYPE_GET }, pin, 0 };
    DWORD need = 0;
    DWORD e = ks_ioctl(filter, kIOCTL_KS_PROPERTY, &p, sizeof p, nullptr, 0, &need);
    if (e != ERROR_MORE_DATA && e != ERROR_INSUFFICIENT_BUFFER && e != ERROR_SUCCESS) return false;
    if (need == 0) need = 4096;
    out.assign(need, 0);
    DWORD r = 0;
    e = ks_ioctl(filter, kIOCTL_KS_PROPERTY, &p, sizeof p, out.data(), need, &r);
    if (e != ERROR_SUCCESS) return false;
    out.resize(r);
    return true;
}

bool ks_set_state(HANDLE pin, ULONG st) {
    KsIdent p{ kKSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET };
    DWORD r = 0;
    return ks_ioctl(pin, kIOCTL_KS_PROPERTY, &p, sizeof p, &st, sizeof st, &r) == ERROR_SUCCESS;
}

struct PinInfo {
    ULONG id;
    std::vector<KsDataRangeAudio> ranges;
};

// 再生用 (データが入ってくる側) で PCM を受け付けるピンを探す
void ks_find_render_pins(HANDLE filter, std::vector<PinInfo>& pins) {
    pins.clear();
    KsIdent p{ kKSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, KSPROPERTY_TYPE_GET };
    ULONG count = 0; DWORD r = 0;
    if (ks_ioctl(filter, kIOCTL_KS_PROPERTY, &p, sizeof p, &count, sizeof count, &r) != ERROR_SUCCESS) return;
    for (ULONG i = 0; i < count && i < 64; i++) {
        ULONG flow = 0, comm = 0;
        if (!ks_pin_ulong(filter, i, KSPROPERTY_PIN_DATAFLOW, flow) || flow != KSPIN_DATAFLOW_IN) continue;
        if (!ks_pin_ulong(filter, i, KSPROPERTY_PIN_COMMUNICATION, comm)) continue;
        if (comm != KSPIN_COMMUNICATION_SINK && comm != KSPIN_COMMUNICATION_BOTH) continue;
        std::vector<uint8_t> d;
        if (!ks_pin_data(filter, i, KSPROPERTY_PIN_DATARANGES, d) || d.size() < sizeof(KsMultipleItem)) continue;
        KsMultipleItem* mi = (KsMultipleItem*)d.data();
        size_t off = sizeof(KsMultipleItem);
        PinInfo pi; pi.id = i;
        for (ULONG k = 0; k < mi->Count && off + sizeof(KsDataFormat) <= d.size(); k++) {
            KsDataFormat* df = (KsDataFormat*)(d.data() + off);
            size_t sz = df->FormatSize;
            if (sz < sizeof(KsDataFormat)) break;
            bool audio = IsEqualGUID(df->MajorFormat, kKSDATAFORMAT_TYPE_AUDIO) || IsEqualGUID(df->MajorFormat, kGUID_NULL);
            bool pcm = IsEqualGUID(df->SubFormat, BS_KSDATAFORMAT_SUBTYPE_PCM) || IsEqualGUID(df->SubFormat, kGUID_NULL);
            bool wfx = IsEqualGUID(df->Specifier, kKSDATAFORMAT_SPECIFIER_WAVEFORMATEX) || IsEqualGUID(df->Specifier, kGUID_NULL);
            if (audio && pcm && wfx && sz >= sizeof(KsDataRangeAudio) && off + sizeof(KsDataRangeAudio) <= d.size())
                pi.ranges.push_back(*(KsDataRangeAudio*)df);
            off += (sz + 7) & ~(size_t)7;
            if (df->Flags & KSDATARANGE_ATTRIBUTES) {   // 属性リストを読み飛ばす
                if (off + sizeof(KsMultipleItem) > d.size()) break;
                KsMultipleItem* at = (KsMultipleItem*)(d.data() + off);
                off += (at->Size + 7) & ~(size_t)7;
            }
        }
        if (!pi.ranges.empty()) pins.push_back(pi);
    }
}

bool range_ok(const KsDataRangeAudio& r, double rate, int ch, int container, int valid) {
    if (r.MaximumChannels != (ULONG)-1 && r.MaximumChannels < (ULONG)ch) return false;
    ULONG fr = (ULONG)(rate + 0.5);
    if (fr < r.MinimumSampleFrequency || fr > r.MaximumSampleFrequency) return false;
    // ドライバによって有効ビット/コンテナのどちらで書くかが違うので緩く判定
    bool b1 = (ULONG)valid >= r.MinimumBitsPerSample && (ULONG)valid <= r.MaximumBitsPerSample;
    bool b2 = (ULONG)container >= r.MinimumBitsPerSample && (ULONG)container <= r.MaximumBitsPerSample;
    return b1 || b2;
}

HANDLE ks_open_filter(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
}

struct KsDev { std::wstring path, name; bool realtime; };

void ks_enum_raw(std::vector<KsDev>& out) {
    HDEVINFO devs = SetupDiGetClassDevsW(&kKSCATEGORY_AUDIO, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return;
    for (DWORD i = 0; i < 256; i++) {
        SP_DEVICE_INTERFACE_DATA ifd = {}; ifd.cbSize = sizeof ifd;
        if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &kKSCATEGORY_AUDIO, i, &ifd)) break;
        SP_DEVICE_INTERFACE_DATA alias = {}; alias.cbSize = sizeof alias;
        if (!SetupDiGetDeviceInterfaceAlias(devs, &ifd, &kKSCATEGORY_RENDER, &alias) || !(alias.Flags & SPINT_ACTIVE)) continue;
        SP_DEVICE_INTERFACE_DATA rt = {}; rt.cbSize = sizeof rt;
        bool realtime = SetupDiGetDeviceInterfaceAlias(devs, &ifd, &kKSCATEGORY_REALTIME, &rt) && (rt.Flags & SPINT_ACTIVE);
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<uint8_t> b(need + 16, 0);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)b.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dd = {}; dd.cbSize = sizeof dd;
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, det, need, nullptr, &dd)) continue;
        KsDev d; d.path = det->DevicePath; d.realtime = realtime;
        HKEY k = SetupDiOpenDeviceInterfaceRegKey(devs, &ifd, 0, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            wchar_t nm[256] = {0}; DWORD sz = sizeof(nm) - sizeof(wchar_t), type = 0;
            if (RegQueryValueExW(k, L"FriendlyName", nullptr, &type, (BYTE*)nm, &sz) == ERROR_SUCCESS && type == REG_SZ) d.name = nm;
            RegCloseKey(k);
        }
        if (d.name.empty()) {
            wchar_t nm[256] = {0};
            if (SetupDiGetDeviceRegistryPropertyW(devs, &dd, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)nm, sizeof(nm) - 2, nullptr) ||
                SetupDiGetDeviceRegistryPropertyW(devs, &dd, SPDRP_DEVICEDESC, nullptr, (BYTE*)nm, sizeof(nm) - 2, nullptr))
                d.name = nm;
        }
        out.push_back(d);
    }
    SetupDiDestroyDeviceInfoList(devs);
}

// ---------------- バックエンド本体 ----------------
class KsBackend : public BsBackend {
public:
    explicit KsBackend(int t) : type_(t) {}
    ~KsBackend() { stop(); close(); detach(); }
    int type() const override { return type_; }
    int resolved_type() const override { return mode_ == M_RT ? BS_OUT_KS_RT : (mode_ == M_PACKET ? BS_OUT_KS_PACKET : type_); }

    bool attach(const std::wstring& id, void*, std::wstring& err) override {
        detach();
        if (!ks_create_pin_fn()) { err = L"ksuser.dll の KsCreatePin が見つかりません"; return false; }
        std::vector<KsDev> devs;
        ks_enum_raw(devs);
        const KsDev* sel = nullptr;
        for (auto& d : devs) if (!id.empty() && _wcsicmp(d.path.c_str(), id.c_str()) == 0) { sel = &d; break; }
        if (!sel && id.empty()) {
            // 未指定なら PCM 再生ピンを持つ最初のもの
            for (auto& d : devs) {
                HANDLE f = ks_open_filter(d.path);
                if (f == INVALID_HANDLE_VALUE) continue;
                std::vector<PinInfo> p; ks_find_render_pins(f, p); CloseHandle(f);
                if (!p.empty()) { sel = &d; break; }
            }
        }
        if (!sel) { err = L"KS の出力デバイスが見つかりません"; return false; }
        filter_ = ks_open_filter(sel->path);
        if (filter_ == INVALID_HANDLE_VALUE) {
            filter_ = nullptr;
            err = bs_hr_str(L"KS フィルタを開けません", (long)GetLastError());
            return false;
        }
        ks_find_render_pins(filter_, pins_);
        if (pins_.empty()) { err = L"このデバイスには PCM 再生用の KS ピンがありません"; detach(); return false; }
        name_ = sel->name; path_ = sel->path; realtimeCat_ = sel->realtime;
        return true;
    }

    void detach() override {
        if (filter_) { CloseHandle(filter_); filter_ = nullptr; }
        pins_.clear();
    }

    bool can_rate(double r, bool dsd, int dsdMode) override {
        if (!filter_) return false;
        if (dsd && dsdMode == BS_DSD_NATIVE) return false;
        double rate = dsd ? r / 16.0 : r;
        for (auto& p : pins_) for (auto& rg : p.ranges) for (auto& c : bs_pcm_candidates(dsd))
            if (range_ok(rg, rate, 2, c.container, c.valid)) return true;
        return false;
    }

    bool open(const BsOpenRequest& rq, BsEngineIO* io, BsFormat& fmt, std::wstring& err) override {
        close();
        if (!filter_) { err = L"KS フィルタが開かれていません"; return false; }
        double rate; bool dop;
        if (!bs_pcm_target(rq, rate, dop, err)) return false;
        io_ = io; s_ = *rq.s;
        chunk_ = bs_chunk_for_hz(rate, s_.pacerHz);

        // 方式の決定: 固定ならそれ、自動なら REALTIME カテゴリを持つかで先に試す方を決める
        std::vector<int> order;
        if (type_ == BS_OUT_KS_PACKET) order = { M_PACKET };
        else if (type_ == BS_OUT_KS_RT) order = { M_RT };
        else order = realtimeCat_ ? std::vector<int>{ M_RT, M_PACKET } : std::vector<int>{ M_PACKET, M_RT };

        std::wstring lastErr = L"対応する形式がありません";
        for (int m : order) {
            for (auto& c : bs_pcm_candidates(dop)) {
                for (auto& p : pins_) {
                    bool any = false;
                    for (auto& rg : p.ranges) if (range_ok(rg, rate, rq.channels, c.container, c.valid)) { any = true; break; }
                    if (!any) continue;
                    std::vector<ULONG> ifaces = m == M_RT ? std::vector<ULONG>{ KSINTERFACE_STANDARD_LOOPED_STREAMING, KSINTERFACE_STANDARD_STREAMING }
                                                          : std::vector<ULONG>{ KSINTERFACE_STANDARD_STREAMING };
                    for (ULONG ifc : ifaces) {
                        BsWaveFormatExtensible w; bs_fill_wfx(w, rate, rq.channels, c.container, c.valid);
                        DWORD e = create_pin(p.id, ifc, w);
                        if (e != ERROR_SUCCESS) {
                            lastErr = (e == ERROR_BUSY || e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED)
                                ? bs_hr_str(L"KS ピンを開けません (Windows の共有モードが使用中の可能性。既定のデバイスを別の出力先にしてください)", (long)e)
                                : bs_hr_str(L"KS ピンを作れません", (long)e);
                            continue;
                        }
                        bs_set_fmt(fmt, rate, rq.channels, c.container, c.valid, dop);
                        fmt_ = fmt;
                        if (m == M_RT) {
                            if (setup_rt(lastErr)) { mode_ = M_RT; return finish_open(); }
                            close_pin();
                            continue;
                        }
                        if (setup_packets(lastErr)) { mode_ = M_PACKET; return finish_open(); }
                        close_pin();
                    }
                }
            }
        }
        err = lastErr;
        return false;
    }

    bool start(std::wstring& err) override {
        if (!pin_ || th_) return th_ != nullptr;
        stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        startedEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        startOk_ = false;
        th_ = CreateThread(nullptr, 0, s_run, this, 0, nullptr);
        WaitForSingleObject(startedEv_, 3000);
        CloseHandle(startedEv_); startedEv_ = nullptr;
        if (!startOk_) { err = L"KS ピンを RUN にできません"; stop(); return false; }
        return true;
    }

    void stop() override {
        if (!th_) return;
        SetEvent(stopEv_);
        WaitForSingleObject(th_, INFINITE);
        CloseHandle(th_); th_ = nullptr;
        CloseHandle(stopEv_); stopEv_ = nullptr;
    }

    void close() override {
        stop();
        close_pin();
        mode_ = M_NONE;
    }

    uint32_t chunk_frames() const override { return chunk_; }
    uint32_t latency_frames() const override {
        if (mode_ == M_PACKET) return chunk_ * (uint32_t)packets_.size();
        if (mode_ == M_RT) return rtNotify_ ? rtFrames_ / 2 : leadFrames();
        return chunk_;
    }
    double effective_hz() const override { return chunk_ ? fmt_.deviceRate / chunk_ : 0; }
    std::wstring device_name() const override { return name_; }
    std::wstring info() const override { return info_; }

private:
    enum { M_NONE = 0, M_PACKET = 1, M_RT = 2 };

    DWORD create_pin(ULONG pinId, ULONG iface, const BsWaveFormatExtensible& w) {
        close_pin();
        size_t connSz = (sizeof(KsPinConnect) + 7) & ~(size_t)7;
        std::vector<uint8_t> buf(connSz + sizeof(KsDataFormat) + sizeof(BsWaveFormatExtensible) + 16, 0);
        KsPinConnect* pc = (KsPinConnect*)buf.data();
        pc->Interface.Set = kKSINTERFACESETID_Standard; pc->Interface.Id = iface; pc->Interface.Flags = 0;
        pc->Medium.Set = kKSMEDIUMSETID_Standard; pc->Medium.Id = 0; pc->Medium.Flags = 0;
        pc->PinId = pinId; pc->PinToHandle = nullptr;
        pc->Priority.PriorityClass = 0x40000000; pc->Priority.PrioritySubClass = 1;
        KsDataFormat* df = (KsDataFormat*)(buf.data() + connSz);
        df->FormatSize = sizeof(KsDataFormat) + sizeof(BsWaveFormatExtensible);
        df->Flags = 0;
        df->SampleSize = w.Format.nBlockAlign;
        df->Reserved = 0;
        df->MajorFormat = kKSDATAFORMAT_TYPE_AUDIO;
        df->SubFormat = BS_KSDATAFORMAT_SUBTYPE_PCM;
        df->Specifier = kKSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        memcpy(buf.data() + connSz + sizeof(KsDataFormat), &w, sizeof w);

        // 事前確認 (対応していないドライバも多いので失敗しても続ける)
        {
            std::vector<uint8_t> pb(sizeof(KsPPin));
            KsPPin* pp = (KsPPin*)pb.data();
            *pp = KsPPin{ { kKSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT, KSPROPERTY_TYPE_SET }, pinId, 0 };
            DWORD r = 0;
            ks_ioctl(filter_, kIOCTL_KS_PROPERTY, pp, sizeof(KsPPin), df, df->FormatSize, &r);
        }

        HANDLE h = nullptr;
        DWORD e = ks_create_pin_fn()(filter_, pc, GENERIC_WRITE | GENERIC_READ, &h);
        if (e != ERROR_SUCCESS || !h || h == INVALID_HANDLE_VALUE) return e ? e : ERROR_GEN_FAILURE;
        pin_ = h;
        return ERROR_SUCCESS;
    }

    void close_pin() {
        if (pin_) {
            if (rtEvent_ && rtNotify_) {
                KsRtNotifyEventProp p{ { kKSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT, KSPROPERTY_TYPE_GET }, rtEvent_ };
                DWORD r = 0; ks_ioctl(pin_, kIOCTL_KS_PROPERTY, &p, sizeof p, nullptr, 0, &r);
            }
            ks_set_state(pin_, KSSTATE_STOP);
            CloseHandle(pin_); pin_ = nullptr;
        }
        for (auto& pk : packets_) {
            if (pk.ov.hEvent) CloseHandle(pk.ov.hEvent);
            if (pk.mem) { VirtualUnlock(pk.mem, pk.bytes); VirtualFree(pk.mem, 0, MEM_RELEASE); }
        }
        packets_.clear();
        if (rtEvent_) { CloseHandle(rtEvent_); rtEvent_ = nullptr; }
        rtBuf_ = nullptr; rtBytes_ = 0; rtFrames_ = 0; rtPosReg_ = nullptr; rtNotify_ = false;
    }

    bool finish_open() {
        wchar_t b[160];
        if (mode_ == M_PACKET)
            swprintf(b, 160, L"KS パケット方式 / %d/%d bit / %u フレーム × %u 個", fmt_.validBits, fmt_.containerBits, chunk_, (unsigned)packets_.size());
        else
            swprintf(b, 160, L"KS WaveRT 方式 / %d/%d bit / バッファ %u フレーム / %ls / %ls", fmt_.validBits, fmt_.containerBits, rtFrames_,
                     rtNotify_ ? L"通知あり" : L"通知なし(タイマー)", rtPosReg_ ? L"位置レジスタ" : L"位置プロパティ");
        info_ = b;
        return true;
    }

    // ---- パケット方式 ----
    struct Packet {
        KsStreamHeader hdr;
        OVERLAPPED ov;
        uint8_t* mem;
        size_t bytes;
    };

    bool setup_packets(std::wstring& err) {
        int q = s_.ksQueue;
        size_t bytes = (size_t)chunk_ * fmt_.frame_bytes();
        packets_.resize((size_t)q);
        for (auto& pk : packets_) {
            memset(&pk, 0, sizeof pk);
            pk.bytes = bytes;
            pk.mem = (uint8_t*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!pk.mem) { err = L"パケット用メモリを確保できません"; return false; }
            if (s_.memLock) VirtualLock(pk.mem, bytes);
            pk.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }
        if (!ks_set_state(pin_, KSSTATE_ACQUIRE)) { err = L"KS ピンを ACQUIRE にできません"; return false; }
        return true;
    }

    bool submit(Packet& pk) {
        memset(&pk.hdr, 0, sizeof pk.hdr);
        pk.hdr.Size = sizeof(KsStreamHeader);
        pk.hdr.PresentationTime.Numerator = 1;
        pk.hdr.PresentationTime.Denominator = 1;
        pk.hdr.FrameExtent = (ULONG)pk.bytes;
        pk.hdr.DataUsed = (ULONG)pk.bytes;
        pk.hdr.Data = pk.mem;
        ResetEvent(pk.ov.hEvent);
        pk.ov.Internal = 0; pk.ov.InternalHigh = 0; pk.ov.Offset = 0; pk.ov.OffsetHigh = 0;
        DWORD r = 0;
        BOOL ok = DeviceIoControl(pin_, kIOCTL_KS_WRITE_STREAM, nullptr, 0, &pk.hdr, sizeof pk.hdr, &r, &pk.ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        return true;
    }

    void run_packets() {
        size_t n = packets_.size();
        // PAUSE の状態で全パケットを送り込んでから RUN
        ks_set_state(pin_, KSSTATE_PAUSE);
        for (auto& pk : packets_) { io_->pull(pk.mem, chunk_); submit(pk); }
        startOk_ = ks_set_state(pin_, KSSTATE_RUN);
        SetEvent(startedEv_);
        if (!startOk_) return;
        size_t next = 0;
        bool timer = s_.driveMode != BS_DRIVE_EVENT;
        BsPacer pacer;
        if (timer) pacer.start(chunk_ / fmt_.deviceRate, s_.driveMode == BS_DRIVE_TIMER_SPIN, s_.spinUs);
        for (;;) {
            if (timer) {
                if (!pacer.wait(stopEv_)) break;
                io_->wake_mark();
                io_->out_thread_poll();
                size_t done = 0;
                while (HasOverlappedIoCompleted(&packets_[next].ov) && done < n) {
                    DWORD r; GetOverlappedResult(pin_, &packets_[next].ov, &r, FALSE);
                    io_->pull(packets_[next].mem, chunk_);
                    if (!submit(packets_[next])) io_->report_error(L"KS: パケットを送れません");
                    next = (next + 1) % n; done++;
                }
                if (done == n) io_->device_underrun();   // 全部空になっていた
            } else {
                HANDLE hs[2] = { stopEv_, packets_[next].ov.hEvent };
                DWORD w = WaitForMultipleObjects(2, hs, FALSE, 2000);
                if (w == WAIT_OBJECT_0) break;
                if (w == WAIT_TIMEOUT) { io_->report_error(L"KS: パケットが完了しません"); continue; }
                io_->wake_mark();
                io_->out_thread_poll();
                DWORD r; GetOverlappedResult(pin_, &packets_[next].ov, &r, FALSE);
                io_->pull(packets_[next].mem, chunk_);
                if (!submit(packets_[next])) io_->report_error(L"KS: パケットを送れません");
                next = (next + 1) % n;
            }
        }
        ks_set_state(pin_, KSSTATE_PAUSE);
        ks_set_state(pin_, KSSTATE_ACQUIRE);
        CancelIo(pin_);
        for (auto& pk : packets_) { DWORD r; GetOverlappedResult(pin_, &pk.ov, &r, TRUE); }
        ks_set_state(pin_, KSSTATE_STOP);
    }

    // ---- WaveRT 方式 ----
    uint32_t leadFrames() const {
        uint32_t l = (uint32_t)(io_ ? io_->rt_lead_ms() * fmt_.deviceRate / 1000.0 : chunk_);
        if (l < 16) l = 16;
        if (rtFrames_ && l > rtFrames_ - chunk_ - 16) l = rtFrames_ > chunk_ + 32 ? rtFrames_ - chunk_ - 16 : rtFrames_ / 2;
        return l;
    }

    bool setup_rt(std::wstring& err) {
        uint32_t fb = fmt_.frame_bytes();
        KsRtBuffer out = {};
        DWORD r = 0;
        bool ok = false;
        rtNotify_ = false;
        if (s_.driveMode == BS_DRIVE_EVENT) {
            KsRtBufferPropNotify p = {};
            p.Property = KsIdent{ kKSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION, KSPROPERTY_TYPE_GET };
            p.BaseAddress = nullptr;
            p.RequestedBufferSize = chunk_ * 2 * fb;
            p.NotificationCount = 2;
            if (ks_ioctl(pin_, kIOCTL_KS_PROPERTY, &p, sizeof p, &out, sizeof out, &r) == ERROR_SUCCESS && out.BufferAddress) {
                rtEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                KsRtNotifyEventProp ep{ { kKSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT, KSPROPERTY_TYPE_GET }, rtEvent_ };
                if (ks_ioctl(pin_, kIOCTL_KS_PROPERTY, &ep, sizeof ep, nullptr, 0, &r) == ERROR_SUCCESS) rtNotify_ = true;
                ok = true;
            }
        }
        if (!ok) {
            // 通知なし: タイマー駆動で「書き込み先行量」ぶん先を書く
            uint32_t lead = (uint32_t)(500 * fmt_.deviceRate / 1000.0);   // 先行量の上限に合わせて確保
            uint32_t want = std::max<uint32_t>(lead + chunk_ * 2, chunk_ * 4);
            KsRtBufferProp p = {};
            p.Property = KsIdent{ kKSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_BUFFER, KSPROPERTY_TYPE_GET };
            p.BaseAddress = nullptr;
            p.RequestedBufferSize = want * fb;
            if (ks_ioctl(pin_, kIOCTL_KS_PROPERTY, &p, sizeof p, &out, sizeof out, &r) == ERROR_SUCCESS && out.BufferAddress) ok = true;
        }
        if (!ok) { err = L"WaveRT のバッファを確保できません (このピンは WaveRT 非対応)"; return false; }
        rtBuf_ = (uint8_t*)out.BufferAddress;
        rtBytes_ = out.ActualBufferSize - out.ActualBufferSize % fb;
        rtFrames_ = rtBytes_ / fb;
        rtBarrier_ = out.CallMemoryBarrier != 0;
        if (rtNotify_) chunk_ = rtFrames_ / 2;

        // 位置レジスタ (直読み) があれば使う
        KsRtHwRegisterProp hp = {};
        hp.Property = KsIdent{ kKSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_POSITIONREGISTER, KSPROPERTY_TYPE_GET };
        hp.BaseAddress = nullptr;
        KsRtHwRegister hr = {};
        if (ks_ioctl(pin_, kIOCTL_KS_PROPERTY, &hp, sizeof hp, &hr, sizeof hr, &r) == ERROR_SUCCESS && hr.Register &&
            (hr.Width == 32 || hr.Width == 64)) {
            rtPosReg_ = hr.Register; rtPosWidth_ = hr.Width;
        }
        if (!ks_set_state(pin_, KSSTATE_ACQUIRE)) { err = L"KS ピンを ACQUIRE にできません"; return false; }
        return true;
    }

    // 再生位置 (バッファ内のバイトオフセット)
    bool rt_position(uint32_t& bytePos) {
        if (rtPosReg_) {
            uint64_t v = rtPosWidth_ == 64 ? *(volatile uint64_t*)rtPosReg_ : *(volatile uint32_t*)rtPosReg_;
            bytePos = (uint32_t)(v % rtBytes_);
            return true;
        }
        KsIdent p{ kKSPROPSETID_Audio, KSPROPERTY_AUDIO_POSITION, KSPROPERTY_TYPE_GET };
        KsAudioPosition pos = {};
        DWORD r = 0;
        if (ks_ioctl(pin_, kIOCTL_KS_PROPERTY, &p, sizeof p, &pos, sizeof pos, &r) != ERROR_SUCCESS) return false;
        bytePos = (uint32_t)(pos.PlayOffset % rtBytes_);
        return true;
    }

    void rt_write(uint32_t frameOff, uint32_t frames) {
        uint32_t fb = fmt_.frame_bytes();
        while (frames) {
            uint32_t n = std::min(frames, rtFrames_ - frameOff);
            io_->pull(rtBuf_ + (size_t)frameOff * fb, n);
            frames -= n; frameOff = (frameOff + n) % rtFrames_;
        }
        if (rtBarrier_) MemoryBarrier();
    }

    void run_rt() {
        uint32_t half = rtFrames_ / 2;
        if (rtNotify_) {
            rt_write(0, rtFrames_);          // 両半分を埋めてから開始
        } else {
            rtWritten_ = leadFrames();
            memset(rtBuf_, 0, rtBytes_);
            rt_write(0, (uint32_t)rtWritten_);
        }
        ks_set_state(pin_, KSSTATE_PAUSE);
        startOk_ = ks_set_state(pin_, KSSTATE_RUN);
        SetEvent(startedEv_);
        if (!startOk_) return;

        if (rtNotify_) {
            HANDLE hs[2] = { stopEv_, rtEvent_ };
            int lastHalf = -1;
            for (;;) {
                DWORD w = WaitForMultipleObjects(2, hs, FALSE, 2000);
                if (w == WAIT_OBJECT_0) break;
                if (w == WAIT_TIMEOUT) { io_->report_error(L"KS WaveRT: 通知が来ません"); continue; }
                io_->wake_mark();
                io_->out_thread_poll();
                uint32_t bp = 0;
                int playing = 0;
                if (rt_position(bp)) playing = (bp / fmt_.frame_bytes()) < half ? 0 : 1;
                else playing = lastHalf < 0 ? 1 : lastHalf ^ 1;
                int fill = playing ^ 1;       // 再生中でない方を埋める
                if (fill == lastHalf) io_->device_underrun();
                rt_write(fill ? half : 0, fill ? rtFrames_ - half : half);
                lastHalf = fill;
            }
        } else {
            BsPacer pacer;
            pacer.start(chunk_ / fmt_.deviceRate, s_.driveMode == BS_DRIVE_TIMER_SPIN, s_.spinUs);
            uint64_t playAbs = 0;
            uint32_t lastPos = 0;
            while (pacer.wait(stopEv_)) {
                io_->wake_mark();
                io_->out_thread_poll();
                uint32_t bp = 0;
                if (!rt_position(bp)) continue;
                uint32_t pf = bp / fmt_.frame_bytes();
                uint32_t adv = pf >= lastPos ? pf - lastPos : pf + rtFrames_ - lastPos;
                playAbs += adv; lastPos = pf;
                uint32_t lead = leadFrames();
                if (rtWritten_ < playAbs) {          // 追い越された
                    io_->device_underrun();
                    rtWritten_ = playAbs;
                }
                uint64_t ahead = rtWritten_ - playAbs;
                if (ahead < lead) {
                    uint32_t n = (uint32_t)(lead - ahead);
                    if (n > rtFrames_ - 16) n = rtFrames_ - 16;
                    rt_write((uint32_t)(rtWritten_ % rtFrames_), n);
                    rtWritten_ += n;
                }
            }
        }
        ks_set_state(pin_, KSSTATE_PAUSE);
        ks_set_state(pin_, KSSTATE_ACQUIRE);
        ks_set_state(pin_, KSSTATE_STOP);
    }

    static DWORD WINAPI s_run(void* p) { ((KsBackend*)p)->run(); return 0; }
    void run() {
        io_->out_thread_begin();
        if (mode_ == M_PACKET) run_packets();
        else if (mode_ == M_RT) run_rt();
        else SetEvent(startedEv_);
        io_->out_thread_end();
    }

    int type_;
    int mode_ = M_NONE;
    HANDLE filter_ = nullptr, pin_ = nullptr;
    std::vector<PinInfo> pins_;
    std::wstring name_, path_, info_;
    bool realtimeCat_ = false;
    BsEngineIO* io_ = nullptr;
    BsSettings s_;
    BsFormat fmt_;
    uint32_t chunk_ = 0;
    std::vector<Packet> packets_;
    HANDLE th_ = nullptr, stopEv_ = nullptr, startedEv_ = nullptr;
    volatile bool startOk_ = false;
    // WaveRT
    uint8_t* rtBuf_ = nullptr;
    uint32_t rtBytes_ = 0, rtFrames_ = 0;
    bool rtBarrier_ = false, rtNotify_ = false;
    HANDLE rtEvent_ = nullptr;
    void* rtPosReg_ = nullptr;
    ULONG rtPosWidth_ = 32;
    uint64_t rtWritten_ = 0;
};
} // namespace

BsBackend* bs_new_ks(int t) { return new KsBackend(t); }

void bs_enum_ks(std::vector<BsDeviceInfo>& out) {
    std::vector<KsDev> devs;
    ks_enum_raw(devs);
    out.push_back(BsDeviceInfo{ L"", L"（自動: 最初に見つかった KS 出力）" });
    for (auto& d : devs) {
        HANDLE f = ks_open_filter(d.path);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::vector<PinInfo> p; ks_find_render_pins(f, p);
        CloseHandle(f);
        if (p.empty()) continue;
        std::wstring nm = d.name.empty() ? d.path : d.name;
        if (d.realtime) nm += L" [WaveRT]";
        out.push_back(BsDeviceInfo{ d.path, nm });
    }
}
