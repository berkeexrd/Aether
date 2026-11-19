#include "aether/bar/aether_bar.hpp"
#include "aether/bar/window_preview.hpp"
#include "aether/bar/bar_edge.hpp"
#include "aether/bar/netstate.hpp"
#include "aether/bar/start_menu.hpp"
#include "aether/bar/flyouts.hpp"
#include "aether/bar/ui_common.hpp"
#include "aether/render/context_menu.hpp"
#include "aether/bar/weather.hpp"
#include "aether/bar/notifications.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include "aether/core/monitors.hpp"
#include "aether/system/shell_keys.hpp"
#include "aether/system/app_identity.hpp"
#include "aether/system/icon_pack.hpp"
#include "aether/system/bar_pin_msg.hpp"
#include <atomic>
#include <map>
#include <unordered_map>
#include <winsock2.h>
#include <iphlpapi.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstdlib>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;
namespace i18n = aether::i18n;

namespace {

constexpr float kDockH = 56.0f;
constexpr float kMargin = 14.0f;
constexpr float kReserved = 82.0f;
constexpr float kTipRoom = 90.0f;
constexpr float kItem = 44.0f;
constexpr float kGap = 4.0f;
constexpr float kPad = 8.0f;
constexpr ULONGLONG kIconRefreshMs = 8000;

constexpr wchar_t kGlyphChevron = 0xE70E;
constexpr wchar_t kGlyphBell = 0xEA8F;
constexpr wchar_t kGlyphMute = 0xE74F;
constexpr wchar_t kGlyphVol0 = 0xE992;
constexpr wchar_t kGlyphVol1 = 0xE993;
constexpr wchar_t kGlyphVol2 = 0xE994;
constexpr wchar_t kGlyphVol3 = 0xE995;

inline D2D1_RECT_F RF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
inline D2D1_RECT_F RF(const Rect& r) { return D2D1::RectF(r.Left(), r.Top(), r.Right(), r.Bottom()); }
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// ---- pins shared by every bar --------------------------------------------------------------
struct PinStore {
    std::vector<std::wstring> pins;
    unsigned version{1};
    bool loaded{false};
    static PinStore& I() { static PinStore s; return s; }

    static std::wstring Path() {
        wchar_t appdata[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return L"";
        std::wstring dir = std::wstring(appdata) + L"\\Aether";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\bar_pins.txt";
    }
    void Load() {
        if (loaded) return;
        loaded = true;
        if (FILE* f = _wfopen(Path().c_str(), L"rt, ccs=UTF-8")) {
            wchar_t line[1024];
            while (fgetws(line, ARRAYSIZE(line), f)) {
                std::wstring s = line;
                while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
                if (!s.empty()) pins.push_back(s);
            }
            fclose(f);
        } else {
            wchar_t win[MAX_PATH];
            GetWindowsDirectoryW(win, MAX_PATH);
            pins.push_back(std::wstring(win) + L"\\explorer.exe");
            const wchar_t* edge = L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";
            if (GetFileAttributesW(edge) != INVALID_FILE_ATTRIBUTES) pins.push_back(edge);
        }
    }
    void Save() {
        if (FILE* f = _wfopen(Path().c_str(), L"wt, ccs=UTF-8")) {
            for (const auto& p : pins) fwprintf(f, L"%ls\n", p.c_str());
            fclose(f);
        }
        ++version;
    }
    int IndexOf(const std::wstring& p) const {
        for (size_t i = 0; i < pins.size(); ++i) if (_wcsicmp(pins[i].c_str(), p.c_str()) == 0) return static_cast<int>(i);
        return -1;
    }
};

// A dropped item becomes a pin. A shortcut to a program is pinned as that program (so its windows group with the button);
// a shortcut with arguments, a folder or any other file is pinned as it is and opens through the shell.
std::wstring ResolvePinTarget(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    const std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
    if (_wcsicmp(ext.c_str(), L".lnk") != 0) return path;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return path;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ))) return path;
    link->Resolve(nullptr, SLR_NO_UI | SLR_NOUPDATE | SLR_NOSEARCH | SLR_NOTRACK);
    wchar_t target[MAX_PATH * 2] = {}, args[1024] = {};
    link->GetPath(target, ARRAYSIZE(target), nullptr, 0);
    link->GetArguments(args, ARRAYSIZE(args));
    const std::wstring t = target;
    const bool exe = t.size() > 4 && _wcsicmp(t.c_str() + t.size() - 4, L".exe") == 0 && GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES;
    return (exe && !args[0]) ? t : path;
}

// The window list is refreshed several times a second: remember the image path of a process for a few seconds.
std::wstring ExePathOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    struct Cached { std::wstring path; ULONGLONG tick; };
    static std::unordered_map<DWORD, Cached> cache;
    const ULONGLONG now = GetTickCount64();
    if (auto it = cache.find(pid); it != cache.end() && now - it->second.tick < 10000) return it->second.path;
    std::wstring result;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD len = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) result.assign(buf, len);
        CloseHandle(h);
    }
    if (cache.size() > 400) cache.clear();
    cache[pid] = {result, now};
    return result;
}

// exePath = what launches / pins the window's app (an .exe, or shell:AppsFolder\<AUMID> for Windows' own Store apps),
// key     = what groups windows into one button.
void IdentifyWindow(HWND h, std::wstring& exePath, std::wstring& key) {
    const std::wstring proc = ExePathOf(h);
    if (proc.empty() || system::IsApplicationFrameHost(proc)) {
        const std::wstring aumid = system::WindowAumid(h);
        if (!aumid.empty()) { exePath = system::AppsFolderPath(aumid); key = exePath; return; }
        exePath.clear();
        key = L"hwnd:" + std::to_wstring(reinterpret_cast<uintptr_t>(h));
        return;
    }
    exePath = proc; key = proc;
}

// Friendly name of a pinned app ("Calculator" instead of "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App").
std::wstring DisplayNameOf(const std::wstring& path) {
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
        PWSTR n = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && n) { std::wstring r = n; CoTaskMemFree(n); return r; }
    }
    return L"";
}

std::wstring FileTitle(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    if (!name.empty()) name[0] = static_cast<wchar_t>(towupper(name[0]));
    return name;
}

bool IEqual(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring WindowTitle(HWND h) {
    wchar_t buf[256];
    int n = GetWindowTextW(h, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n > 0 ? n : 0);
}

bool IsTaskWindow(HWND h, HWND self) {
    if (h == self || !IsWindowVisible(h)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64];
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"AetherBarClass") ||
        !wcscmp(cls, L"AetherStartMenuClass")) return false;
    return true;
}

struct EnumCtx { std::vector<HWND> out; HWND self; };
BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    if (IsTaskWindow(h, c->self)) c->out.push_back(h);
    return TRUE;
}

HICON WindowIcon(HWND h) {
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return reinterpret_cast<HICON>(r);
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return reinterpret_cast<HICON>(r);
    if (HICON i = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON))) return i;
    return reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICONSM));
}

// Win+D (show desktop) is the only real Windows shortcut still used; quick settings, calendar and tray are Aether's own panels.
void SendWinCombo(WORD key) {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = key;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = key; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

ComPtr<IAudioEndpointVolume> DefaultEndpointVolume() {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioEndpointVolume> vol;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(vol.GetAddressOf()));
    }
    return vol;
}

} // namespace

// =============================================================================================
//  Stock taskbar guard
// =============================================================================================

// Explorer's tray window. TrayHost also owns a "Shell_TrayWnd", so skip every window of this process.
static HWND StockTray() {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId()) return h;
    }
    return nullptr;
}

template <class Fn>
static void ForEachShellBar(Fn fn) {
    if (HWND t = StockTray()) fn(t);
    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) fn(sec);
}

// The stock taskbar's original state is written to %APPDATA%\Aether\taskbar-state.txt before it is changed and removed again on Restore().
// If Aether is killed (task manager, uninstaller, crash) the file survives, so the next start / the uninstaller can still put the Windows taskbar back.
static std::wstring TaskbarStateFile() {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return L"";
    std::wstring dir = std::wstring(appdata) + L"\\Aether";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\taskbar-state.txt";
}
static bool ReadSavedTaskbarState(UINT& out) {
    const std::wstring f = TaskbarStateFile();
    if (f.empty()) return false;
    FILE* fp = _wfopen(f.c_str(), L"rt");
    if (!fp) return false;
    unsigned v = 0; const bool ok = fscanf(fp, "%u", &v) == 1;
    fclose(fp);
    if (ok) out = v;
    return ok;
}
static void WriteSavedTaskbarState(UINT v) {
    const std::wstring f = TaskbarStateFile();
    if (f.empty()) return;
    if (FILE* fp = _wfopen(f.c_str(), L"wt")) { fprintf(fp, "%u\n", v); fclose(fp); }
}

// Explorer shows its taskbar again every now and then (a periodic refresh about once a minute, auto-hide reveals, a display change). Polling
// only hid it again a tenth of a second later, so it flashed up from the bottom over our bar. This hook hides it the moment it appears.
namespace {
std::atomic<bool> g_stockHide{false};
HANDLE g_stockThread{nullptr};
DWORD g_stockTid{0};

bool IsStockBarWnd(HWND h) {
    wchar_t cls[40] = {};
    if (!GetClassNameW(h, cls, 40)) return false;
    if (wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    return pid != GetCurrentProcessId();                    // our own TrayHost also owns a "Shell_TrayWnd"
}

void CALLBACK StockBarEvent(HWINEVENTHOOK, DWORD, HWND h, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_stockHide || !h || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (IsWindowVisible(h) && IsStockBarWnd(h))     // asynchronous: Explorer's thread may be busy showing it, and this one must not wait for it
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

DWORD WINAPI StockBarThread(LPVOID) {
    MSG msg; PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const DWORD events[3] = {0x8002 /*EVENT_OBJECT_SHOW*/, 0x8018 /*EVENT_OBJECT_UNCLOAKED*/, 0x800B /*EVENT_OBJECT_LOCATIONCHANGE*/};
    HWINEVENTHOOK hooks[3] = {};
    for (int i = 0; i < 3; ++i) hooks[i] = SetWinEventHook(events[i], events[i], nullptr, StockBarEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    for (HWINEVENTHOOK hk : hooks) if (hk) UnhookWinEvent(hk);
    return 0;
}

void SetStockWatch(bool on) {
    g_stockHide = on;
    if (on && !g_stockThread) g_stockThread = CreateThread(nullptr, 0, StockBarThread, nullptr, 0, &g_stockTid);
    else if (!on && g_stockThread) {
        PostThreadMessageW(g_stockTid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_stockThread, 500);
        CloseHandle(g_stockThread); g_stockThread = nullptr;
    }
}
} // namespace

void ShellTaskbarGuard::Hide(bool hard) {
    m_wanted = true; m_hard = hard;                  // remembered: when Explorer's taskbar does not exist yet KeepHidden() retries
    HWND tray = StockTray();
    if (!tray) return;
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = tray;
    if (!m_hidden) {
        // a state file left behind by a run that was killed holds the REAL original state; the live state may already be our auto-hide
        if (!ReadSavedTaskbarState(m_prevState)) {
            m_prevState = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
            WriteSavedTaskbarState(m_prevState);
        }
    }
    // Auto-hide releases the work area the stock bar reserved so ours can claim it.
    abd.lParam = static_cast<LPARAM>(m_prevState | ABS_AUTOHIDE);
    SHAppBarMessage(ABM_SETSTATE, &abd);
    m_hard = hard;
    if (hard) ForEachShellBar([](HWND h) { ShowWindow(h, SW_HIDE); });
    SetStockWatch(hard);
    m_hidden = true;
}

bool ShellTaskbarGuard::KeepHidden() {
    if (!m_wanted) return false;
    if (!m_hidden) {                                  // Explorer's taskbar was not there yet at start-up (the bar often starts first)
        Hide(m_hard);
        return m_hidden;
    }
    bool changed = false;
    // Explorer applies its own taskbar settings a moment after it creates the bar, and again after a restart: the auto-hide that releases the
    // work area is set back on, otherwise its reserved strip stays and windows (maximized ones) sit above / under our bar.
    const ULONGLONG now = GetTickCount64();
    if (now - m_lastStateCheck >= 1500) {
        m_lastStateCheck = now;
        if (HWND tray = StockTray()) {
            APPBARDATA abd = {sizeof(abd)};
            abd.hWnd = tray;
            const UINT st = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
            if (!(st & ABS_AUTOHIDE)) {
                abd.lParam = static_cast<LPARAM>(st | ABS_AUTOHIDE);
                SHAppBarMessage(ABM_SETSTATE, &abd);
                changed = true;
            }
        }
    }
    if (m_hard) ForEachShellBar([&](HWND h) { if (IsWindowVisible(h)) { ShowWindow(h, SW_HIDE); changed = true; } });
    return changed;
}

void ShellTaskbarGuard::Restore() {
    SetStockWatch(false);                             // before showing it again, or the hook would hide it right back
    HWND tray = StockTray();
    if (tray) {
        APPBARDATA abd = {sizeof(abd)};
        abd.hWnd = tray;
        UINT state = m_hidden ? m_prevState : static_cast<UINT>(ABS_ALWAYSONTOP);
        state &= ~static_cast<UINT>(ABS_AUTOHIDE);
        abd.lParam = static_cast<LPARAM>(state | ABS_ALWAYSONTOP);
        SHAppBarMessage(ABM_SETSTATE, &abd);
    }
    ForEachShellBar([](HWND h) { ShowWindow(h, SW_SHOWNA); });
    if (m_hidden) { const std::wstring f = TaskbarStateFile(); if (!f.empty()) DeleteFileW(f.c_str()); }
    m_hidden = false;
    m_wanted = false;                                 // restored on purpose: KeepHidden() must not hide it again
}

// =============================================================================================
//  Entry
// =============================================================================================

Entry::Entry()
    : appear(0.0f, animation::SpringConfig{420.0f, 30.0f, 1.0f, 0.002f})
    , hover(0.0f, animation::SpringConfig{620.0f, 36.0f, 1.0f, 0.002f})
    , active(0.0f, animation::SpringConfig{520.0f, 38.0f, 1.0f, 0.002f}) {
    appear.SetTarget(1.0f);
}

// =============================================================================================
//  Window / lifecycle
// =============================================================================================

AetherBar::AetherBar(HMONITOR monitor, bool primary, StartMenu* start)
    : m_mon(monitor), m_primary(primary), m_start(start)
    , m_startHover(0.0f, animation::SpringConfig{620.0f, 36.0f, 1.0f, 0.002f})
    , m_dockAppear(0.0f, animation::SpringConfig{300.0f, 28.0f, 1.0f, 0.002f}) {}

AetherBar::~AetherBar() {
    if (m_hwnd) DeregisterShellHookWindow(m_hwnd);
    UnregisterAppBar();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

LRESULT CALLBACK AetherBar::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AetherBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = reinterpret_cast<AetherBar*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<AetherBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void AetherBar::PositionWindow() {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(m_mon, &mi);
    m_monRect = mi.rcMonitor;
    const float monScale = core::MonitorScale(m_mon);
    m_windowW = m_monRect.right - m_monRect.left;
    // The concept was drawn on a 1672 px wide canvas; on wider screens the bar is scaled up a little
    // (up to 15 %) so it keeps the same proportions. Window / work-area metrics use the plain scale.
    const float design = std::clamp(static_cast<float>(m_windowW) / (1672.0f * std::max(1.0f, monScale)), 1.0f, 1.15f);
    m_dpi = monScale * design;
    m_windowH = static_cast<int>((kReserved + kTipRoom) * monScale);
    m_reservedPx = static_cast<int>(kReserved * monScale);
    if (m_hwnd) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, m_monRect.left, m_top ? m_monRect.top : m_monRect.bottom - m_windowH, m_windowW, m_windowH, SWP_NOACTIVATE);
    }
}

bool AetherBar::Create() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AetherBarClass";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);  // harmless if already registered by the first bar

    g_barAtTop = ReadTaskbarEdge() == 1;
    m_top = g_barAtTop;
    PositionWindow();
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Aether Bar", WS_POPUP, m_monRect.left, m_top ? m_monRect.top : m_monRect.bottom - m_windowH,
                             m_windowW, m_windowH, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;
    // the (non-elevated) shell talks to this elevated window: let its two messages through UIPI
    ChangeWindowMessageFilterEx(m_hwnd, shellkeys::kMsgWinCombo, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, shellkeys::kMsgActivate, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, system::barpin::MsgHover(), MSGFLT_ALLOW, nullptr);   // items dragged in from the desktop
    ChangeWindowMessageFilterEx(m_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, kMsgDemoFlyout, MSGFLT_ALLOW, nullptr);

    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, m_windowW, m_windowH)) return false;
    m_brush = m_dcomp->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1.0f});
    CreateFonts();
    m_preview = std::make_unique<WindowPreview>();
    if (!m_preview->Create([this](HWND w) { ActivateWindow(w); })) m_preview.reset();   // no preview, everything else keeps working

    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (m_primary) PollInputLanguage();                // so the language button is there from the first frame (no later widening of the pill)
    m_shellHookMsg = RegisterWindowMessageW(L"SHELLHOOK");
    RegisterShellHookWindow(m_hwnd);                  // HSHELL_FLASH: an app wants attention
    // Explorer (not elevated) broadcasts this when its taskbar is created, at logon and after a restart. This window is elevated, so UIPI
    // dropped the broadcast and the stock taskbar was never hidden again.
    if (m_taskbarCreatedMsg) ChangeWindowMessageFilterEx(m_hwnd, m_taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    PinStore::I().Load();

    RegisterAppBar();
    RefreshWindows();
    m_dockAppear.SetTarget(1.0f);
    ComputeLayout();
    UpdateRegion();
    RefreshBackdrop(false);   // window not visible yet
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    return true;
}

// The pills sit in the reserved strip, where only the wallpaper can be behind them. A blurred snapshot of that
// strip gives them a (light) frosted look. Re-taken when the wallpaper changes: the bar is hidden for a frame.
void AetherBar::RefreshBackdrop(bool hideWindowFirst) {
    if (!m_dcomp || !m_hwnd) return;
    const bool hide = hideWindowFirst && m_visible && IsWindowVisible(m_hwnd);
    if (hide) { ShowWindow(m_hwnd, SW_HIDE); DwmFlush(); }
    RECT wr; GetWindowRect(m_hwnd, &wr);
    render::CaptureBackdrop(m_dcomp->GetContext(), wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, 2, m_backdrop);
    if (hide) ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
}

void AetherBar::RegisterAppBar() {
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = m_hwnd;
    abd.uCallbackMessage = WM_APPBAR;
    SHAppBarMessage(ABM_NEW, &abd);
    SetAppBarPos();
}

void AetherBar::UnregisterAppBar() {
    if (!m_hwnd) return;
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = m_hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd);
}

void AetherBar::SetAppBarPos() {
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = m_hwnd;
    abd.uEdge = m_top ? ABE_TOP : ABE_BOTTOM;
    abd.rc = m_top ? RECT{m_monRect.left, m_monRect.top, m_monRect.right, m_monRect.top + m_reservedPx}
                   : RECT{m_monRect.left, m_monRect.bottom - m_reservedPx, m_monRect.right, m_monRect.bottom};
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    if (m_top) abd.rc.bottom = abd.rc.top + m_reservedPx; else abd.rc.top = abd.rc.bottom - m_reservedPx;
    SHAppBarMessage(ABM_SETPOS, &abd);
}

void AetherBar::CreateFonts() {
    auto dw = m_dcomp->GetDWriteFactory();
    if (!dw) return;
    ComPtr<IDWriteFontCollection> col;
    dw->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) {
        UINT32 i = 0; BOOL ex = FALSE;
        return col && SUCCEEDED(col->FindFamilyName(n, &i, &ex)) && ex;
    };
    const wchar_t* display = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icons = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
    m_hasIconFont = icons != nullptr;

    auto make = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* family, DWRITE_FONT_WEIGHT w, float size) {
        f.Reset();
        if (!family) return;
        dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * m_dpi, L"en-us", &f);
        if (f) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };
    make(m_fontTip,  display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f);
    make(m_fontTime, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f);
    make(m_fontDate, display, DWRITE_FONT_WEIGHT_NORMAL, 11.0f);
    make(m_fontIcon, icons,   DWRITE_FONT_WEIGHT_NORMAL, 16.0f);
    if (m_fontTip) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ell;
        dw->CreateEllipsisTrimmingSign(m_fontTip.Get(), &ell);
        m_fontTip->SetTrimming(&trim, ell.Get());
    }
}

// =============================================================================================
//  Running windows + pins
// =============================================================================================

Entry* AetherBar::FindEntry(const std::wstring& key) {
    for (auto& e : m_entries) if (IEqual(e->key, key)) return e.get();
    return nullptr;
}

// Sharp icon straight from the executable (up to 256 px, scaled down by Direct2D) instead of the 32 px window icon
// that used to be stretched to 48 px. Returns null for hosts whose exe icon would be wrong (UWP frame host).
static ComPtr<ID2D1Bitmap1> ShellIconBitmap(ID2D1DeviceContext* ctx, const std::wstring& path, int px) {
    ComPtr<ID2D1Bitmap1> out;
    if (!ctx || path.empty() || path.find(L"ApplicationFrameHost") != std::wstring::npos) return out;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory) return out;
    HBITMAP hbm = nullptr;
    if (FAILED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) || !hbm) return out;
    BITMAP bm{};
    GetObjectW(hbm, sizeof(bm), &bm);
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        std::vector<uint32_t> pixels(static_cast<size_t>(bm.bmWidth) * bm.bmHeight);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, hbm, 0, bm.bmHeight, pixels.data(), &bi, DIB_RGB_COLORS)) {
            bool anyAlpha = false;
            for (uint32_t p : pixels) if (p >> 24) { anyAlpha = true; break; }
            if (!anyAlpha) for (auto& p : pixels) if (p & 0x00FFFFFF) p |= 0xFF000000;
            D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ctx->CreateBitmap(D2D1::SizeU(bm.bmWidth, bm.bmHeight), pixels.data(), bm.bmWidth * 4, props, &out);
        }
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(hbm);
    return out;
}

ComPtr<ID2D1Bitmap1> AetherBar::LoadIcon(const Entry& e) {
    if (system::iconpack::Active()) {                                   // the user's icon pack wins over the program's own icon
        const std::wstring f = system::iconpack::Find(e.exePath);
        if (!f.empty()) if (auto themed = system::iconpack::LoadBitmap(m_dcomp->GetContext(), f)) return themed;
    }
    if (auto sharp = ShellIconBitmap(m_dcomp->GetContext(), e.exePath, 128)) return sharp;
    HICON icon = nullptr;
    bool owned = false;
    for (HWND w : e.windows) { icon = WindowIcon(w); if (icon) break; }
    if (!icon && !e.exePath.empty()) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(e.exePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
            icon = sfi.hIcon; owned = true;
        }
    }
    ComPtr<ID2D1Bitmap1> bmp;
    if (!icon) return bmp;

    const int px = static_cast<int>(48 * std::max(1.0f, m_dpi));
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        memset(bits, 0, static_cast<size_t>(px) * px * 4);
        DrawIconEx(dc, 0, 0, icon, px, px, 0, nullptr, DI_NORMAL);
        GdiFlush();
        auto* p = static_cast<uint32_t*>(bits);
        bool anyAlpha = false;
        for (int i = 0; i < px * px; ++i) if (p[i] >> 24) { anyAlpha = true; break; }
        if (!anyAlpha) for (int i = 0; i < px * px; ++i) if (p[i] & 0x00FFFFFF) p[i] |= 0xFF000000;
        if (auto ctx = m_dcomp->GetContext()) {
            D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ctx->CreateBitmap(D2D1::SizeU(px, px), bits, px * 4, props, &bmp);
        }
        SelectObject(dc, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(dc);
    if (owned) DestroyIcon(icon);
    return bmp;
}

void AetherBar::RefreshWindows() {
    PinStore& store = PinStore::I();
    EnumCtx ctx{{}, m_hwnd};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));

    for (auto& e : m_entries) e->windows.clear();

    // pins first, so pinned apps exist even when nothing is running
    for (const auto& p : store.pins) {
        if (!FindEntry(p)) {
            auto e = std::make_unique<Entry>();
            e->key = p; e->exePath = p;
            e->label = system::IsAppsFolderPath(p) ? DisplayNameOf(p) : FileTitle(p);
            if (e->label.empty()) e->label = FileTitle(p);
            m_entries.push_back(std::move(e));
        }
    }
    for (auto& e : m_entries) e->pinned = store.IndexOf(e->exePath) >= 0 && !e->exePath.empty();

    for (HWND h : ctx.out) {
        // each bar lists only the windows that live on its own monitor
        if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != m_mon) continue;
        std::wstring exe, key;
        IdentifyWindow(h, exe, key);
        Entry* e = FindEntry(key);
        if (!e) {
            auto ne = std::make_unique<Entry>();
            ne->key = key; ne->exePath = exe;
            ne->label = (exe.empty() || system::IsAppsFolderPath(exe)) ? WindowTitle(h) : FileTitle(exe);
            e = ne.get();
            m_entries.push_back(std::move(ne));
        }
        e->windows.push_back(h);
    }

    const ULONGLONG now = GetTickCount64();
    {   // a different icon pack was chosen: forget every loaded icon so they come back in the new style
        const unsigned pv = system::iconpack::Version();
        if (pv != m_packVer) { m_packVer = pv; m_iconCache.clear(); for (auto& e : m_entries) { e->icon.Reset(); e->iconStamp = 0; } }
    }
    int loadBudget = 2;      // shell icon loads are slow: at most two per refresh, so a start-up with twenty apps never freezes the bar
    for (auto& e : m_entries) {
        e->appear.SetTarget((!e->Running() && !e->pinned) ? 0.0f : 1.0f);
        if (e->Running()) {
            const std::wstring t = WindowTitle(e->windows.front());
            if (!t.empty()) e->label = t;
        }
        if (e->exePath.empty()) {                   // nothing to ask the shell: the window's own icon (apps change it at runtime)
            if (!e->icon || now - e->iconStamp > kIconRefreshMs) {
                auto bmp = LoadIcon(*e);
                if (bmp) e->icon = bmp;
                e->iconStamp = now;
            }
            continue;
        }
        auto cached = m_iconCache.find(e->exePath);
        const bool fresh = cached != m_iconCache.end() && cached->second && now - e->iconStamp <= kIconRefreshMs * 12;
        if (fresh) { if (e->icon != cached->second) e->icon = cached->second; continue; }
        if (loadBudget <= 0 || (!e->icon && e->iconStamp && now - e->iconStamp < 1500)) continue;     // retry a failed load only now and then
        --loadBudget;
        m_iconCache.erase(e->exePath);
        auto bmp = LoadIcon(*e);
        if (bmp) { m_iconCache[e->exePath] = bmp; e->icon = bmp; }
        e->iconStamp = now;
    }
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [](const std::unique_ptr<Entry>& e) {
        return !e->pinned && !e->Running() && e->appear.GetValue() < 0.02f;
    }), m_entries.end());

    // pinned first (in pin order), everything else keeps its first-seen order
    std::stable_sort(m_entries.begin(), m_entries.end(), [&](const std::unique_ptr<Entry>& a, const std::unique_ptr<Entry>& b) {
        const int ia = a->pinned ? store.IndexOf(a->exePath) : INT_MAX;
        const int ib = b->pinned ? store.IndexOf(b->exePath) : INT_MAX;
        return ia < ib;
    });
    m_pinVersion = store.version;
}

// =============================================================================================
//  Update / layout
// =============================================================================================

bool AetherBar::IsAnimating() const {
    if (!m_startHover.IsResting() || !m_dockAppear.IsResting()) return true;
    if (m_dragging) return true;
    if (m_pinHoverAmt > 0.01f || GetTickCount64() - m_pinHoverTick < 300) return true;
    if (m_preview && m_preview->IsOpen()) return true;
    { const ULONGLONG now = GetTickCount64(); for (const auto& f : m_flash) if (now - f.second < 6000) return true; }   // the pulse runs 6 s, then the ring stays still
    for (const auto& e : m_entries) {
        if (!e->appear.IsResting() || !e->hover.IsResting() || !e->active.IsResting() || e->slide != 0.0f) return true;
    }
    for (int k = 0; k < 16; ++k) if (m_hoverAmt[k] > 0.0f && m_hoverAmt[k] < 1.0f) return true;
    return std::abs(m_tipAlpha - (m_tipWanted ? 1.0f : 0.0f)) > 0.01f;
}

void AetherBar::Tick(float dt) {
    SyncEdge(dt);
    {
        const bool over = GetTickCount64() - m_pinHoverTick < 300;      // the hover messages stop when the drag leaves or ends
        m_pinHoverAmt += ((over ? 1.0f : 0.0f) - m_pinHoverAmt) * std::min(1.0f, dt * 14.0f);
        if (!over && m_pinHoverAmt < 0.01f) m_pinHoverAmt = 0.0f;
    }
    UpdatePreview(dt);
    if (!m_flash.empty()) {                                                    // forget windows that are gone or have been brought to the front
        HWND fg = GetAncestor(GetForegroundWindow(), GA_ROOT);
        for (auto it = m_flash.begin(); it != m_flash.end();) it = (!IsWindow(it->first) || it->first == fg) ? m_flash.erase(it) : std::next(it);
    }
    m_langTimer += dt;
    if (m_primary && m_langTimer >= 0.3f) { m_langTimer = 0.0f; PollInputLanguage(); }
    m_refreshTimer += dt;
    if (!m_dragging && (m_refreshTimer >= 0.35f || m_pinVersion != PinStore::I().version)) { m_refreshTimer = 0.0f; RefreshWindows(); }
    m_keepHiddenTimer += dt;
    if (m_keepHiddenTimer >= 0.1f) {
        m_keepHiddenTimer = 0.0f;
        if (ShellTaskbarGuard::Instance().KeepHidden()) SetAppBarPos();   // the stock bar let go of the work area: claim it
    }

    if (m_primary) {
        m_netTimer += dt;
        if (m_netTimer >= 3.0f) {
            m_netTimer = 0.0f;
            const NetState ns = QueryNetState();
            int kind = ns.connected ? (ns.wifi ? 1 : 2) : 0;
            m_netKind = kind;
        }
        m_weatherTimer += dt;
        if (m_weatherTimer >= 1.0f) {
            m_weatherTimer = 0.0f;
            const WeatherNow w = WeatherService::Instance().Get();
            m_weatherValid = w.valid;
            if (w.valid) {
                m_weatherTemp = w.tempC;
                m_weatherKind = static_cast<int>(WeatherService::KindOf(w.code, w.day));
                m_weatherText = WeatherService::Describe(w.code, w.day);
            }
        }
        m_volTimer += dt;
        if (m_volTimer >= 1.0f) {
            m_volTimer = 0.0f;
            if (auto ev = DefaultEndpointVolume()) {
                float v = 0; BOOL mute = FALSE;
                if (SUCCEEDED(ev->GetMasterVolumeLevelScalar(&v))) m_volume = v;
                if (SUCCEEDED(ev->GetMute(&mute))) m_muted = mute != FALSE;
            }
        }
    }

    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        Entry& e = *m_entries[i];
        bool isActive = false;
        for (HWND w : e.windows) if (w == fg || w == fgRoot) { isActive = true; break; }
        e.active.SetTarget(isActive ? 1.0f : 0.0f);
        e.hover.SetTarget((m_hoverKind == Hit::Item && m_hoverItem == i) ? 1.0f : 0.0f);
        e.appear.Update(dt); e.hover.Update(dt); e.active.Update(dt);
        if (e.slide != 0.0f) {                       // gliding into the slot after a re-order
            e.slide *= std::exp(-dt * 16.0f);
            if (std::fabs(e.slide) < 0.4f) e.slide = 0.0f;
        }
    }
    for (int k = 0; k < 16; ++k) {
        const float t = static_cast<int>(m_hoverKind) == k ? 1.0f : 0.0f;
        m_hoverAmt[k] += (t - m_hoverAmt[k]) * (1.0f - std::exp(-dt * 16.0f));
        if (std::abs(t - m_hoverAmt[k]) < 0.004f) m_hoverAmt[k] = t;
    }
    m_startHover.SetTarget(m_hoverKind == Hit::Start ? 1.0f : 0.0f);
    m_startHover.Update(dt);
    m_dockAppear.Update(dt);

    if (m_tipOverrideTimer > 0.0f) m_tipOverrideTimer -= dt;
    m_tipAlpha += ((m_tipWanted ? 1.0f : 0.0f) - m_tipAlpha) * (1.0f - std::exp(-dt * 16.0f));
}

void AetherBar::ComputeLayout() {
    const float s = m_dpi;
    const float h = kDockH * s;
    const float top = m_top ? kMargin * s : static_cast<float>(m_windowH) - kMargin * s - h;

    float contentW = 0.0f;
    for (const auto& e : m_entries) contentW += (kItem + kGap) * s * e->appear.GetValue();
    const float startW = kItem * s;
    const float searchW = kItem * s + kGap * s;
    const float dockW = (kPad * 2 + 6) * s + startW + searchW + contentW;
    const float left = (static_cast<float>(m_windowW) - dockW) * 0.5f;

    m_layout.dock = Rect{left, top, dockW, h};
    m_layout.start = Rect{left + kPad * s, top + (h - kItem * s) * 0.5f, startW, kItem * s};

    m_layout.items.assign(m_entries.size(), Rect{});
    m_layout.search = Rect{m_layout.start.Right() + 6 * s, top + (h - kItem * s) * 0.5f, kItem * s, kItem * s};
    float x = m_layout.search.Right() + kGap * s;
    {
        // while a button is dragged the others make room for it at the drop position
        std::vector<int> order(m_entries.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        const bool dragging = m_dragging && m_dragIdx >= 0 && m_dragIdx < static_cast<int>(order.size());
        if (dragging) {
            order.erase(order.begin() + m_dragIdx);
            order.insert(order.begin() + std::clamp(m_dropIdx, 0, static_cast<int>(order.size())), m_dragIdx);
        }
        const float itemsLeft = x;
        for (int k : order) {
            m_layout.items[k] = Rect{x + m_entries[k]->slide, top + (h - kItem * s) * 0.5f, kItem * s, kItem * s};
            x += (kItem + kGap) * s * m_entries[k]->appear.GetValue();
        }
        if (dragging) {                                           // the dragged button follows the cursor
            const float dw = kItem * s;
            Rect& r = m_layout.items[m_dragIdx];
            r.x = std::clamp(m_mouse.x - dw * 0.5f, itemsLeft - dw * 0.5f, x - dw * 0.5f);
            r.y += (m_top ? 6.0f : -6.0f) * s;
        }
    }

    SYSTEM_POWER_STATUS ps{};
    const bool battery = m_primary && GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100;
    const float chevW = m_primary ? 34.0f * s : 0.0f;
    const float netW = m_primary ? 38.0f * s : 0.0f;
    const float langW = (m_primary && m_langCount > 1 && !m_langCode.empty()) ? 40.0f * s : 0.0f;   // input language (TR / EN), only with 2+ layouts
    const float bellW = m_primary ? 40.0f * s : 0.0f;
    const float quickW = m_primary ? (battery ? 104.0f : 40.0f) * s : 0.0f;
    const float clockW = 84.0f * s;
    const float deskW = m_primary ? 14.0f * s : 0.0f;   // show-desktop zone (invisible until hovered)
    const float rightW = 10.0f * s + chevW + quickW + netW + langW + clockW + bellW + deskW;
    const float rx = static_cast<float>(m_windowW) - kMargin * s * 1.4f - rightW;
    m_layout.right = Rect{rx, top, rightW, h};

    float cx = rx + 10.0f * s;
    const float iy = top + 6.0f * s, ih = h - 12.0f * s;
    m_layout.chevron = Rect{cx, iy, chevW, ih}; cx += chevW;
    m_layout.quick = Rect{cx, iy, quickW, ih}; cx += quickW;
    m_layout.network = Rect{cx, iy, netW, ih}; cx += netW;
    m_layout.lang = Rect{cx, iy, langW, ih}; cx += langW;
    m_layout.clock = Rect{cx, iy, clockW, ih}; cx += clockW;
    m_layout.bell = Rect{cx, iy, bellW, ih}; cx += bellW;
    m_layout.desktop = Rect{cx, iy, deskW, ih};
    m_layout.weather = m_primary ? Rect{kMargin * s * 1.4f, top, 156.0f * s, h} : Rect{};
}

float AetherBar::LiftPx() const {
    return (m_top ? -1.0f : 1.0f) * (1.0f - Clamp01(m_dockAppear.GetValue())) * 30.0f * m_dpi;
}

// The keyboard layout of the window in front ("TR" / "EN"). Layouts are per window / thread, so a click on this (non-activating) bar asks that
// window to move on to the next one, like Windows' own language button does.
bool AetherBar::IsFlashing(const Entry& e) const {
    if (m_flash.empty()) return false;
    for (HWND w : e.windows) if (m_flash.count(w)) return true;
    return false;
}

void AetherBar::PollInputLanguage() {
    static ULONGLONG countAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - countAt > 4000) { countAt = now; m_langCount = std::max(1, GetKeyboardLayoutList(0, nullptr)); }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    const DWORD thread = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
    if (!thread || pid == GetCurrentProcessId()) return;                       // our own flyouts: keep what was shown
    const LANGID id = LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(thread)));
    wchar_t code[16] = {}, name[64] = {};
    if (!GetLocaleInfoW(id, LOCALE_SISO639LANGNAME, code, 16)) return;
    GetLocaleInfoW(id, LOCALE_SLOCALIZEDLANGUAGENAME, name, 64);
    for (wchar_t* p = code; *p; ++p) *p = static_cast<wchar_t>(towupper(*p));
    if (m_langCode != code) m_langCode = code;
    if (m_langName != name) m_langName = name;
}

void AetherBar::SwitchInputLanguage() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return;
    PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, INPUTLANGCHANGE_FORWARD, static_cast<LPARAM>(1) /* HKL_NEXT */);
    m_langTimer = 9.0f;                                                        // re-read it right away
}

// Polls the taskbar position every 2 s (the user can move it in Windows' settings while we run).
void AetherBar::SyncEdge(float dt) {
    m_edgeTimer += dt;
    if (m_edgeTimer < 2.0f) return;
    m_edgeTimer = 0.0f;
    const bool top = ReadTaskbarEdge() == 1;
    g_barAtTop = top;
    if (top != m_top) ApplyEdge(top);
}

void AetherBar::ApplyEdge(bool top) {
    HidePreview();
    m_top = top;
    PositionWindow();
    SetAppBarPos();
    m_regionKey.clear();
    ComputeLayout();
    UpdateRegion();
    RefreshBackdrop(true);
}

void AetherBar::HidePreview() {
    if (m_preview) m_preview->Hide();
    m_prevItem = -1; m_hoverDwell = 0.0f; m_leaveDwell = 0.0f;
}

// Hover on a running app's button opens its window preview after a short delay (instantly when moving from one app to the next);
// it closes shortly after the cursor left both the button and the preview.
void AetherBar::UpdatePreview(float dt) {
    if (!m_preview) return;
    const bool open = m_preview->IsOpen();
    const bool onItem = !m_dragging && m_hoverKind == Hit::Item && m_hoverItem >= 0 && m_hoverItem < static_cast<int>(m_entries.size()) &&
                        m_entries[m_hoverItem]->Running() && !m_entries[m_hoverItem]->windows.empty() &&
                        m_hoverItem < static_cast<int>(m_layout.items.size());
    if (onItem) {
        m_leaveDwell = 0.0f;
        if (m_prevItem != m_hoverItem) { m_prevItem = m_hoverItem; m_hoverDwell = 0.0f; }
        m_hoverDwell += dt;
        const Entry& e = *m_entries[m_hoverItem];
        const bool sameApp = open && IEqual(m_prevKey, e.key);
        if (sameApp) {
            m_prevSync += dt;
            if (m_prevSync >= 0.4f) { m_prevSync = 0.0f; m_preview->SetWindows(e.windows); }
        } else if ((open && m_hoverDwell >= 0.05f) || m_hoverDwell >= 0.45f) {
            m_prevKey = e.key; m_prevSync = 0.0f;
            RECT wr; GetWindowRect(m_hwnd, &wr);
            const float cx = wr.left + m_layout.items[m_hoverItem].Center().x;
            m_preview->Show(m_mon, cx, wr.top + static_cast<int>(m_top ? m_layout.dock.Bottom() : m_layout.dock.Top()), m_top, e.exePath, e.windows);
        }
        return;
    }
    m_prevItem = -1; m_hoverDwell = 0.0f;
    if (!open) return;
    if (m_preview->Hovered()) { m_leaveDwell = 0.0f; return; }
    m_leaveDwell += dt;
    if (m_leaveDwell >= 0.30f) HidePreview();
}

void AetherBar::ComputeTooltip() {
    std::wstring text;
    float anchor = 0.0f;
    if (m_tipOverrideTimer > 0.0f && !m_tipOverride.empty()) {
        text = m_tipOverride; anchor = m_layout.quick.Center().x;
    } else {
        switch (m_hoverKind) {
            case Hit::Item:
                if (m_preview && m_preview->IsOpen()) break;          // the preview already names the windows
                if (m_hoverItem >= 0 && m_hoverItem < static_cast<int>(m_entries.size()) && m_hoverItem < static_cast<int>(m_layout.items.size())) {
                    text = m_entries[m_hoverItem]->label.empty() ? L"App" : m_entries[m_hoverItem]->label;
                    anchor = m_layout.items[m_hoverItem].Center().x;
                }
                break;
            case Hit::Start:   text = i18n::Pick(L"Start", L"Başlat"); anchor = m_layout.start.Center().x; break;
            case Hit::Search:  text = i18n::Pick(L"Search", L"Ara"); anchor = m_layout.search.Center().x; break;
            case Hit::Chevron: text = i18n::Pick(L"System tray (hidden icons)", L"Sistem tepsisi (gizli simgeler)"); anchor = m_layout.chevron.Center().x; break;
            case Hit::Quick:   text = i18n::Pick(L"Quick settings  -  scroll to change volume", L"Hızlı ayarlar  -  ses için kaydırın"); anchor = m_layout.quick.Center().x; break;
            case Hit::Network: text = m_netKind == 0 ? i18n::Pick(L"No network connection", L"Ağ bağlantısı yok") : i18n::Pick(L"Network", L"Ağ"); anchor = m_layout.network.Center().x; break;
            case Hit::Lang:    text = (i18n::Pick(L"Input language: ", L"Giriş dili: ") + m_langName + i18n::Pick(L"  -  click to switch", L"  -  değiştirmek için tıkla")); anchor = m_layout.lang.Center().x; break;
            case Hit::Clock:   text = i18n::Pick(L"Calendar", L"Takvim"); anchor = m_layout.clock.Center().x; break;
            case Hit::Bell:    text = i18n::Pick(L"Notifications", L"Bildirimler"); anchor = m_layout.bell.Center().x; break;
            case Hit::Weather: {
                const WeatherNow w = WeatherService::Instance().Get();
                text = w.valid && !w.city.empty() ? w.city : std::wstring(i18n::Pick(L"Weather", L"Hava durumu"));
                anchor = m_layout.weather.Center().x;
                break;
            }
            case Hit::Desktop: text = i18n::Pick(L"Show desktop", L"Masaüstünü göster"); anchor = m_layout.desktop.Center().x; break;
            default: break;
        }
    }
    m_tipWanted = !text.empty();
    if (m_tipWanted) { m_tipText = text; m_tipAnchor = anchor; }

    m_tipRect = Rect{};
    if (m_tipAlpha > 0.02f && !m_tipText.empty()) {
        const float s = m_dpi;
        auto dw = m_dcomp->GetDWriteFactory();
        ComPtr<IDWriteTextLayout> tl;
        float tw = 0;
        if (dw && m_fontTip && SUCCEEDED(dw->CreateTextLayout(m_tipText.c_str(), static_cast<UINT32>(m_tipText.size()), m_fontTip.Get(), 4096, 40, &tl))) {
            DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); tw = m.widthIncludingTrailingWhitespace;
        }
        const float w = std::min(tw + 28.0f * s, 340.0f * s), h = 30.0f * s;
        Rect r{m_tipAnchor - w / 2, m_top ? m_layout.dock.Bottom() + 10.0f * s - (1.0f - m_tipAlpha) * 6.0f * s
                                      : m_layout.dock.Top() - h - 10.0f * s + (1.0f - m_tipAlpha) * 6.0f * s, w, h};
        r.x = std::clamp(r.x, 8.0f * s, static_cast<float>(m_windowW) - w - 8.0f * s);
        m_tipRect = r;
    }
}

// The window region both clips drawing and decides where clicks are swallowed. Foreign windows can
// NOT be made click-through with HTTRANSPARENT, so only the pills (and tooltip) are part of the window.
void AetherBar::UpdateRegion() {
    const float s = m_dpi;
    const float lift = LiftPx();
    auto box = [&](const Rect& r, float padX, float padTop, float padBottom, std::vector<int>& key, int out[4]) {
        out[0] = static_cast<int>(r.Left() - padX);
        out[1] = static_cast<int>(r.Top() - padTop + lift);
        out[2] = static_cast<int>(r.Right() + padX);
        out[3] = static_cast<int>(r.Bottom() + padBottom + lift);
        for (int i = 0; i < 4; ++i) key.push_back(out[i]);
    };
    std::vector<int> key;
    int dock[4], right[4], wea[4] = {0, 0, 0, 0}, tip[4] = {0, 0, 0, 0};
    box(m_layout.dock, 26 * s, 26 * s, 26 * s, key, dock);
    box(m_layout.right, 26 * s, 26 * s, 26 * s, key, right);
    const bool hasWeather = m_layout.weather.width > 1.0f;
    if (hasWeather) box(m_layout.weather, 26 * s, 26 * s, 26 * s, key, wea);
    const bool hasTip = m_tipRect.width > 1.0f;
    if (hasTip) box(m_tipRect, 6 * s, 6 * s, 6 * s, key, tip);
    if (key == m_regionKey) return;
    m_regionKey = key;

    const int r = static_cast<int>(96 * s);
    HRGN rgn = CreateRoundRectRgn(dock[0], dock[1], dock[2], dock[3], r, r);
    HRGN p2 = CreateRoundRectRgn(right[0], right[1], right[2], right[3], r, r);
    CombineRgn(rgn, rgn, p2, RGN_OR);
    DeleteObject(p2);
    if (hasWeather) {
        HRGN pw = CreateRoundRectRgn(wea[0], wea[1], wea[2], wea[3], r, r);
        CombineRgn(rgn, rgn, pw, RGN_OR);
        DeleteObject(pw);
    }
    if (hasTip) {
        HRGN p3 = CreateRoundRectRgn(tip[0], tip[1], tip[2], tip[3], r, r);
        CombineRgn(rgn, rgn, p3, RGN_OR);
        DeleteObject(p3);
    }
    SetWindowRgn(m_hwnd, rgn, FALSE);   // the system owns the region from here on
}

// =============================================================================================
//  Rendering
// =============================================================================================

void AetherBar::DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f,
                         const D2D1_RECT_F& r, const Color& c) {
    if (!f || t.empty()) return;
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, &r, m_brush.Get());
}

void AetherBar::DrawGlyph(ID2D1DeviceContext* ctx, wchar_t g, const D2D1_RECT_F& r, const Color& c) {
    if (!m_hasIconFont || !m_fontIcon) return;
    wchar_t buf[2] = {g, 0};
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(buf, 1, m_fontIcon.Get(), &r, m_brush.Get());
}

void AetherBar::DrawPill(ID2D1DeviceContext* ctx, const Rect& r, float radius) {
    // The outer glow of all pills is drawn once in Render(); this is only the glass body.
    if (auto geo = m_dcomp->CreateSquircleGeometry(r, radius)) {
        if (m_backdrop) render::FillBackdrop(ctx, m_backdrop, geo.Get(), Rect{0.0f, 0.0f, static_cast<float>(m_windowW), static_cast<float>(m_windowH)}, 0.9f);
        render::DrawGlassBody(ctx, m_dcomp.get(), m_brush.Get(), geo.Get(), r, m_dpi, 0.8f, m_backdrop ? 0.72f : 1.0f);
    }
}

void AetherBar::DrawStartLogo(ID2D1DeviceContext* ctx, const Vec2& c, float size, float alpha) {
    const float cell = size * 0.46f, gap = size * 0.08f;
    const float x0 = c.x - cell - gap * 0.5f, y0 = c.y - cell - gap * 0.5f;
    const auto& th = render::glass::CurrentTheme();
    ID2D1Brush* fill = m_brush.Get();
    ComPtr<ID2D1LinearGradientBrush> grad;
    if (th.flat) m_brush->SetColor(D2D1::ColorF(ui::kPurpleLight.r, ui::kPurpleLight.g, ui::kPurpleLight.b, alpha));
    else if (th.bold) m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
    else {
        // lilac -> theme accent, like the concept's glossy logo
        const Color top = Color::Lerp(Color{1.0f, 1.0f, 1.0f, 1.0f}, render::glass::kVioletLight, 0.50f);
        const Color bot = Color::Lerp(render::glass::kAccent2, render::glass::kVioletLight, 0.25f);
        grad = m_dcomp->CreateLinearGradientBrush(render::glass::A(top, alpha), render::glass::A(bot, alpha),
                                                  D2D1::Point2F(x0, y0), D2D1::Point2F(x0 + 2 * cell + gap, y0 + 2 * cell + gap));
        if (grad) fill = grad.Get(); else m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
    }
    for (int iy = 0; iy < 2; ++iy) for (int ix = 0; ix < 2; ++ix) {
        const float x = x0 + ix * (cell + gap), y = y0 + iy * (cell + gap);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(x, y, x + cell, y + cell), size * 0.11f, size * 0.11f), fill);
    }
}

// Small vector weather glyphs (no icon font needed). kind = WeatherService::Kind.
void AetherBar::DrawWeatherIcon(ID2D1DeviceContext* ctx, const Vec2& c, float size, int kind) {
    using K = WeatherService::Kind;
    const K k = static_cast<K>(kind);
    auto circle = [&](float x, float y, float r, const Color& col) {
        m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a));
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), m_brush.Get());
    };
    auto cloud = [&](float cx, float cy, float u, const Color& col) {
        circle(cx - 0.55f * u, cy + 0.12f * u, 0.42f * u, col);
        circle(cx - 0.05f * u, cy - 0.15f * u, 0.55f * u, col);
        circle(cx + 0.52f * u, cy + 0.10f * u, 0.44f * u, col);
        m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(cx - 0.9f * u, cy + 0.10f * u, cx + 0.92f * u, cy + 0.55f * u), 0.22f * u, 0.22f * u), m_brush.Get());
    };
    const Color sun{1.0f, 0.80f, 0.22f, 1.0f}, white{0.93f, 0.95f, 1.0f, 1.0f}, grey{0.62f, 0.66f, 0.76f, 1.0f}, blue{0.42f, 0.70f, 1.0f, 1.0f};
    const float u = size * 0.5f;
    auto rays = [&](float x, float y, float r) {
        m_brush->SetColor(D2D1::ColorF(sun.r, sun.g, sun.b, 0.9f));
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.7853982f;
            ctx->DrawLine(D2D1::Point2F(x + std::cos(a) * r * 1.35f, y + std::sin(a) * r * 1.35f),
                          D2D1::Point2F(x + std::cos(a) * r * 1.75f, y + std::sin(a) * r * 1.75f), m_brush.Get(), size * 0.07f, nullptr);
        }
    };
    switch (k) {
        case K::Clear: circle(c.x, c.y, u * 0.55f, sun); rays(c.x, c.y, u * 0.55f); break;
        case K::PartlyCloudy:
            circle(c.x - u * 0.28f, c.y - u * 0.28f, u * 0.46f, sun); rays(c.x - u * 0.28f, c.y - u * 0.28f, u * 0.46f);
            cloud(c.x + u * 0.12f, c.y + u * 0.20f, u * 0.78f, white); break;
        case K::Cloudy: cloud(c.x, c.y + u * 0.05f, u * 0.95f, white); break;
        case K::Fog:
            cloud(c.x, c.y - u * 0.2f, u * 0.85f, grey);
            m_brush->SetColor(D2D1::ColorF(grey.r, grey.g, grey.b, 0.9f));
            ctx->DrawLine(D2D1::Point2F(c.x - u * 0.8f, c.y + u * 0.6f), D2D1::Point2F(c.x + u * 0.8f, c.y + u * 0.6f), m_brush.Get(), size * 0.08f, nullptr);
            break;
        case K::Rain:
            cloud(c.x, c.y - u * 0.25f, u * 0.85f, grey);
            m_brush->SetColor(D2D1::ColorF(blue.r, blue.g, blue.b, 1.0f));
            for (int i = -1; i <= 1; ++i)
                ctx->DrawLine(D2D1::Point2F(c.x + i * u * 0.5f, c.y + u * 0.45f), D2D1::Point2F(c.x + i * u * 0.5f - u * 0.14f, c.y + u * 0.85f), m_brush.Get(), size * 0.08f, nullptr);
            break;
        case K::Snow:
            cloud(c.x, c.y - u * 0.25f, u * 0.85f, grey);
            for (int i = -1; i <= 1; ++i) circle(c.x + i * u * 0.5f, c.y + u * 0.68f, size * 0.05f, white);
            break;
        case K::Storm:
            cloud(c.x, c.y - u * 0.25f, u * 0.85f, grey);
            m_brush->SetColor(D2D1::ColorF(sun.r, sun.g, sun.b, 1.0f));
            ctx->DrawLine(D2D1::Point2F(c.x + u * 0.1f, c.y + u * 0.3f), D2D1::Point2F(c.x - u * 0.15f, c.y + u * 0.62f), m_brush.Get(), size * 0.09f, nullptr);
            ctx->DrawLine(D2D1::Point2F(c.x - u * 0.15f, c.y + u * 0.62f), D2D1::Point2F(c.x + u * 0.1f, c.y + u * 0.62f), m_brush.Get(), size * 0.09f, nullptr);
            ctx->DrawLine(D2D1::Point2F(c.x + u * 0.1f, c.y + u * 0.62f), D2D1::Point2F(c.x - u * 0.1f, c.y + u * 0.95f), m_brush.Get(), size * 0.09f, nullptr);
            break;
        case K::Night:
            circle(c.x, c.y, u * 0.62f, Color{0.86f, 0.88f, 1.0f, 1.0f});
            circle(c.x + u * 0.28f, c.y - u * 0.18f, u * 0.55f, Color{0.055f, 0.055f, 0.07f, 1.0f});
            break;
        case K::NightCloudy:
            circle(c.x - u * 0.3f, c.y - u * 0.3f, u * 0.42f, Color{0.86f, 0.88f, 1.0f, 1.0f});
            circle(c.x - u * 0.15f, c.y - u * 0.42f, u * 0.38f, Color{0.055f, 0.055f, 0.07f, 1.0f});
            cloud(c.x + u * 0.12f, c.y + u * 0.2f, u * 0.78f, white);
            break;
    }
}

void AetherBar::DrawBattery(ID2D1DeviceContext* ctx, float x, float cy, int percent, bool charging) {
    const float s = m_dpi, w = 24 * s, h = 12 * s;
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
    ctx->DrawRoundedRectangle(D2D1::RoundedRect(RF(x, cy - h / 2, x + w, cy + h / 2), 3 * s, 3 * s), m_brush.Get(), 1.2f * s);
    ctx->FillRectangle(RF(x + w + 1.5f * s, cy - 2.5f * s, x + w + 3 * s, cy + 2.5f * s), m_brush.Get());
    const float f = Clamp01(percent / 100.0f);
    Color c = charging ? Color{0.19f, 0.82f, 0.35f, 1.0f} : (percent <= 20 ? Color{1.0f, 0.27f, 0.23f, 1.0f} : Color{1.0f, 1.0f, 1.0f, 0.92f});
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(x + 2 * s, cy - h / 2 + 2 * s, x + 2 * s + (w - 4 * s) * f, cy + h / 2 - 2 * s), 1.5f * s, 1.5f * s), m_brush.Get());
}

void AetherBar::Render() {
    if (!m_dcomp || !m_visible) return;
    ComputeLayout();
    ComputeTooltip();
    UpdateRegion();

    const float s = m_dpi;
    // glow silhouettes of every pill (recorded before BeginDraw)
    auto gDock = m_dcomp->CreateSquircleGeometry(m_layout.dock, 24.0f * s);
    auto gRight = m_dcomp->CreateSquircleGeometry(m_layout.right, 24.0f * s);
    ComPtr<ID2D1PathGeometry> gWeather;
    if (m_layout.weather.width > 1.0f) gWeather = m_dcomp->CreateSquircleGeometry(m_layout.weather, 24.0f * s);
    auto silhouette = render::RecordSilhouette(m_dcomp.get(), {gDock.Get(), gRight.Get(), gWeather.Get()}, s, 0.75f);

    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(0, LiftPx()));
    render::DrawGlassGlow(ctx, silhouette.Get(), s, 0.75f);

    // ---- dock -----------------------------------------------------------------------------------
    DrawPill(ctx, m_layout.dock, 24.0f * s);
    if (m_pinHoverAmt > 0.01f) {                    // items are being carried over the dock: light it up and show where they would land
        const float a = m_pinHoverAmt;
        const auto& ac = render::glass::kVioletLight;
        if (auto g = m_dcomp->CreateSquircleGeometry(m_layout.dock, 24.0f * s)) {
            m_brush->SetColor(D2D1::ColorF(ac.r, ac.g, ac.b, 0.10f * a));
            ctx->FillGeometry(g.Get(), m_brush.Get());
            m_brush->SetColor(D2D1::ColorF(ac.r, ac.g, ac.b, 0.70f * a));
            ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.6f * s);
        }
        const float ly = m_layout.dock.y + 9.0f * s, lh = m_layout.dock.height - 18.0f * s;
        for (int k = 2; k >= 0; --k) {                // soft glow around the insertion line
            const float w = (3.0f + 5.0f * k) * s;
            m_brush->SetColor(D2D1::ColorF(ac.r, ac.g, ac.b, (k == 0 ? 0.95f : 0.14f) * a));
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(m_pinLineX - w * 0.5f, ly, m_pinLineX + w * 0.5f, ly + lh), w * 0.5f, w * 0.5f), m_brush.Get());
        }
    }

    const float sh = m_startHover.GetValue();
    if (!render::glass::CurrentTheme().flat) {
        const Rect& sr = m_layout.start;
        const bool bold = render::glass::CurrentTheme().bold;
        if (!bold) {
            // same quiet tile as the dock items: a soft squircle that fades in on hover / while the menu is open
            const float open = (m_start && m_start->IsOpen()) ? 1.0f : 0.0f;
            const float a = std::max(sh, open);
            if (a > 0.01f) {
                if (auto g = m_dcomp->CreateSquircleGeometry(sr, 14.0f * s)) {
                    m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f * sh));
                    ctx->FillGeometry(g.Get(), m_brush.Get());
                    m_brush->SetColor(D2D1::ColorF(render::glass::kViolet.r, render::glass::kViolet.g, render::glass::kViolet.b, 0.20f * open));
                    ctx->FillGeometry(g.Get(), m_brush.Get());
                }
            }
        } else {
        for (int i = 3; i >= 1; --i) {   // violet glow
            if (auto g = m_dcomp->CreateSquircleGeometry(Rect{sr.x - i * 2.0f * s, sr.y - i * 2.0f * s, sr.width + i * 4.0f * s, sr.height + i * 4.0f * s}, 15.0f * s + i * 2.0f * s)) {
                m_brush->SetColor(D2D1::ColorF(ui::kPurple.r, ui::kPurple.g, ui::kPurple.b, (0.05f + 0.05f * sh) * (4 - i)));
                ctx->FillGeometry(g.Get(), m_brush.Get());
            }
        }
        if (auto g = m_dcomp->CreateSquircleGeometry(sr, 15.0f * s)) {
            const Color top = Color{ui::kPurpleLight.r * 0.92f, ui::kPurpleLight.g * 0.92f, ui::kPurpleLight.b, 0.97f};
            const Color bot = Color{ui::kPurple.r, ui::kPurple.g, ui::kPurple.b, 0.95f};
            auto fill = m_dcomp->CreateLinearGradientBrush(top, bot,
                                                           D2D1::Point2F(sr.x, sr.y), D2D1::Point2F(sr.Right(), sr.Bottom()));
            if (fill) ctx->FillGeometry(g.Get(), fill.Get());
            m_brush->SetColor(D2D1::ColorF(ui::kPurpleLight.r, ui::kPurpleLight.g, ui::kPurpleLight.b, 0.85f));
            ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.3f * s);
        }
        }
    }
    DrawStartLogo(ctx, m_layout.start.Center(), (19.0f + 2.0f * sh) * s, 1.0f);

    {   // search button: icon only, same hover tile as the other items
        const Rect& sb = m_layout.search;
        const float a = m_hoverAmt[static_cast<int>(Hit::Search)];
        if (a > 0.01f) {
            if (auto g = m_dcomp->CreateSquircleGeometry(sb, 14.0f * s)) {
                m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f * a));
                ctx->FillGeometry(g.Get(), m_brush.Get());
            }
        }
        DrawGlyph(ctx, ui::kGlyphSearch, RF(sb), render::glass::A(Color::Lerp(Color{1.0f, 1.0f, 1.0f, 1.0f}, render::glass::kVioletLight, 0.35f), 0.80f + 0.20f * a));
    }

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& e = *m_entries[i];
        const float appear = Clamp01(e.appear.GetValue());
        if (appear < 0.01f) continue;
        const float hov = e.hover.GetValue(), act = e.active.GetValue();
        const Rect& slot = m_layout.items[i];
        const Vec2 c = slot.Center();

        if (act > 0.01f || hov > 0.01f) {
            if (auto g = m_dcomp->CreateSquircleGeometry(slot, 14.0f * s)) {
                if (render::glass::CurrentTheme().bold) {
                    m_brush->SetColor(D2D1::ColorF(ui::kPurple.r, ui::kPurple.g, ui::kPurple.b, 0.30f * act + 0.10f * hov));
                    ctx->FillGeometry(g.Get(), m_brush.Get());
                    m_brush->SetColor(D2D1::ColorF(ui::kPurpleLight.r, ui::kPurpleLight.g, ui::kPurpleLight.b, 0.55f * act));
                    ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.2f * s);
                } else {
                    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.07f * act + 0.08f * hov));
                    ctx->FillGeometry(g.Get(), m_brush.Get());
                }
            }
        }
        const float scale = appear * (1.0f + 0.14f * hov);
        const float icon = 28.0f * s * scale;
        const float cy = c.y - 3.0f * s * hov;
        if (e.icon) {
            const D2D1_RECT_F dst = RF(c.x - icon / 2, cy - icon / 2 - 1.0f * s, c.x + icon / 2, cy + icon / 2 - 1.0f * s);
            ctx->DrawBitmap(e.icon.Get(), &dst, (e.Running() ? 1.0f : 0.78f) * appear, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        if (IsFlashing(e)) {                                                       // the app wants attention: a pulsing amber ring around the button
            ULONGLONG since = 0; for (HWND fw : e.windows) { auto fi = m_flash.find(fw); if (fi != m_flash.end()) since = std::max(since, fi->second); }
            const float pulse = GetTickCount64() - since < 6000 ? 0.5f + 0.5f * std::sin(static_cast<float>(GetTickCount64() % 1200) / 1200.0f * 6.2831853f) : 0.7f;
            if (auto g = m_dcomp->CreateSquircleGeometry(slot, 14.0f * s)) {
                m_brush->SetColor(D2D1::ColorF(1.0f, 0.70f, 0.20f, (0.10f + 0.20f * pulse) * appear));
                ctx->FillGeometry(g.Get(), m_brush.Get());
                m_brush->SetColor(D2D1::ColorF(1.0f, 0.74f, 0.30f, (0.40f + 0.50f * pulse) * appear));
                ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.4f * s);
            }
        }
        if (e.Running()) {
            // thin lilac "light line" hugging the bottom rim: bright centre fading to both ends + soft bloom
            const float dw = (14.0f + 8.0f * act) * s, dh = 2.2f * s;
            const float dy = m_top ? m_layout.dock.Top() + 6.0f * s : m_layout.dock.Bottom() - 6.0f * s;
            const Color ind = render::glass::kVioletLight;
            const float gl = (0.35f + 0.65f * act) * appear;
            const int nWin = std::min<int>(4, static_cast<int>(e.windows.size()));
            if (auto bl = m_dcomp->CreateRadialGradientBrush(render::glass::A(ind, 0.34f * gl), render::glass::A(ind, 0.0f), D2D1::Point2F(c.x, dy), dw * 0.95f, 5.5f * s))
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, dy), dw * 0.95f, 5.5f * s), bl.Get());
            if (nWin <= 1) {
                if (auto ln = m_dcomp->CreateLinearGradientBrush({{0.0f, render::glass::A(ind, 0.0f)}, {0.28f, render::glass::A(ind, (0.60f + 0.35f * act) * appear)},
                                                                  {0.72f, render::glass::A(ind, (0.60f + 0.35f * act) * appear)}, {1.0f, render::glass::A(ind, 0.0f)}},
                                                                 D2D1::Point2F(c.x - dw / 2, dy), D2D1::Point2F(c.x + dw / 2, dy)))
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(c.x - dw / 2, dy - dh / 2, c.x + dw / 2, dy + dh / 2), dh / 2, dh / 2), ln.Get());
            } else {
                // several windows: one short dash per window, so a glance tells how many are open (more than four: the fourth dash is full-bright)
                const float seg = 5.0f * s, gap = 3.0f * s;
                const float total = nWin * seg + (nWin - 1) * gap;
                float sx = c.x - total / 2;
                m_brush->SetColor(D2D1::ColorF(ind.r, ind.g, ind.b, (0.62f + 0.38f * act) * appear));
                for (int k = 0; k < nWin; ++k, sx += seg + gap)
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(RF(sx, dy - dh / 2, sx + seg, dy + dh / 2), dh / 2, dh / 2), m_brush.Get());
            }
        }
    }

    // ---- system pill --------------------------------------------------------------------------------
    DrawPill(ctx, m_layout.right, 24.0f * s);
    // Each icon gets its own compact, eased highlight; the old full-height boxes touched their neighbours.
    auto hoverBg = [&](const Rect& r, Hit kind) {
        const float a = m_hoverAmt[static_cast<int>(kind)];
        if (a < 0.01f || r.width < 1.0f) return;
        const float hh = std::min(r.height, 38.0f * s);
        const Rect q{r.x + 3.0f * s, r.Center().y - hh / 2, r.width - 6.0f * s, hh};
        if (auto g = m_dcomp->CreateSquircleGeometry(q, 13.0f * s)) {
            m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.075f * a));
            ctx->FillGeometry(g.Get(), m_brush.Get());
            m_brush->SetColor(D2D1::ColorF(render::glass::kVioletLight.r, render::glass::kVioletLight.g, render::glass::kVioletLight.b, 0.16f * a));
            ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.0f * s);
        }
    };
    hoverBg(m_layout.chevron, Hit::Chevron);
    hoverBg(m_layout.quick, Hit::Quick);
    hoverBg(m_layout.network, Hit::Network);
    hoverBg(m_layout.lang, Hit::Lang);
    hoverBg(m_layout.clock, Hit::Clock);
    hoverBg(m_layout.bell, Hit::Bell);

    if (m_primary) {
        DrawGlyph(ctx, kGlyphChevron, RF(m_layout.chevron), Color{1.0f, 1.0f, 1.0f, 0.85f});

        wchar_t vg = kGlyphVol3;
        if (m_muted || m_volume <= 0.005f) vg = kGlyphMute;
        else if (m_volume < 0.10f) vg = kGlyphVol0;
        else if (m_volume < 0.34f) vg = kGlyphVol1;
        else if (m_volume < 0.67f) vg = kGlyphVol2;
        const Rect& q = m_layout.quick;
        DrawGlyph(ctx, vg, RF(q.x + 4 * s, q.y, q.x + 38 * s, q.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.92f});

        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100) {
            const float bx = q.x + 40 * s;
            DrawBattery(ctx, bx, q.Center().y, ps.BatteryLifePercent, ps.ACLineStatus == 1);
            wchar_t pct[8];
            swprintf_s(pct, L"%d%%", ps.BatteryLifePercent);
            DrawText(ctx, pct, m_fontDate.Get(), RF(bx + 30 * s, q.y, q.Right(), q.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.85f});
        }
    }

    if (m_primary) {
        const Rect& nw = m_layout.network;
        DrawGlyph(ctx, m_netKind == 2 ? ui::kGlyphEthernet : ui::kGlyphWifi, RF(nw), Color{1.0f, 1.0f, 1.0f, m_netKind == 0 ? 0.35f : 0.92f});
        if (m_layout.lang.width > 0) DrawText(ctx, m_langCode, m_fontTime.Get(), RF(m_layout.lang), Color{1.0f, 1.0f, 1.0f, 0.92f});
        const bool tinted = !render::glass::CurrentTheme().flat;
        DrawGlyph(ctx, kGlyphBell, RF(m_layout.bell),
                  tinted ? Color::Lerp(Color{1.0f, 1.0f, 1.0f, 1.0f}, render::glass::kVioletLight, 0.60f) : Color{1.0f, 1.0f, 1.0f, 0.88f});
        {   // hairline divider between the system icons and the clock
            const Rect& ck2 = m_layout.clock;
            m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f));
            ctx->FillRectangle(RF(ck2.x - 3 * s, ck2.y + 6 * s, ck2.x - 3 * s + 1.0f * s, ck2.Bottom() - 6 * s), m_brush.Get());
        }
        // notification badge (refreshed lazily)
        {
            static ULONGLONG lastPoll = 0; const ULONGLONG now = GetTickCount64();
            if (now - lastPoll > 5000) { lastPoll = now; NotificationFeed::Instance().RequestRefresh(); }
            if (NotificationFeed::Instance().Count() > 0) {
                const Rect& bl = m_layout.bell;
                ID2D1SolidColorBrush* dot = m_brush.Get();
                if (render::glass::CurrentTheme().flat) dot->SetColor(D2D1::ColorF(1.0f, 0.36f, 0.45f, 1.0f));
                else dot->SetColor(D2D1::ColorF(render::glass::kAccent2.r, render::glass::kAccent2.g, render::glass::kAccent2.b, 1.0f));
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bl.Center().x + 7 * s, bl.Center().y - 7 * s), 4.2f * s, 4.2f * s), dot);
            }
        }
        // show-desktop zone: invisible until hovered, then a short lilac line fades in (no heavy block)
        {
            const float a = m_hoverAmt[static_cast<int>(Hit::Desktop)];
            if (a > 0.01f) {
                const Rect& dk = m_layout.desktop;
                const Color l = render::glass::kVioletLight;
                m_brush->SetColor(D2D1::ColorF(l.r, l.g, l.b, 0.75f * a));
                const float cx = dk.Right() - 6.0f * s, hh = 22.0f * s;
                ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 1.0f * s, dk.Center().y - hh / 2, cx + 1.0f * s, dk.Center().y + hh / 2), 1.0f * s, 1.0f * s), m_brush.Get());
            }
        }
    }

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t tm[16]; swprintf_s(tm, L"%02d:%02d", st.wHour, st.wMinute);
    const std::wstring date = i18n::ShortDate(st);
    const Rect& ck = m_layout.clock;
    DrawText(ctx, tm, m_fontTime.Get(), RF(ck.x, ck.y + 2 * s, ck.Right(), ck.y + 24 * s), render::glass::A(render::glass::kText, 0.98f));
    DrawText(ctx, date, m_fontDate.Get(), RF(ck.x, ck.y + 22 * s, ck.Right(), ck.Bottom() - 2 * s), render::glass::A(render::glass::kMuted, 0.90f));

    // ---- weather pill (bottom-left) -----------------------------------------------------------------
    if (m_layout.weather.width > 1.0f) {
        const Rect& wp = m_layout.weather;
        DrawPill(ctx, wp, 24.0f * s);
        if (m_hoverKind == Hit::Weather) {
            if (auto g = m_dcomp->CreateSquircleGeometry(Rect{wp.x + 5 * s, wp.y + 5 * s, wp.width - 10 * s, wp.height - 10 * s}, 18.0f * s)) {
                m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.08f));
                ctx->FillGeometry(g.Get(), m_brush.Get());
            }
        }
        if (m_weatherValid) {
            DrawWeatherIcon(ctx, Vec2{wp.x + 32.0f * s, wp.Center().y}, 28.0f * s, m_weatherKind);
            wchar_t deg[16]; swprintf_s(deg, L"%d\u00B0C", m_weatherTemp);
            m_fontTime->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_fontDate->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            DrawText(ctx, deg, m_fontTime.Get(), RF(wp.x + 62 * s, wp.y + 4 * s, wp.Right() - 8 * s, wp.y + wp.height * 0.52f), Color{1.0f, 1.0f, 1.0f, 0.96f});
            DrawText(ctx, m_weatherText, m_fontDate.Get(), RF(wp.x + 62 * s, wp.y + wp.height * 0.48f, wp.Right() - 8 * s, wp.Bottom() - 4 * s), Color{1.0f, 1.0f, 1.0f, 0.62f});
            m_fontTime->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_fontDate->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            DrawWeatherIcon(ctx, Vec2{wp.x + 34.0f * s, wp.Center().y}, 30.0f * s, static_cast<int>(WeatherService::Kind::Cloudy));
            DrawText(ctx, i18n::Pick(L"Weather", L"Hava durumu"), m_fontDate.Get(), RF(wp.x + 54 * s, wp.y, wp.Right() - 8 * s, wp.Bottom()), Color{1.0f, 1.0f, 1.0f, 0.6f});
        }
    }

    // ---- tooltip ------------------------------------------------------------------------------------
    if (m_tipRect.width > 1.0f) {
        if (auto g = m_dcomp->CreateSquircleGeometry(m_tipRect, 12.0f * s)) {
            m_brush->SetColor(D2D1::ColorF(0.05f, 0.05f, 0.065f, 0.96f * m_tipAlpha));
            ctx->FillGeometry(g.Get(), m_brush.Get());
            m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * m_tipAlpha));
            ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.0f * s);
        }
        DrawText(ctx, m_tipText, m_fontTip.Get(), RF(m_tipRect.Left() + 12 * s, m_tipRect.Top(), m_tipRect.Right() - 12 * s, m_tipRect.Bottom()),
                 Color{1.0f, 1.0f, 1.0f, 0.96f * m_tipAlpha});
    }

    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dcomp->EndDraw();
}

// =============================================================================================
//  Interaction
// =============================================================================================

AetherBar::Hit AetherBar::HitTest(const Vec2& pt, int& index) const {
    index = -1;
    const Vec2 p{pt.x, pt.y - LiftPx()};
    if (m_layout.start.Contains(p)) return Hit::Start;
    if (m_layout.search.Contains(p)) return Hit::Search;
    const size_t nItems = std::min(m_layout.items.size(), m_entries.size());     // the layout can lag one frame behind the entry list
    for (size_t i = 0; i < nItems; ++i) {
        if (m_entries[i]->appear.GetValue() > 0.5f && m_layout.items[i].Contains(p)) { index = static_cast<int>(i); return Hit::Item; }
    }
    if (m_layout.chevron.width > 0 && m_layout.chevron.Contains(p)) return Hit::Chevron;
    if (m_layout.quick.width > 0 && m_layout.quick.Contains(p)) return Hit::Quick;
    if (m_layout.network.width > 0 && m_layout.network.Contains(p)) return Hit::Network;
    if (m_layout.lang.width > 0 && m_layout.lang.Contains(p)) return Hit::Lang;
    if (m_layout.clock.Contains(p)) return Hit::Clock;
    if (m_layout.bell.width > 0 && m_layout.bell.Contains(p)) return Hit::Bell;
    if (m_layout.weather.width > 0 && m_layout.weather.Contains(p)) return Hit::Weather;
    if (m_layout.desktop.width > 0 && m_layout.desktop.Contains(p)) return Hit::Desktop;
    if (m_layout.right.Contains(p)) return Hit::Right;
    if (m_layout.dock.Contains(p)) return Hit::Dock;
    return Hit::None;
}

void AetherBar::ActivateWindow(HWND hwnd) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD me = GetCurrentThreadId();
    if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, FALSE);
}

void AetherBar::LaunchEntry(Entry& e) {
    if (e.exePath.empty()) return;
    e.launchTick = GetTickCount64();
    ShellExecuteW(nullptr, L"open", e.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Every top-level window of an entry's app on ANY monitor (the entry itself only holds the ones on its own bar).
std::vector<HWND> AetherBar::WindowsOfEntryEverywhere(const Entry& e) {
    std::vector<HWND> out;
    EnumCtx ctx{{}, m_hwnd};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    for (HWND h : ctx.out) {
        std::wstring exe, key;
        IdentifyWindow(h, exe, key);
        if (IEqual(key, e.key)) out.push_back(h);
    }
    return out;
}

void AetherBar::OnItemClick(Entry& e) {
    if (!e.Running()) {
        // Not running on THIS monitor does not mean not running: bring the window from the other monitor instead of starting a second copy.
        const std::vector<HWND> elsewhere = WindowsOfEntryEverywhere(e);
        if (!elsewhere.empty()) { ActivateWindow(elsewhere.front()); return; }
        if (GetTickCount64() - e.launchTick < 3000) return;          // a double click must not start the app twice
        LaunchEntry(e);
        return;
    }
    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    if (e.windows.size() == 1) {
        HWND w = e.windows[0];
        if ((w == fg || w == fgRoot) && !IsIconic(w)) ShowWindow(w, SW_MINIMIZE);
        else ActivateWindow(w);
        return;
    }
    e.cycle = (e.cycle + 1) % static_cast<int>(e.windows.size());
    ActivateWindow(e.windows[e.cycle]);
}

// ---- drag & drop re-ordering of the buttons ---------------------------------------------------------------------------
// Pinned apps stay in the pinned block (their new order is saved), the others can be re-ordered among themselves.
void AetherBar::BeginDrag() {
    if (m_downIdx < 0 || m_downIdx >= static_cast<int>(m_entries.size())) return;
    m_dragging = true;
    HidePreview();
    m_dragIdx = m_dropIdx = m_downIdx;
    m_hoverKind = Hit::None; m_hoverItem = -1;
}

void AetherBar::UpdateDrag() {
    const int n = static_cast<int>(m_entries.size());
    if (!m_dragging || m_dragIdx < 0 || m_dragIdx >= n) return;
    int pinnedCount = 0;
    for (const auto& e : m_entries) if (e->pinned) ++pinnedCount;
    const bool pinned = m_entries[m_dragIdx]->pinned;
    const int lo = pinned ? 0 : pinnedCount, hi = pinned ? pinnedCount - 1 : n - 1;
    if (hi < lo) return;
    const float pitch = (kItem + kGap) * m_dpi;
    const float first = m_layout.search.Right() + kGap * m_dpi;             // left edge of the first button
    const int target = std::clamp(static_cast<int>(std::floor((m_mouse.x - first) / pitch)), lo, hi);
    ApplyDropIndex(target);
}

void AetherBar::ApplyDropIndex(int target) {
    if (target == m_dropIdx) return;
    const int n = static_cast<int>(m_entries.size());
    ComputeLayout();
    std::vector<float> before(n, 0.0f);
    for (int k = 0; k < n && k < static_cast<int>(m_layout.items.size()); ++k) before[k] = m_layout.items[k].x;
    m_dropIdx = target;
    ComputeLayout();
    for (int k = 0; k < n && k < static_cast<int>(m_layout.items.size()); ++k)   // everybody glides from where it was drawn to where it now belongs
        if (k != m_dragIdx) m_entries[k]->slide += before[k] - m_layout.items[k].x;
}

void AetherBar::EndDrag(bool commit) {
    if (!m_dragging) return;
    const int n = static_cast<int>(m_entries.size());
    ComputeLayout();
    const float drawnX = (m_dragIdx >= 0 && m_dragIdx < static_cast<int>(m_layout.items.size())) ? m_layout.items[m_dragIdx].x : 0.0f;
    m_dragging = false;
    if (commit && m_dragIdx >= 0 && m_dragIdx < n && m_dropIdx >= 0 && m_dropIdx < n && m_dropIdx != m_dragIdx) {
        auto moved = std::move(m_entries[m_dragIdx]);
        m_entries.erase(m_entries.begin() + m_dragIdx);
        m_entries.insert(m_entries.begin() + m_dropIdx, std::move(moved));
        PinStore& store = PinStore::I();
        if (m_entries[m_dropIdx]->pinned) {                                       // persist the new order of the pinned apps
            std::vector<std::wstring> pins;
            for (const auto& e : m_entries) if (e->pinned) pins.push_back(e->exePath);
            store.pins = pins;
            store.Save();
        }
        ComputeLayout();
        if (m_dropIdx < static_cast<int>(m_layout.items.size())) m_entries[m_dropIdx]->slide = drawnX - m_layout.items[m_dropIdx].x;   // glide into the slot
    } else if (m_dragIdx >= 0 && m_dragIdx < n) {
        ComputeLayout();
        if (m_dragIdx < static_cast<int>(m_layout.items.size())) m_entries[m_dragIdx]->slide = drawnX - m_layout.items[m_dragIdx].x;
    }
    m_dragIdx = m_dropIdx = -1;
}

bool PinToBar(const std::wstring& path) {
    PinStore& st = PinStore::I(); st.Load();
    if (path.empty() || st.IndexOf(path) >= 0) return false;
    st.pins.push_back(path); st.Save();
    return true;
}
bool IsPinnedToBar(const std::wstring& path) { PinStore& st = PinStore::I(); st.Load(); return st.IndexOf(path) >= 0; }
void UnpinFromBar(const std::wstring& path) {
    PinStore& st = PinStore::I(); st.Load();
    const int i = st.IndexOf(path);
    if (i >= 0) { st.pins.erase(st.pins.begin() + i); st.Save(); }
}

// ---- pinning by drop ------------------------------------------------------------------------------------------------------
int AetherBar::PinSlotAt(float screenX) const {
    int pinned = 0;
    for (const auto& e : m_entries) if (e->pinned) ++pinned;
    RECT wr{}; GetWindowRect(m_hwnd, &wr);
    const float x = screenX - static_cast<float>(wr.left);
    const float pitch = (kItem + kGap) * m_dpi;
    const float first = m_layout.search.Right() + kGap * m_dpi;
    return std::clamp(static_cast<int>(std::floor((x - first) / pitch + 0.5f)), 0, pinned);
}

void AetherBar::OnPinHover(int sx, int) {
    m_pinHoverTick = GetTickCount64();
    m_pinSlot = PinSlotAt(static_cast<float>(sx));
    const float pitch = (kItem + kGap) * m_dpi;
    m_pinLineX = m_layout.search.Right() + kGap * m_dpi + pitch * static_cast<float>(m_pinSlot) - kGap * m_dpi * 0.5f;
}

void AetherBar::AddPins(const std::vector<std::wstring>& paths, int screenX) {
    PinStore& store = PinStore::I();
    store.Load();
    int slot = std::min(PinSlotAt(static_cast<float>(screenX)), static_cast<int>(store.pins.size()));
    bool changed = false;
    for (const auto& raw : paths) {
        const std::wstring target = ResolvePinTarget(raw);
        if (target.empty() || GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (store.IndexOf(target) >= 0) continue;                                   // already pinned
        store.pins.insert(store.pins.begin() + slot++, target);
        changed = true;
    }
    m_pinHoverTick = 0;
    if (changed) store.Save();                                                      // every bar rebuilds its buttons on the next tick
}

void AetherBar::AdjustVolume(float delta) {
    auto ev = DefaultEndpointVolume();
    if (!ev) return;
    float v = m_volume;
    ev->GetMasterVolumeLevelScalar(&v);
    v = Clamp01(v + delta);
    ev->SetMasterVolumeLevelScalar(v, nullptr);
    if (v > 0.0f) ev->SetMute(FALSE, nullptr);
    m_volume = v; m_muted = false;
    wchar_t buf[32];
    swprintf_s(buf, L"Volume  %d%%", static_cast<int>(v * 100.0f + 0.5f));
    m_tipOverride = buf;
    m_tipOverrideTimer = 1.3f;
}

// "Jump list" of a dock button: the files that were opened lately with this program. Windows keeps them as shortcuts in the Recent folder; a
// file belongs to the program when that program is the one Windows opens the file's type with. Newest first, at most `maxItems`.
static std::vector<std::wstring> RecentFilesOf(const std::wstring& exePath, size_t maxItems) {
    std::vector<std::wstring> out;
    if (exePath.empty() || system::IsAppsFolderPath(exePath)) return out;
    PWSTR rp = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &rp)) || !rp) return out;
    const std::wstring dir = rp; CoTaskMemFree(rp);
    struct Lnk { std::wstring path; ULONGLONG t; };
    std::vector<Lnk> lnks;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*.lnk").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do { lnks.push_back({dir + L"\\" + fd.cFileName, (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime}); } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(lnks.begin(), lnks.end(), [](const Lnk& a, const Lnk& b) { return a.t > b.t; });
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return out;
    ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf))) return out;
    std::map<std::wstring, std::wstring> handlerOf;                       // extension -> program that opens it
    size_t looked = 0;
    for (const auto& l : lnks) {
        if (out.size() >= maxItems || ++looked > 60) break;
        wchar_t target[MAX_PATH] = {};
        if (FAILED(pf->Load(l.path.c_str(), STGM_READ)) || FAILED(sl->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) || !target[0]) continue;
        const DWORD attr = GetFileAttributesW(target);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;      // gone, or a folder
        const wchar_t* ext = PathFindExtensionW(target);
        if (!ext || !*ext) continue;
        auto it = handlerOf.find(ext);
        if (it == handlerOf.end()) {
            wchar_t exe[MAX_PATH] = {}; DWORD n = MAX_PATH;
            const bool ok = SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, ext, L"open", exe, &n));
            it = handlerOf.emplace(ext, ok ? exe : L"").first;
        }
        if (!it->second.empty() && _wcsicmp(it->second.c_str(), exePath.c_str()) == 0) out.push_back(target);
    }
    return out;
}

void AetherBar::ShowItemMenu(int index) {
    if (index < 0 || index >= static_cast<int>(m_entries.size())) return;
    Entry& e = *m_entries[index];
    POINT pt; GetCursorPos(&pt);
    std::vector<aether::ui::MenuEntry> items;
    const std::vector<std::wstring> recent = RecentFilesOf(e.exePath, 6);
    for (size_t i = 0; i < recent.size(); ++i) {
        std::wstring name = recent[i].substr(recent[i].find_last_of(L'\\') + 1);
        if (name.size() > 42) name = name.substr(0, 40) + L"…";
        items.push_back(aether::ui::MenuItem(100 + static_cast<int>(i), name, 0xE8A5));
    }
    if (!recent.empty()) items.push_back(aether::ui::MenuSeparator());
    items.push_back(aether::ui::MenuItem(1, i18n::Pick(L"Open new window", L"Yeni pencere aç"), 0xE8A7));
    if (!e.exePath.empty()) items.push_back(aether::ui::MenuItem(2, e.pinned ? i18n::Pick(L"Unpin from bar", L"Çubuktan kaldır") : i18n::Pick(L"Pin to bar", L"Çubuğa sabitle"), 0xE718));
    if (e.Running()) items.push_back(aether::ui::MenuItem(3, e.windows.size() > 1 ? i18n::Pick(L"Close all windows", L"Tüm pencereleri kapat") : i18n::Pick(L"Close window", L"Pencereyi kapat"), 0xE711, true));
    const int cmd = aether::ui::ShowContextMenu(items, pt, true);
    PinStore& store = PinStore::I();
    if (cmd >= 100 && cmd < 100 + static_cast<int>(recent.size())) {
        const std::wstring& f = recent[static_cast<size_t>(cmd - 100)];
        ShellExecuteW(nullptr, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (cmd == 1 && !e.exePath.empty()) LaunchEntry(e);
    else if (cmd == 2) {
        const int idx = store.IndexOf(e.exePath);
        if (idx >= 0) store.pins.erase(store.pins.begin() + idx); else store.pins.push_back(e.exePath);
        store.Save();
    }
    else if (cmd == 3) for (HWND w : e.windows) PostMessageW(w, WM_CLOSE, 0, 0);
}

void AetherBar::ToggleFlyout(Flyout* panel, const core::Rect& anchor) {
    if (!panel) return;
    if (panel->IsOpen()) { panel->Hide(); return; }
    if (panel->JustDismissed()) return;   // the same click just closed it
    for (Flyout* f : {m_quick, m_calendar, m_tray, m_notify, m_weather, m_net}) if (f && f != panel) f->Hide();
    POINT p = {static_cast<LONG>(anchor.Center().x), 0};
    ClientToScreen(m_hwnd, &p);
    panel->Show(m_mon, m_reservedPx, static_cast<float>(p.x));
}

void AetherBar::ShowBarMenu() {
    POINT pt; GetCursorPos(&pt);
    const int cmd = aether::ui::ShowContextMenu({
        aether::ui::MenuItem(1, i18n::Pick(L"Task Manager", L"Görev Yöneticisi"), 0xE9D9),
        aether::ui::MenuItem(2, i18n::Pick(L"Windows Settings", L"Windows Ayarları"), 0xE713),
        aether::ui::MenuItem(3, i18n::Pick(L"System tray", L"Sistem tepsisi"), 0xE70E),
        aether::ui::MenuSeparator(),
        aether::ui::MenuItem(9, i18n::Pick(L"Exit Aether Bar", L"Aether Bar'dan çık"), 0xE7E8, true, i18n::Pick(L"restores the Windows taskbar", L"Windows görev çubuğunu geri getirir"))}, pt, true);
    if (cmd == 1) ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 2) ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 3) OpenTray();
    else if (cmd == 9) PostQuitMessage(0);
}

LRESULT AetherBar::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == shellkeys::kMsgWinCombo) { shellkeys::g_winComboTick = GetTickCount64(); return 0; }
    if (msg == shellkeys::kMsgActivate) {
        HWND target = reinterpret_cast<HWND>(wp);
        if (target && IsWindow(target)) ActivateWindow(target);
        return 0;
    }
    if (msg == kMsgDemoFlyout) {
        if (m_primary) {
            for (Flyout* f : {m_quick, m_calendar, m_tray, m_notify, m_weather, m_net}) if (f && f->IsOpen()) f->Hide();
            switch (wp) {
                case 1: ToggleFlyout(m_quick, m_layout.quick); break;
                case 2: ToggleFlyout(m_calendar, m_layout.clock); break;
                case 3: ToggleFlyout(m_tray, m_layout.chevron); break;
                case 4: ToggleFlyout(m_weather, m_layout.weather); break;
                case 5: ToggleFlyout(m_notify, m_layout.bell); break;
                case 6: if (m_start && !m_start->IsOpen()) m_start->Show(m_mon, m_reservedPx); break;   // Start menu (the injected Windows key would open Windows' own)
                case 7: if (m_start && m_start->IsOpen()) m_start->Hide(); break;
                default: break;
            }
        }
        return 0;
    }
    if (m_shellHookMsg && msg == m_shellHookMsg) {
        HWND target = reinterpret_cast<HWND>(lp);
        if (wp == 0x8006 /*HSHELL_FLASH*/) { if (target && target != GetForegroundWindow()) m_flash[target] = GetTickCount64(); }
        else if (wp == 4 /*HSHELL_WINDOWACTIVATED*/ || wp == 0x8004 /*HSHELL_RUDEAPPACTIVATED*/) m_flash.erase(target);
        return 0;
    }
    if (m_taskbarCreatedMsg && msg == m_taskbarCreatedMsg) {
        UnregisterAppBar(); RegisterAppBar();                          // the new Explorer knows nothing about our work-area reservation
        PostThreadMessageW(GetCurrentThreadId(), WM_APP + 3, 0, 0);   // manager re-applies the guard
        return 0;
    }
    if (msg == system::barpin::MsgHover()) { OnPinHover(static_cast<int>(wp), static_cast<int>(lp)); return 0; }
    switch (msg) {
        case WM_COPYDATA: {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (!cds || cds->dwData != system::barpin::kDropData || !cds->lpData || cds->cbData < sizeof(wchar_t)) break;
            const std::wstring blob(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
            std::vector<std::wstring> lines; size_t from = 0;
            while (from < blob.size()) {
                size_t to = blob.find(L'\n', from);
                if (to == std::wstring::npos) to = blob.size();
                std::wstring ln = blob.substr(from, to - from);
                if (!ln.empty() && ln.back() == L'\0') ln.pop_back();
                if (!ln.empty()) lines.push_back(ln);
                from = to + 1;
            }
            if (lines.size() < 2) return TRUE;
            const int sx = _wtoi(lines[0].c_str());
            lines.erase(lines.begin());
            AddPins(lines, sx);
            return TRUE;
        }
        case WM_APPBAR:
            if (wp == ABN_POSCHANGED) SetAppBarPos();
            else if (wp == ABN_FULLSCREENAPP) {
                m_visible = !lp;
                ShowWindow(hwnd, m_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETDESKWALLPAPER) SetTimer(hwnd, 77, 1500, nullptr);   // let the new wallpaper settle first
            break;
        case WM_TIMER:
            if (wp == 77) { KillTimer(hwnd, 77); RefreshBackdrop(true); return 0; }
            break;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            PostThreadMessageW(GetCurrentThreadId(), WM_APP + 2, 0, 0);   // manager rebuilds all bars
            return 0;
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            int idx;
            return HitTest(Vec2{static_cast<float>(pt.x), static_cast<float>(pt.y)}, idx) == Hit::None ? HTTRANSPARENT : HTCLIENT;
        }
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const bool hand = m_hoverKind != Hit::None && m_hoverKind != Hit::Dock && m_hoverKind != Hit::Right;
                ui::ApplyCursor(hand);
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            m_mouse = Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            if (m_downIdx >= 0 && (wp & MK_LBUTTON)) {           // left button held on a button: start / continue a drag
                if (!m_dragging && (std::fabs(m_mouse.x - m_downPt.x) > 8.0f * m_dpi || std::fabs(m_mouse.y - m_downPt.y) > 8.0f * m_dpi)) BeginDrag();
                if (m_dragging) UpdateDrag();
            }
            if (!m_dragging) m_hoverKind = HitTest(m_mouse, m_hoverItem);
            if (!m_trackingMouse) {
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                m_trackingMouse = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            m_trackingMouse = false; m_hoverKind = Hit::None; m_hoverItem = -1; m_mouse = Vec2{-1.0f, -1.0f};
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(pt.x), static_cast<float>(pt.y)}, idx);
            if (m_primary && h == Hit::Quick) {   // only the volume icon reacts to the wheel
                AdjustVolume(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 0.03f);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int idx;
            const Vec2 pt{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            const Hit h = HitTest(pt, idx);
            m_downIdx = (h == Hit::Item) ? idx : -1;
            m_downPt = pt;
            HidePreview();
            if (m_downIdx >= 0) SetCapture(hwnd);                 // keep the drag alive when the cursor leaves the bar's shape
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (m_dragging) EndDrag(false);
            m_downIdx = -1;
            return 0;
        case WM_LBUTTONUP: {
            if (m_dragging) { EndDrag(true); ReleaseCapture(); m_downIdx = -1; return 0; }
            const int pressedOn = m_downIdx;
            m_downIdx = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, idx);
            if (h == Hit::Item && pressedOn >= 0 && idx != pressedOn) return 0;     // pressed on one button, released on another: not a click
            switch (h) {
                case Hit::Start:   if (m_start) m_start->Toggle(m_mon, m_reservedPx); break;
                case Hit::Search:  if (m_start && !m_start->IsOpen()) m_start->Show(m_mon, m_reservedPx); break;   // opens with the search box focused
                case Hit::Item:    if (idx >= 0 && idx < static_cast<int>(m_entries.size())) OnItemClick(*m_entries[idx]); break;
                case Hit::Chevron: ToggleFlyout(m_tray, m_layout.chevron); break;      // Aether tray
                case Hit::Quick:   ToggleFlyout(m_quick, m_layout.quick); break;       // quick settings
                case Hit::Network: ToggleFlyout(m_net, m_layout.network); break;       // own network centre
                case Hit::Lang:    SwitchInputLanguage(); break;                         // next keyboard layout of the window in front
                case Hit::Clock:   ToggleFlyout(m_calendar, m_layout.clock); break;    // clock + calendar
                case Hit::Bell:    ToggleFlyout(m_notify, m_layout.bell); break;   // Aether notification centre
                case Hit::Weather: ToggleFlyout(m_weather, m_layout.weather); break;     // animated weather panel
                case Hit::Desktop: {
                    // AetherDesktop does "show desktop" itself; a synthetic Win+D would reach Explorer and raise its own bare desktop over ours
                    HWND dw = FindWindowW(L"AetherDesktopWindow", nullptr);
                    if (dw) PostMessageW(dw, WM_APP + 4, 0, 0); else SendWinCombo('D');
                    break;
                }
                default: break;
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, idx);
            if (h == Hit::Item && idx >= 0 && idx < static_cast<int>(m_entries.size()) && !m_entries[idx]->exePath.empty()) {
                ShellExecuteW(nullptr, L"open", m_entries[idx]->exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (h == Hit::Quick) {
                if (auto ev = DefaultEndpointVolume()) { ev->SetMute(!m_muted, nullptr); m_muted = !m_muted; }
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            int idx;
            const Hit h = HitTest(Vec2{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, idx);
            HidePreview();
            if (h == Hit::Item) ShowItemMenu(idx); else ShowBarMenu();
            return 0;
        }
        case WM_QUERYENDSESSION:
        case WM_ENDSESSION:
            ShellTaskbarGuard::Instance().Restore();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool AetherBar::Frame(float dt) {
    render::glass::SyncTheme();
    if (!m_hwnd) return false;
    Tick(dt);
    if (m_layout.items.size() != m_entries.size()) ComputeLayout();    // entries were added / removed: never leave the layout stale
    const bool previewAnim = m_preview && m_preview->Frame(dt);
    const bool animating = IsAnimating() || previewAnim;
    m_idleRender += dt;
    if (animating || m_idleRender >= 0.5f || m_hoverKind != Hit::None) {
        m_idleRender = 0.0f;
        Render();
    }
    return animating;
}

} // namespace aether::bar
