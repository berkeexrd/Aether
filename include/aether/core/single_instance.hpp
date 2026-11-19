// Single-instance guard shared by the Aether executables.
//
// A plain CreateMutexW(nullptr, ...) gets a default DACL: when the first copy runs elevated (AetherBar always does, and any exe
// started from an elevated installer / shell does too) a normal copy cannot open the mutex at all. It gets NULL + ERROR_ACCESS_DENIED
// instead of ERROR_ALREADY_EXISTS, so the usual "if (GetLastError() == ERROR_ALREADY_EXISTS) return" check let it through and a second
// island / desktop / bar stacked on top of the first. The same goes for named events used to talk to a running copy.
// Everything here is open to everyone (with a low integrity label) and ACCESS_DENIED counts as "already running".
#pragma once
#include <windows.h>
#include <sddl.h>

namespace aether::core {

// Security attributes for a named kernel object that every process of the user (elevated or not) can open. Keep alive across the Create call.
class OpenAcl {
public:
    OpenAcl() {
        m_sa.nLength = sizeof(m_sa);
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &m_sd, nullptr))
            m_sa.lpSecurityDescriptor = m_sd;
    }
    ~OpenAcl() { if (m_sd) LocalFree(m_sd); }
    OpenAcl(const OpenAcl&) = delete;
    OpenAcl& operator=(const OpenAcl&) = delete;
    SECURITY_ATTRIBUTES* Get() { return m_sd ? &m_sa : nullptr; }
private:
    SECURITY_ATTRIBUTES m_sa{};
    PSECURITY_DESCRIPTOR m_sd{nullptr};
};

// true: this process is the only copy (keep *handle alive for the whole run). false: another copy already runs.
inline bool AcquireSingleInstance(const wchar_t* name, HANDLE* handle = nullptr) {
    OpenAcl acl;
    HANDLE h = CreateMutexW(acl.Get(), FALSE, name);
    const DWORD err = GetLastError();
    if (!h) return err != ERROR_ACCESS_DENIED;     // denied = someone else owns it; any other failure: do not block start-up
    if (err == ERROR_ALREADY_EXISTS) { CloseHandle(h); return false; }
    if (handle) *handle = h;                        // intentionally never closed: the OS releases it when the process ends
    return true;
}

} // namespace aether::core
