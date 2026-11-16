#include "aether/bar/window_preview.hpp"
#include "aether/bar/ui_common.hpp"
#include "aether/core/monitors.hpp"
#include "aether/render/glass.hpp"
#include "aether/system/app_identity.hpp"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;
namespace gl = ui;

namespace {
inline D2D1_RECT_F RF(const Rect& r) { return D2D1::RectF(r.Left(), r.Top(), r.Right(), r.Bottom()); }
inline float C01(float v) { return std::clamp(v, 0.0f, 1.0f); }
constexpr wchar_t kClass[] = L"AetherWindowPreviewClass";
constexpr int kMaxCards = 12;

std::wstring TitleOf(HWND h) {
    wchar_t b[256];
    const int n = GetWindowTextW(h, b, 256);
    return std::wstring(b, n > 0 ? n : 0);
}
HICON WindowIconOf(HWND h) {
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return reinterpret_cast<HICON>(r);
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 40, &r) && r) return reinterpret_cast<HICON>(r);
    return reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON));
}
} // namespace

WindowPreview::WindowPreview()
    : m_anim(0.0f, animation::SpringConfig{520.0f, 38.0f, 1.0f, 0.002f}) {}

WindowPreview::~WindowPreview() {
    ReleaseThumbs();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

bool WindowPreview::Create(ActivateFn activate) {
    m_activate = std::move(activate);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);   // harmless when another bar registered it already
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"Aether Preview", WS_POPUP,
                             0, 0, 64, 64, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;
    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, 64, 64)) { DestroyWindow(m_hwnd); m_hwnd = nullptr; return false; }
    m_brush = m_dcomp->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1.0f});
    return true;
}

LRESULT CALLBACK WindowPreview::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    WindowPreview* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<WindowPreview*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else self = reinterpret_cast<WindowPreview*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

int WindowPreview::CardAt(const Vec2& p) const {
    for (int i = 0; i < static_cast<int>(m_cards.size()); ++i) if (m_cards[i].rect.Contains(p)) return i;
    return -1;
}

void WindowPreview::CloseCard(int i) {
    if (i < 0 || i >= static_cast<int>(m_cards.size())) return;
    PostMessageW(m_cards[i].hwnd, WM_CLOSE, 0, 0);
    if (m_cards[i].thumb) DwmUnregisterThumbnail(m_cards[i].thumb);
    m_cards.erase(m_cards.begin() + i);
    if (m_cards.empty()) { Hide(); return; }
    m_hover = -1;
    Layout();
}

LRESULT WindowPreview::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(l)), static_cast<float>(GET_Y_LPARAM(l))};
            if (!m_tracking) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); m_tracking = true; }
            m_inside = true;
            const int c = CardAt(p);
            const bool onClose = c >= 0 && m_cards[c].close.Contains(p);
            if (c != m_hover || onClose != m_hoverClose) { m_hover = c; m_hoverClose = onClose; m_dirty = true; }
            return 0;
        }
        case WM_MOUSELEAVE: m_tracking = false; m_inside = false; m_hover = -1; m_hoverClose = false; m_dirty = true; return 0;
        case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) { gl::ApplyCursor(m_hover >= 0); return TRUE; } break;
        case WM_LBUTTONUP: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(l)), static_cast<float>(GET_Y_LPARAM(l))};
            const int c = CardAt(p);
            if (c < 0) return 0;
            if (m_cards[c].close.Contains(p)) { CloseCard(c); return 0; }
            const HWND target = m_cards[c].hwnd;
            Hide();
            if (m_activate) m_activate(target);
            return 0;
        }
        case WM_MBUTTONUP: {
            const int c = CardAt(Vec2{static_cast<float>(GET_X_LPARAM(l)), static_cast<float>(GET_Y_LPARAM(l))});
            if (c >= 0) CloseCard(c);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void WindowPreview::ReleaseThumbs() {
    for (auto& c : m_cards) { if (c.thumb) { DwmUnregisterThumbnail(c.thumb); c.thumb = nullptr; } c.hasThumb = false; }
}

void WindowPreview::Show(HMONITOR mon, float anchorScreenX, int dockEdgeScreenY, bool below, const std::wstring& exePath, const std::vector<HWND>& windows) {
    if (!m_hwnd || windows.empty()) return;
    m_mon = mon; m_anchorX = anchorScreenX; m_dockTop = dockEdgeScreenY; m_below = below;
    const float s = core::MonitorScale(mon);
    if (s != m_scale || !m_fTitle) {
        m_scale = s;
        if (auto dw = m_dcomp->GetDWriteFactory()) {
            ComPtr<IDWriteFontCollection> col; dw->GetSystemFontCollection(&col, FALSE);
            auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL e = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &e)) && e; };
            const wchar_t* text = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
            const wchar_t* ico = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
            m_hasIconFont = ico != nullptr;
            auto mk = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* fam, DWRITE_FONT_WEIGHT wt, float sz, DWRITE_TEXT_ALIGNMENT al) {
                f.Reset(); if (!fam) return;
                dw->CreateTextFormat(fam, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sz * s, L"en-us", &f);
                if (!f) return;
                f->SetTextAlignment(al); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; ComPtr<IDWriteInlineObject> e;
                dw->CreateEllipsisTrimmingSign(f.Get(), &e); f->SetTrimming(&t, e.Get());
            };
            mk(m_fTitle, text, DWRITE_FONT_WEIGHT_NORMAL, 12.5f, DWRITE_TEXT_ALIGNMENT_LEADING);
            mk(m_fIcon, ico, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    // the app icon, shared by every card
    const std::wstring key = exePath.empty() ? L"hwnd:" + std::to_wstring(reinterpret_cast<uintptr_t>(windows.front())) : exePath;
    if (key != m_iconKey || !m_icon) {
        m_iconKey = key; m_icon.Reset();
        const int px = static_cast<int>(64 * s);
        if (!exePath.empty()) m_icon = gl::ExeIconBitmap(m_dcomp->GetContext(), exePath, 128);
        if (!m_icon) if (HICON ic = WindowIconOf(windows.front())) m_icon = gl::IconToBitmap(m_dcomp->GetContext(), ic, px);
    }
    m_exe = exePath;
    ReleaseThumbs();
    m_cards.clear();
    for (HWND w : windows) {
        if (static_cast<int>(m_cards.size()) >= kMaxCards) break;
        Card c; c.hwnd = w; c.title = TitleOf(w);
        m_cards.push_back(std::move(c));
    }
    m_hover = -1; m_hoverClose = false; m_inside = false;
    const bool wasOpen = m_open;
    m_open = true;
    Layout();
    if (!wasOpen) m_anim.SnapTo(0.0f);
    m_anim.SetTarget(1.0f);
    m_dirty = true;
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Render();
}

void WindowPreview::SetWindows(const std::vector<HWND>& windows) {
    if (!m_open) return;
    bool same = windows.size() == m_cards.size() || static_cast<int>(windows.size()) > kMaxCards;
    if (same) for (size_t i = 0; i < m_cards.size() && i < windows.size(); ++i) if (m_cards[i].hwnd != windows[i]) { same = false; break; }
    if (same) return;
    if (windows.empty()) { Hide(); return; }
    ReleaseThumbs();
    std::vector<Card> keep;
    for (HWND w : windows) {
        if (static_cast<int>(keep.size()) >= kMaxCards) break;
        Card c; c.hwnd = w; c.title = TitleOf(w);
        keep.push_back(std::move(c));
    }
    m_cards = std::move(keep);
    m_hover = -1;
    Layout();
    m_dirty = true;
}

void WindowPreview::Hide() {
    if (!m_open) return;
    m_open = false; m_inside = false; m_hover = -1;
    m_anim.SetTarget(0.0f);
}

void WindowPreview::Teardown() {
    ReleaseThumbs();
    m_cards.clear();
    if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
    m_anim.SnapTo(0.0f);
    m_inside = false; m_tracking = false;
}

void WindowPreview::Layout() {
    if (!m_hwnd || m_cards.empty()) return;
    const float s = m_scale;
    const float cardW = 236 * s, thumbH = 130 * s, headH = 34 * s, gap = 10 * s, pad = 12 * s, cardH = headH + thumbH + 8 * s;
    m_pad = 6 * s;
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(m_mon, &mi);
    const float maxW = static_cast<float>(mi.rcMonitor.right - mi.rcMonitor.left) - 48 * s;
    const int perRow = std::clamp(static_cast<int>((maxW - 2 * pad + gap) / (cardW + gap)), 1, 6);
    const int n = static_cast<int>(m_cards.size());
    const int cols = std::min(n, perRow), rows = (n + perRow - 1) / perRow;
    m_panelW = 2 * pad + cols * cardW + (cols - 1) * gap;
    m_panelH = 2 * pad + rows * cardH + (rows - 1) * gap;
    const int ww = static_cast<int>(std::ceil(m_panelW + 2 * m_pad)), wh = static_cast<int>(std::ceil(m_panelH + 2 * m_pad));

    for (int i = 0; i < n; ++i) {
        Card& c = m_cards[i];
        const int col = i % perRow, row = i / perRow;
        c.rect = Rect{m_pad + pad + col * (cardW + gap), m_pad + pad + row * (cardH + gap), cardW, cardH};
        c.head = Rect{c.rect.x, c.rect.y, cardW, headH};
        c.thumbArea = Rect{c.rect.x + 8 * s, c.rect.y + headH, cardW - 16 * s, thumbH};
        c.close = Rect{c.rect.Right() - 30 * s, c.rect.y + (headH - 22 * s) / 2, 22 * s, 22 * s};
    }
    int x = static_cast<int>(m_anchorX) - ww / 2;
    x = std::clamp(x, static_cast<int>(mi.rcMonitor.left) + 6, std::max(static_cast<int>(mi.rcMonitor.left) + 6, static_cast<int>(mi.rcMonitor.right) - ww - 6));
    const int y = m_below ? m_dockTop + static_cast<int>(10 * s) - static_cast<int>(m_pad)
                          : m_dockTop - static_cast<int>(10 * s) - wh + static_cast<int>(m_pad);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, ww, wh, SWP_NOACTIVATE);
    m_dcomp->Resize(ww, wh);
    // only the panel takes the mouse: the margin around it stays click-through
    HRGN rgn = CreateRoundRectRgn(static_cast<int>(m_pad), static_cast<int>(m_pad), static_cast<int>(m_pad + m_panelW) + 1, static_cast<int>(m_pad + m_panelH) + 1,
                                  static_cast<int>(44 * s), static_cast<int>(44 * s));
    SetWindowRgn(m_hwnd, rgn, FALSE);
    RegisterThumbs();
}

void WindowPreview::RegisterThumbs() {
    ReleaseThumbs();
    for (auto& c : m_cards) {
        if (DwmRegisterThumbnail(m_hwnd, c.hwnd, &c.thumb) != S_OK || !c.thumb) { c.thumb = nullptr; continue; }
        SIZE sz{};
        if (DwmQueryThumbnailSourceSize(c.thumb, &sz) != S_OK || sz.cx <= 0 || sz.cy <= 0) {
            DwmUnregisterThumbnail(c.thumb); c.thumb = nullptr; continue;
        }
        c.src = sz; c.hasThumb = true;
        const float tw = c.thumbArea.width, th = c.thumbArea.height;
        const float k = std::min(1.0f, std::min(tw / sz.cx, th / sz.cy));                 // never enlarge a small window
        const float dw = sz.cx * k, dh = sz.cy * k;
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
        p.rcDestination = RECT{static_cast<LONG>(c.thumbArea.x + (tw - dw) / 2), static_cast<LONG>(c.thumbArea.y + (th - dh) / 2),
                               static_cast<LONG>(c.thumbArea.x + (tw + dw) / 2), static_cast<LONG>(c.thumbArea.y + (th + dh) / 2)};
        p.fVisible = TRUE;
        p.opacity = static_cast<BYTE>(255.0f * C01(m_anim.GetValue()));
        DwmUpdateThumbnailProperties(c.thumb, &p);
    }
}

void WindowPreview::UpdateThumbOpacity() {
    const BYTE a = static_cast<BYTE>(255.0f * C01(m_anim.GetValue()));
    for (auto& c : m_cards) {
        if (!c.thumb) continue;
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_OPACITY; p.opacity = a;
        DwmUpdateThumbnailProperties(c.thumb, &p);
    }
}

bool WindowPreview::Frame(float dt) {
    if (!m_hwnd) return false;
    render::glass::SyncTheme();
    m_anim.Update(dt);
    if (!m_open && m_anim.IsResting() && IsWindowVisible(m_hwnd)) { Teardown(); return false; }
    if (!IsWindowVisible(m_hwnd)) return false;

    if (m_open) {
        // windows closed by the app itself disappear from the preview; titles follow the window (a browser tab switch)
        m_titleTimer += dt;
        if (m_titleTimer >= 0.5f) {
            m_titleTimer = 0.0f;
            bool relayout = false;
            for (int i = static_cast<int>(m_cards.size()) - 1; i >= 0; --i) {
                if (!IsWindow(m_cards[i].hwnd)) {
                    if (m_cards[i].thumb) DwmUnregisterThumbnail(m_cards[i].thumb);
                    m_cards.erase(m_cards.begin() + i); relayout = true; continue;
                }
                const std::wstring t = TitleOf(m_cards[i].hwnd);
                if (t != m_cards[i].title) { m_cards[i].title = t; m_dirty = true; }
            }
            if (m_cards.empty()) { Hide(); return true; }
            if (relayout) { Layout(); m_dirty = true; }
        }
    }
    const bool animating = !m_anim.IsResting();
    if (animating) UpdateThumbOpacity();
    if (animating || m_dirty) { m_dirty = false; Render(); }
    return animating;
}

void WindowPreview::Render() {
    if (!m_dcomp || !m_hwnd || m_cards.empty()) return;
    const float s = m_scale, o = C01(m_anim.GetValue());
    const Rect panel{m_pad, m_pad, m_panelW, m_panelH};
    auto body = m_dcomp->CreateSquircleGeometry(panel, 22 * s);
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;
    D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), o, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    ctx->PushLayer(&lp, nullptr);
    if (body) render::DrawGlassBody(ctx, m_dcomp.get(), m_brush.Get(), body.Get(), panel, s, 1.0f, 1.0f);

    const Color acc = render::glass::kVioletLight;
    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    for (int i = 0; i < static_cast<int>(m_cards.size()); ++i) {
        const Card& c = m_cards[i];
        const bool hot = i == m_hover, active = c.hwnd == fg || c.hwnd == fgRoot;
        if (auto g = m_dcomp->CreateSquircleGeometry(c.rect, 16 * s)) {
            m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, hot ? 0.11f : 0.045f));
            ctx->FillGeometry(g.Get(), m_brush.Get());
            if (active || hot) {
                m_brush->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, active ? 0.75f : 0.35f));
                ctx->DrawGeometry(g.Get(), m_brush.Get(), 1.3f * s);
            }
        }
        // header: app icon, title, close button
        if (m_icon) {
            const float isz = 18 * s;
            const D2D1_RECT_F d = D2D1::RectF(c.head.x + 12 * s, c.head.y + (c.head.height - isz) / 2, c.head.x + 12 * s + isz, c.head.y + (c.head.height + isz) / 2);
            ctx->DrawBitmap(m_icon.Get(), &d, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        if (m_fTitle) {
            const Rect tr{c.head.x + 38 * s, c.head.y, c.head.width - 38 * s - 38 * s, c.head.height};
            m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, active || hot ? 0.96f : 0.80f));
            const D2D1_RECT_F rc = RF(tr);
            const std::wstring& t = c.title.empty() ? std::wstring(L"-") : c.title;
            ctx->DrawText(t.c_str(), static_cast<UINT32>(t.size()), m_fTitle.Get(), &rc, m_brush.Get());
        }
        if (hot && m_hasIconFont && m_fIcon) {
            if (m_hoverClose) {
                if (auto g = m_dcomp->CreateSquircleGeometry(c.close, 8 * s)) {
                    m_brush->SetColor(D2D1::ColorF(0.92f, 0.28f, 0.30f, 0.90f));
                    ctx->FillGeometry(g.Get(), m_brush.Get());
                }
            }
            m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, m_hoverClose ? 1.0f : 0.7f));
            const wchar_t x = 0xE711;                                        // Segoe Fluent "Cancel"
            const D2D1_RECT_F rc = RF(c.close);
            ctx->DrawText(&x, 1, m_fIcon.Get(), &rc, m_brush.Get());
        }
        // no live thumbnail (minimised UWP window ...): a large icon instead
        if (!c.hasThumb && m_icon) {
            const float isz = 56 * s;
            const D2D1_RECT_F d = D2D1::RectF(c.thumbArea.Center().x - isz / 2, c.thumbArea.Center().y - isz / 2, c.thumbArea.Center().x + isz / 2, c.thumbArea.Center().y + isz / 2);
            ctx->DrawBitmap(m_icon.Get(), &d, 0.85f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else if (c.hasThumb) {
            if (auto g = m_dcomp->CreateSquircleGeometry(c.thumbArea, 8 * s)) {              // quiet plate behind the thumbnail
                m_brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f));
                ctx->FillGeometry(g.Get(), m_brush.Get());
            }
        }
    }
    ctx->PopLayer();
    // The thumbnail areas stay see-through: DWM draws the live window there (above or below this surface, both work).
    for (const Card& c : m_cards) {
        if (!c.hasThumb) continue;
        const D2D1_RECT_F r = RF(c.thumbArea);
        ctx->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        ctx->Clear(D2D1::ColorF(0, 0, 0, 0.0f));
        ctx->PopAxisAlignedClip();
    }
    m_dcomp->EndDraw();
}

} // namespace aether::bar
