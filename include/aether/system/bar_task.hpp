// The "AetherBar" logon task: AetherBar.exe needs administrator rights, so Windows starts it through a highest-privilege scheduled task
// (no UAC prompt at logon). The task stores the full path of the exe it was created for. When the program moves (reinstall, another
// build folder) the task keeps starting the OLD file, or nothing at all. These helpers let the shell notice that, and let the
// (elevated) bar repair the task by itself.
#pragma once
#include <windows.h>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace aether::system::bartask {

inline bool ReadRegValue(const wchar_t* subKey, const wchar_t* name, DWORD type, std::vector<BYTE>& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    DWORD t = 0, sz = 0;
    bool ok = RegQueryValueExW(k, name, nullptr, &t, nullptr, &sz) == ERROR_SUCCESS && t == type && sz > 0;
    if (ok) { out.resize(sz); ok = RegQueryValueExW(k, name, nullptr, &t, out.data(), &sz) == ERROR_SUCCESS; out.resize(ok ? sz : 0); }
    RegCloseKey(k);
    return ok;
}

inline const wchar_t* TreeKey() { return L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\AetherBar"; }

inline bool Exists() { HKEY k = nullptr; if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TreeKey(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false; RegCloseKey(k); return true; }

// Does the task start `exe`? Reads the task cache (readable without admin rights): the action's command line is stored as UTF-16 inside the
// "Actions" blob. true also when it cannot be read, so an unreadable cache never causes a needless repair / UAC prompt.
inline bool RunsExe(const std::wstring& exe) {
    std::vector<BYTE> id;
    if (!ReadRegValue(TreeKey(), L"Id", REG_SZ, id)) return true;
    id.push_back(0); id.push_back(0);                          // the registry does not promise a terminating NUL
    const std::wstring guid(reinterpret_cast<const wchar_t*>(id.data()));
    std::vector<BYTE> blob;
    if (!ReadRegValue((std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tasks\\") + guid).c_str(), L"Actions", REG_BINARY, blob)) return true;
    const size_t n = exe.size();
    if (!n || blob.size() < n * 2) return true;
    for (size_t i = 0; i + n * 2 <= blob.size(); ++i) {        // the strings are not aligned inside the blob: try every byte offset
        size_t j = 0;
        for (; j < n; ++j) {
            wchar_t c; memcpy(&c, &blob[i + j * 2], sizeof(c));
            if (towlower(c) != towlower(exe[j])) break;
        }
        if (j == n) return true;
    }
    return false;
}

// schtasks arguments that create (or replace) the task for `exe`; the only definition, used by the settings page and by Repair().
inline std::wstring CreateArgs(const std::wstring& exe) {
    return L"/Create /TN \"AetherBar\" /TR \"\\\"" + exe + L"\\\"\" /SC ONLOGON /RL HIGHEST /F";
}

// Re-creates the task for `exe`. Needs administrator rights (the bar has them), no prompt. Fire and forget.
inline void Repair(const std::wstring& exe) {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tool = std::wstring(sys) + L"\\schtasks.exe";
    std::wstring cl = L"\"" + tool + L"\" " + CreateArgs(exe);
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (CreateProcessW(tool.c_str(), cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

} // namespace aether::system::bartask
