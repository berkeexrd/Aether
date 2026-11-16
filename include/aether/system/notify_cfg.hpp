#pragma once
// Settings of Aether's own notification system live in %APPDATA%\Aether\notify.cfg (key=value lines, UTF-8). The bar process (which draws the
// toasts) and the Settings Center (which edits them) are different programs, so they talk through this small file, like the desktop does.
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>
#include "aether/system/desktop_cfg.hpp"

namespace aether::system::notifycfg {

inline std::wstring Path() {
    wchar_t b[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::wstring(b) + L"\\Aether\\notify.cfg";
}

struct Settings {
    bool enabled{true};     // Aether shows the notifications (and hides Windows' own pop-ups)
    bool dnd{false};        // do not disturb: nothing pops up (the centre still collects everything)
    int position{0};        // 0 = top right, 1 = bottom right
    int seconds{7};         // how long a toast stays
    bool island{false};     // the Dynamic Island announces them as well
    long long test{0};      // bumped by the Settings Center's "send a test"
};

inline Settings Load() {
    Settings s;
    for (const std::wstring& line : desktopcfg::Lines(desktopcfg::ReadAll(Path()))) {
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        const long long iv = _wtoi64(v.c_str());
        if (k == L"enabled") s.enabled = iv != 0;
        else if (k == L"dnd") s.dnd = iv != 0;
        else if (k == L"pos") s.position = static_cast<int>(std::clamp<long long>(iv, 0, 1));
        else if (k == L"secs") s.seconds = static_cast<int>(std::clamp<long long>(iv, 3, 30));
        else if (k == L"island") s.island = iv != 0;
        else if (k == L"test") s.test = iv;
    }
    return s;
}

inline void Set(const std::wstring& key, const std::wstring& value) {
    const std::wstring p = Path();
    if (p.empty()) return;
    std::vector<std::wstring> lines = desktopcfg::Lines(desktopcfg::ReadAll(p));
    bool found = false;
    for (auto& line : lines)
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == L'=') { line = key + L"=" + value; found = true; break; }
    if (!found) lines.push_back(key + L"=" + value);
    std::wstring w; for (const auto& l : lines) w += l + L"\n";
    const int bl = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string b(static_cast<size_t>(std::max(0, bl)), '\0');
    if (bl > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), b.data(), bl, nullptr, nullptr);
    const std::wstring dir = p.substr(0, p.find_last_of(L'\\'));
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &wr, nullptr); CloseHandle(h);
}
inline void SetInt(const std::wstring& key, long long v) { Set(key, std::to_wstring(v)); }

// Last-write stamp, to notice that the file changed without reading it.
inline unsigned long long Stamp() {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    const std::wstring p = Path();
    if (p.empty() || !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

// For the Dynamic Island (another process): should it pop the toast out as well? Re-read at most every two seconds.
inline bool IslandAlso() {
    static ULONGLONG at = 0; static Settings s;
    const ULONGLONG now = GetTickCount64();
    if (now - at > 2000 || at == 0) { at = now; s = Load(); }
    if (s.dnd) return false;
    return !s.enabled || s.island;
}

} // namespace aether::system::notifycfg
