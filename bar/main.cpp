#include <windows.h>
#include <shellapi.h>
#include <cstdlib>
#include <objbase.h>
#include <shellscalingapi.h>
#include <chrono>
#include <optional>
#include <cwchar>
#include <memory>
#include <vector>
#include "aether/bar/aether_bar.hpp"
#include "aether/bar/start_menu.hpp"
#include "aether/bar/flyouts.hpp"
#include "aether/bar/panels.hpp"
#include "aether/bar/tray_host.hpp"
#include "aether/bar/toast_host.hpp"
#include "aether/bar/notifications.hpp"
#include "aether/bar/weather.hpp"
#include "aether/core/i18n.hpp"
#include "aether/core/monitors.hpp"
#include "aether/system/shell_keys.hpp"
#include "aether/core/crash.hpp"
#include <atomic>

using namespace aether::bar;

namespace {

constexpr UINT kMsgToggleStart = WM_APP + 1;   // Win key tapped
constexpr UINT kMsgRebuild     = WM_APP + 2;   // monitors / DPI changed
constexpr UINT kMsgReapplyHide = WM_APP + 3;   // Explorer restarted

DWORD g_mainThread = 0;
HHOOK g_hook = nullptr;                 // owned by the hook thread
DWORD g_hookThreadId = 0;
HANDLE g_hookThread = nullptr;
bool  g_winDown = false;                // hook thread only
bool  g_combo = false;                  // hook thread only
std::atomic<HWND> g_shellCtl{nullptr};  // AetherShell's control window (it owns the Alt+Tab switcher and the run box)
std::atomic<bool> g_shellElevated{false};

bool IsPidElevated(DWORD pid) {
    if (!pid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return true;                                  // cannot even look at it: it is more privileged than we may see
    bool elevated = true;
    HANDLE tok = nullptr;
    if (OpenProcessToken(p, TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te{}; DWORD n = 0;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n)) elevated = te.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    CloseHandle(p);
    return elevated;
}

void RefreshShellInfo() {
    HWND c = FindWindowExW(HWND_MESSAGE, nullptr, aether::shellkeys::kCtlClass, nullptr);
    g_shellCtl = c;
    DWORD pid = 0;
    if (c) GetWindowThreadProcessId(c, &pid);
    g_shellElevated = pid != 0 && IsPidElevated(pid);
}

bool PostToShell(UINT m, WPARAM w, LPARAM l) {
    HWND c = g_shellCtl;
    return c && PostMessageW(c, m, w, l) != 0;
}

// AetherShell's own hook cannot see keys while an elevated window (admin console, our own Start menu ...) has focus: UIPI.
// In exactly that case this (elevated) process takes the shortcuts over, so Windows' own Alt+Tab / Win+R never show up.
bool ElevatedForeground() {
    if (!g_shellCtl.load() || g_shellElevated.load()) return false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return IsPidElevated(pid);
}

// Win key tap (press + release with nothing in between) opens Aether Start instead of the stock one.
// Win+E, Win+D, Win+B ... still work because those count as a combo.
LRESULT CALLBACK KeyboardHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (aether::shellkeys::Route(k, wp, PostToShell, ElevatedForeground)) return 1;
        if (!(k->flags & LLKHF_INJECTED)) {
            const bool isWin = (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN);
            const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            if (isWin) {
                if (down) {
                    if (!g_winDown) { g_winDown = true; g_combo = false; }
                } else {
                    // AetherShell may have consumed a Win+<key> shortcut before this hook ever saw the key: it tells us (g_winComboTick)
                    const bool shellCombo = GetTickCount64() - aether::shellkeys::g_winComboTick.load() < 1500;
                    if (g_winDown && !g_combo && !shellCombo) {
                        // Inject an unassigned key so Windows treats this as Win+<key> and skips its own Start
                        aether::shellkeys::MaskKeys();
                        PostThreadMessageW(g_mainThread, kMsgToggleStart, 0, 0);
                    }
                    g_winDown = false;
                }
            } else if (down && g_winDown) {
                g_combo = true;
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

// The hook gets a thread of its own. On the render thread a slow frame (icon loading, window enumeration) made the hook answer
// too late, and Windows then drops such a hook silently: no Start menu on the Win key until the bar was restarted.
DWORD WINAPI HookThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    RefreshShellInfo();
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
    SetTimer(nullptr, 1, 3000, nullptr);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message != WM_TIMER) continue;
        RefreshShellInfo();
        if (g_winDown || aether::shellkeys::g_swActive) continue;
        if (HHOOK fresh = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0)) {   // fresh hook in case the system dropped ours
            if (g_hook) UnhookWindowsHookEx(g_hook);
            g_hook = fresh;
        }
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    return 0;
}

void StartKeyHook() { g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId); }
void StopKeyHook() {
    if (!g_hookThread) return;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hookThread, 2000);
    CloseHandle(g_hookThread); g_hookThread = nullptr;
}

class BarManager {
public:
    explicit BarManager(bool hardHide) : m_hard(hardHide) {}

    bool Init() {
        if (!m_start.Create()) return false;
        m_start.PreloadAsync();
        WeatherService::Instance().Start();
        // Panels use unique window classes; they are shared by every bar.
        m_quick.Create(L"Aether.QuickPanel");
        m_calendar.Create(L"Aether.CalendarPanel");
        m_tray.Create(L"Aether.TrayPanel");
        m_notify.Create(L"Aether.NotifyPanel");
        m_weatherPanel.Create(L"Aether.WeatherPanel");
        m_net.Create(L"Aether.NetPanel");
        // Aether's own notification pop-ups; Windows' banners are hidden while they are on
        m_toast.Create();
        NotificationFeed::Instance().StartWatching();
        NativeToastHider::Enable(aether::system::notifycfg::Load().enabled);
        NativeToastHider::Start();
        ShellTaskbarGuard::Instance().Hide(m_hard);
        Rebuild();
        return !m_bars.empty();
    }

    // The tray host is torn down while the bars are rebuilt and created again once every bar has
    // registered its AppBar, so apps re-send their icons (TaskbarCreated broadcast).
    void Rebuild() {
        TrayHost::Instance().Destroy();
        m_bars.clear();
        for (HMONITOR mon : aether::core::EnumerateMonitors()) {
            MONITORINFO mi = {sizeof(mi)};
            GetMonitorInfoW(mon, &mi);
            auto bar = std::make_unique<AetherBar>(mon, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, &m_start);
            if (!bar->Create()) continue;
            bar->SetPanels(&m_quick, &m_calendar, &m_tray);
            bar->SetExtraPanels(&m_notify, &m_weatherPanel, &m_net);
            m_bars.push_back(std::move(bar));
        }
        if (!m_bars.empty()) TrayHost::Instance().Create();
    }

    void Shutdown() {
        NativeToastHider::Stop();
        NotificationFeed::Instance().StopWatching();
        m_toast.Shutdown();
        WeatherService::Instance().Stop();
        TrayHost::Instance().Destroy();   // hands the icons back to Explorer
        m_bars.clear();
    }

    void ToggleStart() {
        POINT pt; GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        int reserved = static_cast<int>(82.0f * aether::core::MonitorScale(mon));
        for (auto& b : m_bars) if (b->Monitor() == mon) reserved = b->ReservedPx();
        m_start.Toggle(mon, reserved);
    }

    void ReapplyHide() { ShellTaskbarGuard::Instance().Hide(m_hard); }

    bool Frame(float dt) {
        if (m_rebuildAt && std::chrono::steady_clock::now() >= *m_rebuildAt) { m_rebuildAt.reset(); Rebuild(); }
        bool animating = false;
        for (auto& b : m_bars) animating |= b->Frame(dt);
        animating |= m_start.Frame(dt);
        animating |= m_quick.Frame(dt);
        animating |= m_calendar.Frame(dt);
        animating |= m_tray.Frame(dt);
        animating |= m_notify.Frame(dt);
        animating |= m_weatherPanel.Frame(dt);
        animating |= m_net.Frame(dt);
        animating |= m_toast.Frame(dt);
        m_pruneTimer += dt;
        if (m_pruneTimer >= 3.0f) { m_pruneTimer = 0.0f; TrayHost::Instance().Prune(); }
        return animating || m_rebuildAt.has_value();
    }

    void ScheduleRebuild() { m_rebuildAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(600); }

private:
    bool m_hard;
    StartMenu m_start;
    QuickPanel m_quick;
    CalendarPanel m_calendar;
    TrayPanel m_tray;
    NotifyPanel m_notify;
    WeatherPanel m_weatherPanel;
    NetPanel m_net;
    ToastHost m_toast;
    float m_pruneTimer{0.0f};
    std::vector<std::unique_ptr<AetherBar>> m_bars;
    std::optional<std::chrono::steady_clock::time_point> m_rebuildAt;
};

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    ShellTaskbarGuard::Instance().Restore();                // the Windows taskbar must come back even when we die
    return aether::core::CrashFilter(ep);                   // mini dump + crash.log in %APPDATA%\Aether\crash
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // AetherBar.exe --restore : emergency switch to bring the stock Windows taskbar back
    if (cmdLine && wcsstr(cmdLine, L"--restore")) {
        ShellTaskbarGuard::Instance().Restore();
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Aether.Bar.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    aether::core::InstallCrashHandler(L"AetherBar");
    SetUnhandledExceptionFilter(CrashHandler);
    atexit([] { ShellTaskbarGuard::Instance().Restore(); });

    // The stock taskbar is hidden completely (our own tray replaces it). --keep-stock leaves it on auto-hide.
    aether::i18n::SetFromCommandLine(cmdLine);
    const bool hard = !(cmdLine && wcsstr(cmdLine, L"--keep-stock"));
    const bool useHook = !(cmdLine && wcsstr(cmdLine, L"--no-start-hook"));

    int rc = 0;
    {
        BarManager manager(hard);
        if (!manager.Init()) {
            ShellTaskbarGuard::Instance().Restore();
            MessageBoxW(nullptr, L"Could not create the Aether Bar window.", L"Aether Bar", MB_ICONERROR);
            return 1;
        }

        g_mainThread = GetCurrentThreadId();
        if (useHook) StartKeyHook();

        LARGE_INTEGER freq, last;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
        bool running = true;
        while (running) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { running = false; break; }
                if (!msg.hwnd) {
                    if (msg.message == kMsgToggleStart) { manager.ToggleStart(); continue; }
                    if (msg.message == kMsgRebuild)     { manager.ScheduleRebuild(); continue; }
                    if (msg.message == kMsgReapplyHide) { manager.ReapplyHide(); continue; }
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running) break;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart);
            last = now;
            dt = dt > 0.1f ? 0.1f : dt;

            if (!manager.Frame(dt)) MsgWaitForMultipleObjectsEx(0, nullptr, 120, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        StopKeyHook();
        manager.Shutdown();
    }
    ShellTaskbarGuard::Instance().Restore();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}
