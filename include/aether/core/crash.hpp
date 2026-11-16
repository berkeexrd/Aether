#pragma once
// Crash diagnostics shared by every Aether program.
//  * An unhandled exception writes a mini dump + a line to %APPDATA%\Aether\crash\ so a crash can be analysed afterwards.
//  * Guard() runs a worker-thread body so that an exception inside it is logged instead of killing the whole process
//    (an exception that leaves a std::thread calls std::terminate).
#include <windows.h>
#include <dbghelp.h>
#include <exception>
#include <thread>
#include <utility>
#include <string>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "dbghelp.lib")

namespace aether::core {

inline std::wstring CrashDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(buf) + L"\\Aether";
    CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\crash";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

inline const wchar_t*& CrashName() { static const wchar_t* n = L"Aether"; return n; }

inline void AppendCrashLine(const std::string& text) {
    const std::wstring dir = CrashDir();
    if (dir.empty()) return;
    const std::wstring path = dir + L"\\crash.log";
    if (FILE* f = _wfopen(path.c_str(), L"ab")) {
        SYSTEMTIME st; GetLocalTime(&st);
        char head[96];
        snprintf(head, sizeof(head), "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        const std::string line = std::string(head) + text + "\n";
        fwrite(line.data(), 1, line.size(), f);
        fclose(f);
    }
}

inline void WriteDump(EXCEPTION_POINTERS* ep) {
    const std::wstring dir = CrashDir();
    if (dir.empty()) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[128];
    swprintf(name, 128, L"\\%ls-%04d%02d%02d-%02d%02d%02d.dmp", CrashName(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE f = CreateFileW((dir + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId(); mei.ExceptionPointers = ep; mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

inline LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%ls: unhandled exception 0x%08lX at %p", CrashName(),
             ep && ep->ExceptionRecord ? static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode) : 0ul,
             ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    AppendCrashLine(buf);
    WriteDump(ep);
    return EXCEPTION_CONTINUE_SEARCH;                 // let Windows end the process (the shell restarts it)
}

inline void OnTerminate() {
    std::string what = "std::terminate";
    try { if (auto e = std::current_exception()) std::rethrow_exception(e); }
    catch (const std::exception& ex) { what += std::string(": ") + ex.what(); }
    catch (...) { what += ": unknown exception"; }
    AppendCrashLine(what);
    WriteDump(nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

inline void InstallCrashHandler(const wchar_t* programName) {
    CrashName() = programName;
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(OnTerminate);
}

// Runs `f`; an exception is logged and swallowed. Use as the whole body of a worker thread.
template <class F> inline void Guard(const char* what, F&& f) noexcept {
    try { f(); }
    catch (const std::exception& ex) { AppendCrashLine(std::string("worker '") + what + "' failed: " + ex.what()); }
    catch (...) { AppendCrashLine(std::string("worker '") + what + "' failed: unknown exception"); }
}

// A std::thread whose body is guarded (see Guard). Drop-in for std::thread(lambda).
template <class F> inline std::thread SafeThread(const char* what, F f) {
    return std::thread([what, f = std::move(f)]() mutable { Guard(what, f); });
}

} // namespace aether::core
