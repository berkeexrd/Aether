#include "aether/bar/native_ui_guard.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>

namespace aether::bar {

namespace {
std::atomic<bool> g_run{false};
HANDLE g_thread{nullptr};
HANDLE g_stop{nullptr};

// Host processes of the Windows surfaces Aether replaces. (ShellHost.exe is left alone: Explorer itself relies on it.)
const wchar_t* const kHosts[] = {
    L"StartMenuExperienceHost.exe",   // Start
    L"SearchHost.exe", L"SearchApp.exe", L"SearchUI.exe",   // Search
    L"Widgets.exe", L"WidgetService.exe", L"WidgetBoard.exe", L"MicrosoftStartFeedProvider.exe",   // Widgets / news / Start feed
    L"ShellExperienceHost.exe",       // quick settings, calendar, notification centre, volume / network flyouts
};

bool IsHost(const wchar_t* exe) {
    for (const wchar_t* n : kHosts) if (_wcsicmp(exe, n) == 0) return true;
    return false;
}

void Sweep() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    const DWORD me = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == me || !IsHost(pe.szExeFile)) continue;
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID)) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    CloseHandle(snap);
}

DWORD WINAPI GuardThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (g_run) {
        Sweep();
        if (WaitForSingleObject(g_stop, 400) == WAIT_OBJECT_0) break;
    }
    return 0;
}
} // namespace

void NativeUiGuard::Start() {
    if (g_run.exchange(true)) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, GuardThread, nullptr, 0, nullptr);
}

void NativeUiGuard::Stop() {
    if (!g_run.exchange(false)) return;
    if (g_stop) SetEvent(g_stop);
    if (g_thread) { WaitForSingleObject(g_thread, 1500); CloseHandle(g_thread); g_thread = nullptr; }
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
}

} // namespace aether::bar
