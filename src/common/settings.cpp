#include "settings.h"
#include <shlobj.h>
#include <cstdlib>
#include <cmath>
#include <cstdio>

std::string bs_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring bs_from_utf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring bs_settings_path() {
    wchar_t buf[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
        if (!n || n >= MAX_PATH) return L"local.json";
    }
    std::wstring dir = std::wstring(buf) + L"\\OtonumaASIO";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\settings.json";
    // 旧名 (BareStream Local) の設定があれば、初回だけ引き継ぐ
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring old = std::wstring(buf) + L"\\BareStream\\local.json";
        if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) CopyFileW(old.c_str(), path.c_str(), TRUE);
    }
    return path;
}

bool bs_load_document(Json& doc) {
    doc = Json::object();
    HANDLE h = CreateFileW(bs_settings_path().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string text;
    char buf[8192]; DWORD rd;
    while (ReadFile(h, buf, sizeof buf, &rd, nullptr) && rd) text.append(buf, rd);
    CloseHandle(h);
    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok || j.t != Json::Obj) return false;
    doc = j;
    return true;
}

bool bs_save_document(const Json& doc) {
    std::wstring path = bs_settings_path();
    std::wstring tmp = path + L".tmp";
    std::string text = doc.dump(2);
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &wr, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

Json bs_settings_to_json(const BsSettings& s) {
    Json j = Json::object();
    j.set("out_type", Json::number(s.outType));
    j.set("dev_wasapi", Json::string(bs_to_utf8(s.devWasapi)));
    j.set("dev_ks", Json::string(bs_to_utf8(s.devKs)));
    j.set("dev_asio", Json::string(bs_to_utf8(s.devAsio)));
    j.set("host_buffer", Json::number(s.hostBuffer));
    j.set("pacer_hz", Json::number(s.pacerHz));
    j.set("prefill_sec", Json::number(s.prefillSec));
    j.set("pump_mode", Json::number(s.pumpMode));
    j.set("drive_mode", Json::number(s.driveMode));
    j.set("ks_queue", Json::number(s.ksQueue));
    j.set("rt_lead_ms", Json::number(s.rtLeadMs));
    j.set("out_priority", Json::number(s.outPriority));
    j.set("cpu_core", Json::number(s.cpuCore));
    j.set("timer_res", Json::number(s.timerRes));
    j.set("mem_lock", Json::number(s.memLock));
    j.set("dsd_mode", Json::number(s.dsdMode));
    j.set("dop_max", Json::number(s.dopMax));
    j.set("drain_on_stop", Json::number(s.drainOnStop));
    j.set("start_silence_ms", Json::number(s.startSilenceMs));
    j.set("ks_fallback", Json::number(s.ksFallback));
    j.set("spin_us", Json::number(s.spinUs));
    j.set("asio_first_ch", Json::number(s.asioFirstCh));
    return j;
}

void bs_settings_from_json(const Json& j, BsSettings& s) {
    if (j.t != Json::Obj) return;
    for (auto& kv : j.o) {
        const Json& v = kv.second;
        std::string val;
        if (v.t == Json::Str) val = v.s;
        else if (v.t == Json::Num || v.t == Json::Bool) {
            char b[64]; snprintf(b, sizeof b, "%.17g", v.num()); val = b;
        } else continue;
        bs_settings_set(s, kv.first, val);
    }
    bs_settings_clamp(s);
}

bool bs_load_settings(BsSettings& s) {
    s = BsSettings();
    Json doc;
    if (!bs_load_document(doc)) return false;
    const Json* cur = doc.get("current");
    if (cur) bs_settings_from_json(*cur, s);
    return cur != nullptr;
}

bool bs_save_settings(const BsSettings& s) {
    Json doc;
    bs_load_document(doc);
    if (doc.t != Json::Obj) doc = Json::object();
    doc.set("current", bs_settings_to_json(s));
    return bs_save_document(doc);
}

template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

void bs_settings_clamp(BsSettings& s) {
    s.outType = clampv(s.outType, 0, (int)BS_OUT_COUNT - 1);
    // ホスト側バッファは 32〜32768 の 2 のべき乗
    int hb = clampv(s.hostBuffer, 32, 32768), p = 32;
    while (p < hb) p <<= 1;
    if (p - hb > hb - p / 2 && p > 32) p >>= 1;
    s.hostBuffer = p;
    if (!(s.pacerHz >= 1.0)) s.pacerHz = 1.0;
    if (s.pacerHz > 1000.0) s.pacerHz = 1000.0;
    if (!(s.prefillSec >= 0.05)) s.prefillSec = 0.05;
    if (s.prefillSec > 2.0) s.prefillSec = 2.0;   // 再生ソフトの曲送り表示とのずれを抑えるため 2 秒まで
    s.pumpMode = clampv(s.pumpMode, 0, 1);
    s.driveMode = clampv(s.driveMode, 0, 2);
    s.ksQueue = clampv(s.ksQueue, 2, 16);
    s.rtLeadMs = clampv(s.rtLeadMs, 1, 500);
    s.outPriority = clampv(s.outPriority, 0, 2);
    s.cpuCore = clampv(s.cpuCore, -1, 255);
    s.timerRes = clampv(s.timerRes, 0, 2);
    s.memLock = s.memLock ? 1 : 0;
    s.dsdMode = clampv(s.dsdMode, 0, 2);
    s.dopMax = clampv(s.dopMax, 0, 3);
    s.drainOnStop = s.drainOnStop ? 1 : 0;
    s.startSilenceMs = clampv(s.startSilenceMs, 0, 2000);
    s.ksFallback = s.ksFallback ? 1 : 0;
    s.spinUs = clampv(s.spinUs, 0, 2000);
    s.asioFirstCh = clampv(s.asioFirstCh, 0, 254);
}

bool bs_settings_set(BsSettings& s, const std::string& key, const std::string& value) {
    double d = atof(value.c_str());
    int i = (int)std::lround(d);
    if (key == "out_type") s.outType = i;
    else if (key == "dev_wasapi") s.devWasapi = bs_from_utf8(value);
    else if (key == "dev_ks") s.devKs = bs_from_utf8(value);
    else if (key == "dev_asio") s.devAsio = bs_from_utf8(value);
    else if (key == "host_buffer") s.hostBuffer = i;
    else if (key == "pacer_hz") s.pacerHz = d;
    else if (key == "prefill_sec") s.prefillSec = d;
    else if (key == "pump_mode") s.pumpMode = i;
    else if (key == "drive_mode") s.driveMode = i;
    else if (key == "ks_queue") s.ksQueue = i;
    else if (key == "rt_lead_ms") s.rtLeadMs = i;
    else if (key == "out_priority") s.outPriority = i;
    else if (key == "cpu_core") s.cpuCore = i;
    else if (key == "timer_res") s.timerRes = i;
    else if (key == "mem_lock") s.memLock = i;
    else if (key == "dsd_mode") s.dsdMode = i;
    else if (key == "dop_max") s.dopMax = i;
    else if (key == "drain_on_stop") s.drainOnStop = i;
    else if (key == "start_silence_ms") s.startSilenceMs = i;
    else if (key == "ks_fallback") s.ksFallback = i;
    else if (key == "spin_us") s.spinUs = i;
    else if (key == "asio_first_ch") s.asioFirstCh = i;
    else return false;
    return true;
}

std::wstring bs_settings_device(const BsSettings& s) {
    switch (s.outType) {
    case BS_OUT_ASIO: return s.devAsio;
    case BS_OUT_WASAPI: return s.devWasapi;
    case BS_OUT_KS_AUTO: case BS_OUT_KS_PACKET: case BS_OUT_KS_RT: return s.devKs;
    default: return std::wstring();
    }
}

int bs_settings_diff_class(const BsSettings& a, const BsSettings& b) {
    int c = BS_APPLY_NOW;
    auto up = [&](int k) { if (k > c) c = k; };
    if (a.outType != b.outType || bs_settings_device(a) != bs_settings_device(b) ||
        a.hostBuffer != b.hostBuffer || a.dsdMode != b.dsdMode || a.dopMax != b.dopMax || a.asioFirstCh != b.asioFirstCh)
        up(BS_APPLY_RESET);
    if (a.pacerHz != b.pacerHz || a.driveMode != b.driveMode || a.ksQueue != b.ksQueue ||
        a.memLock != b.memLock || a.spinUs != b.spinUs || a.ksFallback != b.ksFallback)
        up(BS_APPLY_REOPEN);
    return c;
}
