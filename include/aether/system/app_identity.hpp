#pragma once
// Which application does a window belong to?
// Windows' own (Store / UWP) apps - Settings, Calculator, Photos ... - all live in ApplicationFrameHost.exe, so the process path says
// nothing about them (and the plain exe icon is wrong). The window's AppUserModelID does: it identifies the app, can be launched
// through the shell's AppsFolder and gives the real tile icon.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propidl.h>
#include <wrl/client.h>
#include <cwchar>
#include <string>
#include <unordered_map>

namespace aether::system {

inline std::wstring FileNameOf(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

inline bool IsApplicationFrameHost(const std::wstring& exe) {
    return _wcsicmp(FileNameOf(exe).c_str(), L"ApplicationFrameHost.exe") == 0;
}

// "shell:AppsFolder\<AUMID>": accepted by ShellExecute (launch) and SHCreateItemFromParsingName (real app icon).
inline std::wstring AppsFolderPath(const std::wstring& aumid) { return L"shell:AppsFolder\\" + aumid; }
inline bool IsAppsFolderPath(const std::wstring& p) { return _wcsnicmp(p.c_str(), L"shell:AppsFolder\\", 17) == 0; }

// The AppUserModelID of a window (empty when it has none). Cached per window: it never changes.
inline std::wstring WindowAumid(HWND h) {
    static std::unordered_map<HWND, std::wstring> cache;
    if (auto it = cache.find(h); it != cache.end()) return it->second;
    static const PROPERTYKEY kAumid = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};   // PKEY_AppUserModel_ID
    std::wstring out;
    Microsoft::WRL::ComPtr<IPropertyStore> ps;
    using GetStoreFn = HRESULT (WINAPI*)(HWND, REFIID, void**);
    static const GetStoreFn getStore = [] {
        HMODULE m = GetModuleHandleW(L"shell32.dll");
        if (!m) m = LoadLibraryW(L"shell32.dll");
        return m ? reinterpret_cast<GetStoreFn>(GetProcAddress(m, "SHGetPropertyStoreForWindow")) : nullptr;   // Vista+, resolved at run time so no header / SDK guard is needed
    }();
    if (getStore && SUCCEEDED(getStore(h, IID_PPV_ARGS(&ps))) && ps) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(kAumid, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
        PropVariantClear(&pv);
    }
    if (cache.size() > 512) cache.clear();
    cache[h] = out;
    return out;
}

} // namespace aether::system
