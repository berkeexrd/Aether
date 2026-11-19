#include "aether/bar/tray_host.hpp"
#include <shellapi.h>
#include <algorithm>
#include <cstring>

namespace aether::bar {

namespace {
// Wire format of the WM_COPYDATA payload apps send through Shell_NotifyIcon (32-bit handles on every bitness).
struct WireNotifyIcon {
    DWORD cbSize, hWnd, uID, uFlags, uCallbackMessage, hIcon;
    WCHAR szTip[128];
    DWORD dwState, dwStateMask;
    WCHAR szInfo[256];
    DWORD uTimeoutOrVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
struct WireTrayData {
    DWORD signature;     // 0x34753423
    DWORD message;       // NIM_ADD / MODIFY / DELETE / SETFOCUS / SETVERSION
    WireNotifyIcon nid;
};
template <class T> T* H(DWORD v) { return reinterpret_cast<T*>(static_cast<LONG_PTR>(static_cast<LONG>(v))); }
} // namespace

bool TrayHost::Create() {
    if (m_hwnd) return true;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Shell_TrayWnd";
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"Shell_TrayWnd", L"",
                             WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;
    ChangeWindowMessageFilterEx(m_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);   // we run elevated (UIPI)
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Apps re-register their icons when they see "TaskbarCreated". One broadcast is not enough: apps that are still starting up, are busy
    // or re-create their message window later miss it, and their icon would never show. So it is repeated a few times after start.
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    m_stage = 0;
    SetTimer(m_hwnd, 1, 1500, nullptr);
    return true;
}

void TrayHost::Rebroadcast() {
    if (!m_hwnd) return;
    // Not to Explorer itself: its own taskbar answered every "TaskbarCreated" by showing itself again, which is what made the hidden Windows
    // taskbar flash up from the bottom once a minute. Everything else still gets it.
    DWORD shellPid = 0;
    if (HWND sh = GetShellWindow()) GetWindowThreadProcessId(sh, &shellPid);
    struct Ctx { UINT msg; DWORD shellPid; } c{RegisterWindowMessageW(L"TaskbarCreated"), shellPid};
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid && (pid == c->shellPid || pid == GetCurrentProcessId())) return TRUE;
        PostMessageW(h, c->msg, 0, 0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    ++m_stage;
    // 1.5 s, +3.5 s, +10 s, +20 s after start, then every 60 s for good measure (cheap: apps that already have their icon just update it)
    static const UINT kDelays[] = {3500, 10000, 20000};
    SetTimer(m_hwnd, 1, m_stage <= 3 ? kDelays[m_stage - 1] : 60000u, nullptr);
}

void TrayHost::Destroy() {
    if (!m_hwnd) return;
    for (auto& i : m_icons) if (i.icon) DestroyIcon(i.icon);
    m_icons.clear();
    KillTimer(m_hwnd, 1);
    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);   // hand icons back to Explorer
}

LRESULT CALLBACK TrayHost::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_TIMER && wp == 1 && self) { self->Rebroadcast(); return 0; }
    if (msg == WM_COPYDATA && self) {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cds && cds->dwData == 1 && cds->lpData) self->OnData(cds->lpData, cds->cbData);
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

TrayIcon* TrayHost::Find(HWND hwnd, UINT id) {
    for (auto& i : m_icons) if (i.hwnd == hwnd && i.id == id) return &i;
    return nullptr;
}

void TrayHost::OnData(const void* data, DWORD size) {
    WireTrayData d{};
    memcpy(&d, data, std::min<size_t>(size, sizeof(d)));
    if (d.signature != 0x34753423) return;
    const WireNotifyIcon& n = d.nid;
    HWND hwnd = H<HWND__>(n.hWnd);
    const UINT id = n.uID;

    enum { kAdd = 0, kModify = 1, kDelete = 2, kSetFocus = 3, kSetVersion = 4 };
    switch (d.message) {
        case kAdd:
        case kModify: {
            TrayIcon* t = Find(hwnd, id);
            if (!t) {
                // A MODIFY for an icon we never saw (the app registered with Explorer before we took over): treat it as an ADD instead of dropping it
                if (!IsWindow(hwnd)) return;
                m_icons.push_back({}); t = &m_icons.back(); t->hwnd = hwnd; t->id = id;
            }
            if (n.uFlags & NIF_MESSAGE) t->callbackMsg = n.uCallbackMessage;
            if (n.uFlags & NIF_TIP) t->tip = n.szTip;
            if (n.uFlags & NIF_STATE) t->hidden = (n.dwStateMask & NIS_HIDDEN) && (n.dwState & NIS_HIDDEN);
            if (n.uFlags & NIF_ICON) {
                HICON copy = n.hIcon ? CopyIcon(H<HICON__>(n.hIcon)) : nullptr;
                if (!copy && !t->icon && IsWindow(hwnd)) {      // the handle could not be copied: the owner window's own icon is better than an empty slot
                    DWORD_PTR r = 0;
                    HICON wi = nullptr;
                    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 50, &r) && r) wi = reinterpret_cast<HICON>(r);
                    if (!wi) wi = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
                    if (wi) copy = CopyIcon(wi);
                }
                if (copy || !t->icon) {                        // keep the old image when the new one is unusable
                    if (t->icon) DestroyIcon(t->icon);
                    t->icon = copy;
                    t->serial = ++m_serial;
                }
            }
            break;
        }
        case kDelete: {
            auto it = std::remove_if(m_icons.begin(), m_icons.end(), [&](TrayIcon& i) {
                if (i.hwnd == hwnd && i.id == id) { if (i.icon) DestroyIcon(i.icon); return true; }
                return false; });
            m_icons.erase(it, m_icons.end());
            break;
        }
        case kSetVersion:
            if (TrayIcon* t = Find(hwnd, id)) t->version = n.uTimeoutOrVersion;
            break;
        default: break;
    }
    ++m_version;
}

void TrayHost::Prune() {
    const size_t before = m_icons.size();
    m_icons.erase(std::remove_if(m_icons.begin(), m_icons.end(), [](TrayIcon& i) {
        if (!IsWindow(i.hwnd)) { if (i.icon) DestroyIcon(i.icon); return true; }
        return false; }), m_icons.end());
    if (m_icons.size() != before) ++m_version;
}

// Delivers a click the way Explorer does, so the owning app can pop up its own menu / window.
void TrayHost::Click(const TrayIcon& t, bool right, bool dbl) {
    if (!IsWindow(t.hwnd) || !t.callbackMsg) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(t.hwnd, &pid);
    AllowSetForegroundWindow(pid);
    POINT pt; GetCursorPos(&pt);
    const bool v4 = t.version >= 4;
    const WPARAM wpV4 = MAKEWPARAM(pt.x, pt.y);
    auto send = [&](UINT event) {
        if (v4) PostMessageW(t.hwnd, t.callbackMsg, wpV4, MAKELPARAM(event, t.id));
        else    PostMessageW(t.hwnd, t.callbackMsg, t.id, event);
    };
    if (!right) {
        if (dbl) { send(WM_LBUTTONDBLCLK); send(WM_LBUTTONUP); return; }
        send(WM_LBUTTONDOWN); send(WM_LBUTTONUP);
        if (t.version >= 3) send(NIN_SELECT);
    } else {
        send(WM_RBUTTONDOWN); send(WM_RBUTTONUP);
        if (v4) send(WM_CONTEXTMENU);
    }
}

} // namespace aether::bar
