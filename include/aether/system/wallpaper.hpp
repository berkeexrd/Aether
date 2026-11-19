#pragma once
// Picks a bundled wallpaper that matches the active Aether theme: purple artwork for purple-toned themes,
// the black metallic artwork for everything dark/neutral. The user's own wallpaper path is remembered once so it can be restored.
#include <windows.h>
#include <tlhelp32.h>
#include <string>

namespace aether::system::wallpaper {

inline bool IsPurpleTheme(int i) { return i == 0 || i == 7; }   // Violet, Lavender

// One bundled wallpaper per theme (index = render::glass theme index):
// 0 Violet, 1 OLED Black, 2 Clear Glass, 3 Ocean, 4 Emerald, 5 Rose, 6 Classic, 7 Lavender, 8 Sunset.
inline const wchar_t* FileFor(int i) {
    switch (i) {
        case 0: return L"violet_neon_burst.jpg";
        case 1: return L"oled_mountains.jpg";
        case 2: return L"glass_planet.jpg";
        case 3: return L"ocean_horizon.jpg";
        case 4: return L"emerald_forest.jpg";
        case 5: return L"rose_acrylic.jpg";
        case 6: return L"classic_snow_peaks.jpg";
        case 7: return L"lavender_mushroom.jpg";
        case 8: return L"sunset_lake.jpg";
        default: return L"black_metal.jpg";
    }
}

inline std::wstring AppDataDir() {
    wchar_t b[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(b) + L"\\Aether"; CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

inline std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

inline std::wstring Current() {
    wchar_t p[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, p, 0);
    return p;
}

// "WallpaperStyle|TileWallpaper" as they were before Aether changed them, so uninstalling restores the exact original fit mode.
inline std::wstring ReadDesktopValue(const wchar_t* name) {
    HKEY k = nullptr; wchar_t b[16] = {}; DWORD n = sizeof(b) - sizeof(wchar_t);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, nullptr, nullptr, reinterpret_cast<BYTE*>(b), &n) != ERROR_SUCCESS) b[0] = 0;
        RegCloseKey(k);
    }
    return b;
}

// Wallpaper Engine draws its live wallpaper over the Windows wallpaper, so changing only the Windows setting is invisible while it runs.
// When it is running, the picture chosen in Aether is handed to it as well through its command line ("-control openWallpaper -file ...").
namespace we {
// Path of the running Wallpaper Engine's command-line executable ("" when it is not running).
inline std::wstring RunningExe() {
    std::wstring found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        const bool w64 = _wcsicmp(pe.szExeFile, L"wallpaper64.exe") == 0, w32 = _wcsicmp(pe.szExeFile, L"wallpaper32.exe") == 0;
        if (!w64 && !w32) continue;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n)) {
                if (w64) { found = path; CloseHandle(h); break; }       // prefer the 64-bit launcher
                if (found.empty()) found = path;
            }
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return found;
}
// Running is decided by the process name alone: the image path can be unreadable (other session / rights) while the engine is still painting.
inline bool Running() {
    bool yes = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !yes; ok = Process32NextW(snap, &pe))
        yes = _wcsicmp(pe.szExeFile, L"wallpaper64.exe") == 0 || _wcsicmp(pe.szExeFile, L"wallpaper32.exe") == 0;
    CloseHandle(snap);
    return yes;
}

// Shows `path` (an image) in Wallpaper Engine. Returns false when it is not running.
inline bool Apply(const std::wstring& path) {
    static std::wstring last; static ULONGLONG at = 0;
    const ULONGLONG now = GetTickCount64();
    if (path.empty()) return false;
    if (path == last && now - at < 4000) return true;                  // the same request twice in a moment
    const std::wstring exe = RunningExe();
    if (exe.empty()) return false;
    last = path; at = now;
    std::wstring cmd = L"\"" + exe + L"\" -control openWallpaper -file \"" + path + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}
} // namespace we

inline void SetFile(const std::wstring& path) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        const wchar_t* style = L"10"; const wchar_t* tile = L"0";   // "Fill", not tiled
        RegSetValueExW(k, L"WallpaperStyle", 0, REG_SZ, reinterpret_cast<const BYTE*>(style), 6);
        RegSetValueExW(k, L"TileWallpaper", 0, REG_SZ, reinterpret_cast<const BYTE*>(tile), 4);
        RegCloseKey(k);
    }
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(path.c_str()), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    we::Apply(path);                                           // Wallpaper Engine (when running) paints over Windows' wallpaper
}

inline void ApplyForTheme(int theme) {
    if (theme == 9) return;                       // "System accent": the colours follow the user's own wallpaper, so it is left alone
    const std::wstring dir = ExeDir() + L"\\wallpapers\\";
    const std::wstring path = dir + FileFor(theme);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const std::wstring cur = Current();
    if (_wcsicmp(cur.c_str(), path.c_str()) == 0) { we::Apply(path); return; }   // Windows already has it, but Wallpaper Engine may still show another one
    // remember the user's own wallpaper the first time we replace it
    const std::wstring store = AppDataDir() + L"\\orig_wallpaper.txt";
    if (!cur.empty() && cur.find(dir) == std::wstring::npos && GetFileAttributesW(store.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(store.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, cur.c_str(), static_cast<DWORD>(cur.size() * sizeof(wchar_t)), &w, nullptr); CloseHandle(h); }
        const std::wstring style = ReadDesktopValue(L"WallpaperStyle") + L"|" + ReadDesktopValue(L"TileWallpaper");
        HANDLE hs = CreateFileW((AppDataDir() + L"\\orig_wallpaper_style.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hs != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(hs, style.c_str(), static_cast<DWORD>(style.size() * sizeof(wchar_t)), &w, nullptr); CloseHandle(hs); }
    }
    SetFile(path);
}

inline void RestoreOriginal() {
    const std::wstring store = AppDataDir() + L"\\orig_wallpaper.txt";
    HANDLE h = CreateFileW(store.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t b[MAX_PATH] = {}; DWORD r = 0; ReadFile(h, b, (MAX_PATH - 1) * sizeof(wchar_t), &r, nullptr); CloseHandle(h);
    if (!b[0]) return;
    SetFile(b);
    // put the original fit mode (fill / fit / tile ...) back as well
    HANDLE hs = CreateFileW((AppDataDir() + L"\\orig_wallpaper_style.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hs == INVALID_HANDLE_VALUE) return;
    wchar_t sb[32] = {}; DWORD sr = 0; ReadFile(hs, sb, sizeof(sb) - sizeof(wchar_t), &sr, nullptr); CloseHandle(hs);
    std::wstring st = sb; const size_t bar = st.find(L'|');
    if (bar == std::wstring::npos) return;
    const std::wstring style = st.substr(0, bar), tile = st.substr(bar + 1);
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (!style.empty()) RegSetValueExW(k, L"WallpaperStyle", 0, REG_SZ, reinterpret_cast<const BYTE*>(style.c_str()), static_cast<DWORD>((style.size() + 1) * sizeof(wchar_t)));
        if (!tile.empty()) RegSetValueExW(k, L"TileWallpaper", 0, REG_SZ, reinterpret_cast<const BYTE*>(tile.c_str()), static_cast<DWORD>((tile.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, b, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

} // namespace aether::system::wallpaper
