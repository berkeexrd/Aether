#include "aether/bar/toast_host.hpp"
#include "aether/core/monitors.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <map>

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;
using Microsoft::WRL::ComPtr;
namespace i18n = aether::i18n;

namespace {
constexpr wchar_t kClass[] = L"Aether.ToastHost";
constexpr float kCardW = 360.0f;          // logical px
constexpr float kPad = 24.0f;             // transparent margin around the stack (shadow room)
constexpr float kGap = 10.0f;
constexpr float kRadius = 20.0f;
constexpr float kEdge = 12.0f;            // distance from the screen edge
constexpr int kMaxVisible = 4;
constexpr float kMaxCardH = 102.0f;

inline Color W(float a) { return Color{1.0f, 1.0f, 1.0f, a}; }
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float EaseOut(float x) { x = Clamp01(x); const float u = 1.0f - x; return 1.0f - u * u * u; }
inline float Back(float x) { x = Clamp01(x); const float c1 = 1.0f, c3 = c1 + 1.0f; const float u = x - 1.0f; return 1.0f + c3 * u * u * u + c1 * u * u; }

std::wstring Clean(const std::wstring& s) {
    std::wstring o; o.reserve(s.size());
    bool sp = false;
    for (wchar_t ch : s) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L' ') { if (!sp && !o.empty()) o.push_back(L' '); sp = true; }
        else { o.push_back(ch); sp = false; }
    }
    while (!o.empty() && o.back() == L' ') o.pop_back();
    return o;
}
} // namespace

ToastHost::~ToastHost() { Shutdown(); }

void ToastHost::Shutdown() {
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    m_dcomp.reset();
}

LRESULT CALLBACK ToastHost::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ToastHost* self = nullptr;
    if (m == WM_NCCREATE) { self = static_cast<ToastHost*>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams); SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    else self = reinterpret_cast<ToastHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

bool ToastHost::Create() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    m_scale = core::MonitorScale(mon);
    const int ww = static_cast<int>((kCardW + kPad * 2) * m_scale), wh = static_cast<int>((kMaxVisible * (kMaxCardH + kGap) + kPad * 2) * m_scale);
    // never takes focus, and mouse clicks fall through everywhere except on a card (the style is toggled in Frame)
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kClass, L"Aether Toasts", WS_POPUP,
                             0, 0, ww, wh, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;
    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, static_cast<uint32_t>(ww), static_cast<uint32_t>(wh))) return false;
    m_brush = m_dcomp->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1.0f});
    MakeFonts();
    m_cfg = system::notifycfg::Load();
    m_cfgStamp = system::notifycfg::Stamp();
    m_lastTest = m_cfg.test;
    return true;
}

void ToastHost::MakeFonts() {
    auto dw = m_dcomp ? m_dcomp->GetDWriteFactory() : nullptr;
    if (!dw) return;
    ComPtr<IDWriteFontCollection> col; dw->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL e = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &e)) && e; };
    const wchar_t* disp = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* text = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* ico = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
    auto mk = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* fam, DWRITE_FONT_WEIGHT wt, float sz, DWRITE_TEXT_ALIGNMENT al, bool wrap, DWRITE_PARAGRAPH_ALIGNMENT pa) {
        f.Reset(); if (!fam) return;
        dw->CreateTextFormat(fam, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sz * m_scale, L"en-us", &f);
        if (!f) return;
        f->SetTextAlignment(al); f->SetParagraphAlignment(pa);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; ComPtr<IDWriteInlineObject> e;
            dw->CreateEllipsisTrimmingSign(f.Get(), &e); f->SetTrimming(&t, e.Get());
        }
    };
    mk(m_fApp,     text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    mk(m_fTitle,   disp, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.5f, DWRITE_TEXT_ALIGNMENT_LEADING, false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    mk(m_fBody,    text, DWRITE_FONT_WEIGHT_NORMAL,    12.5f, DWRITE_TEXT_ALIGNMENT_LEADING, true,  DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    mk(m_fIcon,    ico,  DWRITE_FONT_WEIGHT_NORMAL,    11.0f, DWRITE_TEXT_ALIGNMENT_CENTER,  false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    mk(m_fInitial, disp, DWRITE_FONT_WEIGHT_BOLD,      17.0f, DWRITE_TEXT_ALIGNMENT_CENTER,  false, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

bool ToastHost::Quiet() {
    QUERY_USER_NOTIFICATION_STATE st = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&st))) return false;
    return st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;   // a game, a full-screen video or a presentation
}

void ToastHost::PollConfig(float dt) {
    m_cfgTimer += dt;
    if (m_cfgTimer < 0.4f) return;
    m_cfgTimer = 0.0f;
    const unsigned long long st = system::notifycfg::Stamp();
    if (st == m_cfgStamp) return;
    m_cfgStamp = st;
    m_cfg = system::notifycfg::Load();
    NativeToastHider::Enable(m_cfg.enabled);
    if (m_cfg.test != m_lastTest) {
        m_lastTest = m_cfg.test;
        if (m_cfg.test != 0) PushText(L"Aether", i18n::Pick(L"Notifications are working", L"Bildirimler çalışıyor"),
                                      i18n::Pick(L"This is how a notification looks. Hover to keep it, click to open the app.", L"Bir bildirim böyle görünür. Tutmak için üstüne gel, uygulamayı açmak için tıkla."));
    }
}

void ToastHost::Prepare(Toast& t) {
    t.layout.Reset(); t.bodyLines = 0;
    auto dw = m_dcomp ? m_dcomp->GetDWriteFactory() : nullptr;
    const std::wstring body = Clean(t.body);
    if (dw && m_fBody && !body.empty()) {
        const float bw = (kCardW - 70.0f - 18.0f) * m_scale;
        ComPtr<IDWriteTextLayout> l;
        if (SUCCEEDED(dw->CreateTextLayout(body.c_str(), static_cast<UINT32>(body.size()), m_fBody.Get(), bw, 400.0f * m_scale, &l)) && l) {
            DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm);
            int lines = static_cast<int>(tm.lineCount);
            if (lines > 2) {                                           // two lines at most: cut the text and end it with an ellipsis
                std::vector<DWRITE_LINE_METRICS> lm(tm.lineCount); UINT32 n = 0;
                l->GetLineMetrics(lm.data(), tm.lineCount, &n);
                const size_t cut = std::min<size_t>(body.size(), static_cast<size_t>(lm[0].length) + lm[1].length);
                std::wstring fit = body.substr(0, cut);
                while (!fit.empty() && iswspace(fit.back())) fit.pop_back();
                if (fit.size() > 1) fit.pop_back();
                fit += L"…";
                ComPtr<IDWriteTextLayout> l2;
                if (SUCCEEDED(dw->CreateTextLayout(fit.c_str(), static_cast<UINT32>(fit.size()), m_fBody.Get(), bw, 400.0f * m_scale, &l2)) && l2) l = l2;
                lines = 2;
            }
            t.layout = l; t.bodyLines = std::clamp(lines, 1, 2);
        }
    }
    t.cardH = t.bodyLines ? 68.0f + 17.0f * t.bodyLines : 62.0f;
}

void ToastHost::PushText(const std::wstring& app, const std::wstring& title, const std::wstring& body) {
    NotifItem n; n.id = m_nextLocalId--; n.app = app; n.title = title; n.body = body;
    Push(n);
}

void ToastHost::Push(const NotifItem& n) {
    if (!m_hwnd || !m_dcomp) return;
    for (const auto& e : m_toasts) if (e.id == n.id) return;
    Toast t;
    t.id = n.id; t.app = n.app; t.appId = n.appId; t.title = n.title.empty() ? n.app : n.title; t.body = n.body;
    t.label = n.app;
    t.life = static_cast<float>(std::clamp(m_cfg.seconds, 3, 30));
    Prepare(t);
    m_toasts.insert(m_toasts.begin(), std::move(t));
    int live = 0;
    for (auto& e : m_toasts) { if (e.closing) continue; if (++live > kMaxVisible) e.closing = true; }   // the oldest make room
    if (!m_shown || m_placedPos != m_cfg.position) Place();
}

void ToastHost::Place() {
    if (!m_hwnd) return;
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    const float s = core::MonitorScale(mon);
    if (std::fabs(s - m_scale) > 0.01f) {
        m_scale = s; MakeFonts();
        for (auto& t : m_toasts) Prepare(t);
    }
    const int ww = static_cast<int>((kCardW + kPad * 2) * s), wh = static_cast<int>((kMaxVisible * (kMaxCardH + kGap) + kPad * 2) * s);
    const RECT& work = mi.rcWork;                       // already excludes the bar
    const int x = work.right - ww + static_cast<int>((kPad - kEdge) * s);
    const int y = m_cfg.position == 1 ? work.bottom - wh + static_cast<int>((kPad - kEdge) * s) : work.top - static_cast<int>((kPad - kEdge) * s);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, ww, wh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (static_cast<int>(m_dcomp->GetWidth()) != ww || static_cast<int>(m_dcomp->GetHeight()) != wh) m_dcomp->Resize(static_cast<uint32_t>(ww), static_cast<uint32_t>(wh));
    m_placedPos = m_cfg.position;
    m_shown = true;
    for (auto& t : m_toasts) t.placed = false;           // snap to the new slots instead of flying across
}

void ToastHost::CloseToast(long long id) { for (auto& t : m_toasts) if (t.id == id) t.closing = true; }

void ToastHost::Activate(const Toast& t) {
    if (t.appId.empty()) return;
    const std::wstring target = L"shell:AppsFolder\\" + t.appId;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32 && (t.appId.find(L".exe") != std::wstring::npos || t.appId.find(L'\\') != std::wstring::npos))
        ShellExecuteW(nullptr, L"open", t.appId.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT ToastHost::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    const Vec2 p{static_cast<float>(GET_X_LPARAM(l)), static_cast<float>(GET_Y_LPARAM(l))};
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            m_mouse = p;
            if (!m_trackLeave) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); m_trackLeave = true; }
            return 0;
        case WM_MOUSELEAVE: m_trackLeave = false; m_mouse = {-1, -1}; return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                bool over = false;
                for (const auto& hit : m_hits) if (hit.card.Contains(m_mouse)) { over = true; break; }
                SetCursor(LoadCursor(nullptr, over ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: {
            for (const auto& hit : m_hits) {
                if (hit.close.Contains(p)) { CloseToast(hit.id); return 0; }
                if (hit.card.Contains(p)) {
                    for (const auto& t : m_toasts) if (t.id == hit.id) { Activate(t); break; }
                    CloseToast(hit.id);
                    return 0;
                }
            }
            return 0;
        }
        case WM_RBUTTONUP:
            for (const auto& hit : m_hits) if (hit.card.Contains(p)) { CloseToast(hit.id); break; }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool ToastHost::Frame(float dt) {
    if (!m_hwnd) return false;
    PollConfig(dt);
    m_time += dt;

    // new notifications from Windows' database
    {
        auto fresh = NotificationFeed::Instance().TakeFresh();
        if (!fresh.empty() && m_cfg.enabled && !m_cfg.dnd && !Quiet()) {
            if (fresh.size() > static_cast<size_t>(kMaxVisible)) fresh.erase(fresh.begin(), fresh.end() - kMaxVisible);
            for (const auto& n : fresh) Push(n);
        }
    }
    if (m_toasts.empty()) {
        if (m_shown) { ShowWindow(m_hwnd, SW_HIDE); m_shown = false; m_hits.clear(); }
        return false;
    }

    // hover (from the last drawn frame), the click-through style, ageing
    POINT cp; GetCursorPos(&cp);
    RECT wr; GetWindowRect(m_hwnd, &wr);
    const Vec2 cursor{static_cast<float>(cp.x - wr.left), static_cast<float>(cp.y - wr.top)};
    bool overCard = false;
    for (auto& t : m_toasts) t.hover = false;
    for (const auto& hit : m_hits) if (hit.card.Contains(cursor)) {
        overCard = true;
        for (auto& t : m_toasts) if (t.id == hit.id) t.hover = true;
    }
    {
        const LONG ex = GetWindowLongW(m_hwnd, GWL_EXSTYLE);
        const bool transparent = (ex & WS_EX_TRANSPARENT) != 0;
        if (overCard && transparent) SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
        else if (!overCard && !transparent) SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    }
    bool anyHover = false;
    for (const auto& t : m_toasts) anyHover |= t.hover;

    // animation state and stack slots
    float offset = 0.0f;
    for (auto& t : m_toasts) {
        if (!t.closing) {
            t.in = std::min(1.0f, t.in + dt / 0.42f);
            if (!anyHover) t.age += dt;                     // the whole stack waits while one card is hovered
            if (t.age >= t.life) t.closing = true;
        } else {
            t.out = std::min(1.0f, t.out + dt / 0.26f);
        }
        t.targetY = offset;
        if (!t.closing) offset += t.cardH + kGap;
        else offset += (t.cardH + kGap) * (1.0f - EaseOut(t.out));   // a closing card gives its slot back smoothly
    }
    for (auto& t : m_toasts) {
        const float want = m_cfg.position == 1 ? (kMaxVisible * (kMaxCardH + kGap) + kPad - kGap) - t.targetY - t.cardH   // bottom corner: grows upwards
                                               : kPad + t.targetY;
        if (!t.placed) { t.y = want; t.placed = true; }
        else t.y += (want - t.y) * std::min(1.0f, dt * 13.0f);
    }
    m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(), [](const Toast& t) { return t.closing && t.out >= 1.0f; }), m_toasts.end());

    Draw();
    return true;
}

void ToastHost::Draw() {
    if (!m_dcomp) return;
    auto c = m_dcomp->BeginDraw();
    if (!c) return;
    m_hits.clear();
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) DrawToast(c, *it);   // oldest first, the newest ends up on top
    m_dcomp->EndDraw();
}

void ToastHost::DrawToast(ID2D1DeviceContext* c, Toast& t) {
    const float s = m_scale;
    const float ein = EaseOut(t.in), eout = EaseOut(t.out);
    const float alpha = Clamp01(ein * 1.4f) * (1.0f - eout);
    if (alpha <= 0.003f) return;
    const float xOff = ((1.0f - Back(t.in)) * 90.0f + eout * 120.0f) * s;
    const Rect R{kPad * s + xOff, t.y * s, kCardW * s, t.cardH * s};
    const auto& acc = render::glass::kVioletLight;

    // app icon, resolved in the background; fall back to a coloured badge with the initial
    AppVisual av;
    const bool haveVis = !t.appId.empty() && NotificationFeed::Instance().AppVisualFor(t.appId, av) && av.size > 0;
    if (haveVis && !av.name.empty()) t.label = av.name;
    if (haveVis && !t.icon) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        c->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(av.size), static_cast<UINT32>(av.size)), av.px.data(), static_cast<UINT32>(av.size) * 4, bp, &t.icon);
    }

    D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    c->PushLayer(&lp, nullptr);
    D2D1_MATRIX_3X2_F old; c->GetTransform(&old);
    const float sc = 0.955f + 0.045f * ein;
    c->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(R.x + R.width * 0.5f, R.y + R.height * 0.5f)) * old);

    // soft shadow
    for (int k = 0; k < 3; ++k) {
        const float e = (2.0f + 5.0f * k) * s;
        m_brush->SetColor(D2D1::ColorF(0, 0, 0, 0.17f - 0.05f * k));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(R.x - e, R.y - e + 7 * s, R.Right() + e, R.Bottom() + e + 7 * s), (kRadius + 2) * s + e, (kRadius + 2) * s + e), m_brush.Get());
    }
    auto geo = m_dcomp->CreateSquircleGeometry(R, kRadius * s);
    if (geo) render::DrawGlassBody(c, m_dcomp.get(), m_brush.Get(), geo.Get(), R, s, 1.0f, 1.0f);
    if (t.hover) {
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.055f));
        if (geo) c->FillGeometry(geo.Get(), m_brush.Get());
    }

    // icon
    const Rect badge{R.x + 16 * s, R.y + 14 * s, 40 * s, 40 * s};
    if (t.icon) {
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.08f));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(badge.x, badge.y, badge.Right(), badge.Bottom()), 12 * s, 12 * s), m_brush.Get());
        const float in = 4 * s;
        c->DrawBitmap(t.icon.Get(), D2D1::RectF(badge.x + in, badge.y + in, badge.Right() - in, badge.Bottom() - in), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        if (auto g = m_dcomp->CreateLinearGradientBrush(render::glass::kVioletLight, render::glass::kVioletDeep, D2D1::Point2F(badge.x, badge.y), D2D1::Point2F(badge.Right(), badge.Bottom())))
            if (auto bg = m_dcomp->CreateSquircleGeometry(badge, 12 * s)) c->FillGeometry(bg.Get(), g.Get());
        const std::wstring ini = t.label.empty() ? L"A" : std::wstring(1, static_cast<wchar_t>(towupper(t.label[0])));
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
        const D2D1_RECT_F br = D2D1::RectF(badge.x, badge.y, badge.Right(), badge.Bottom());
        if (m_fInitial) c->DrawText(ini.c_str(), static_cast<UINT32>(ini.size()), m_fInitial.Get(), &br, m_brush.Get());
    }

    // texts
    const float tx = R.x + 70 * s, tw = R.Right() - tx - (t.hover ? 40 : 18) * s;
    if (m_fApp) {
        m_brush->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, 0.92f));
        const D2D1_RECT_F r = D2D1::RectF(tx, R.y + 12 * s, tx + tw, R.y + 28 * s);
        c->DrawText(t.label.c_str(), static_cast<UINT32>(t.label.size()), m_fApp.Get(), &r, m_brush.Get());
    }
    if (m_fTitle) {
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.97f));
        const D2D1_RECT_F r = D2D1::RectF(tx, R.y + 28 * s, R.Right() - 18 * s, R.y + 50 * s);
        c->DrawText(t.title.c_str(), static_cast<UINT32>(t.title.size()), m_fTitle.Get(), &r, m_brush.Get());
    }
    if (t.layout) {
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.70f));
        c->DrawTextLayout(D2D1::Point2F(tx, R.y + 51 * s), t.layout.Get(), m_brush.Get());
    }

    // close button (on hover) and the remaining-time line
    Rect closeR{R.Right() - 34 * s, R.y + 10 * s, 24 * s, 24 * s};
    if (t.hover && !t.closing) {
        const bool over = closeR.Contains(m_mouse);
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, over ? 0.22f : 0.10f));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(closeR.x, closeR.y, closeR.Right(), closeR.Bottom()), 12 * s, 12 * s), m_brush.Get());
        if (m_fIcon) {
            const wchar_t g = 0xE711; m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
            const D2D1_RECT_F r = D2D1::RectF(closeR.x, closeR.y, closeR.Right(), closeR.Bottom());
            c->DrawText(&g, 1, m_fIcon.Get(), &r, m_brush.Get());
        }
    }
    if (!t.closing) {
        const float remain = Clamp01(1.0f - t.age / std::max(t.life, 0.1f));
        const float bx = R.x + 22 * s, bw = R.width - 44 * s, by = R.Bottom() - 8 * s;
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.06f));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + 2 * s), s, s), m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, t.hover ? 0.35f : 0.65f));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + std::max(bw * remain, 2 * s), by + 2 * s), s, s), m_brush.Get());
    }
    c->SetTransform(old);
    c->PopLayer();

    if (!t.closing) m_hits.push_back({R, t.hover ? closeR : Rect{-100, -100, 0, 0}, t.id});
}

// =============================================================================================
//  Windows' own banners
// =============================================================================================
namespace {
std::atomic<bool> g_hideOn{false};
std::atomic<bool> g_hiderRun{false};
HANDLE g_hiderThread{nullptr};
DWORD g_hiderTid{0};
std::map<DWORD, bool> g_pidIsShell;          // used on the hider thread only

bool IsShellHostPid(DWORD pid) {
    auto it = g_pidIsShell.find(pid);
    if (it != g_pidIsShell.end()) return it->second;
    bool ok = false;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
            ok = _wcsicmp(base, L"ShellExperienceHost.exe") == 0 || _wcsicmp(base, L"ShellHost.exe") == 0;
        }
        CloseHandle(p);
    }
    if (g_pidIsShell.size() > 64) g_pidIsShell.clear();
    g_pidIsShell[pid] = ok;
    return ok;
}

// A banner is a small CoreWindow of the shell experience host (the action centre, quick settings and calendar are much taller).
void HideIfBanner(HWND h) {
    wchar_t cls[64] = {};
    if (!GetClassNameW(h, cls, 64) || wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0) return;
    RECT r{}; if (!GetWindowRect(h, &r)) return;
    if (r.left <= -20000) return;                               // already moved away
    const float k = static_cast<float>(GetDpiForWindow(h)) / 96.0f;
    const int w = r.right - r.left, hh = r.bottom - r.top;
    if (w < static_cast<int>(300 * k) || w > static_cast<int>(460 * k) || hh < static_cast<int>(40 * k) || hh > static_cast<int>(300 * k)) return;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (!pid || !IsShellHostPid(pid)) return;
    SetWindowPos(h, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void CALLBACK HiderEvent(HWINEVENTHOOK, DWORD, HWND h, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_hideOn || !h || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    HideIfBanner(h);
}

BOOL CALLBACK SweepProc(HWND h, LPARAM) { if (IsWindowVisible(h)) HideIfBanner(h); return TRUE; }

DWORD WINAPI HiderThread(LPVOID) {
    MSG msg; PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    // show, uncloak (XAML windows are cloaked/uncloaked instead of shown) and location changes
    const DWORD events[3] = {0x8002 /*EVENT_OBJECT_SHOW*/, 0x8018 /*EVENT_OBJECT_UNCLOAKED*/, 0x800B /*EVENT_OBJECT_LOCATIONCHANGE*/};
    HWINEVENTHOOK hooks[3] = {};
    for (int i = 0; i < 3; ++i) hooks[i] = SetWinEventHook(events[i], events[i], nullptr, HiderEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(nullptr, 1, 250, nullptr);                         // safety net in case an event is missed
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && g_hideOn) EnumWindows(SweepProc, 0);
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    for (auto hk : hooks) if (hk) UnhookWinEvent(hk);
    return 0;
}
} // namespace

void NativeToastHider::Enable(bool on) { g_hideOn = on; }

void NativeToastHider::Start() {
    if (g_hiderRun.exchange(true)) return;
    g_hiderThread = CreateThread(nullptr, 0, HiderThread, nullptr, 0, &g_hiderTid);
}

void NativeToastHider::Stop() {
    if (!g_hiderRun.exchange(false) || !g_hiderThread) return;
    PostThreadMessageW(g_hiderTid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hiderThread, 1500);
    CloseHandle(g_hiderThread); g_hiderThread = nullptr;
}

} // namespace aether::bar
