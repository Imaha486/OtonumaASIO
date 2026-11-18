// asiohosttest.cpp - 自作の簡易 ASIO ホスト (結合試験用)
//   asiohosttest --list
//   asiohosttest [--name "OtonumaASIO" | --dll OtonumaASIO64.dll] [--rate 44100] [--seconds 5]
//                [--buffer N] [--pattern sine|ramp] [--dsd]
//   asiohosttest --verify dump.raw --bits 32 [--dsd]   (null 出力の書き出しとランプ信号の一致を確認)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../src/common/asio_abi.h"
#include "../src/common/bs_common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

static IASIO* g_drv = nullptr;
static void* g_bufs[2][2];
static long g_bufSize = 0;
static long g_type = ASIOSTInt32LSB;
static bool g_dsd = false;
static bool g_ramp = true;
static double g_rate = 44100;
static unsigned long long g_frame = 0;     // PCM: サンプル / DSD: バイト
static volatile long g_switches = 0;
static volatile long g_resets = 0;
static double g_phase = 0;
// --lazy: コールバックでは合図だけして、別スレッドが少し後にバッファを埋める (ASIO 規約上は次の bufferSwitch までに埋めればよい)
static bool g_lazy = false, g_useOutReady = false;
static HANDLE g_lazyEv = nullptr;
static volatile long g_lazyIdx = 0;
static volatile bool g_quit = false;
// --strict: リセット/再同期/遅延変更の通知を受けたら、多くの再生ソフトと同じく開き直す
static bool g_strict = false;
static volatile long g_reopenReq = 0, g_latMsgs = 0, g_resyncMsgs = 0;

static void fill(long idx) {
    for (int c = 0; c < 2; c++) {
        uint8_t* d = (uint8_t*)g_bufs[c][idx & 1];
        if (!d) continue;
        if (g_dsd) {
            long bytes = g_bufSize / 8;
            for (long i = 0; i < bytes; i++) {
                unsigned long long n = g_frame + (unsigned long long)i;
                d[i] = g_ramp ? (uint8_t)((n + (unsigned)c * 0x55 + 1) & 0xFF) : (uint8_t)0x69;
            }
        } else {
            int32_t* s = (int32_t*)d;
            for (long i = 0; i < g_bufSize; i++) {
                unsigned long long n = g_frame + (unsigned long long)i;
                if (g_ramp) s[i] = (int32_t)((uint32_t)((n * 2 + (unsigned)c + 1) & 0xFFFFFF) << 8);
                else s[i] = (int32_t)(std::sin(g_phase + 2.0 * 3.14159265358979 * 1000.0 * (double)i / g_rate) * 0.1 * 2147483647.0) & ~0xFF;
            }
        }
    }
    if (!g_ramp && !g_dsd) g_phase = std::fmod(g_phase + 2.0 * 3.14159265358979 * 1000.0 * g_bufSize / g_rate, 2.0 * 3.14159265358979);
    g_frame += g_dsd ? (unsigned long long)(g_bufSize / 8) : (unsigned long long)g_bufSize;
    InterlockedIncrement(&g_switches);
}

static void on_switch(long idx) {
    if (!g_lazy) { fill(idx); return; }
    g_lazyIdx = idx;
    SetEvent(g_lazyEv);
}
static DWORD WINAPI lazy_thread(void*) {
    while (!g_quit) {
        if (WaitForSingleObject(g_lazyEv, 100) != WAIT_OBJECT_0) continue;
        // 周期の 30% だけ遅れて埋める
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        double period = (g_dsd ? g_bufSize : (double)g_bufSize) / g_rate;
        do { QueryPerformanceCounter(&t); } while ((double)(t.QuadPart - t0.QuadPart) / f.QuadPart < period * 0.3);
        fill(g_lazyIdx);
        if (g_useOutReady && g_drv) g_drv->outputReady();
    }
    return 0;
}
static void cb_switch(long idx, ASIOBool) { on_switch(idx); }
static ASIOTime* cb_switch_ti(ASIOTime* t, long idx, ASIOBool) { on_switch(idx); return t; }
static void cb_rate(ASIOSampleRate r) { printf("[host] sampleRateDidChange %.0f\n", r); }
static long cb_msg(long sel, long value, void*, double*) {
    switch (sel) {
    case kAsioSelectorSupported:
        return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioResyncRequest ||
               value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo;
    case kAsioEngineVersion: return 2;
    case kAsioResetRequest: InterlockedIncrement(&g_resets); InterlockedIncrement(&g_reopenReq); printf("[host] kAsioResetRequest\n"); return 1;
    case kAsioResyncRequest: InterlockedIncrement(&g_resyncMsgs); if (g_strict) InterlockedIncrement(&g_reopenReq); return 1;
    case kAsioLatenciesChanged: InterlockedIncrement(&g_latMsgs); if (g_strict) InterlockedIncrement(&g_reopenReq); return 1;
    case kAsioSupportsTimeInfo: return 1;
    }
    return 0;
}

static void list_drivers() {
    HKEY root;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) { printf("ASIO ドライバの登録がありません\n"); return; }
    for (DWORD i = 0;; i++) {
        char name[256]; DWORD nl = 256;
        if (RegEnumKeyExA(root, i, name, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        printf("  %s\n", name);
    }
    RegCloseKey(root);
}

static IASIO* load_by_name(const std::string& name) {
    HKEY k;
    std::string path = "SOFTWARE\\ASIO\\" + name;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return nullptr;
    char v[64] = {0}; DWORD sz = sizeof v - 1;
    RegQueryValueExA(k, "CLSID", nullptr, nullptr, (BYTE*)v, &sz);
    RegCloseKey(k);
    wchar_t w[64]; MultiByteToWideChar(CP_ACP, 0, v, -1, w, 64);
    CLSID clsid;
    if (FAILED(CLSIDFromString(w, &clsid))) return nullptr;
    IASIO* d = nullptr;
    CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&d);
    return d;
}

static IASIO* load_by_dll(const std::string& dll) {
    HMODULE m = LoadLibraryA(dll.c_str());
    if (!m) { printf("DLL を読み込めません (%lu)\n", GetLastError()); return nullptr; }
    typedef HRESULT (STDAPICALLTYPE *PFN)(REFCLSID, REFIID, LPVOID*);
    PFN f = (PFN)GetProcAddress(m, "DllGetClassObject");
    if (!f) { printf("DllGetClassObject がありません\n"); return nullptr; }
    IClassFactory* cf = nullptr;
    if (FAILED(f(CLSID_OtonumaASIO, IID_IClassFactory, (void**)&cf)) || !cf) return nullptr;
    IASIO* d = nullptr;
    cf->CreateInstance(nullptr, CLSID_OtonumaASIO, (void**)&d);
    cf->Release();
    return d;
}

// ---- 書き出しファイルの検査 ----
static int verify(const char* path, int bits, bool dsd) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("開けません: %s\n", path); return 2; }
    std::vector<uint8_t> d;
    uint8_t b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f);
    int sb = bits / 8, fb = sb * 2;
    size_t frames = d.size() / fb;
    auto sample = [&](size_t fr, int c) -> uint32_t {
        const uint8_t* p = &d[fr * fb + c * sb];
        uint32_t v = 0;
        for (int i = 0; i < sb; i++) v |= (uint32_t)p[i] << (8 * i);
        return v << (32 - bits);
    };
    size_t i = 0, ok = 0, bad = 0, markerBad = 0;
    if (!dsd) {
        while (i < frames && sample(i, 0) == 0 && sample(i, 1) == 0) i++;
        uint32_t mask = bits == 16 ? 0xFFFF0000u : 0xFFFFFF00u;
        // コンテナで切り捨てられて先頭が 0 に見える分を戻す
        unsigned long long k0 = 0;
        while ((((uint32_t)((k0 * 2 + 1) & 0xFFFFFF) << 8) & mask) == 0 && (((uint32_t)((k0 * 2 + 2) & 0xFFFFFF) << 8) & mask) == 0) k0++;
        i = i >= k0 ? i - (size_t)k0 : 0;
        size_t start = i;
        unsigned long long k = 0;
        for (; i < frames; i++, k++) {
            uint32_t a = sample(i, 0), c = sample(i, 1);
            if (a == 0 && c == 0 && k > k0) break;   // 末尾の無音
            uint32_t ea = ((uint32_t)((k * 2 + 1) & 0xFFFFFF) << 8) & mask;
            uint32_t ec = ((uint32_t)((k * 2 + 2) & 0xFFFFFF) << 8) & mask;
            if (a == ea && c == ec) ok++; else { if (bad < 5) printf("  不一致 frame %zu: %08X %08X (期待 %08X %08X)\n", i, a, c, ea, ec); bad++; }
        }
        printf("PCM: 先頭無音 %zu フレーム, 一致 %zu, 不一致 %zu\n", start, ok, bad);
    } else {
        // DoP: 24bit 値 = marker<<16 | b0<<8 | b1 (32bit コンテナなら左詰め)
        auto dop = [&](size_t fr, int c, uint8_t& m, uint8_t& b0, uint8_t& b1) {
            uint32_t v = sample(fr, c) >> 8;
            m = (uint8_t)(v >> 16); b0 = (uint8_t)(v >> 8); b1 = (uint8_t)v;
        };
        uint8_t prevM = 0;
        for (size_t fr = 0; fr < frames; fr++) {
            uint8_t m0, x, y, m1;
            dop(fr, 0, m0, x, y); dop(fr, 1, m1, x, y);
            if (m0 != m1 || (m0 != 0x05 && m0 != 0xFA) || (fr && m0 == prevM)) markerBad++;
            prevM = m0;
        }
        while (i < frames) { uint8_t m, b0, b1; dop(i, 0, m, b0, b1); if (!(b0 == 0x69 && b1 == 0x69)) break; i++; }
        size_t start = i;
        unsigned long long k = 0;
        for (; i < frames; i++, k++) {
            uint8_t m, b0, b1, m2, c0, c1;
            dop(i, 0, m, b0, b1); dop(i, 1, m2, c0, c1);
            if (b0 == 0x69 && b1 == 0x69 && c0 == 0x69 && c1 == 0x69) break;
            uint8_t e0 = (uint8_t)((2 * k + 1) & 0xFF), e1 = (uint8_t)((2 * k + 2) & 0xFF);
            uint8_t f0 = (uint8_t)((2 * k + 0x55 + 1) & 0xFF), f1 = (uint8_t)((2 * k + 0x55 + 2) & 0xFF);
            if (b0 == e0 && b1 == e1 && c0 == f0 && c1 == f1) ok++; else { if (bad < 5) printf("  不一致 frame %zu\n", i); bad++; }
        }
        printf("DoP: 先頭無音 %zu フレーム, 一致 %zu, 不一致 %zu, マーカー異常 %zu\n", start, ok, bad, markerBad);
    }
    return (bad == 0 && markerBad == 0 && ok > 0) ? 0 : 1;
}

// 終了時にコンソールの文字コードを元に戻す (呼び出し元のバッチの表示が化けないように)
struct ConsoleCpGuard {
    UINT cp;
    ConsoleCpGuard() : cp(GetConsoleOutputCP()) { SetConsoleOutputCP(CP_UTF8); }
    ~ConsoleCpGuard() { fflush(stdout); if (cp) SetConsoleOutputCP(cp); }
};

int main(int argc, char** argv) {
    ConsoleCpGuard cpGuard;
    std::string name = BSL_DRIVER_NAME, dll, verifyPath;
    double seconds = 5;
    long buffer = 0;
    int bits = 32;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") { list_drivers(); return 0; }
        else if (a == "--name") name = next();
        else if (a == "--dll") dll = next();
        else if (a == "--rate") g_rate = atof(next().c_str());
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--buffer") buffer = atol(next().c_str());
        else if (a == "--pattern") g_ramp = next() != "sine";
        else if (a == "--dsd") g_dsd = true;
        else if (a == "--lazy") g_lazy = true;
        else if (a == "--strict") g_strict = true;
        else if (a == "--outready") { g_lazy = true; g_useOutReady = true; }
        else if (a == "--verify") verifyPath = next();
        else if (a == "--bits") bits = atoi(next().c_str());
        else { printf("不明な引数: %s\n", a.c_str()); return 2; }
    }
    if (!verifyPath.empty()) return verify(verifyPath.c_str(), bits, g_dsd);
    if (g_dsd && g_rate < 2822400) g_rate = 2822400;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_drv = dll.empty() ? load_by_name(name) : load_by_dll(dll);
    if (!g_drv) { printf("ドライバを読み込めません\n"); return 1; }
    if (!g_drv->init(GetConsoleWindow())) {
        char m[128] = {0}; g_drv->getErrorMessage(m);
        printf("init 失敗: %s\n", m); g_drv->Release(); return 1;
    }
    char dn[64] = {0}; g_drv->getDriverName(dn);
    long in = 0, out = 0; g_drv->getChannels(&in, &out);
    printf("ドライバ: %s (ver %ld)  入力 %ld / 出力 %ld\n", dn, g_drv->getDriverVersion(), in, out);
    if (g_dsd) {
        ASIOIoFormat f = {}; f.FormatType = kASIODSDFormat;
        ASIOError e = g_drv->future(kAsioSetIoFormat, &f);
        printf("SetIoFormat(DSD) = %ld\n", e);
        if (e != ASE_SUCCESS) { g_drv->Release(); return 1; }
    }
    printf("canSampleRate(%.0f) = %ld\n", g_rate, g_drv->canSampleRate(g_rate));
    if (g_drv->setSampleRate(g_rate) != ASE_OK) { printf("setSampleRate 失敗\n"); g_drv->Release(); return 1; }
    long mn, mx, pref, gran;
    g_drv->getBufferSize(&mn, &mx, &pref, &gran);
    g_bufSize = buffer ? buffer : pref;
    printf("バッファ min %ld max %ld pref %ld gran %ld → %ld\n", mn, mx, pref, gran, g_bufSize);
    ASIOBufferInfo bi[2] = {};
    for (int c = 0; c < 2; c++) { bi[c].isInput = ASIOFalse; bi[c].channelNum = c; }
    static ASIOCallbacks cbs = { cb_switch, cb_rate, cb_msg, cb_switch_ti };
    if (g_drv->createBuffers(bi, 2, g_bufSize, &cbs) != ASE_OK) {
        char m[128] = {0}; g_drv->getErrorMessage(m);
        printf("createBuffers 失敗: %s\n", m); g_drv->Release(); return 1;
    }
    for (int c = 0; c < 2; c++) { g_bufs[c][0] = bi[c].buffers[0]; g_bufs[c][1] = bi[c].buffers[1]; }
    if (g_lazy) {
        g_lazyEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CreateThread(nullptr, 0, lazy_thread, nullptr, 0, nullptr);
        if (g_useOutReady) printf("outputReady() = %ld\n", g_drv->outputReady());
        printf("遅延フィルのホストとして動作します\n");
    }
    ASIOChannelInfo ci = {}; ci.channel = 0; ci.isInput = ASIOFalse;
    g_drv->getChannelInfo(&ci);
    g_type = ci.type;
    printf("チャンネル型 %ld (%s)\n", ci.type, ci.name);
    long il, ol; g_drv->getLatencies(&il, &ol);
    printf("遅延 出力 %ld サンプル\n", ol);
    if (g_drv->start() != ASE_OK) {
        char m[128] = {0}; g_drv->getErrorMessage(m);
        printf("start 失敗: %s\n", m);
    } else {
        DWORD t0 = GetTickCount();
        long reopens = 0;
        while (GetTickCount() - t0 < (DWORD)(seconds * 1000)) {
            Sleep(500);
            if (g_strict && InterlockedExchange(&g_reopenReq, 0)) {
                // 通知を受けたので開き直す (stop → disposeBuffers → createBuffers → start)
                reopens++;
                g_drv->stop(); g_drv->disposeBuffers();
                if (g_drv->createBuffers(bi, 2, g_bufSize, &cbs) != ASE_OK) { printf("createBuffers 再失敗\n"); break; }
                for (int c = 0; c < 2; c++) { g_bufs[c][0] = bi[c].buffers[0]; g_bufs[c][1] = bi[c].buffers[1]; }
                g_drv->start();
            }
            ASIOSamples sp; ASIOTimeStamp ts;
            g_drv->getSamplePosition(&sp, &ts);
            g_drv->getLatencies(&il, &ol);
            printf("  %5.1fs  コールバック %ld 回  再生位置 %llu  遅延 %ld  開き直し %ld 回\n", (GetTickCount() - t0) / 1000.0, g_switches,
                   asio_u64(sp), ol, reopens);
        }
        g_drv->stop();
    }
    g_quit = true; Sleep(150);
    g_drv->disposeBuffers();
    g_drv->Release();
    printf("終了 (リセット要求 %ld 回, 遅延変更通知 %ld 回, 再同期要求 %ld 回)\n", g_resets, g_latMsgs, g_resyncMsgs);
    printf("再生ソフトが作った量: %llu %s\n", g_frame, g_dsd ? "バイト/ch" : "フレーム");
    CoUninitialize();
    return 0;
}
