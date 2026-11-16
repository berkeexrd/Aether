#pragma once
// Icon packs: a folder of PNG files named after programs replaces the icons of those programs in the taskbar, the Start menu and Alt+Tab.
//
//   <Aether folder>\iconpacks\<Pack>\chrome.png                       bundled packs
//   %APPDATA%\Aether\iconpacks\<Pack>\chrome.png                      the user's own packs (same name = theirs wins)
//   %APPDATA%\Aether\iconpack.txt                                     name of the active pack (missing / empty = the system's own icons)
//
// A file is picked by the executable's name without extension ("Code.exe" -> code.png) or, for Store apps, by the part of the AppUserModelID
// before the "_" ("Microsoft.WindowsCalculator_8wekyb3d8bbwe!App" -> microsoft.windowscalculator.png). Programs without a file keep their own icon.
// Every Aether process reads the same setting file, so a change in the settings reaches the bar, the switcher and the Start menu within seconds.
#include <windows.h>
#include <shellapi.h>
#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace aether::system::iconpack {

inline std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}
inline std::wstring AppDataDir() {
    wchar_t b[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    return (!n || n >= MAX_PATH) ? std::wstring() : std::wstring(b) + L"\\Aether";
}
inline std::wstring BundledRoot() { return ExeDir() + L"\\iconpacks"; }
inline std::wstring UserRoot() { const std::wstring a = AppDataDir(); return a.empty() ? a : a + L"\\iconpacks"; }
inline std::wstring SettingFile() { const std::wstring a = AppDataDir(); return a.empty() ? a : a + L"\\iconpack.txt"; }

inline std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    return s;
}
inline std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}
inline bool IsDir(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
inline bool IsFile(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

inline bool SafeName(const std::wstring& n) {
    if (n.empty() || n == L"." || n == L"..") return false;
    for (wchar_t c : n) if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') return false;
    return true;
}
inline std::wstring ResolveDir(const std::wstring& name) {
    if (!SafeName(name)) return {};
    const std::wstring u = UserRoot();
    if (!u.empty() && IsDir(u + L"\\" + name)) return u + L"\\" + name;
    if (IsDir(BundledRoot() + L"\\" + name)) return BundledRoot() + L"\\" + name;
    return {};
}

struct State { bool loaded{false}, has{false}; ULONGLONG checked{0}; FILETIME stamp{}; unsigned version{0}; std::wstring name, dir; };
inline State& St() { static State s; return s; }

// Bumps whenever the active pack changes (cheap: the setting file is looked at most every 1.5 s). Callers drop their icon caches on a new value.
inline unsigned Version() {
    State& s = St();
    const ULONGLONG now = GetTickCount64();
    if (s.loaded && now - s.checked < 1500) return s.version;
    s.checked = now;
    const std::wstring f = SettingFile();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    const bool has = !f.empty() && GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa);
    FILETIME ft{}; if (has) ft = fa.ftLastWriteTime;
    if (!s.loaded || has != s.has || CompareFileTime(&ft, &s.stamp) != 0) {
        s.loaded = true; s.has = has; s.stamp = ft; s.name.clear();
        if (has) {
            HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                char buf[512] = {}; DWORD n = 0; ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr); CloseHandle(h);
                std::string str(buf, n);
                if (str.size() >= 3 && static_cast<unsigned char>(str[0]) == 0xEF && static_cast<unsigned char>(str[1]) == 0xBB && static_cast<unsigned char>(str[2]) == 0xBF) str.erase(0, 3);
                while (!str.empty() && (str.back() == '\n' || str.back() == '\r' || str.back() == ' ')) str.pop_back();
                s.name = FromUtf8(str);
            }
        }
        s.dir = ResolveDir(s.name);
        ++s.version;
    }
    return s.version;
}
inline bool Active() { Version(); return !St().dir.empty(); }
inline std::wstring ActiveName() { Version(); return St().dir.empty() ? std::wstring() : St().name; }

inline void SetActive(const std::wstring& name) {
    const std::wstring f = SettingFile();
    if (f.empty()) return;
    CreateDirectoryW(AppDataDir().c_str(), nullptr);
    if (name.empty()) DeleteFileW(f.c_str());
    else {
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { const std::string u = ToUtf8(name); DWORD w = 0; WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &w, nullptr); CloseHandle(h); }
    }
    St().checked = 0;            // this process notices at once
}

// All pack names (user packs and bundled ones), sorted, without duplicates.
inline std::vector<std::wstring> ListPacks() {
    std::vector<std::wstring> out;
    for (const std::wstring& root : {UserRoot(), BundledRoot()}) {
        if (root.empty()) continue;
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            bool dup = false; for (const auto& n : out) if (_wcsicmp(n.c_str(), fd.cFileName) == 0) { dup = true; break; }
            if (!dup) out.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

inline void OpenUserFolder() {
    const std::wstring a = AppDataDir(), u = UserRoot();
    if (u.empty()) return;
    CreateDirectoryW(a.c_str(), nullptr); CreateDirectoryW(u.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }

// File-name candidates for a program: an exe path, "shell:AppsFolder\<AUMID>", a bare AUMID or a classic AppsFolder parsing name.
inline std::vector<std::wstring> Keys(std::wstring s) {
    std::vector<std::wstring> k;
    if (s.size() > 17 && _wcsnicmp(s.c_str(), L"shell:AppsFolder\\", 17) == 0) s.erase(0, 17);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {                                   // a path: the file name without extension
        std::wstring f = s.substr(slash + 1);
        const size_t dot = f.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) f.resize(dot);
        k.push_back(Lower(f));
        return k;
    }
    std::wstring a = s;                                                  // an AUMID: "Package_hash!App" / "Vendor.Product"
    const size_t us = a.find(L'_'); if (us != std::wstring::npos) a.resize(us);
    const size_t ex = a.find(L'!'); if (ex != std::wstring::npos) a.resize(ex);
    a = Lower(a);
    if (!a.empty()) k.push_back(a);
    const size_t e = a.find(L".exe");                                    // "Microsoft.Office.WINWORD.EXE.15" -> winword
    if (e != std::wstring::npos && e > 0) { const size_t p = a.find_last_of(L'.', e - 1); k.push_back(a.substr(p == std::wstring::npos ? 0 : p + 1, e - (p == std::wstring::npos ? 0 : p + 1))); }
    return k;
}

// The PNG the active pack has for this program, or "" (no pack active / no file for it).
inline std::wstring Find(const std::wstring& program) {
    Version();
    const State& s = St();
    if (s.dir.empty() || program.empty()) return {};
    for (const std::wstring& key : Keys(program)) {
        if (!SafeName(key)) continue;
        const std::wstring p = s.dir + L"\\" + key + L".png";
        if (IsFile(p)) return p;
    }
    return {};
}

inline Microsoft::WRL::ComPtr<ID2D1Bitmap1> LoadBitmap(ID2D1DeviceContext* ctx, const std::wstring& png) {
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> out;
    if (!ctx || png.empty()) return out;
    Microsoft::WRL::ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return out;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return out;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return out;
    Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return out;
    if (FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return out;
    ctx->CreateBitmapFromWicBitmap(conv.Get(), &out);
    return out;
}

} // namespace aether::system::iconpack
