#pragma once
// The desktop's look lives in %APPDATA%\Aether\desktop.cfg (key=value lines, UTF-8). AetherDesktop watches the file and applies changes
// within a second, so the Settings Center edits the very same file: it changes only the keys it owns and keeps every other line
// (icon positions, other monitors) exactly as it found them.
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace aether::system::desktopcfg {

inline std::wstring Path() {
    wchar_t b[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::wstring(b) + L"\\Aether\\desktop.cfg";
}

inline std::wstring ReadAll(const std::wstring& p) {
    std::wstring w;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return w;
    std::string buf(1 << 20, '\0'); DWORD n = 0;
    ReadFile(h, buf.data(), static_cast<DWORD>(buf.size() - 1), &n, nullptr);
    CloseHandle(h);
    buf.resize(n);
    if (buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF && static_cast<unsigned char>(buf[1]) == 0xBB && static_cast<unsigned char>(buf[2]) == 0xBF) buf.erase(0, 3);
    const int wl = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0);
    w.assign(static_cast<size_t>(std::max(0, wl)), L'\0');
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), w.data(), wl);
    return w;
}

inline std::vector<std::wstring> Lines(const std::wstring& w) {
    std::vector<std::wstring> v; size_t pos = 0;
    while (pos < w.size()) {
        size_t e = w.find(L'\n', pos); if (e == std::wstring::npos) e = w.size();
        std::wstring line = w.substr(pos, e - pos); pos = e + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) v.push_back(std::move(line));
    }
    return v;
}

// What the Settings Center shows; the defaults are the desktop's own defaults for keys that are not in the file yet.
struct Look {
    int iconSize{48}, glow{100}, label{0}, accent{0}, dim{0}, blur{0}, fit{-1}, slideMin{5}, sort{0};
    bool autoArrange{false}, clock{false}, slide{false}, icons{true}, pc{true}, bin{true}, user{false}, net{false}, aether{true};
    std::wstring slideDir;
};

inline Look Load() {
    Look l;
    for (const std::wstring& line : Lines(ReadAll(Path()))) {
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        const int iv = _wtoi(v.c_str());
        if (k == L"isize") l.iconSize = std::clamp(iv, 32, 110);
        else if (k == L"glow") l.glow = std::clamp(iv, 0, 200);
        else if (k == L"label") l.label = std::clamp(iv, 0, 2);
        else if (k == L"accent") l.accent = std::clamp(iv, 0, 5);
        else if (k == L"dim") l.dim = std::clamp(iv, 0, 70);
        else if (k == L"blur") l.blur = std::clamp(iv, 0, 3);
        else if (k == L"fit") l.fit = iv;
        else if (k == L"slidemin") l.slideMin = std::clamp(iv, 1, 600);
        else if (k == L"sort") l.sort = std::clamp(iv, 0, 3);
        else if (k == L"auto") l.autoArrange = iv != 0;
        else if (k == L"clock") l.clock = iv != 0;
        else if (k == L"slide") l.slide = iv != 0;
        else if (k == L"icons") l.icons = iv != 0;
        else if (k == L"pc") l.pc = iv != 0;
        else if (k == L"bin") l.bin = iv != 0;
        else if (k == L"user") l.user = iv != 0;
        else if (k == L"net") l.net = iv != 0;
        else if (k == L"aether") l.aether = iv != 0;
        else if (k == L"slidedir") l.slideDir = v;
    }
    return l;
}

// Writes one key (replacing its line, or appending it), leaving the rest of the file untouched.
inline void Set(const std::wstring& key, const std::wstring& value) {
    const std::wstring p = Path();
    if (p.empty()) return;
    std::vector<std::wstring> lines = Lines(ReadAll(p));
    bool found = false;
    for (auto& line : lines) {
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == L'=') { line = key + L"=" + value; found = true; break; }
    }
    if (!found) lines.push_back(key + L"=" + value);
    if (lines.empty() || lines.front() != L"ver=3") { bool has = false; for (const auto& l : lines) if (l.rfind(L"ver=", 0) == 0) has = true; if (!has) lines.insert(lines.begin(), L"ver=3"); }
    std::wstring w; for (const auto& l : lines) w += l + L"\n";
    const int bl = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string b(static_cast<size_t>(std::max(0, bl)), '\0');
    if (bl > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), b.data(), bl, nullptr, nullptr);
    const std::wstring dir = p.substr(0, p.find_last_of(L'\\'));
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &wr, nullptr); CloseHandle(h);
}
inline void SetInt(const std::wstring& key, int v) { Set(key, std::to_wstring(v)); }

// Default look (the desktop's "reset"): every appearance key at its default.
inline void ResetLook() {
    SetInt(L"dim", 0); SetInt(L"blur", 0); SetInt(L"fit", -1); SetInt(L"label", 0); SetInt(L"accent", 0); SetInt(L"glow", 100);
    SetInt(L"isize", 48); SetInt(L"clock", 0); SetInt(L"auto", 0); SetInt(L"slide", 0);
}

} // namespace aether::system::desktopcfg
