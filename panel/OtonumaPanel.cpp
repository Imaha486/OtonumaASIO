// OtonumaPanel.cpp - OtonumaASIO コントロールパネル (Win32 のみ)
//   設定の変更・保存、プリセット、計測の表示、ベンダーパネル呼び出し、ブラインド A/B (ABX)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../src/common/bs_common.h"
#include "../src/common/settings.h"
#include "../src/backend/backend.h"
#include <objbase.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <random>
#include <cstdlib>
#include <cwchar>
#include <algorithm>

#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

enum {
    ID_OUTTYPE = 100, ID_DEVICE, ID_REFRESH, ID_HOSTBUF, ID_PACER, ID_PREFILL, ID_PUMP, ID_DRIVE, ID_KSQ, ID_RTLEAD,
    ID_PRIO, ID_CORE, ID_TIMER, ID_MEMLOCK, ID_DSD, ID_DOPMAX, ID_DRAIN, ID_SILENCE, ID_FALLBACK, ID_APPLY, ID_REVERT,
    ID_PRESET, ID_PRESETNAME, ID_PSAVE, ID_PLOAD, ID_PDEL,
    ID_VENDOR, ID_RESETSTATS,
    ID_ABX_A, ID_ABX_B, ID_ABX_N, ID_ABX_START, ID_ABX_PLAYA, ID_ABX_PLAYB, ID_ABX_PLAYX, ID_ABX_ANSA, ID_ABX_ANSB,
    ID_ABX_SAVE, ID_ABX_STOP,
    ID_TIMER_TICK = 1
};

HINSTANCE g_inst;
HWND g_wnd;
HFONT g_font, g_fontBold;
double g_scale = 1.0;
int S(int v) { return (int)std::lround(v * g_scale); }

HWND h(int id) { return GetDlgItem(g_wnd, id); }

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int hgt, int id, HFONT f = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(hgt), g_wnd,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}
HWND label(const wchar_t* t, int x, int y, int w, int id = -1, HFONT f = nullptr) { return mk(L"STATIC", t, SS_LEFT, x, y + 3, w, 20, id, f); }
HWND combo(int id, int x, int y, int w) { return mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id); }
HWND edit(int id, int x, int y, int w) { return mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x, y, w, 23, id); }
HWND button(const wchar_t* t, int id, int x, int y, int w) { return mk(L"BUTTON", t, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 26, id); }
HWND check(const wchar_t* t, int id, int x, int y, int w) { return mk(L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id); }
HWND group(const wchar_t* t, int x, int y, int w, int hgt) { return mk(L"BUTTON", t, BS_GROUPBOX, x, y, w, hgt, -1, g_fontBold); }

void cb_add(int id, const wchar_t* t) { SendMessageW(h(id), CB_ADDSTRING, 0, (LPARAM)t); }
int cb_sel(int id) { return (int)SendMessageW(h(id), CB_GETCURSEL, 0, 0); }
void cb_set(int id, int i) { SendMessageW(h(id), CB_SETCURSEL, i, 0); }
std::wstring get_text(int id) {
    int n = GetWindowTextLengthW(h(id));
    std::wstring s((size_t)n + 1, 0);
    GetWindowTextW(h(id), &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}
void set_text(int id, const std::wstring& t) {
    std::wstring cur = get_text(id);
    if (cur != t) SetWindowTextW(h(id), t.c_str());
}
std::wstring fmt(const wchar_t* f, ...) {
    wchar_t b[1024];
    va_list ap; va_start(ap, f); _vsnwprintf(b, 1023, f, ap); va_end(ap);
    b[1023] = 0;
    return b;
}

// ---- 状態 ----
std::vector<BsDeviceInfo> g_devices;
int g_devType = -1;
BsSettings g_cur;
Json g_doc;
BsTelemetry g_tel;
bool g_telOk = false;
HWND g_status = nullptr;
RECT g_histRect;

// ---- 操作パイプ ----
bool pipe_call(const std::string& req, std::string& resp) {
    char buf[65536]; DWORD rd = 0;
    if (!CallNamedPipeW(BSL_PIPE_NAME, (void*)req.data(), (DWORD)req.size(), buf, sizeof buf - 1, &rd, 1000)) return false;
    resp.assign(buf, rd);
    return true;
}

// ---- 共有メモリ (計測) ----
bool read_telemetry(BsTelemetry& out) {
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, BSL_TELEMETRY_NAME);
    if (!m) return false;
    const BsTelemetry* t = (const BsTelemetry*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(BsTelemetry));
    bool ok = false;
    if (t) {
        for (int tries = 0; tries < 20 && !ok; tries++) {
            LONG s1 = t->seq;
            if (s1 & 1) { Sleep(0); continue; }
            MemoryBarrier();
            memcpy(&out, t, sizeof out);
            MemoryBarrier();
            ok = (t->seq == s1) && out.magic == BSL_TELEMETRY_MAGIC && out.version == BSL_TELEMETRY_VERSION;
        }
        UnmapViewOfFile(t);
    }
    CloseHandle(m);
    if (ok) {
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, out.pid);
        if (!p) return false;
        bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
        CloseHandle(p);
        ok = alive;
    }
    return ok;
}

// ---- 設定 ⇔ 画面 ----
const int kHostBufs[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };

std::wstring dev_key(int outType, const std::wstring& id) { return fmt(L"%d:", outType) + id; }

void fill_devices(int outType, const std::wstring& selId) {
    if (outType != g_devType) {
        g_devices.clear();
        bs_enum_devices(outType, g_devices);
        g_devType = outType;
    }
    SendMessageW(h(ID_DEVICE), CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t i = 0; i < g_devices.size(); i++) {
        cb_add(ID_DEVICE, g_devices[i].name.c_str());
        if (g_devices[i].id == selId) sel = (int)i;
    }
    if (g_devices.empty()) cb_add(ID_DEVICE, L"（見つかりません）");
    cb_set(ID_DEVICE, sel);
}

std::wstring num(double v) {
    wchar_t b[64]; swprintf(b, 64, L"%.6g", v); return b;
}

void settings_to_ui(const BsSettings& s) {
    cb_set(ID_OUTTYPE, s.outType);
    fill_devices(s.outType, bs_settings_device(s));
    int hb = 5;
    for (int i = 0; i < 11; i++) if (kHostBufs[i] == s.hostBuffer) hb = i;
    cb_set(ID_HOSTBUF, hb);
    SetWindowTextW(h(ID_PACER), num(s.pacerHz).c_str());
    SetWindowTextW(h(ID_PREFILL), num(s.prefillSec).c_str());
    cb_set(ID_PUMP, s.pumpMode);
    cb_set(ID_DRIVE, s.driveMode);
    SetWindowTextW(h(ID_KSQ), num(s.ksQueue).c_str());
    SetWindowTextW(h(ID_RTLEAD), num(s.rtLeadMs).c_str());
    cb_set(ID_PRIO, s.outPriority);
    cb_set(ID_CORE, s.cpuCore + 1);
    cb_set(ID_TIMER, s.timerRes);
    SendMessageW(h(ID_MEMLOCK), BM_SETCHECK, s.memLock ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(h(ID_DRAIN), BM_SETCHECK, s.drainOnStop ? BST_CHECKED : BST_UNCHECKED, 0);
    cb_set(ID_DSD, s.dsdMode);
    cb_set(ID_DOPMAX, s.dopMax);
    SetWindowTextW(h(ID_SILENCE), num(s.startSilenceMs).c_str());
    SendMessageW(h(ID_FALLBACK), BM_SETCHECK, s.ksFallback ? BST_CHECKED : BST_UNCHECKED, 0);
}

BsSettings ui_to_settings() {
    BsSettings s = g_cur;
    s.outType = cb_sel(ID_OUTTYPE);
    int d = cb_sel(ID_DEVICE);
    std::wstring id = (d >= 0 && d < (int)g_devices.size()) ? g_devices[(size_t)d].id : std::wstring();
    switch (s.outType) {
    case BS_OUT_ASIO: s.devAsio = id; break;
    case BS_OUT_WASAPI: s.devWasapi = id; break;
    case BS_OUT_KS_AUTO: case BS_OUT_KS_PACKET: case BS_OUT_KS_RT: s.devKs = id; break;
    }
    int hb = cb_sel(ID_HOSTBUF);
    s.hostBuffer = kHostBufs[hb < 0 ? 5 : hb];
    s.pacerHz = _wtof(get_text(ID_PACER).c_str());
    s.prefillSec = _wtof(get_text(ID_PREFILL).c_str());
    s.pumpMode = cb_sel(ID_PUMP);
    s.driveMode = cb_sel(ID_DRIVE);
    s.ksQueue = _wtoi(get_text(ID_KSQ).c_str());
    s.rtLeadMs = _wtoi(get_text(ID_RTLEAD).c_str());
    s.outPriority = cb_sel(ID_PRIO);
    s.cpuCore = cb_sel(ID_CORE) - 1;
    s.timerRes = cb_sel(ID_TIMER);
    s.memLock = SendMessageW(h(ID_MEMLOCK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.drainOnStop = SendMessageW(h(ID_DRAIN), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.dsdMode = cb_sel(ID_DSD);
    s.dopMax = cb_sel(ID_DOPMAX) < 0 ? 2 : cb_sel(ID_DOPMAX);
    s.startSilenceMs = _wtoi(get_text(ID_SILENCE).c_str());
    s.ksFallback = SendMessageW(h(ID_FALLBACK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    bs_settings_clamp(s);
    return s;
}

void status(const std::wstring& t) { SetWindowTextW(g_status, t.c_str()); }

// 保存 (current とデバイスごとの最終設定) して、動作中なら DLL へ送る
int apply_settings(const BsSettings& s, bool quiet, bool showStatus) {
    bs_load_document(g_doc);
    if (g_doc.t != Json::Obj) g_doc = Json::object();
    g_doc.set("current", bs_settings_to_json(s));
    Json* devs = g_doc.get("devices");
    if (!devs || devs->t != Json::Obj) devs = &g_doc.set("devices", Json::object());
    devs->set(bs_to_utf8(dev_key(s.outType, bs_settings_device(s))), bs_settings_to_json(s));
    bool saved = bs_save_document(g_doc);
    int cls = -1;
    std::string resp;
    std::string req = std::string(quiet ? "SETQ " : "SET ") + bs_settings_to_json(s).dump();
    if (pipe_call(req, resp) && resp.compare(0, 3, "OK ") == 0) cls = atoi(resp.c_str() + 3);
    if (showStatus) {
        std::wstring m = saved ? L"設定を保存しました。" : L"設定ファイルを保存できませんでした。";
        if (cls < 0) m += L"（再生ソフトで OtonumaASIO が動いていないため、次回から反映）";
        else if (cls == BS_APPLY_NOW) m += L" すぐ反映しました。";
        else if (cls == BS_APPLY_REOPEN) m += L" 出力を開き直して反映しました（一瞬途切れます）。";
        else m += L" 再生ソフトに再初期化を要求しました。";
        status(m);
    }
    g_cur = s;
    return cls;
}

// ---- プリセット ----
void refresh_presets() {
    bs_load_document(g_doc);
    int ids[3] = { ID_PRESET, ID_ABX_A, ID_ABX_B };
    for (int id : ids) {
        int old = cb_sel(id);
        SendMessageW(h(id), CB_RESETCONTENT, 0, 0);
        const Json* p = g_doc.get("presets");
        if (p && p->t == Json::Obj) for (auto& kv : p->o) cb_add(id, bs_from_utf8(kv.first).c_str());
        int n = (int)SendMessageW(h(id), CB_GETCOUNT, 0, 0);
        cb_set(id, old >= 0 && old < n ? old : (id == ID_ABX_B && n > 1 ? 1 : 0));
    }
}

bool preset_get(const std::wstring& name, BsSettings& s) {
    bs_load_document(g_doc);
    const Json* p = g_doc.get("presets");
    const Json* j = p ? p->get(bs_to_utf8(name)) : nullptr;
    if (!j) return false;
    s = BsSettings();
    bs_settings_from_json(*j, s);
    return true;
}

std::wstring cb_text(int id) {
    int i = cb_sel(id);
    if (i < 0) return L"";
    int n = (int)SendMessageW(h(id), CB_GETLBTEXTLEN, i, 0);
    std::wstring s((size_t)n + 1, 0);
    SendMessageW(h(id), CB_GETLBTEXT, i, (LPARAM)&s[0]);
    s.resize((size_t)n);
    return s;
}

// ---- ABX ----
struct AbxTrial { int x; int answer; };
struct Abx {
    bool active = false;
    BsSettings a, b, saved;
    std::wstring nameA, nameB;
    int n = 10;
    int cur = 0;
    int x = 0;
    std::vector<AbxTrial> trials;
    std::mt19937 rng{ (unsigned)GetTickCount() ^ (unsigned)time(nullptr) };
    std::wstring lastResult;
} g_abx;

void abx_switch(const BsSettings& s) {
    // 同じプリセットへの切替でも必ず同じ開き直しを行う (途切れで判別できないように)
    apply_settings(s, true, false);
    std::string resp;
    pipe_call("REOPEN", resp);
}

double binom_tail(int n, int k) {   // P(X >= k), X ~ Bin(n, 0.5)
    double p = 0;
    for (int i = k; i <= n; i++) {
        double c = 1;
        for (int j = 0; j < i; j++) c = c * (n - j) / (j + 1);
        p += c;
    }
    return p / std::pow(2.0, n);
}

void abx_ui() {
    bool a = g_abx.active;
    EnableWindow(h(ID_ABX_START), !a);
    EnableWindow(h(ID_ABX_STOP), a);
    EnableWindow(h(ID_ABX_PLAYA), a);
    EnableWindow(h(ID_ABX_PLAYB), a);
    EnableWindow(h(ID_ABX_PLAYX), a);
    EnableWindow(h(ID_ABX_ANSA), a);
    EnableWindow(h(ID_ABX_ANSB), a);
    EnableWindow(h(ID_ABX_SAVE), !g_abx.trials.empty() && !a);
    if (a) set_text(ID_ABX_SAVE + 100, fmt(L"試行 %d / %d ：X を聴いて「X は A」か「X は B」を選んでください（A・B も聴けます）", g_abx.cur + 1, g_abx.n));
    else set_text(ID_ABX_SAVE + 100, g_abx.lastResult);
}

void abx_next() {
    g_abx.x = (int)(g_abx.rng() & 1);
    abx_switch(g_abx.x ? g_abx.b : g_abx.a);
    abx_ui();
}

void abx_finish() {
    g_abx.active = false;
    int k = 0;
    for (auto& t : g_abx.trials) if (t.x == t.answer) k++;
    int n = (int)g_abx.trials.size();
    double p = n ? binom_tail(n, k) : 1.0;
    g_abx.lastResult = fmt(L"結果: %d / %d 正解　偶然でこれ以上当たる確率 p = %.4f%ls", k, n, p,
                           p < 0.05 ? L"（聞き分けられていると言える水準）" : L"（偶然の範囲）");
    abx_switch(g_abx.saved);
    settings_to_ui(g_abx.saved);
    abx_ui();
}

void abx_start() {
    std::wstring na = cb_text(ID_ABX_A), nb = cb_text(ID_ABX_B);
    BsSettings a, b;
    if (na.empty() || nb.empty() || !preset_get(na, a) || !preset_get(nb, b)) {
        MessageBoxW(g_wnd, L"プリセット A と B を選んでください。先に「プリセット」欄で保存しておきます。", L"ABX", MB_ICONINFORMATION);
        return;
    }
    if (bs_settings_diff_class(a, b) == BS_APPLY_RESET) {
        MessageBoxW(g_wnd, L"A と B で、出力方式・デバイス・ホスト側バッファ・DSD の送り方が同じプリセットを選んでください。\n（これらは再生ソフトの再初期化が必要なため、切り替えで判別できてしまいます）", L"ABX", MB_ICONWARNING);
        return;
    }
    if (!g_telOk || g_tel.state != BS_ST_RUNNING) {
        if (MessageBoxW(g_wnd, L"再生中ではないようです。再生しながら行ってください。このまま始めますか？", L"ABX", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    g_abx.a = a; g_abx.b = b; g_abx.nameA = na; g_abx.nameB = nb;
    g_abx.saved = g_cur;
    g_abx.n = std::max(1, std::min(100, _wtoi(get_text(ID_ABX_N).c_str())));
    g_abx.cur = 0;
    g_abx.trials.clear();
    g_abx.active = true;
    g_abx.lastResult.clear();
    abx_next();
}

void abx_answer(int ans) {
    if (!g_abx.active) return;
    g_abx.trials.push_back(AbxTrial{ g_abx.x, ans });
    g_abx.cur++;
    if (g_abx.cur >= g_abx.n) abx_finish();
    else abx_next();
}

void abx_save_csv() {
    wchar_t file[MAX_PATH] = L"abx_result.csv";
    OPENFILENAMEW of = { sizeof of };
    of.hwndOwner = g_wnd; of.lpstrFilter = L"CSV (*.csv)\0*.csv\0"; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.Flags = OFN_OVERWRITEPROMPT; of.lpstrDefExt = L"csv";
    if (!GetSaveFileNameW(&of)) return;
    std::string out = "\xEF\xBB\xBF";   // Excel 用 BOM
    out += "trial,X,answer,correct\r\n";
    int k = 0, i = 1;
    for (auto& t : g_abx.trials) {
        char b[96];
        snprintf(b, sizeof b, "%d,%s,%s,%d\r\n", i++, t.x ? "B" : "A", t.answer ? "B" : "A", t.x == t.answer);
        out += b;
        if (t.x == t.answer) k++;
    }
    int n = (int)g_abx.trials.size();
    char b[256];
    snprintf(b, sizeof b, "\r\nA,%s\r\nB,%s\r\ncorrect,%d\r\ntrials,%d\r\np_value,%.6f\r\n",
             bs_to_utf8(g_abx.nameA).c_str(), bs_to_utf8(g_abx.nameB).c_str(), k, n, n ? binom_tail(n, k) : 1.0);
    out += b;
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { status(L"CSV を保存できません"); return; }
    DWORD wr; WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr); CloseHandle(f);
    status(L"ABX の結果を CSV に保存しました");
}

// ---- 計測表示 ----
std::wstring ft_str(uint64_t ft) {
    FILETIME f; f.dwLowDateTime = (DWORD)ft; f.dwHighDateTime = (DWORD)(ft >> 32);
    FILETIME lf; SYSTEMTIME st;
    FileTimeToLocalFileTime(&f, &lf); FileTimeToSystemTime(&lf, &st);
    return fmt(L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

const wchar_t* state_name(int s) {
    switch (s) {
    case BS_ST_INIT: return L"待機（init 済み）";
    case BS_ST_PREPARED: return L"準備完了（停止中）";
    case BS_ST_RUNNING: return L"再生中";
    case BS_ST_ERROR: return L"エラー";
    }
    return L"未使用";
}

void update_telemetry() {
    g_telOk = read_telemetry(g_tel);
    const BsTelemetry& t = g_tel;
    if (!g_telOk) {
        set_text(ID_RESETSTATS + 100, L"再生ソフトで OtonumaASIO が使われていません。\r\n再生ソフトの出力先に「OtonumaASIO」(ASIO) を選んでください。");
        set_text(ID_RESETSTATS + 101, L"");
        set_text(ID_RESETSTATS + 102, L"");
        SendMessageW(h(ID_RESETSTATS + 103), PBM_SETPOS, 0, 0);
        EnableWindow(h(ID_VENDOR), FALSE);
        InvalidateRect(g_wnd, &g_histRect, TRUE);
        return;
    }
    std::wstring kind = t.isDsd ? (t.dsdTransport == 2 ? L"DSD→DoP" : (t.dsdTransport == 1 ? L"DSD ネイティブ" : L"DSD")) : L"PCM";
    std::wstring rate = t.isDsd ? fmt(L"DSD%.0f", t.sampleRate / 44100.0) : fmt(L"%.1f kHz", t.sampleRate / 1000.0);
    std::wstring path = fmt(L"%ls → %ls %ls", t.hostApp, rate.c_str(), kind.c_str());
    if (t.state == BS_ST_RUNNING) path += fmt(L" / %d/%d bit → %ls → %ls", t.validBits, t.containerBits,
                                              bs_out_type_name(t.outResolved >= 0 ? t.outResolved : t.outType), t.deviceName);
    else path += fmt(L" → %ls → %ls", bs_out_type_name(t.outResolved >= 0 ? t.outResolved : t.outType), t.deviceName);
    std::wstring line2 = fmt(L"状態: %ls　受け渡し周期 %.6g Hz → 実効 %.1f Hz（1回 %u フレーム）　ホスト側バッファ %u　遅延 %.1f ms",
                             state_name(t.state), t.pacerHzReq, t.pacerHzEff, t.chunkFrames, t.hostBuffer, t.latencyMs);
    std::wstring line3 = t.backendInfo;
    if (t.state == BS_ST_RUNNING && t.hostFillMode >= 0)
        line3 += t.hostFillMode == 2 ? L"　／ 再生ソフト: outputReady で受け取り"
               : (t.hostFillMode == 1 ? L"　／ 再生ソフト: その場で書き込み" : L"　／ 再生ソフト: 後から書き込み（1 周期遅らせて受け取り）");
    set_text(ID_RESETSTATS + 100, path + L"\r\n" + line2 + L"\r\n" + line3);

    double sec = t.deviceRate > 0 ? 1.0 / t.deviceRate : 0;
    int pct = t.ringCapacity ? (int)(t.ringFill * 1000 / t.ringCapacity) : 0;
    SendMessageW(h(ID_RESETSTATS + 103), PBM_SETPOS, pct, 0);
    std::wstring ring = fmt(L"リング残量 %.3f 秒 / 目標 %.3f 秒 / 容量 %.3f 秒　（%ls）",
                            t.ringFill * sec, t.ringTarget * sec, t.ringCapacity * sec, t.pumpMode ? L"先読みモード" : L"同期モード");
    std::wstring ur = fmt(L"アンダーラン %u 回", t.underruns);
    if (t.underrunLogCount) {
        ur += L"　直近:";
        uint32_t n = std::min<uint32_t>(t.underrunLogCount, 4);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = (t.underrunLogCount - 1 - i) % BSL_UNDERRUN_LOG;
            ur += L" " + ft_str(t.underrunTimes[idx]);
        }
    }
    std::wstring bm = t.bitMatch == 0 ? L"判定中" : (t.bitMatch == 1 ? fmt(L"一致（%u 秒分）", t.bitMatchWindows)
                                                                    : fmt(L"不一致 %u / %u 秒", t.bitMatchFails, t.bitMatchWindows));
    std::wstring wake = fmt(L"出力スレッド起床間隔: 期待 %.0f µs / 平均 %.0f µs　ずれ 中央値 %.1f µs・99%% %.1f µs・最大 %.1f µs（%u 回）　CPU %.2f %%",
                            t.wakeExpectedUs, t.wakeMeanUs, t.wakeMedianUs, t.wakeP99Us, t.wakeMaxUs, t.wakeCount, t.outCpuPercent);
    std::wstring l2 = ring + L"\r\n" + ur + L"\r\n" + wake + L"\r\nパイプライン内ビット一致: " + bm;
    if (t.notBitPerfect) l2 += L"\r\n⚠ 下位ビットを切り捨てています（ビットパーフェクトではありません）";
    set_text(ID_RESETSTATS + 101, l2);
    set_text(ID_RESETSTATS + 102, t.lastError[0] ? std::wstring(L"メッセージ: ") + t.lastError : L"");
    EnableWindow(h(ID_VENDOR), t.vendorPanel != 0);
    InvalidateRect(g_wnd, &g_histRect, TRUE);
}

void paint_hist(HDC dc) {
    RECT r = g_histRect;
    HBRUSH bg = CreateSolidBrush(RGB(250, 250, 250));
    FillRect(dc, &r, bg); DeleteObject(bg);
    FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* labels[BSL_HIST_BINS] = { L"<1", L"<2", L"<5", L"<10", L"<20", L"<50", L"<100", L"<200", L"<500", L"<1m", L"<2m", L"<5m", L"5m+" };
    int n = BSL_HIST_BINS;
    int w = (r.right - r.left - S(16)) / n;
    int top = r.top + S(22), bottom = r.bottom - S(20);
    SetTextColor(dc, RGB(60, 60, 60));
    RECT tr = { r.left + S(6), r.top + S(3), r.right, r.top + S(20) };
    DrawTextW(dc, L"起床間隔のずれ（µs, m=ms）の分布", -1, &tr, DT_LEFT | DT_SINGLELINE);
    uint32_t mx = 1;
    if (g_telOk) for (int i = 0; i < n; i++) mx = std::max(mx, g_tel.hist[i]);
    HBRUSH bar = CreateSolidBrush(RGB(70, 120, 200));
    for (int i = 0; i < n; i++) {
        int x = r.left + S(8) + i * w;
        if (g_telOk && g_tel.hist[i]) {
            double f = std::log1p((double)g_tel.hist[i]) / std::log1p((double)mx);   // 対数スケール
            int y = bottom - (int)((bottom - top) * f);
            RECT b = { x + 2, y, x + w - 2, bottom };
            FillRect(dc, &b, bar);
        }
        RECT lr = { x, bottom + 2, x + w, r.bottom };
        DrawTextW(dc, labels[i], -1, &lr, DT_CENTER | DT_SINGLELINE);
    }
    DeleteObject(bar);
}

// ---- 画面構築 ----
void build_ui() {
    int x0 = 12, y = 10;
    group(L"設定", x0 - 4, y, 440, 558);
    y += 22;
    int lx = x0 + 6, cx = x0 + 190, cw = 236;
    auto row = [&](const wchar_t* lab, const wchar_t* tag) {
        label(lab, lx, y, 150);
        HWND t = label(tag, lx + 150, y, 34);
        (void)t;
    };
    row(L"出力方式", L"[再]"); combo(ID_OUTTYPE, cx, y, cw);
    for (int i = 0; i < BS_OUT_COUNT; i++) cb_add(ID_OUTTYPE, bs_out_type_name(i));
    y += 30;
    row(L"出力デバイス", L"[再]"); combo(ID_DEVICE, cx, y, cw - 52); button(L"更新", ID_REFRESH, cx + cw - 48, y, 48);
    y += 30;
    row(L"ホスト側バッファ", L"[再]"); combo(ID_HOSTBUF, cx, y, 120);
    for (int v : kHostBufs) cb_add(ID_HOSTBUF, fmt(L"%d サンプル", v).c_str());
    y += 30;
    row(L"受け渡し周期 (Hz)", L"[開]"); edit(ID_PACER, cx, y, 90); label(L"1〜1000", cx + 96, y, 120);
    y += 30;
    row(L"先読み量 (秒)", L"[即]"); edit(ID_PREFILL, cx, y, 90); label(L"0.05〜2（先読みモード時）", cx + 96, y, 150);
    y += 30;
    row(L"ポンプモード", L"[即]"); combo(ID_PUMP, cx, y, cw);
    cb_add(ID_PUMP, L"同期（普通の ASIO と同じ）"); cb_add(ID_PUMP, L"先読み（RAM に溜める）");
    y += 30;
    row(L"駆動方式", L"[開]"); combo(ID_DRIVE, cx, y, cw);
    cb_add(ID_DRIVE, L"イベント"); cb_add(ID_DRIVE, L"タイマー"); cb_add(ID_DRIVE, L"タイマー＋スピン（1コア占有）");
    y += 30;
    row(L"KS キュー深さ", L"[開]"); edit(ID_KSQ, cx, y, 90); label(L"2〜16", cx + 96, y, 120);
    y += 30;
    row(L"WaveRT 書き込み先行量 (ms)", L"[即]"); edit(ID_RTLEAD, cx, y, 90); label(L"1〜500", cx + 96, y, 120);
    y += 30;
    row(L"出力スレッド優先度", L"[即]"); combo(ID_PRIO, cx, y, cw);
    cb_add(ID_PRIO, L"MMCSS Pro Audio"); cb_add(ID_PRIO, L"MMCSS Pro Audio＋Critical"); cb_add(ID_PRIO, L"TimeCritical（MMCSSなし）");
    y += 30;
    row(L"CPU コア固定", L"[即]"); combo(ID_CORE, cx, y, 120);
    cb_add(ID_CORE, L"自動");
    { SYSTEM_INFO si; GetSystemInfo(&si); for (DWORD i = 0; i < si.dwNumberOfProcessors && i < 64; i++) cb_add(ID_CORE, fmt(L"コア %lu", i).c_str()); }
    y += 30;
    row(L"タイマー分解能", L"[即]"); combo(ID_TIMER, cx, y, 120);
    cb_add(ID_TIMER, L"OS 既定"); cb_add(ID_TIMER, L"1 ms"); cb_add(ID_TIMER, L"0.5 ms");
    y += 30;
    row(L"DSD の送り方", L"[再]"); combo(ID_DSD, cx, y, 100);
    cb_add(ID_DSD, L"自動"); cb_add(ID_DSD, L"ネイティブ"); cb_add(ID_DSD, L"DoP");
    label(L"DoP 上限", cx + 106, y, 56); combo(ID_DOPMAX, cx + 162, y, 74);
    cb_add(ID_DOPMAX, L"DSD64"); cb_add(ID_DOPMAX, L"DSD128"); cb_add(ID_DOPMAX, L"DSD256"); cb_add(ID_DOPMAX, L"DSD512");
    y += 30;
    row(L"開始時の無音 (ms)", L"[即]"); edit(ID_SILENCE, cx, y, 90); label(L"0〜2000", cx + 96, y, 120);
    y += 30;
    check(L"メモリ固定（VirtualLock） [開]", ID_MEMLOCK, lx, y, 210);
    check(L"停止時に溜めた分を鳴らし切る [即]", ID_DRAIN, lx + 212, y, 216);
    y += 26;
    check(L"KS で開けないときは WASAPI 排他に自動で切り替える [開]", ID_FALLBACK, lx, y, 420);
    y += 30;
    label(L"[即] すぐ反映　[開] 出力を開き直す（一瞬途切れる）　[再] 再生ソフトを再初期化", lx, y, 425);
    y += 28;
    button(L"適用", ID_APPLY, lx, y, 120);
    button(L"元に戻す", ID_REVERT, lx + 128, y, 100);
    y += 40;

    group(L"プリセット", x0 - 4, y, 440, 92);
    y += 24;
    combo(ID_PRESET, lx, y, 200); button(L"呼び出し", ID_PLOAD, lx + 206, y, 90); button(L"削除", ID_PDEL, lx + 302, y, 80);
    y += 32;
    edit(ID_PRESETNAME, lx, y, 200); button(L"今の設定を保存", ID_PSAVE, lx + 206, y, 176);

    // 右側: 計測
    int rx = 460, ry = 10, rw = 560;
    group(L"いま何が起きているか", rx, ry, rw, 420);
    mk(L"STATIC", L"", SS_LEFT, rx + 10, ry + 22, rw - 20, 64, ID_RESETSTATS + 100);
    mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, rx + 10, ry + 92, rw - 20, 16, ID_RESETSTATS + 103);
    SendMessageW(h(ID_RESETSTATS + 103), PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    mk(L"STATIC", L"", SS_LEFT, rx + 10, ry + 114, rw - 20, 96, ID_RESETSTATS + 101);
    g_histRect = { S(rx + 10), S(ry + 214), S(rx + rw - 10), S(ry + 344) };
    mk(L"STATIC", L"", SS_LEFT, rx + 10, ry + 350, rw - 20, 36, ID_RESETSTATS + 102);
    button(L"ベンダーのコントロールパネル（ASIO 中継時）", ID_VENDOR, rx + 10, ry + 388, 300);

    int ay = ry + 430;
    group(L"ブラインド A/B（ABX）", rx, ay, rw, 238);
    ay += 24;
    label(L"A", rx + 10, ay, 16); combo(ID_ABX_A, rx + 28, ay, 170);
    label(L"B", rx + 210, ay, 16); combo(ID_ABX_B, rx + 228, ay, 170);
    label(L"回数", rx + 410, ay, 34); edit(ID_ABX_N, rx + 446, ay, 50); SetWindowTextW(h(ID_ABX_N), L"10");
    ay += 32;
    button(L"開始", ID_ABX_START, rx + 10, ay, 90);
    button(L"中止", ID_ABX_STOP, rx + 106, ay, 70);
    button(L"A を聴く", ID_ABX_PLAYA, rx + 200, ay, 100);
    button(L"B を聴く", ID_ABX_PLAYB, rx + 306, ay, 100);
    button(L"X を聴く", ID_ABX_PLAYX, rx + 412, ay, 100);
    ay += 34;
    button(L"X は A", ID_ABX_ANSA, rx + 200, ay, 150);
    button(L"X は B", ID_ABX_ANSB, rx + 362, ay, 150);
    button(L"CSV 保存", ID_ABX_SAVE, rx + 10, ay, 100);
    ay += 34;
    mk(L"STATIC", L"", SS_LEFT, rx + 10, ay, rw - 20, 40, ID_ABX_SAVE + 100);

    g_status = mk(L"STATIC", L"", SS_LEFT | SS_SUNKEN, 8, 680, 1012, 22, -1);
}


// ======================= タスクトレイ常駐 =======================
// ウインドウの × / 最小化ではトレイに格納し、クリックで開く。右クリックでプリセット切替・常駐設定・終了。
// 格納中は計測の読み取りを 2 秒に 1 回まで落とし、余計な起床を減らす。
const UINT WM_TRAY = WM_APP + 1;
const UINT ID_TRAY_TIMER = 2;
enum { IDM_OPEN = 1000, IDM_STARTUP, IDM_EXIT, IDM_PRESET0 = 1100 };
UINT g_msgTaskbarCreated = 0;
UINT g_msgShow = 0;
bool g_trayAdded = false;
bool g_trayHintShown = false;
bool g_exiting = false;
HICON g_iconApp = nullptr;
HICON g_iconState[4] = {};   // 0=未使用 1=待機 2=再生中 3=アンダーラン
int g_iconCur = -1;
uint32_t g_trayLastUnder = 0;
DWORD g_trayUnderTick = 0;
std::vector<std::wstring> g_trayPresets;
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunName = L"OtonumaASIOPanel";

// 32bit ARGB で小さなアイコンを描く (リソースファイルを使わずに済むように)
HICON make_icon(int size, COLORREF dot, bool withDot) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size; bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    uint32_t* px = (uint32_t*)bits;
    auto put = [&](int x, int y, uint32_t argb, double a) {
        if (x < 0 || y < 0 || x >= size || y >= size || a <= 0) return;
        if (a > 1) a = 1;
        uint32_t& d = px[y * size + x];
        double da = ((d >> 24) & 255) / 255.0, sa = a;
        double oa = sa + da * (1 - sa);
        auto ch = [&](int sh) {
            double s = ((argb >> sh) & 255), t = ((d >> sh) & 255);
            return (uint32_t)((s * sa + t * da * (1 - sa)) / (oa > 0 ? oa : 1) + 0.5);   // 乗算しないアルファ
        };
        uint32_t r = ch(16), g = ch(8), b = ch(0);
        d = ((uint32_t)(oa * 255 + 0.5) << 24) | (r << 16) | (g << 8) | b;
    };
    double s = size / 32.0;
    // 角丸の四角 (濃紺)
    double rr = 6 * s;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        double cx = x + 0.5, cy = y + 0.5;
        double dx = std::max({ rr - cx, 0.0, cx - (size - rr) }), dy = std::max({ rr - cy, 0.0, cy - (size - rr) });
        double dist = std::sqrt(dx * dx + dy * dy) - rr;
        double a = 0.5 - dist;
        put(x, y, 0xFF203A5C, a);
    }
    // レベルメーター風の白い縦棒 3 本
    const double hs[3] = { 0.45, 0.75, 0.58 };
    for (int i = 0; i < 3; i++) {
        double x0 = (6 + i * 7) * s, w = 4.5 * s;
        double y1 = size - 6 * s, y0 = y1 - hs[i] * (size - 12 * s);
        for (int y = (int)y0; y <= (int)y1; y++) for (int x = (int)x0; x <= (int)(x0 + w); x++) {
            double ax = std::min(1.0, std::min(x + 1 - x0, x0 + w - x));
            double ay = std::min(1.0, std::min(y + 1 - y0, y1 - y));
            put(x, y, 0xFFFFFFFF, std::min(ax, ay));
        }
    }
    // 状態を表す丸 (右下)
    if (withDot) {
        double r0 = 7.5 * s, cx0 = size - r0 - 0.5 * s, cy0 = size - r0 - 0.5 * s;
        uint32_t c = 0xFF000000 | ((uint32_t)GetRValue(dot) << 16) | ((uint32_t)GetGValue(dot) << 8) | GetBValue(dot);
        for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
            double d = std::sqrt((x + 0.5 - cx0) * (x + 0.5 - cx0) + (y + 0.5 - cy0) * (y + 0.5 - cy0));
            put(x, y, 0xFFFFFFFF, r0 + 0.5 - d);          // 白い縁
            put(x, y, c, r0 - 1.2 * s + 0.5 - d);
        }
    }
    std::vector<uint8_t> zero((size_t)(((size + 15) / 16) * 2 * size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zero.data());
    ICONINFO ii = {}; ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}

void make_icons() {
    int sm = GetSystemMetrics(SM_CXSMICON);
    g_iconApp = make_icon(GetSystemMetrics(SM_CXICON), 0, false);
    g_iconState[0] = make_icon(sm, 0, false);
    g_iconState[1] = make_icon(sm, RGB(150, 150, 150), true);
    g_iconState[2] = make_icon(sm, RGB(40, 190, 90), true);
    g_iconState[3] = make_icon(sm, RGB(235, 120, 30), true);
}

std::wstring tray_tip() {
    if (!g_telOk) return L"OtonumaASIO\n再生ソフトで使われていません";
    const BsTelemetry& t = g_tel;
    std::wstring kind = t.isDsd ? (t.dsdTransport == 2 ? L"DSD→DoP" : L"DSD") : L"PCM";
    std::wstring rate = t.isDsd ? fmt(L"DSD%.0f", t.sampleRate / 44100.0) : fmt(L"%.1fkHz", t.sampleRate / 1000.0);
    std::wstring s = L"OtonumaASIO\n";
    s += t.state == BS_ST_RUNNING ? fmt(L"再生中 %ls %ls → %ls", rate.c_str(), kind.c_str(), bs_out_type_name(t.outResolved >= 0 ? t.outResolved : t.outType))
                                   : std::wstring(state_name(t.state));
    s += fmt(L"\nアンダーラン %u 回", t.underruns);
    if (s.size() > 127) s.resize(127);
    return s;
}

void tray_update(bool readTel) {
    if (!g_trayAdded) return;
    if (readTel) g_telOk = read_telemetry(g_tel);
    int st = 0;
    if (g_telOk) {
        st = g_tel.state == BS_ST_RUNNING ? 2 : 1;
        if (g_tel.underruns != g_trayLastUnder) { g_trayLastUnder = g_tel.underruns; g_trayUnderTick = GetTickCount(); }
        if (st == 2 && g_trayUnderTick && GetTickCount() - g_trayUnderTick < 10000) st = 3;   // 直近 10 秒にアンダーラン
    }
    NOTIFYICONDATAW nd = { sizeof nd };
    nd.hWnd = g_wnd; nd.uID = 1;
    nd.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = tray_tip();
    wcsncpy(nd.szTip, tip.c_str(), 127);
    if (st != g_iconCur) { nd.uFlags |= NIF_ICON; nd.hIcon = g_iconState[st]; g_iconCur = st; }
    Shell_NotifyIconW(NIM_MODIFY, &nd);
}

void tray_add() {
    NOTIFYICONDATAW nd = { sizeof nd };
    nd.hWnd = g_wnd; nd.uID = 1;
    nd.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nd.uCallbackMessage = WM_TRAY;
    nd.hIcon = g_iconState[0];
    wcsncpy(nd.szTip, L"OtonumaASIO", 127);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nd) != FALSE;
    nd.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nd);
    g_iconCur = 0;
    tray_update(true);
}

void tray_remove() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nd = { sizeof nd };
    nd.hWnd = g_wnd; nd.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nd);
    g_trayAdded = false;
}

void tray_balloon(const wchar_t* title, const wchar_t* text) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nd = { sizeof nd };
    nd.hWnd = g_wnd; nd.uID = 1;
    nd.uFlags = NIF_INFO;
    wcsncpy(nd.szInfoTitle, title, 63);
    wcsncpy(nd.szInfo, text, 255);
    nd.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &nd);
}

bool startup_enabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kRunName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

void startup_set(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = std::wstring(L"\"") + exe + L"\" /tray";
        RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunName);
    }
    RegCloseKey(k);
}

void panel_show() {
    ShowWindow(g_wnd, IsIconic(g_wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_wnd);
    KillTimer(g_wnd, ID_TRAY_TIMER);
    SetTimer(g_wnd, ID_TIMER_TICK, 200, nullptr);
    refresh_presets();
    update_telemetry();
}

void panel_hide() {
    ShowWindow(g_wnd, SW_HIDE);
    KillTimer(g_wnd, ID_TIMER_TICK);
    SetTimer(g_wnd, ID_TRAY_TIMER, 2000, nullptr);
    if (!g_trayHintShown) {
        g_trayHintShown = true;
        tray_balloon(L"OtonumaASIO", L"タスクトレイに常駐しています。クリックで開き、右クリックで終了できます。");
    }
}

void tray_menu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"コントロールパネルを開く");
    SetMenuDefaultItem(m, IDM_OPEN, FALSE);
    HMENU pm = CreatePopupMenu();
    g_trayPresets.clear();
    bs_load_document(g_doc);
    const Json* p = g_doc.get("presets");
    if (p && p->t == Json::Obj) for (auto& kv : p->o) g_trayPresets.push_back(bs_from_utf8(kv.first));
    for (size_t i = 0; i < g_trayPresets.size() && i < 50; i++)
        AppendMenuW(pm, MF_STRING, IDM_PRESET0 + (UINT)i, g_trayPresets[i].c_str());
    if (g_trayPresets.empty()) AppendMenuW(pm, MF_STRING | MF_GRAYED, 0, L"（プリセットがありません）");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pm, L"プリセットを適用");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (startup_enabled() ? MF_CHECKED : 0), IDM_STARTUP, L"Windows の起動時に常駐する");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"終了");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == IDM_OPEN) panel_show();
    else if (cmd == IDM_STARTUP) startup_set(!startup_enabled());
    else if (cmd == IDM_EXIT) { g_exiting = true; DestroyWindow(g_wnd); }
    else if (cmd >= IDM_PRESET0 && cmd < IDM_PRESET0 + g_trayPresets.size()) {
        BsSettings s;
        const std::wstring& name = g_trayPresets[cmd - IDM_PRESET0];
        if (preset_get(name, s)) {
            int cls = apply_settings(s, false, true);
            settings_to_ui(s);
            std::wstring how = cls < 0 ? L"（再生ソフトで使われていないため、次回から反映）"
                             : (cls == BS_APPLY_NOW ? L"すぐ反映しました" : (cls == BS_APPLY_REOPEN ? L"出力を開き直して反映しました" : L"再生ソフトに再初期化を要求しました"));
            tray_balloon((L"プリセット「" + name + L"」").c_str(), how.c_str());
        }
    }
}

LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = w;
        build_ui();
        bs_load_settings(g_cur);
        settings_to_ui(g_cur);
        refresh_presets();
        abx_ui();
        SetTimer(w, ID_TIMER_TICK, 200, nullptr);
        update_telemetry();
        status(fmt(L"OtonumaASIO %ls　設定ファイル: %ls", BSL_VERSION_STR_W, bs_settings_path().c_str()));
        SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)g_iconApp);
        SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)g_iconState[0]);
        tray_add();
        return 0;
    case WM_TIMER:
        if (wp == ID_TRAY_TIMER) tray_update(true);
        else { update_telemetry(); tray_update(false); }
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONDBLCLK:
            if (IsWindowVisible(w) && !IsIconic(w)) SetForegroundWindow(w); else panel_show();
            break;
        case WM_CONTEXTMENU:
            tray_menu();
            break;
        }
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && g_trayAdded) { panel_hide(); return 0; }
        break;
    case WM_CLOSE:
        if (g_trayAdded && !g_exiting) { panel_hide(); return 0; }   // × はトレイに格納
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
        paint_hist(dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case ID_OUTTYPE:
            if (code == CBN_SELCHANGE) {
                BsSettings s = g_cur; s.outType = cb_sel(ID_OUTTYPE);
                fill_devices(s.outType, bs_settings_device(s));
            }
            break;
        case ID_DEVICE:
            if (code == CBN_SELCHANGE) {
                // そのデバイスで最後に使った設定を呼び出す
                BsSettings s = ui_to_settings();
                bs_load_document(g_doc);
                const Json* devs = g_doc.get("devices");
                const Json* j = devs ? devs->get(bs_to_utf8(dev_key(s.outType, bs_settings_device(s)))) : nullptr;
                if (j) {
                    BsSettings d = s; bs_settings_from_json(*j, d);
                    d.outType = s.outType; d.devAsio = s.devAsio; d.devKs = s.devKs; d.devWasapi = s.devWasapi;
                    BsSettings keep = g_cur; g_cur = d; settings_to_ui(d); g_cur = keep;
                    status(L"このデバイスで最後に使った設定を読み込みました（「適用」で反映）");
                }
            }
            break;
        case ID_REFRESH: g_devType = -1; fill_devices(cb_sel(ID_OUTTYPE), L""); break;
        case ID_APPLY: apply_settings(ui_to_settings(), false, true); settings_to_ui(g_cur); break;
        case ID_REVERT: bs_load_settings(g_cur); settings_to_ui(g_cur); status(L"保存済みの設定に戻しました"); break;
        case ID_PSAVE: {
            std::wstring name = get_text(ID_PRESETNAME);
            if (name.empty()) name = cb_text(ID_PRESET);
            if (name.empty()) { status(L"プリセット名を入れてください"); break; }
            bs_load_document(g_doc);
            if (g_doc.t != Json::Obj) g_doc = Json::object();
            Json* p = g_doc.get("presets");
            if (!p || p->t != Json::Obj) p = &g_doc.set("presets", Json::object());
            p->set(bs_to_utf8(name), bs_settings_to_json(ui_to_settings()));
            bs_save_document(g_doc);
            refresh_presets();
            status(L"プリセット「" + name + L"」を保存しました");
            break;
        }
        case ID_PLOAD: {
            BsSettings s;
            std::wstring name = cb_text(ID_PRESET);
            if (preset_get(name, s)) { apply_settings(s, false, true); settings_to_ui(s); }
            break;
        }
        case ID_PDEL: {
            std::wstring name = cb_text(ID_PRESET);
            if (name.empty()) break;
            bs_load_document(g_doc);
            Json* p = g_doc.get("presets");
            if (p) p->erase(bs_to_utf8(name));
            bs_save_document(g_doc);
            refresh_presets();
            status(L"プリセット「" + name + L"」を削除しました");
            break;
        }
        case ID_VENDOR: { std::string r; if (!pipe_call("VENDORPANEL", r)) status(L"ドライバに接続できません"); break; }
        case ID_ABX_START: abx_start(); break;
        case ID_ABX_STOP: if (g_abx.active) abx_finish(); break;
        case ID_ABX_PLAYA: if (g_abx.active) abx_switch(g_abx.a); break;
        case ID_ABX_PLAYB: if (g_abx.active) abx_switch(g_abx.b); break;
        case ID_ABX_PLAYX: if (g_abx.active) abx_switch(g_abx.x ? g_abx.b : g_abx.a); break;
        case ID_ABX_ANSA: abx_answer(0); break;
        case ID_ABX_ANSB: abx_answer(1); break;
        case ID_ABX_SAVE: abx_save_csv(); break;
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_abx.active) abx_finish();
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) { g_trayAdded = false; tray_add(); return 0; }   // エクスプローラー再起動
    if (msg == g_msgShow && g_msgShow) { panel_show(); return 0; }
    return DefWindowProcW(w, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int show) {
    g_inst = inst;
    bool startInTray = cmdLine && (wcsstr(cmdLine, L"/tray") || wcsstr(cmdLine, L"-tray"));
    g_msgShow = RegisterWindowMessageW(L"OtonumaASIO.Panel.Show");
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // 多重起動しない (既に常駐していればそのウインドウを開く)
    HANDLE mtx = CreateMutexW(nullptr, FALSE, L"Local\\OtonumaASIO.Panel");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND ex = FindWindowW(L"OtonumaASIOPanel", nullptr);
        if (ex && !startInTray) {
            DWORD pid = 0; GetWindowThreadProcessId(ex, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(ex, g_msgShow, 0, 0);
        }
        return 0;
    }
    SetProcessDPIAware();
    HDC sdc = GetDC(nullptr);
    g_scale = GetDeviceCaps(sdc, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, sdc);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    NONCLIENTMETRICSW ncm = { sizeof ncm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    g_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    g_fontBold = CreateFontIndirectW(&lf);
    make_icons();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OtonumaASIOPanel";
    wc.hIcon = g_iconApp ? g_iconApp : LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r = { 0, 0, S(1030), S(710) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    HWND w = CreateWindowW(L"OtonumaASIOPanel", L"OtonumaASIO コントロールパネル",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (startInTray && g_trayAdded) {
        g_trayHintShown = true;
        KillTimer(w, ID_TIMER_TICK);
        SetTimer(w, ID_TRAY_TIMER, 2000, nullptr);
    } else {
        ShowWindow(w, show);
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
