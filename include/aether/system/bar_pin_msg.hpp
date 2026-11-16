#pragma once
// Pinning by drag & drop. The Desktop (a normal process) starts an OLE drag when icons leave its window, but the bar runs elevated, so Windows
// does not let that drag reach a drop target in the bar. The two processes therefore talk with plain window messages instead:
//   hover:  PostMessage(bar, MsgHover(), screenX, screenY)   while the drag is over a bar (the bar shows where the app would land)
//   drop:   WM_COPYDATA(bar, kDropData, "<screenX>\n<path>\n<path>...") once the button is released over a bar
// The bar lets these through UIPI (ChangeWindowMessageFilterEx) and pins the paths.
#include <windows.h>
#include <string>
#include <vector>

namespace aether::system::barpin {

constexpr ULONG_PTR kDropData = 0x41455450;          // 'AETP'
constexpr wchar_t kBarClass[] = L"AetherBarClass";

inline UINT MsgHover() { static const UINT m = RegisterWindowMessageW(L"Aether.BarPinHover"); return m; }

// The bar window under a screen point (or null).
inline HWND BarAt(POINT pt) {
    HWND h = WindowFromPoint(pt);
    if (!h) return nullptr;
    h = GetAncestor(h, GA_ROOT);
    wchar_t cls[32] = {};
    if (!h || !GetClassNameW(h, cls, 32) || wcscmp(cls, kBarClass) != 0) return nullptr;
    return h;
}

inline void PostHover(HWND bar, POINT pt) { if (bar) PostMessageW(bar, MsgHover(), static_cast<WPARAM>(pt.x), static_cast<LPARAM>(pt.y)); }

inline bool SendDrop(HWND bar, POINT pt, const std::vector<std::wstring>& paths) {
    if (!bar || paths.empty()) return false;
    std::wstring blob = std::to_wstring(pt.x) + L"\n";
    for (const auto& p : paths) blob += p + L"\n";
    COPYDATASTRUCT cds{};
    cds.dwData = kDropData;
    cds.cbData = static_cast<DWORD>((blob.size() + 1) * sizeof(wchar_t));
    cds.lpData = blob.data();
    DWORD_PTR res = 0;
    return SendMessageTimeoutW(bar, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 2000, &res) != 0;
}

} // namespace aether::system::barpin
