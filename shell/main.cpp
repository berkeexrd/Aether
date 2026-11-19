// AetherShell: the session manager of a complete Aether desktop.
//
//  * starts and supervises AetherBar, AetherDesktop and Aether (island)             - restarts them after a crash
//  * runs the logon startup items that explorer.exe normally runs (Run / RunOnce / Startup folders)
//  * provides what explorer.exe provides globally: Alt+Tab, Win+Tab, Win+R, Win+M, Win+I
//  * Shift held at logon (or %APPDATA%\Aether\use-explorer) starts the normal Windows shell instead - the escape hatch
//
// When explorer.exe is still the running shell (testing from a normal session) only the hotkeys / switcher are active.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/glass.hpp"
#include "aether/render/mini_gfx.hpp"
#include "aether/system/app_identity.hpp"
#include "aether/system/shell_keys.hpp"
#include "aether/system/icon_pack.hpp"
#include "aether/core/crash.hpp"
#include "aether/core/single_instance.hpp"
#include "aether/system/bar_task.hpp"

using namespace aether;
using aether::mg::Color;
using aether::mg::Rect;
using aether::mg::kWhite;
using aether::mg::WithA;
using aether::mg::Mix;
using Microsoft::WRL::ComPtr;

namespace {

using shellkeys::kMsgSwStart; using shellkeys::kMsgSwNext; using shellkeys::kMsgSwMove; using shellkeys::kMsgSwCommit; using shellkeys::kMsgSwCancel;
using shellkeys::kMsgRun; using shellkeys::kMsgMinAll; using shellkeys::kMsgSettings; using shellkeys::kMsgWinCombo;
using shellkeys::g_swActive;
using shellkeys::MaskKeys;
constexpr UINT_PTR kTimerSupervise = 1;

HWND g_ctl = nullptr;

std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}
std::wstring AppData() {
    wchar_t buf[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(buf) + L"\\Aether"; CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
bool FileExists(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES; }
bool DirExists(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

void ForceForeground(HWND h) {
    if (!h) return;
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
    if (fg && fg != me) AttachThreadInput(me, fg, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (fg && fg != me) AttachThreadInput(me, fg, FALSE);
}

std::wstring ProcessPath(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (!pid) return L"";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2] = {}; DWORD n = MAX_PATH * 2;
    std::wstring r;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) r = buf;
    CloseHandle(p);
    return r;
}

// ---- windows that belong in Alt+Tab / "minimize all" -----------------------------------------------------------------------
bool IsAppWindow(HWND h, bool includeMinimized) {
    if (!IsWindowVisible(h)) return false;
    if (!includeMinimized && IsIconic(h)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if ((ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    BOOL cloaked = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    static const wchar_t* skip[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"AetherBarClass", L"AetherStartMenuClass",
                                    L"AetherDesktopWindow", L"AetherSwitcherWindow", L"AetherRunWindow", L"Windows.UI.Core.CoreWindow"};
    for (auto* s : skip) if (!wcscmp(cls, s)) return false;
    return true;
}
std::vector<HWND> AppWindows(bool includeMinimized) {
    std::vector<HWND> v;
    struct Ctx { std::vector<HWND>* v; bool m; } c{&v, includeMinimized};
    EnumWindows([](HWND h, LPARAM l) -> BOOL { auto* c = reinterpret_cast<Ctx*>(l); if (IsAppWindow(h, c->m)) c->v->push_back(h); return TRUE; }, reinterpret_cast<LPARAM>(&c));
    return v;
}
void Activate(HWND h) {
    if (!h || !IsWindow(h)) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    ForceForeground(h);
    // An elevated window cannot be taken to the front by a non-elevated process: the (elevated) bar does it for us.
    if (GetForegroundWindow() != h && GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) != h) {
        if (HWND bar = FindWindowW(L"AetherBarClass", nullptr)) PostMessageW(bar, shellkeys::kMsgActivate, reinterpret_cast<WPARAM>(h), 0);
    }
}

HMONITOR WorkMonitor() {
    HWND fg = GetForegroundWindow();
    if (fg) return MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    POINT pt; GetCursorPos(&pt);
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
}

// =============================================================================================================================
//   Alt+Tab / Win+Tab switcher
// =============================================================================================================================
class Switcher {
public:
    void Start(bool sticky, bool reverse);
    void Next(int dir);
    void Move(UINT vk);
    void Commit();
    void Cancel();
    bool Active() const { return m_active; }
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    struct Entry { HWND h; std::wstring title; ComPtr<ID2D1Bitmap1> icon; bool min; };
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    std::vector<Entry> m_items;
    unsigned m_packVer{0};
    std::map<std::wstring, ComPtr<ID2D1Bitmap1>> m_iconCache;
    int m_sel{0}, m_hover{-1};
    bool m_active{false}, m_shown{false}, m_sticky{false};
    float m_a{0.0f}, m_scale{1.0f}, m_W{0}, m_H{0};
    int m_cols{1}, m_rows{1};
    float m_k{1.0f};
    Rect m_panel{0, 0, 0, 0};
    LARGE_INTEGER m_freq{}, m_last{};

    bool EnsureWindow();
    void BuildList();
    ComPtr<ID2D1Bitmap1> IconFor(HWND h);
    void Show();
    void Hide();
    void Layout();
    Rect Card(int i) const;
    void Render();
    int HitCard(float x, float y) const;
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
};
Switcher g_sw;

bool Switcher::EnsureWindow() {
    if (m_hwnd) return true;
    static bool reg = false;
    if (!reg) {
        reg = true;
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"AetherSwitcherWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    }
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(WorkMonitor(), &mi);
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"AetherSwitcherWindow", L"", WS_POPUP,
                             mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return false;
    QueryPerformanceFrequency(&m_freq);
    RECT rc; GetClientRect(m_hwnd, &rc);
    m_dc = std::make_unique<render::DCompLayer>();
    if (!m_dc->Initialize(m_hwnd, rc.right, rc.bottom)) { DestroyWindow(m_hwnd); m_hwnd = nullptr; return false; }
    m_g.Init(m_dc.get());
    return true;
}

ComPtr<ID2D1Bitmap1> Switcher::IconFor(HWND h) {
    auto* ctx = m_dc->GetContext();
    { const unsigned pv = aether::system::iconpack::Version(); if (pv != m_packVer) { m_packVer = pv; m_iconCache.clear(); } }   // new icon pack chosen
    const std::wstring path = ProcessPath(h);
    const bool frame = path.empty() || StrStrIW(path.c_str(), L"ApplicationFrameHost") != nullptr;
    // Store / UWP apps (Settings, Calculator ...) live in ApplicationFrameHost: their real tile icon comes through the AppUserModelID.
    std::wstring src = path;
    if (frame) { const std::wstring aumid = aether::system::WindowAumid(h); src = aumid.empty() ? std::wstring() : aether::system::AppsFolderPath(aumid); }
    if (!src.empty()) {
        auto it = m_iconCache.find(src);
        if (it != m_iconCache.end() && it->second) return it->second;
        int w = 0, hh = 0; std::vector<uint32_t> px;
        ComPtr<ID2D1Bitmap1> bmp;
        if (aether::system::iconpack::Active()) { const std::wstring f = aether::system::iconpack::Find(src); if (!f.empty()) bmp = aether::system::iconpack::LoadBitmap(ctx, f); }
        if (!bmp && mg::ShellImage(src, 96, false, w, hh, px)) bmp = mg::BitmapFromPixels(ctx, px, w, hh);
        m_iconCache[src] = bmp;
        if (bmp) return bmp;
    }
    DWORD_PTR r = 0; HICON ic = nullptr;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 60, &r) && r) ic = reinterpret_cast<HICON>(r);
    if (!ic) ic = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON));
    if (!ic && SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 60, &r) && r) ic = reinterpret_cast<HICON>(r);
    return mg::BitmapFromHicon(ctx, ic);
}

void Switcher::BuildList() {
    m_items.clear();
    for (HWND h : AppWindows(true)) {
        wchar_t t[256] = {}; GetWindowTextW(h, t, 256);
        m_items.push_back({h, t, nullptr, IsIconic(h) != FALSE});
    }
}

void Switcher::Start(bool sticky, bool reverse) {
    m_active = true; m_sticky = sticky; m_hover = -1;
    if (!EnsureWindow()) { m_active = false; g_swActive = false; return; }
    BuildList();
    if (m_items.empty()) { m_active = false; g_swActive = false; return; }
    const int n = static_cast<int>(m_items.size());
    m_sel = n > 1 ? (reverse ? n - 1 : 1) : 0;
    if (sticky) Show(); else SetTimer(m_hwnd, 1, 110, nullptr);       // a quick Alt+Tab never shows anything
}

void Switcher::Next(int dir) {
    if (!m_active || m_items.empty()) return;
    const int n = static_cast<int>(m_items.size());
    m_sel = ((m_sel + dir) % n + n) % n;
    if (!m_shown) Show();
    Render();
}

void Switcher::Move(UINT vk) {
    if (!m_active || !m_shown) { if (vk == VK_LEFT || vk == VK_UP) Next(-1); else Next(1); return; }
    const int n = static_cast<int>(m_items.size());
    if (vk == VK_LEFT) Next(-1);
    else if (vk == VK_RIGHT) Next(1);
    else {
        const int step = vk == VK_UP ? -m_cols : m_cols;
        const int t = m_sel + step;
        if (t >= 0 && t < n) { m_sel = t; Render(); }
    }
}

void Switcher::Commit() {
    if (!m_active) return;
    const HWND target = (m_sel >= 0 && m_sel < static_cast<int>(m_items.size())) ? m_items[m_sel].h : nullptr;
    Hide();
    m_active = false; g_swActive = false;
    Activate(target);
}
void Switcher::Cancel() { Hide(); m_active = false; g_swActive = false; }

void Switcher::Show() {
    if (m_shown || !m_hwnd) return;
    for (auto& e : m_items) e.icon = IconFor(e.h);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(WorkMonitor(), &mi);
    const int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    SetWindowPos(m_hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_NOACTIVATE);
    m_dc->Resize(w, h);
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    m_W = w / m_scale; m_H = h / m_scale;
    m_g.ClearFormats();
    Layout();
    m_a = 0.0f; m_shown = true;
    QueryPerformanceCounter(&m_last);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(m_hwnd, 2, 15, nullptr);
    Render();
}

void Switcher::Hide() {
    KillTimer(m_hwnd, 1); KillTimer(m_hwnd, 2);
    if (m_shown) ShowWindow(m_hwnd, SW_HIDE);
    m_shown = false;
}

void Switcher::Layout() {
    const int n = static_cast<int>(m_items.size());
    m_k = 1.0f;
    const float pad = 26.0f, head = 46.0f;
    for (;;) {
        const float cw = 150 * m_k, ch = 158 * m_k, gap = 14 * m_k;
        m_cols = std::max(1, std::min(n, static_cast<int>((m_W * 0.86f - 2 * pad + gap) / (cw + gap))));
        m_rows = (n + m_cols - 1) / m_cols;
        const float ph = 2 * pad + head + m_rows * (ch + gap) - gap;
        if (ph <= m_H * 0.84f || m_k <= 0.55f) break;
        m_k -= 0.05f;
    }
    const float cw = 150 * m_k, ch = 158 * m_k, gap = 14 * m_k;
    const float pw = 2 * pad + m_cols * (cw + gap) - gap, ph = 2 * pad + head + m_rows * (ch + gap) - gap;
    m_panel = Rect{(m_W - pw) / 2, (m_H - ph) / 2, pw, ph};
}

Rect Switcher::Card(int i) const {
    const float cw = 150 * m_k, ch = 158 * m_k, gap = 14 * m_k, pad = 26.0f, head = 46.0f;
    const int col = i % m_cols, row = i / m_cols;
    // the last row is centred
    const int inRow = (row == m_rows - 1) ? static_cast<int>(m_items.size()) - row * m_cols : m_cols;
    const float rowW = inRow * (cw + gap) - gap;
    const float x0 = m_panel.x + (m_panel.width - rowW) / 2;
    return Rect{x0 + col * (cw + gap), m_panel.y + pad + head + row * (ch + gap), cw, ch};
}

int Switcher::HitCard(float x, float y) const {
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) if (Card(i).Contains(x, y)) return i;
    return -1;
}

void Switcher::Render() {
    if (!m_shown || !m_dc) return;
    if (!m_g.Begin()) return;
    auto* ctx = m_g.ctx;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
    const float e = 1.0f - (1.0f - m_a) * (1.0f - m_a);
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight;
    m_g.FillRect(Rect{0, 0, m_W, m_H}, Color{0.0f, 0.0f, 0.02f, 0.46f * e});
    Rect p = m_panel; p.y += (1.0f - e) * 16.0f;
    const float dy = p.y - m_panel.y;
    m_g.Glow(p, 22, v, 22.0f, 0.8f * e);
    m_g.VGradientRR(p, 22, Color{0.09f, 0.06f, 0.19f, 0.94f * e}, Color{0.04f, 0.03f, 0.10f, 0.96f * e});
    m_g.StrokeRR(p, 22, WithA(vl, 0.38f * e), 1.2f);
    if (m_sel >= 0 && m_sel < static_cast<int>(m_items.size()))
        m_g.Text(m_items[m_sel].title, Rect{p.x + 26, p.y + 16, p.width - 52, 32}, 17.0f, 600, WithA(kWhite, e), DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        Rect c = Card(i); c.y += dy;
        const bool sel = i == m_sel, hot = i == m_hover;
        if (sel) { m_g.Glow(c, 16, v, 12.0f, e); m_g.FillRR(c, 16, WithA(Mix(v, vl, 0.2f), 0.34f * e)); m_g.StrokeRR(c, 16, WithA(vl, 0.75f * e), 1.4f); }
        else if (hot) { m_g.FillRR(c, 16, WithA(vl, 0.12f * e)); m_g.StrokeRR(c, 16, WithA(vl, 0.28f * e), 1.0f); }
        const float isz = 72 * m_k;
        const Rect ir{c.x + (c.width - isz) / 2, c.y + 16 * m_k, isz, isz};
        const float ia = (m_items[i].min ? 0.62f : 1.0f) * e;
        if (m_items[i].icon) {
            const D2D1_RECT_F dr = mg::D(ir);
            ctx->DrawBitmap(m_items[i].icon.Get(), &dr, ia, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else m_g.FillRR(ir, 14, WithA(kWhite, 0.14f * e));
        m_g.TextBox(m_items[i].title, Rect{c.x + 8, ir.Bottom() + 12 * m_k, c.width - 16, 34 * m_k}, 12.0f, sel ? 600 : 400, WithA(kWhite, (sel ? 1.0f : 0.82f) * e));
    }
    m_g.End();
}

LRESULT CALLBACK Switcher::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Switcher* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Switcher*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = h;
    } else self = reinterpret_cast<Switcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Switcher::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(m_hwnd, &ps); EndPaint(m_hwnd, &ps); Render(); return 0; }
    case WM_TIMER:
        if (w == 1) { KillTimer(m_hwnd, 1); if (m_active && !m_shown) Show(); }
        else if (w == 2) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const float dt = static_cast<float>(now.QuadPart - m_last.QuadPart) / static_cast<float>(m_freq.QuadPart);
            m_last = now;
            m_a = std::min(1.0f, m_a + dt / 0.16f);
            Render();
            if (m_a >= 1.0f) KillTimer(m_hwnd, 2);
        }
        return 0;
    case WM_MOUSEMOVE: {
        const int hit = HitCard(GET_X_LPARAM(l) / m_scale, GET_Y_LPARAM(l) / m_scale);
        if (hit != m_hover) { m_hover = hit; Render(); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float x = GET_X_LPARAM(l) / m_scale, y = GET_Y_LPARAM(l) / m_scale;
        const int hit = HitCard(x, y - ((1.0f - m_a) * 16.0f));
        if (hit >= 0) { m_sel = hit; Commit(); }
        else if (!m_panel.Contains(x, y)) Cancel();
        return 0;
    }
    case WM_MBUTTONDOWN: {                         // middle click closes the window under the cursor
        const int hit = HitCard(GET_X_LPARAM(l) / m_scale, GET_Y_LPARAM(l) / m_scale);
        if (hit >= 0) {
            PostMessageW(m_items[hit].h, WM_CLOSE, 0, 0);
            m_items.erase(m_items.begin() + hit);
            if (m_items.empty()) { Cancel(); return 0; }
            m_sel = std::min(m_sel, static_cast<int>(m_items.size()) - 1);
            Layout(); Render();
        }
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

// =============================================================================================================================
//   Win+R : run box
// =============================================================================================================================
class RunBox {
public:
    void Open();
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
private:
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    std::wstring m_text, m_err;
    size_t m_caret{0};
    std::vector<std::wstring> m_hist;
    int m_histPos{-1};
    float m_scale{1.0f};
    bool m_ok{false};
    bool m_admin{false};          // run the command elevated ("runas")
    bool m_selAll{false};         // Ctrl+A: the whole text is selected
    static constexpr float kW = 580.0f, kH = 206.0f;
    Rect AdminRect() const { return Rect{30.0f, 10.0f + 106.0f, 300.0f, 28.0f}; }

    bool EnsureWindow();
    void Render();
    void Execute();
    void LoadHist();
    void SaveHist();
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    void Insert(const std::wstring& s) {
        if (m_selAll) { m_text.clear(); m_caret = 0; m_selAll = false; }
        m_text.insert(m_caret, s); m_caret += s.size(); m_err.clear();
    }
    bool EraseSelection() { if (!m_selAll) return false; m_text.clear(); m_caret = 0; m_selAll = false; m_err.clear(); return true; }
    void CopyText();
};
RunBox g_run;

void RunBox::LoadHist() {
    m_hist.clear();
    const std::wstring p = AppData() + L"\\run.hist";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string b(1 << 16, '\0'); DWORD n = 0;
    ReadFile(h, b.data(), static_cast<DWORD>(b.size() - 1), &n, nullptr); CloseHandle(h); b.resize(n);
    const int wl = MultiByteToWideChar(CP_UTF8, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, wl)), L'\0');
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, b.data(), static_cast<int>(b.size()), w.data(), wl);
    size_t pos = 0;
    while (pos < w.size() && m_hist.size() < 20) {
        size_t e = w.find(L'\n', pos); if (e == std::wstring::npos) e = w.size();
        std::wstring line = w.substr(pos, e - pos); pos = e + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) m_hist.push_back(line);
    }
}
void RunBox::SaveHist() {
    std::wstring w;
    for (const auto& s : m_hist) w += s + L"\n";
    const int bl = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string b(static_cast<size_t>(std::max(0, bl)), '\0');
    if (bl > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), b.data(), bl, nullptr, nullptr);
    HANDLE h = CreateFileW((AppData() + L"\\run.hist").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &wr, nullptr); CloseHandle(h);
}

bool RunBox::EnsureWindow() {
    if (m_hwnd) return true;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"AetherRunWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, L"AetherRunWindow", L"Çalıştır", WS_POPUP, 0, 0, 600, 230, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return false;
    m_dc = std::make_unique<render::DCompLayer>();
    if (!m_dc->Initialize(m_hwnd, 600, 230)) { DestroyWindow(m_hwnd); m_hwnd = nullptr; return false; }
    m_g.Init(m_dc.get());
    return true;
}

void RunBox::Open() {
    if (!EnsureWindow()) return;
    LoadHist(); m_text.clear(); m_caret = 0; m_err.clear(); m_histPos = -1; m_admin = false; m_selAll = false;
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(WorkMonitor(), &mi);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 100, 100, SWP_NOACTIVATE | SWP_NOMOVE | SWP_HIDEWINDOW);
    SetWindowPos(m_hwnd, HWND_TOPMOST, mi.rcWork.left, mi.rcWork.top, 100, 100, SWP_NOACTIVATE | SWP_NOSIZE);
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    const int pw = static_cast<int>(kW * m_scale), ph = static_cast<int>(kH * m_scale);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - pw) / 2, y = mi.rcWork.top + static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.3f);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, pw, ph, SWP_NOACTIVATE);
    m_dc->Resize(pw, ph);
    m_g.ClearFormats();
    ShowWindow(m_hwnd, SW_SHOW);
    ForceForeground(m_hwnd);
    SetFocus(m_hwnd);
    Render();
}

void RunBox::Render() {
    if (!m_g.Begin()) return;
    auto* ctx = m_g.ctx;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight;
    const Rect panel{10, 10, kW - 20, kH - 20};
    m_g.Glow(panel, 20, v, 10.0f, 0.8f);
    m_g.VGradientRR(panel, 20, Color{0.09f, 0.06f, 0.19f, 0.97f}, Color{0.04f, 0.03f, 0.10f, 0.98f});
    m_g.StrokeRR(panel, 20, WithA(vl, 0.40f), 1.2f);
    m_g.Glyph(0xE756, Rect{panel.x + 18, panel.y + 12, 30, 30}, 17.0f, vl);
    m_g.Text(L"Çalıştır", Rect{panel.x + 52, panel.y + 12, 200, 30}, 16.0f, 600, kWhite);
    const Rect field{panel.x + 20, panel.y + 56, panel.width - 40, 44};
    m_g.FillRR(field, 12, Color{0.0f, 0.0f, 0.0f, 0.38f});
    m_g.StrokeRR(field, 12, WithA(vl, 0.75f), 1.4f);
    const Rect tx{field.x + 14, field.y, field.width - 28, field.height};
    if (m_text.empty()) m_g.Text(L"Program, klasör, belge veya web adresi yazın", tx, 14.0f, 400, WithA(kWhite, 0.38f));
    else {
        if (m_selAll) {                                                    // selection highlight behind the text
            const float tw = m_g.TextWidth(m_text, 15.0f, 400);
            m_g.FillRR(Rect{tx.x - 2, field.y + 9, tw + 4, 26}, 5, WithA(vl, 0.45f));
        }
        m_g.Text(m_text, tx, 15.0f, 400, kWhite);
    }
    if (!m_selAll) {
        const float cx = m_g.TextWidth(m_text.substr(0, m_caret), 15.0f, 400);
        m_g.FillRR(Rect{tx.x + cx + 0.5f, field.y + 11, 1.6f, 22}, 0.8f, kWhite);
    }
    // "run as administrator": a check box (click) that Ctrl+Shift+Enter also does in one go
    {
        const Rect ar{panel.x + 20, panel.y + 106, 300, 28};
        const Rect box{ar.x, ar.y + 4, 20, 20};
        m_g.FillRR(box, 6, m_admin ? WithA(vl, 0.95f) : Color{0.0f, 0.0f, 0.0f, 0.38f});
        m_g.StrokeRR(box, 6, WithA(vl, m_admin ? 1.0f : 0.6f), 1.2f);
        if (m_admin) m_g.Glyph(0xE73E, box, 13.0f, Color{0.05f, 0.03f, 0.12f, 1.0f});
        m_g.Text(L"Yönetici olarak çalıştır", Rect{ar.x + 30, ar.y, 220, 28}, 13.5f, 500, WithA(kWhite, m_admin ? 1.0f : 0.78f));
        m_g.Text(L"Ctrl+Shift+Enter", Rect{panel.x + panel.width - 160, ar.y, 140, 28}, 11.5f, 400, WithA(kWhite, 0.38f), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (!m_err.empty()) m_g.Text(m_err, Rect{panel.x + 22, panel.y + 140, panel.width - 44, 24}, 12.5f, 400, Color{1.0f, 0.55f, 0.55f, 1.0f});
    else m_g.Text(L"Enter: çalıştır      Esc: kapat      ↑ ↓: geçmiş", Rect{panel.x + 22, panel.y + 140, panel.width - 44, 24}, 12.0f, 400, WithA(kWhite, 0.45f));
    m_g.End();
}

void RunBox::Execute() {
    std::wstring cmd = m_text;
    while (!cmd.empty() && iswspace(cmd.front())) cmd.erase(0, 1);
    while (!cmd.empty() && iswspace(cmd.back())) cmd.pop_back();
    if (cmd.empty()) return;
    wchar_t ex[4096] = {};
    ExpandEnvironmentStringsW(cmd.c_str(), ex, 4096);
    const std::wstring expanded = ex;
    std::wstring file, args;
    if (!expanded.empty() && expanded[0] == L'"') {
        const size_t q = expanded.find(L'"', 1);
        file = expanded.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
        if (q != std::wstring::npos) args = expanded.substr(q + 1);
    } else if (FileExists(expanded)) file = expanded;
    else {
        const size_t sp = expanded.find(L' ');
        file = expanded.substr(0, sp);
        if (sp != std::wstring::npos) args = expanded.substr(sp + 1);
    }
    while (!args.empty() && args.front() == L' ') args.erase(0, 1);
    bool ok = false;
    const wchar_t* verb = m_admin ? L"runas" : L"open";
    INT_PTR rc = 0;
    SetLastError(0);
    if (DirExists(file)) {                                        // folders open in Aether Explorer
        const std::wstring ae = ExeDir() + L"\\AetherExplorer.exe";
        const std::wstring a = L"\"" + file + L"\"";
        ok = FileExists(ae) ? reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", ae.c_str(), a.c_str(), nullptr, SW_SHOWNORMAL)) > 32
                            : reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    } else {
        rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, verb, file.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL));
        ok = rc > 32;
    }
    if (!ok) {
        const bool declined = m_admin && (rc == SE_ERR_ACCESSDENIED || GetLastError() == ERROR_CANCELLED);
        m_err = declined ? std::wstring(L"Yönetici izni verilmedi.") : L"“" + file + L"” bulunamadı. Adı doğru yazdığınızdan emin olun.";
        Render(); return;
    }
    m_hist.erase(std::remove(m_hist.begin(), m_hist.end(), cmd), m_hist.end());
    m_hist.insert(m_hist.begin(), cmd);
    if (m_hist.size() > 20) m_hist.resize(20);
    SaveHist();
    ShowWindow(m_hwnd, SW_HIDE);
}

void RunBox::CopyText() {
    if (m_text.empty() || !OpenClipboard(m_hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (m_text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(g)) { memcpy(p, m_text.c_str(), bytes); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
        else GlobalFree(g);
    }
    CloseClipboard();
}

LRESULT CALLBACK RunBox::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    RunBox* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<RunBox*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = h;
    } else self = reinterpret_cast<RunBox*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT RunBox::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(m_hwnd, &ps); EndPaint(m_hwnd, &ps); Render(); return 0; }
    case WM_KILLFOCUS: if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE); return 0;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (w) {
            case VK_ESCAPE: ShowWindow(m_hwnd, SW_HIDE); return 0;
            case VK_RETURN: if (ctrl && shift) m_admin = true; Execute(); return 0;          // Ctrl+Shift+Enter: as administrator, like Windows' own box
            case VK_TAB: m_admin = !m_admin; break;
            case VK_LEFT: m_selAll = false; if (m_caret) --m_caret; break;
            case VK_RIGHT: m_selAll = false; if (m_caret < m_text.size()) ++m_caret; break;
            case VK_HOME: m_selAll = false; m_caret = 0; break;
            case VK_END: m_selAll = false; m_caret = m_text.size(); break;
            case VK_DELETE: if (!EraseSelection() && m_caret < m_text.size()) { m_text.erase(m_caret, 1); m_err.clear(); } break;
            case VK_UP: if (m_histPos + 1 < static_cast<int>(m_hist.size())) { m_text = m_hist[++m_histPos]; m_caret = m_text.size(); m_err.clear(); } break;
            case VK_DOWN:
                if (m_histPos > 0) { m_text = m_hist[--m_histPos]; m_caret = m_text.size(); }
                else if (m_histPos == 0) { m_histPos = -1; m_text.clear(); m_caret = 0; }
                break;
            case 'V':
                if (ctrl && OpenClipboard(m_hwnd)) {
                    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
                        if (auto* p = static_cast<const wchar_t*>(GlobalLock(hd))) {
                            std::wstring s = p; GlobalUnlock(hd);
                            s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }), s.end());
                            Insert(s);
                        }
                    }
                    CloseClipboard();
                }
                break;
            case 'A': if (ctrl) { m_selAll = !m_text.empty(); m_caret = m_text.size(); } break;
            case 'C': if (ctrl) CopyText(); break;
            case 'X': if (ctrl) { CopyText(); EraseSelection(); } break;
        }
        Render();
        return 0;
    }
    case WM_CHAR:
        if (w == 8) {                                                      // Backspace, Ctrl+Backspace deletes a word
            if (!EraseSelection() && m_caret) {
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    size_t b = m_caret;
                    while (b > 0 && iswspace(m_text[b - 1])) --b;
                    while (b > 0 && !iswspace(m_text[b - 1])) --b;
                    m_text.erase(b, m_caret - b); m_caret = b;
                } else m_text.erase(--m_caret, 1);
                m_err.clear();
            }
        }
        else if (w == 127 || w == 1 || w == 3 || w == 24 || w == 9 || w == 13 || w == 27) {}      // control characters produced by Ctrl+key / Tab / Enter / Esc
        else if (w >= 32) Insert(std::wstring(1, static_cast<wchar_t>(w)));
        Render();
        return 0;
    case WM_LBUTTONDOWN: {
        const float x = GET_X_LPARAM(l) / m_scale, y = GET_Y_LPARAM(l) / m_scale;
        if (AdminRect().Contains(x, y)) { m_admin = !m_admin; Render(); }
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

// =============================================================================================================================
//   Win+V : clipboard history
// =============================================================================================================================
// Text that was copied, newest first. Kept in memory only (nothing is written to disk: clipboards carry passwords). Items that a program marks as
// private (password managers set "ExcludeClipboardContentFromMonitorProcessing" / "CanIncludeInClipboardHistory" = 0) are never recorded.
class ClipPanel {
public:
    void OnClipboardChanged();                 // WM_CLIPBOARDUPDATE: reads the text on a worker thread (an owner that renders lazily can take long)
    void AddText(std::wstring* text);          // the worker's result (heap string, may be null), back on the shell thread
    void Open();
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
private:
    struct Item { std::wstring text; ULONGLONG at; };
    int m_pasteTries{0};
    static constexpr size_t kMax = 25;
    static constexpr int kRows = 8;
    static constexpr float kW = 460.0f, kHead = 56.0f, kRow = 46.0f, kFoot = 38.0f;
    HWND m_hwnd{nullptr}, m_prev{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    std::vector<Item> m_items;
    DWORD m_ownSeq{0};
    int m_sel{0}, m_top{0}, m_hover{-1};
    float m_scale{1.0f};

    bool EnsureWindow();
    void Render();
    void Hide() { if (m_hwnd && IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE); }
    void Remove(int i);
    void Reshape();
    void Commit(int i);
    float PanelH() const { return kHead + kRow * static_cast<float>(std::max(1, std::min<int>(kRows, static_cast<int>(m_items.size())))) + kFoot; }
    Rect RowRect(int vis) const { return Rect{20.0f, 10.0f + kHead + kRow * static_cast<float>(vis), kW - 20.0f, kRow - 4.0f}; }     // panel is x 10 .. 10 + kW
    Rect ClearRect() const { return Rect{kW - 130.0f, 10.0f + PanelH() - kFoot, 130.0f, kFoot - 8.0f}; }
    int HitRow(float x, float y) const;
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
};
ClipPanel g_clip;

namespace {
bool ClipFormatBlocks() {
    static const UINT excl = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT canHist = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(excl)) return true;
    if (IsClipboardFormatAvailable(canHist)) {
        if (HANDLE h = GetClipboardData(canHist)) {
            if (auto* p = static_cast<const DWORD*>(GlobalLock(h))) { const bool no = *p == 0; GlobalUnlock(h); if (no) return true; }
        }
    }
    return false;
}
std::wstring OneLine(const std::wstring& s, size_t maxLen) {
    std::wstring o; o.reserve(std::min(s.size(), maxLen + 1));
    bool sp = true;
    for (wchar_t c : s) {
        const bool white = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
        if (white) { if (!sp) { o.push_back(L' '); sp = true; } } else { o.push_back(c); sp = false; }
        if (o.size() >= maxLen) break;
    }
    while (!o.empty() && o.back() == L' ') o.pop_back();
    return o;
}
std::wstring AgeText(ULONGLONG at) {
    const ULONGLONG s = (GetTickCount64() - at) / 1000;
    if (s < 45) return L"şimdi";
    if (s < 3600) return std::to_wstring(std::max<ULONGLONG>(1, s / 60)) + L" dk";
    return std::to_wstring(s / 3600) + L" sa";
}
} // namespace

void ClipPanel::OnClipboardChanged() {
    const DWORD seq = GetClipboardSequenceNumber();
    if (seq == m_ownSeq) return;                                           // we put it there ourselves (a paste from the history)
    constexpr UINT kMsgClipText = WM_APP + 13;
    const HWND ctl = g_ctl;
    std::thread([ctl]() {
        std::wstring* result = nullptr;
        bool opened = false;
        for (int i = 0; i < 8 && !opened; ++i) { opened = OpenClipboard(nullptr) != FALSE; if (!opened) Sleep(15); }   // another program may hold it for a moment
        if (opened) {
            std::wstring text;
            if (!ClipFormatBlocks() && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {         // may wait for the owner to render: that is why this is not on the shell thread
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                        const size_t cap = GlobalSize(h) / sizeof(wchar_t);
                        size_t n = 0; while (n < cap && p[n] && n < 20000) ++n;
                        text.assign(p, n);
                        GlobalUnlock(h);
                    }
                }
            }
            CloseClipboard();
            if (text.find_first_not_of(L" \t\r\n") != std::wstring::npos) result = new std::wstring(std::move(text));   // not empty / whitespace only
        }
        if (!result || !IsWindow(ctl) || !PostMessageW(ctl, kMsgClipText, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}

void ClipPanel::AddText(std::wstring* p) {
    if (!p) return;
    std::wstring text = std::move(*p); delete p;
    for (size_t i = 0; i < m_items.size(); ++i) if (m_items[i].text == text) { m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(i)); break; }
    m_items.insert(m_items.begin(), Item{std::move(text), GetTickCount64()});
    if (m_items.size() > kMax) m_items.resize(kMax);
    if (m_hwnd && IsWindowVisible(m_hwnd)) { m_sel = 0; m_top = 0; Reshape(); Render(); }
}

bool ClipPanel::EnsureWindow() {
    if (m_hwnd) return true;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"AetherClipWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, L"AetherClipWindow", L"Pano geçmişi", WS_POPUP, 0, 0, 480, 480, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return false;
    m_dc = std::make_unique<render::DCompLayer>();
    if (!m_dc->Initialize(m_hwnd, 480, 480)) { DestroyWindow(m_hwnd); m_hwnd = nullptr; return false; }
    m_g.Init(m_dc.get());
    return true;
}

void ClipPanel::Open() {
    if (!EnsureWindow()) return;
    if (IsWindowVisible(m_hwnd)) { Hide(); return; }                           // Win+V again closes it
    HWND fg = GetForegroundWindow();
    m_prev = (fg && fg != m_hwnd) ? fg : nullptr;                              // where the pasted text goes
    m_sel = 0; m_top = 0; m_hover = -1;
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(WorkMonitor(), &mi);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 100, 100, SWP_NOACTIVATE | SWP_NOMOVE | SWP_HIDEWINDOW);
    SetWindowPos(m_hwnd, HWND_TOPMOST, mi.rcWork.left, mi.rcWork.top, 100, 100, SWP_NOACTIVATE | SWP_NOSIZE);   // on that monitor first: its DPI applies
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    const int pw = static_cast<int>((kW + 20) * m_scale), ph = static_cast<int>((PanelH() + 20) * m_scale);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - pw) / 2, y = mi.rcWork.top + static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.22f);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, pw, ph, SWP_NOACTIVATE);
    m_dc->Resize(pw, ph);
    m_g.ClearFormats();
    ShowWindow(m_hwnd, SW_SHOW);
    ForceForeground(m_hwnd);
    SetFocus(m_hwnd);
    Render();
}

int ClipPanel::HitRow(float x, float y) const {
    const int n = std::min<int>(kRows, static_cast<int>(m_items.size()) - m_top);
    for (int i = 0; i < n; ++i) if (RowRect(i).Contains(x, y)) return m_top + i;
    return -1;
}

void ClipPanel::Remove(int i) {
    if (i < 0 || i >= static_cast<int>(m_items.size())) return;
    m_items.erase(m_items.begin() + i);
    m_sel = std::clamp(m_sel, 0, std::max(0, static_cast<int>(m_items.size()) - 1));
    m_top = std::clamp(m_top, 0, std::max(0, static_cast<int>(m_items.size()) - kRows));
    Reshape();
}

void ClipPanel::Reshape() {                                                    // the panel's height follows the number of rows
    const int ph = static_cast<int>((PanelH() + 20) * m_scale);
    RECT r; GetWindowRect(m_hwnd, &r);
    SetWindowPos(m_hwnd, nullptr, 0, 0, r.right - r.left, ph, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    m_dc->Resize(r.right - r.left, ph);
}

void ClipPanel::Commit(int i) {
    if (i < 0 || i >= static_cast<int>(m_items.size())) return;
    const std::wstring text = m_items[static_cast<size_t>(i)].text;
    if (OpenClipboard(m_hwnd)) {
        EmptyClipboard();
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
            if (void* p = GlobalLock(g)) { memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
            else GlobalFree(g);
        }
        CloseClipboard();
        m_ownSeq = GetClipboardSequenceNumber();
    }
    Item it = m_items[static_cast<size_t>(i)]; it.at = GetTickCount64();
    m_items.erase(m_items.begin() + i); m_items.insert(m_items.begin(), it);
    Hide();
    if (m_prev && IsWindow(m_prev)) Activate(m_prev);
    SetTimer(m_hwnd, 1, 110, nullptr);                                         // the window must be in front before Ctrl+V is sent
}

void ClipPanel::Render() {
    if (!m_g.Begin()) return;
    auto* ctx = m_g.ctx;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
    const Color v = render::glass::kViolet, vl = render::glass::kVioletLight;
    const Rect panel{10, 10, kW, PanelH()};
    m_g.Glow(panel, 20, v, 10.0f, 0.8f);
    m_g.VGradientRR(panel, 20, Color{0.09f, 0.06f, 0.19f, 0.97f}, Color{0.04f, 0.03f, 0.10f, 0.98f});
    m_g.StrokeRR(panel, 20, WithA(vl, 0.40f), 1.2f);
    m_g.Glyph(0xE8C8, Rect{panel.x + 18, panel.y + 12, 30, 30}, 17.0f, vl);
    m_g.Text(L"Pano geçmişi", Rect{panel.x + 52, panel.y + 12, 240, 30}, 16.0f, 600, kWhite);
    if (m_items.empty()) {
        m_g.Text(L"Henüz kopyalanmış bir şey yok", Rect{20, 10 + kHead, kW - 40, kRow}, 13.5f, 400, WithA(kWhite, 0.55f), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        const int n = std::min<int>(kRows, static_cast<int>(m_items.size()) - m_top);
        for (int i = 0; i < n; ++i) {
            const int idx = m_top + i;
            const Rect r = RowRect(i);
            const bool sel = idx == m_sel, hot = idx == m_hover;
            if (sel) { m_g.FillRR(r, 12, WithA(vl, 0.26f)); m_g.StrokeRR(r, 12, WithA(vl, 0.7f), 1.2f); }
            else if (hot) m_g.FillRR(r, 12, WithA(vl, 0.12f));
            if (idx < 9) m_g.Text(std::to_wstring(idx + 1), Rect{r.x + 10, r.y, 16, r.height}, 11.5f, 500, WithA(kWhite, 0.38f), DWRITE_TEXT_ALIGNMENT_CENTER);
            m_g.Text(OneLine(m_items[static_cast<size_t>(idx)].text, 160), Rect{r.x + 34, r.y, r.width - 34 - 92, r.height}, 13.5f, sel ? 600 : 400, kWhite);
            m_g.Text(AgeText(m_items[static_cast<size_t>(idx)].at), Rect{r.Right() - 84, r.y, 44, r.height}, 11.5f, 400, WithA(kWhite, 0.42f), DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (sel || hot) m_g.Glyph(0xE711, Rect{r.Right() - 34, r.y + (r.height - 24) / 2, 24, 24}, 10.0f, WithA(kWhite, hot ? 0.7f : 0.45f));   // remove
        }
    }
    const Rect cr = ClearRect();
    m_g.Text(L"Enter: yapıştır   Del: sil   Esc: kapat", Rect{panel.x + 20, cr.y, 270, cr.height}, 11.5f, 400, WithA(kWhite, 0.42f));
    if (!m_items.empty()) m_g.Text(L"Tümünü temizle", cr, 12.0f, 500, WithA(vl, m_hover == -2 ? 1.0f : 0.8f), DWRITE_TEXT_ALIGNMENT_TRAILING);
    m_g.End();
}

LRESULT CALLBACK ClipPanel::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ClipPanel* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<ClipPanel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = h;
    } else self = reinterpret_cast<ClipPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT ClipPanel::Handle(UINT m, WPARAM w, LPARAM l) {
    const int n = static_cast<int>(m_items.size());
    auto reveal = [&]() { if (m_sel < m_top) m_top = m_sel; else if (m_sel >= m_top + kRows) m_top = m_sel - kRows + 1; };
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(m_hwnd, &ps); EndPaint(m_hwnd, &ps); Render(); return 0; }
    case WM_KILLFOCUS: Hide(); return 0;
    case WM_TIMER:
        if (w == 1) {                                                          // paste into the window that was in front
            // Win / Alt still held (Enter pressed right after Win+V): wait a little, or the target would get Win+Ctrl+V
            if ((shellkeys::KeyHeld(VK_LWIN) || shellkeys::KeyHeld(VK_RWIN) || shellkeys::KeyHeld(VK_MENU)) && ++m_pasteTries < 12) return 0;
            m_pasteTries = 0;
            KillTimer(m_hwnd, 1);
            INPUT in[4] = {};
            for (auto& i : in) i.type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_CONTROL; in[1].ki.wVk = 'V'; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, in, sizeof(INPUT));
        }
        return 0;
    case WM_KEYDOWN:
        switch (w) {
            case VK_ESCAPE: Hide(); return 0;
            case VK_RETURN: Commit(m_sel); return 0;
            case VK_UP: if (m_sel > 0) --m_sel; reveal(); break;
            case VK_DOWN: if (m_sel + 1 < n) ++m_sel; reveal(); break;
            case VK_HOME: m_sel = 0; reveal(); break;
            case VK_END: m_sel = std::max(0, n - 1); reveal(); break;
            case VK_DELETE: Remove(m_sel); break;
            default:
                if (w >= '1' && w <= '9' && static_cast<int>(w - '1') < n) { Commit(static_cast<int>(w - '1')); return 0; }
        }
        Render();
        return 0;
    case WM_MOUSEWHEEL: {
        const int d = GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1;
        m_top = std::clamp(m_top + d, 0, std::max(0, n - kRows));
        Render();
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = GET_X_LPARAM(l) / m_scale, y = GET_Y_LPARAM(l) / m_scale;
        const int hr = HitRow(x, y);
        const int hv = hr >= 0 ? hr : (n > 0 && ClearRect().Contains(x, y) ? -2 : -1);
        if (hv != m_hover) { m_hover = hv; Render(); }
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, m_hwnd, 0}; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: if (m_hover != -1) { m_hover = -1; Render(); } return 0;
    case WM_LBUTTONDOWN: {
        const float x = GET_X_LPARAM(l) / m_scale, y = GET_Y_LPARAM(l) / m_scale;
        const int hr = HitRow(x, y);
        if (hr >= 0) {
            const Rect r = RowRect(hr - m_top);
            if (x >= r.Right() - 40) { Remove(hr); Render(); } else Commit(hr);        // the little x removes, the rest pastes
        } else if (n > 0 && ClearRect().Contains(x, y)) { m_items.clear(); m_sel = m_top = 0; m_hover = -1; Reshape(); Render(); }
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

// =============================================================================================================================
//   Win+Arrow : snap / maximize / restore / minimize, Win+Shift+Arrow: to the next monitor
// =============================================================================================================================
std::map<HWND, RECT> g_snapSaved;      // the window's visible rect before it was snapped, to give back on restore

bool Snappable(HWND h) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h)) return false;
    const LONG st = static_cast<LONG>(GetWindowLongPtrW(h, GWL_STYLE));
    if (!(st & WS_THICKFRAME) || !(st & WS_MAXIMIZEBOX)) return false;           // dialogs and fixed-size windows are left alone
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    static const wchar_t* skip[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"AetherBarClass", L"AetherDesktopWindow"};
    for (auto* s : skip) if (!wcscmp(cls, s)) return false;
    return true;
}
RECT VisibleRect(HWND h) {
    RECT r{}; if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    return r;
}
// Puts the VISIBLE part of the window (without the invisible resize borders Windows 10/11 add) at `want`.
void PlaceVisible(HWND h, RECT want) {
    if (IsZoomed(h) || IsIconic(h)) ShowWindow(h, SW_RESTORE);
    RECT wr{}, vr = VisibleRect(h); GetWindowRect(h, &wr);
    const int l = vr.left - wr.left, t = vr.top - wr.top, r = wr.right - vr.right, b = wr.bottom - vr.bottom;
    SetWindowPos(h, nullptr, want.left - l, want.top - t, (want.right - want.left) + l + r, (want.bottom - want.top) + t + b, SWP_NOZORDER | SWP_NOACTIVATE);
}
bool Near(const RECT& a, const RECT& b) { return std::abs(a.left - b.left) <= 3 && std::abs(a.top - b.top) <= 3 && std::abs(a.right - b.right) <= 3 && std::abs(a.bottom - b.bottom) <= 3; }

void SnapWindow(UINT vk, bool shift) {
    for (auto it = g_snapSaved.begin(); it != g_snapSaved.end();) it = IsWindow(it->first) ? std::next(it) : g_snapSaved.erase(it);   // closed windows (and reused handles)
    HWND h = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!Snappable(h)) return;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    const RECT wa = mi.rcWork;
    const int mid = (wa.left + wa.right) / 2;
    const RECT left{wa.left, wa.top, mid, wa.bottom}, right{mid, wa.top, wa.right, wa.bottom};

    if (shift && (vk == VK_LEFT || vk == VK_RIGHT)) {                              // to the neighbouring monitor, keeping the shape
        std::vector<HMONITOR> mons;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM l) -> BOOL { reinterpret_cast<std::vector<HMONITOR>*>(l)->push_back(m); return TRUE; }, reinterpret_cast<LPARAM>(&mons));
        if (mons.size() < 2) return;
        auto leftOf = [](HMONITOR a, HMONITOR b) { MONITORINFO x = {sizeof(x)}, y = {sizeof(y)}; GetMonitorInfoW(a, &x); GetMonitorInfoW(b, &y); return x.rcMonitor.left < y.rcMonitor.left; };
        std::sort(mons.begin(), mons.end(), leftOf);
        const size_t at = static_cast<size_t>(std::find(mons.begin(), mons.end(), mon) - mons.begin());
        const size_t to = vk == VK_RIGHT ? (at + 1) % mons.size() : (at + mons.size() - 1) % mons.size();
        MONITORINFO ni = {sizeof(ni)}; GetMonitorInfoW(mons[to], &ni);
        if (IsZoomed(h)) { ShowWindow(h, SW_RESTORE); }
        const RECT vr = VisibleRect(h);
        const double sx = double(ni.rcWork.right - ni.rcWork.left) / (wa.right - wa.left), sy = double(ni.rcWork.bottom - ni.rcWork.top) / (wa.bottom - wa.top);
        RECT want{ni.rcWork.left + static_cast<LONG>((vr.left - wa.left) * sx), ni.rcWork.top + static_cast<LONG>((vr.top - wa.top) * sy), 0, 0};
        want.right = want.left + static_cast<LONG>((vr.right - vr.left) * sx); want.bottom = want.top + static_cast<LONG>((vr.bottom - vr.top) * sy);
        const bool wasMax = (GetWindowLongPtrW(h, GWL_STYLE) & WS_MAXIMIZE) != 0;
        PlaceVisible(h, want);
        if (wasMax) ShowWindow(h, SW_MAXIMIZE);
        return;
    }

    enum { Normal, Max, Min, Left, Right } st = Normal;
    if (IsIconic(h)) st = Min;
    else if (IsZoomed(h)) st = Max;
    else { const RECT vr = VisibleRect(h); if (Near(vr, left)) st = Left; else if (Near(vr, right)) st = Right; }

    auto restore = [&]() {
        auto it = g_snapSaved.find(h);
        if (it != g_snapSaved.end()) { PlaceVisible(h, it->second); g_snapSaved.erase(it); return; }
        const int w = (wa.right - wa.left) * 7 / 10, hh = (wa.bottom - wa.top) * 7 / 10;
        PlaceVisible(h, RECT{wa.left + ((wa.right - wa.left) - w) / 2, wa.top + ((wa.bottom - wa.top) - hh) / 2, wa.left + ((wa.right - wa.left) - w) / 2 + w, wa.top + ((wa.bottom - wa.top) - hh) / 2 + hh});
    };
    auto snapTo = [&](const RECT& r) {
        if (st == Normal) g_snapSaved[h] = VisibleRect(h);                    // always the CURRENT free-floating rect
        else if (st == Max) { ShowWindow(h, SW_RESTORE); g_snapSaved[h] = VisibleRect(h); }
        PlaceVisible(h, r);
    };
    switch (vk) {
        case VK_LEFT:  if (st == Right) restore(); else if (st != Left) snapTo(left); break;
        case VK_RIGHT: if (st == Left) restore(); else if (st != Right) snapTo(right); break;
        case VK_UP:
            if (st == Min) ShowWindow(h, SW_RESTORE);
            else if (st != Max) { if (st == Normal) g_snapSaved[h] = VisibleRect(h); ShowWindow(h, SW_MAXIMIZE); }
            break;
        case VK_DOWN:
            if (st == Max) { ShowWindow(h, SW_RESTORE); g_snapSaved.erase(h); }
            else if (st == Left || st == Right) restore();
            else if (st == Normal) ShowWindow(h, SW_MINIMIZE);
            break;
    }
}

// =============================================================================================================================
//   the global keyboard hook
// =============================================================================================================================
// The hook lives on a thread of its own that does nothing else. A low-level hook has to answer within ~300 ms; on the main
// thread (window painting, icon loading, Sleep) it sometimes did not, and Windows then removes such a hook SILENTLY: from that
// moment on Windows' own Alt+Tab / Win+R came up again. The thread also puts a fresh hook in place every few seconds in case
// the system dropped it anyway.
HHOOK g_hook = nullptr;
DWORD g_hookThreadId = 0;
HANDLE g_hookThread = nullptr;

bool PostToShell(UINT m, WPARAM w, LPARAM l) { return g_ctl && PostMessageW(g_ctl, m, w, l) != 0; }

LRESULT CALLBACK KeyHook(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION && shellkeys::Route(reinterpret_cast<const KBDLLHOOKSTRUCT*>(l), w, PostToShell, nullptr)) return 1;
    return CallNextHookEx(nullptr, n, w, l);
}

DWORD WINAPI HookThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);              // create the thread's message queue
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    SetTimer(nullptr, 1, 4000, nullptr);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message != WM_TIMER) continue;
        if (shellkeys::g_swActive) continue;                                 // never swap the hook in the middle of a sequence
        if (HHOOK fresh = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0)) {
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

// =============================================================================================================================
//   supervisor + startup items
// =============================================================================================================================
struct Proc {
    std::wstring exe;
    bool required{false};
    HANDLE h{nullptr};
    int crashes{0};
    ULONGLONG windowStart{0};
    ULONGLONG restartAt{0};
    // An elevated exe started through the scheduled task has no handle we can wait on: its window class tells whether it is up.
    const wchar_t* cls{nullptr};
    bool viaTask{false};          // started by the task; supervised through `cls`
    bool seen{false};             // `cls` has been seen at least once since the start
    ULONGLONG deadline{0};        // the task start counts as failed when `cls` has not shown up by then
    int startFails{0};            // consecutive failed start attempts (a required exe is retried before Windows' own shell takes over)
};
std::vector<Proc> g_procs;
bool g_realShell = false, g_fallbackStarted = false, g_elevationDeclined = false;
std::vector<HWND> g_minimized;

// AetherBar.exe asks for administrator rights; a non-elevated process cannot start it directly. The autostart option creates a
// highest-privilege task for it, which can be run without a UAC prompt.
bool StartViaTask(const std::wstring& exe) {
    const size_t k = exe.find_last_of(L"\\/");
    std::wstring name = k == std::wstring::npos ? exe : exe.substr(k + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tool = std::wstring(sys) + L"\\schtasks.exe";
    std::wstring cl = L"\"" + tool + L"\" /Run /TN \"" + name + L"\"";
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(tool.c_str(), cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    DWORD code = 0;     // a slow Task Scheduler (right at logon) is not a failure: Supervise() checks that the bar really came up
    if (WaitForSingleObject(pi.hProcess, 4000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// The task is missing / points to another copy of the exe / did not bring the exe up: ask for the elevation directly (one UAC prompt).
// The elevated bar then repairs the task itself, so this is a one-time cost.
bool StartElevated(Proc& p) {
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    const std::wstring dir = ExeDir();
    g_elevationDeclined = false;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas"; sei.lpFile = p.exe.c_str(); sei.lpDirectory = dir.c_str(); sei.nShow = SW_SHOWNORMAL;
    sei.lpParameters = L"--repair-task";                            // the elevated bar re-points its logon task to this copy
    if (!ShellExecuteExW(&sei)) { g_elevationDeclined = GetLastError() == ERROR_CANCELLED; return false; }
    if (p.h) CloseHandle(p.h);
    p.h = sei.hProcess;                                             // may be null on some systems: then `cls` supervises
    p.viaTask = p.h == nullptr;
    p.seen = false; p.deadline = GetTickCount64() + 8000;
    return true;
}

bool StartProc(Proc& p) {
    if (!FileExists(p.exe)) return false;
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    std::wstring cl = L"\"" + p.exe + L"\"";
    const std::wstring dir = ExeDir();
    if (!CreateProcessW(p.exe.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        if (GetLastError() != ERROR_ELEVATION_REQUIRED) return false;
        p.viaTask = false; p.seen = false;
        if (p.cls && FindWindowW(p.cls, nullptr)) { p.viaTask = true; p.seen = true; return true; }   // already running (elevated): adopt it
        // The task is only trusted when it starts THIS exe; schtasks /Run says "ok" for a task whose file is gone or crashes at once.
        if (aether::system::bartask::Exists() && aether::system::bartask::RunsExe(p.exe) && StartViaTask(p.exe)) {
            p.viaTask = true; p.deadline = GetTickCount64() + 8000;
            return true;
        }
        return StartElevated(p);
    }
    CloseHandle(pi.hThread);
    if (p.h) CloseHandle(p.h);
    p.h = pi.hProcess;
    p.viaTask = false;
    return true;
}

void StartExplorerFallback() {
    if (g_fallbackStarted) return;
    g_fallbackStarted = true;
    wchar_t sys[MAX_PATH]; GetWindowsDirectoryW(sys, MAX_PATH);
    const std::wstring ex = std::wstring(sys) + L"\\explorer.exe";
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    std::wstring cl = L"\"" + ex + L"\"";
    if (CreateProcessW(ex.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

// A required exe (the bar) that could not be started is retried for a while before Windows' own shell takes over. Falling back at the first
// hiccup (the Task Scheduler is often not ready yet right at logon) put Windows' taskbar on screen, and the bar then came up under / over it.
// A missing exe or a declined UAC prompt will not get better by asking again: those go to Windows' shell at once.
void StartFailed(Proc& p, bool final = false) {
    if (!p.required || !g_realShell) return;
    if (!final && ++p.startFails < 6) p.restartAt = GetTickCount64() + 2500;
    else StartExplorerFallback();
}

void Supervise() {
    const ULONGLONG now = GetTickCount64();
    for (auto& p : g_procs) {
        if (p.viaTask) {                                            // started by the task: no handle yet, the window tells
            HWND w = p.cls ? FindWindowW(p.cls, nullptr) : nullptr;
            if (w) {
                DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
                if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { p.h = h; p.viaTask = false; p.startFails = 0; }
                else p.seen = true;                                 // cannot wait on it, but it is up
                if (!p.h) continue;
            } else {
                p.viaTask = false;
                if (p.seen) continue;                               // it was up and has gone (closed): leave it closed
                if (now < p.deadline) { p.viaTask = true; continue; }   // still starting
                // the task did not bring it up in time (stale path, crash): ask directly
                if (!StartElevated(p)) StartFailed(p, g_elevationDeclined);
                continue;
            }
        }
        if (!p.h) {
            if (p.restartAt && now >= p.restartAt) {                // a delayed restart (never Sleep here: the main thread must stay responsive)
                p.restartAt = 0;
                if (!StartProc(p)) StartFailed(p, g_elevationDeclined || !FileExists(p.exe));
            }
            continue;
        }
        if (WaitForSingleObject(p.h, 0) != WAIT_OBJECT_0) continue;
        DWORD code = 0; GetExitCodeProcess(p.h, &code);
        CloseHandle(p.h); p.h = nullptr;
        if (code == 0) continue;                                   // closed on purpose: stay closed
        if (now - p.windowStart > 120000) { p.windowStart = now; p.crashes = 0; }
        if (++p.crashes > 4) {                                      // crash loop
            if (p.required && g_realShell) StartExplorerFallback();
            continue;
        }
        p.restartAt = now + 400;
    }
}

// ---- logon startup items (what explorer.exe does) -------------------------------------------------------------------------
bool StartupApproved(HKEY root, const wchar_t* sub, const std::wstring& name) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return true;
    BYTE data[16] = {}; DWORD sz = sizeof(data), type = 0;
    const LONG r = RegQueryValueExW(k, name.c_str(), nullptr, &type, data, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_BINARY || sz < 1) return true;
    return (data[0] & 1) == 0;                                      // 02 / 06 = enabled, 03 / 07 = disabled by the user
}

void RunCommand(const std::wstring& raw) {
    if (raw.empty()) return;
    wchar_t ex[4096] = {};
    ExpandEnvironmentStringsW(raw.c_str(), ex, 4096);
    std::wstring cl = ex;
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return; }
    ShellExecuteW(nullptr, L"open", cl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RunRegKey(HKEY root, const wchar_t* sub, REGSAM view, const wchar_t* approvedSub, bool once) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | (once ? KEY_WRITE : 0) | view, &k) != ERROR_SUCCESS) return;
    std::vector<std::pair<std::wstring, std::wstring>> items;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512]; DWORD nl = 512; BYTE data[8192]; DWORD dl = sizeof(data), type = 0;
        const LONG r = RegEnumValueW(k, i, name, &nl, nullptr, &type, data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        items.emplace_back(name, reinterpret_cast<wchar_t*>(data));
    }
    for (auto& it : items) {
        if (approvedSub && !StartupApproved(root, approvedSub, it.first)) continue;
        if (once) RegDeleteValueW(k, it.first.c_str());               // RunOnce entries are consumed before they run
        RunCommand(it.second);
        Sleep(120);
    }
    RegCloseKey(k);
}

void RunStartupFolder(const KNOWNFOLDERID& id, HKEY approvedRoot, const wchar_t* approvedSub) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) return;
    const std::wstring dir = p; CoTaskMemFree(p);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
        if (!StartupApproved(approvedRoot, approvedSub, fd.cFileName)) continue;
        ShellExecuteW(nullptr, L"open", (dir + L"\\" + fd.cFileName).c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        Sleep(150);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void RunStartupItems() {
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Sleep(2500);                                                // let the bar / desktop come up first, like Windows does
        const wchar_t* approvedBase = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved";
        const std::wstring run = std::wstring(approvedBase) + L"\\Run", run32 = std::wstring(approvedBase) + L"\\Run32", folder = std::wstring(approvedBase) + L"\\StartupFolder";
        RunRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", KEY_WOW64_64KEY, nullptr, true);
        RunRegKey(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0, nullptr, true);
        RunRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_64KEY, run.c_str(), false);
        RunRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_32KEY, run32.c_str(), false);
        RunRegKey(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, run.c_str(), false);
        RunStartupFolder(FOLDERID_CommonStartup, HKEY_LOCAL_MACHINE, folder.c_str());
        RunStartupFolder(FOLDERID_Startup, HKEY_CURRENT_USER, folder.c_str());
        CoUninitialize();
    }).detach();
}

// ---- Win+M ------------------------------------------------------------------------------------------------------------------
void MinimizeAll(bool restore) {
    if (restore) {
        for (auto it = g_minimized.rbegin(); it != g_minimized.rend(); ++it) if (IsWindow(*it) && IsIconic(*it)) ShowWindow(*it, SW_RESTORE);
        g_minimized.clear();
        return;
    }
    g_minimized.clear();
    for (HWND h : AppWindows(false)) if (GetWindowLongPtrW(h, GWL_STYLE) & WS_MINIMIZEBOX) { g_minimized.push_back(h); ShowWindow(h, SW_SHOWMINNOACTIVE); }
}

LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case kMsgSwStart: g_sw.Start(w != 0, l != 0); return 0;
    case kMsgSwNext: g_sw.Next(static_cast<int>(static_cast<INT_PTR>(w))); return 0;
    case kMsgSwMove: g_sw.Move(static_cast<UINT>(w)); return 0;
    case kMsgSwCommit: g_sw.Commit(); return 0;
    case kMsgSwCancel: g_sw.Cancel(); return 0;
    case kMsgRun: g_run.Open(); return 0;
    case shellkeys::kMsgClip: g_clip.Open(); return 0;
    case shellkeys::kMsgSnap: SnapWindow(static_cast<UINT>(w), l != 0); return 0;
    case WM_CLIPBOARDUPDATE: g_clip.OnClipboardChanged(); return 0;
    case WM_APP + 13: g_clip.AddText(reinterpret_cast<std::wstring*>(l)); return 0;     // the clipboard worker's text
    case kMsgMinAll: MinimizeAll(w != 0); return 0;
    case kMsgSettings: ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL); return 0;
    case kMsgWinCombo:                                              // the bar must not open Start when the Win key is released
        if (HWND bar = FindWindowW(L"AetherBarClass", nullptr)) PostMessageW(bar, kMsgWinCombo, 0, 0);
        return 0;
    case WM_TIMER: if (w == kTimerSupervise) Supervise(); return 0;
    case WM_ENDSESSION: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    aether::core::InstallCrashHandler(L"AetherShell");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!aether::core::AcquireSingleInstance(L"Aether.Shell.SingleInstance")) return 0;
    SetCurrentProcessExplicitAppUserModelID(L"Aether.Shell");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_realShell = GetShellWindow() == nullptr;                      // nobody is the shell yet: we are being started by Windows
    if (g_realShell) {
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift || FileExists(AppData() + L"\\use-explorer")) { StartExplorerFallback(); return 0; }   // the escape hatch
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = CtlProc; wc.hInstance = inst; wc.lpszClassName = shellkeys::kCtlClass;
    RegisterClassExW(&wc);
    g_ctl = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!g_ctl) return 1;
    if (g_realShell) AddClipboardFormatListener(g_ctl);              // the clipboard history (Win+V); with explorer.exe running, Windows has its own

    const std::wstring dir = ExeDir();
    if (g_realShell && !FileExists(AppData() + L"\\no-intro")) {          // greeting animation at logon; the desktop loads behind it
        const std::wstring intro = dir + L"\\AetherIntro.exe";
        if (FileExists(intro)) {
            STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
            std::wstring cl = L"\"" + intro + L"\"";
            if (CreateProcessW(intro.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        }
    }
    g_procs.push_back({dir + L"\\AetherBar.exe", true});
    g_procs.back().cls = L"AetherBarClass";
    g_procs.push_back({dir + L"\\AetherDesktop.exe", false});
    g_procs.push_back({dir + L"\\Aether.exe", false});
    for (auto& p : g_procs) {
        p.windowStart = GetTickCount64();
        if (StartProc(p)) Sleep(250);
        else StartFailed(p, g_elevationDeclined || !FileExists(p.exe));   // no bar = no usable desktop: retried, and only then Windows' own shell
    }
    SetTimer(g_ctl, kTimerSupervise, 2000, nullptr);
    StartKeyHook();
    if (g_realShell) RunStartupItems();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    StopKeyHook();
    return 0;
}
