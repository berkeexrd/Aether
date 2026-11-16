#pragma once
// "Try Aether" (temporary) mode. Setup puts the program files into %LOCALAPPDATA%\Aether\Trial together with an empty trial.flag and registers nothing
// else: no shortcuts, no Start-with-Windows entry, no shell replacement. Uninstall.exe (a copy of the installer) removes the trial again and puts
// the wallpaper / taskbar back; Windows runs it by itself at the next sign-in (RunOnce) or the user ends the trial from the island's menu.
#include <windows.h>
#include <shellapi.h>
#include <string>

namespace aether::system::trial {

inline std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

inline bool IsTrial() {
    static const bool v = GetFileAttributesW((ExeDir() + L"\\trial.flag").c_str()) != INVALID_FILE_ATTRIBUTES;
    return v;
}

// Ends the trial: runs the uninstaller silently. It stops every Aether process (this one included), so call it last.
inline void End() {
    const std::wstring dir = ExeDir();
    ShellExecuteW(nullptr, L"open", (dir + L"\\Uninstall.exe").c_str(), L"--uninstall /S", dir.c_str(), SW_HIDE);
}

} // namespace aether::system::trial
