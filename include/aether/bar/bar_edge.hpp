#pragma once
// Which screen edge the dock lives on. Follows the taskbar position set in Windows (Settings > Personalisation > Taskbar, or the
// StuckRects3 registry blob on Windows 10). Top and bottom are supported; a left / right taskbar falls back to the bottom.
#include <windows.h>

namespace aether::bar {

inline bool g_barAtTop = false;     // shared by the bars, flyouts, Start menu and the window preview (one UI thread)

// 0 = left, 1 = top, 2 = right, 3 = bottom (the AppBar edge numbering as Explorer stores it in byte 12).
inline int ReadTaskbarEdge() {
    HKEY k = nullptr;
    int edge = 3;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects3", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        BYTE buf[64] = {};
        DWORD sz = sizeof(buf), type = 0;
        if (RegQueryValueExW(k, L"Settings", nullptr, &type, buf, &sz) == ERROR_SUCCESS && type == REG_BINARY && sz > 12 && buf[12] <= 3) edge = buf[12];
        RegCloseKey(k);
    }
    return edge;
}

} // namespace aether::bar
