// otonumareg.cpp - OtonumaASIO の ASIO 登録・解除 (regsvr32 の代わり)
//   otonumareg install     … 同じフォルダの OtonumaASIO64.dll / OtonumaASIO32.dll を登録
//   otonumareg uninstall   … 登録を消す
//   otonumareg status      … 登録状態を表示
// 管理者権限がなければ自動で昇格して実行し直す。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../src/common/bs_common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <string>

static bool is_64bit_os() {
#ifdef _WIN64
    return true;
#else
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    return wow != FALSE;
#endif
}

static LONG set_str(REGSAM view, const std::wstring& path, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE | view, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

static void del_key(REGSAM view, const std::wstring& path) {
    typedef LONG (WINAPI *PFN)(HKEY, LPCWSTR, REGSAM, DWORD);
    static PFN f = (PFN)GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegDeleteKeyExW");
    if (f) f(HKEY_LOCAL_MACHINE, path.c_str(), view, 0);
    else RegDeleteKeyW(HKEY_LOCAL_MACHINE, path.c_str());
}

static std::wstring read_str(REGSAM view, const std::wstring& path, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return L"";
    wchar_t v[MAX_PATH * 2] = {0}; DWORD sz = sizeof(v) - 2;
    RegQueryValueExW(k, name, nullptr, nullptr, (BYTE*)v, &sz);
    RegCloseKey(k);
    return v;
}

static const std::wstring kClsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + BSL_CLSID_STR_W;
static const std::wstring kAsioKey = std::wstring(L"SOFTWARE\\ASIO\\") + BSL_DRIVER_NAME_W;

static bool reg_one(REGSAM view, const std::wstring& dll) {
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    bool ok = set_str(view, kClsidKey, nullptr, BSL_DRIVER_NAME_W) == ERROR_SUCCESS;
    ok &= set_str(view, kClsidKey + L"\\InprocServer32", nullptr, dll) == ERROR_SUCCESS;
    ok &= set_str(view, kClsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment") == ERROR_SUCCESS;
    ok &= set_str(view, kAsioKey, L"CLSID", BSL_CLSID_STR_W) == ERROR_SUCCESS;
    ok &= set_str(view, kAsioKey, L"Description", BSL_DRIVER_NAME_W) == ERROR_SUCCESS;
    return ok;
}

static void unreg_one(REGSAM view) {
    del_key(view, kClsidKey + L"\\InprocServer32");
    del_key(view, kClsidKey);
    del_key(view, kAsioKey);
}

// 終了時にコンソールの文字コードを元に戻す (呼び出し元のバッチの表示が化けないように)
struct ConsoleCpGuard {
    UINT cp;
    ConsoleCpGuard() : cp(GetConsoleOutputCP()) { SetConsoleOutputCP(CP_UTF8); }
    ~ConsoleCpGuard() { fflush(stdout); if (cp) SetConsoleOutputCP(cp); }
};

int wmain(int argc, wchar_t** argv) {
    ConsoleCpGuard cpGuard;
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd != L"install" && cmd != L"uninstall" && cmd != L"status") {
        printf("使い方: otonumareg install | uninstall | status\n");
        return 2;
    }
    if (cmd != L"status" && !IsUserAnAdmin()) {
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring params = cmd + L" --elevated";
        SHELLEXECUTEINFOW sei = { sizeof sei };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas"; sei.lpFile = self; sei.lpParameters = params.c_str(); sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) { printf("管理者権限で実行できませんでした\n"); return 1; }
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code); CloseHandle(sei.hProcess);
        printf(code == 0 ? "完了しました\n" : "失敗しました (コード %lu)\n", code);
        return (int)code;
    }
    bool elevated = argc > 2 && wcscmp(argv[2], L"--elevated") == 0;

    wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* b = wcsrchr(dir, L'\\'); if (b) *(b + 1) = 0;
    std::wstring dll64 = std::wstring(dir) + L"OtonumaASIO64.dll";
    std::wstring dll32 = std::wstring(dir) + L"OtonumaASIO32.dll";
    bool os64 = is_64bit_os();
    REGSAM v64 = os64 ? KEY_WOW64_64KEY : 0, v32 = os64 ? KEY_WOW64_32KEY : 0;

    int rc = 0;
    if (cmd == L"install") {
        if (os64) {
            if (reg_one(v64, dll64)) printf("64bit 版を登録しました: %ls\n", dll64.c_str());
            else { printf("64bit 版を登録できません (%ls)\n", dll64.c_str()); rc = 1; }
        }
        if (reg_one(v32, dll32)) printf("32bit 版を登録しました: %ls\n", dll32.c_str());
        else printf("32bit 版は登録しませんでした (ファイルなし: %ls)\n", dll32.c_str());
    } else if (cmd == L"uninstall") {
        if (os64) unreg_one(v64);
        unreg_one(v32);
        printf("登録を解除しました\n");
    } else {
        if (os64) {
            std::wstring p = read_str(v64, kClsidKey + L"\\InprocServer32", nullptr);
            printf("64bit: %ls\n", p.empty() ? L"(未登録)" : p.c_str());
        }
        std::wstring p = read_str(v32, kClsidKey + L"\\InprocServer32", nullptr);
        printf("32bit: %ls\n", p.empty() ? L"(未登録)" : p.c_str());
    }
    if (elevated) { printf("\nEnter で閉じます"); getchar(); }
    return rc;
}
