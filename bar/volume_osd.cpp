#include "aether/bar/volume_osd.hpp"
#include "aether/core/monitors.hpp"
#include "aether/core/types.hpp"
#include "aether/render/glass.hpp"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace aether::bar {

using core::Rect;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kClass[] = L"Aether.VolumeOsd";
constexpr float kW = 300.0f, kH = 62.0f;     // logical px
constexpr float kPad = 26.0f;                // transparent margin (shadow room)
constexpr float kLift = 16.0f;               // distance above the bottom of the work area
constexpr float kRadius = 22.0f;
constexpr float kHold = 1.6f;                // seconds visible after the last change

constexpr wchar_t kGlyphMute = 0xE74F, kGlyphVol0 = 0xE992, kGlyphVol1 = 0xE993, kGlyphVol2 = 0xE994, kGlyphVol3 = 0xE995;

inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float EaseOut(float x) { x = Clamp01(x); const float u = 1.0f - x; return 1.0f - u * u * u; }

// Called by the audio engine on its own thread for every change of the endpoint's volume / mute.
class Callback final : public IAudioEndpointVolumeCallback {
public:
    explicit Callback(VolumeOsd::Shared* s) : m_s(s) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --m_ref; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) { *out = static_cast<IAudioEndpointVolumeCallback*>(this); AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (!d) return S_OK;
        m_s->level = d->fMasterVolume;
        m_s->mute = d->bMuted != FALSE;
        ++m_s->stamp;
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
    VolumeOsd::Shared* m_s;
};

std::wstring DefaultRenderId() {
    std::wstring id;
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        LPWSTR w = nullptr;
        if (SUCCEEDED(dev->GetId(&w)) && w) { id = w; CoTaskMemFree(w); }
    }
    return id;
}
} // namespace

VolumeOsd::~VolumeOsd() { Shutdown(); }

void VolumeOsd::Shutdown() {
    Detach();
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    m_dcomp.reset();
}

bool VolumeOsd::Create() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    m_scale = core::MonitorScale(mon);
    const int ww = static_cast<int>((kW + kPad * 2) * m_scale), wh = static_cast<int>((kH + kPad * 2) * m_scale);
    // never takes focus and never catches the mouse
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kClass, L"Aether Volume", WS_POPUP,
                             0, 0, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!m_hwnd) return false;
    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, static_cast<uint32_t>(ww), static_cast<uint32_t>(wh))) return false;
    m_brush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 1.0f});
    MakeFonts();
    Attach();
    return true;
}

void VolumeOsd::MakeFonts() {
    auto dw = m_dcomp ? m_dcomp->GetDWriteFactory() : nullptr;
    if (!dw) return;
    ComPtr<IDWriteFontCollection> col; dw->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL e = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &e)) && e; };
    const wchar_t* ico = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const wchar_t* txt = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    m_fIcon.Reset(); m_fPct.Reset();
    dw->CreateTextFormat(ico, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 21.0f * m_scale, L"en-us", &m_fIcon);
    dw->CreateTextFormat(txt, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f * m_scale, L"en-us", &m_fPct);
    if (m_fIcon) { m_fIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); m_fIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    if (m_fPct) { m_fPct->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING); m_fPct->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
}

void VolumeOsd::Nudge(int dir) {
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev; ComPtr<IAudioEndpointVolume> ep;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) return;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ep.GetAddressOf())))) return;
    if (dir == 0) { BOOL m = FALSE; ep->GetMute(&m); ep->SetMute(!m, nullptr); return; }
    float v = 0.0f; ep->GetMasterVolumeLevelScalar(&v);
    v = Clamp01(std::round(v * 50.0f + static_cast<float>(dir)) / 50.0f);      // steps of 2 %, like Windows
    ep->SetMasterVolumeLevelScalar(v, nullptr);
    if (dir > 0 || v > 0.0f) ep->SetMute(FALSE, nullptr);
}

void VolumeOsd::Detach() {
    if (m_ep && m_cb) m_ep->UnregisterControlChangeNotify(m_cb.Get());
    m_ep.Reset(); m_cb.Reset();
}

void VolumeOsd::Attach() {
    Detach();
    m_devId = DefaultRenderId();
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) return;
    ComPtr<IAudioEndpointVolume> ep;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ep.GetAddressOf())))) return;
    ComPtr<IAudioEndpointVolumeCallback> cb; cb.Attach(new Callback(&m_shared));
    if (FAILED(ep->RegisterControlChangeNotify(cb.Get()))) return;
    m_ep = ep; m_cb = cb;
    m_attachedAt = GetTickCount64();
    m_seen = m_shared.stamp.load();                   // what happened before this moment is not news
}

void VolumeOsd::Place() {
    if (!m_hwnd) return;
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    const float s = core::MonitorScale(mon);
    if (std::fabs(s - m_scale) > 0.01f) { m_scale = s; MakeFonts(); }
    const int ww = static_cast<int>((kW + kPad * 2) * s), wh = static_cast<int>((kH + kPad * 2) * s);
    const RECT& work = mi.rcWork;                                   // already excludes the bar
    const int x = (work.left + work.right - ww) / 2;
    const int y = work.bottom - wh + static_cast<int>((kPad - kLift) * s);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, ww, wh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (static_cast<int>(m_dcomp->GetWidth()) != ww || static_cast<int>(m_dcomp->GetHeight()) != wh) m_dcomp->Resize(static_cast<uint32_t>(ww), static_cast<uint32_t>(wh));
}

bool VolumeOsd::Frame(float dt) {
    if (!m_hwnd || !m_dcomp) return false;

    m_devTimer += dt;
    if (m_devTimer >= 2.0f) {                                       // another default playback device (headset plugged in ...)
        m_devTimer = 0.0f;
        if (DefaultRenderId() != m_devId || !m_ep) Attach();
    }

    const unsigned st = m_shared.stamp.load();
    if (st != m_seen) {
        m_seen = st;
        m_level = Clamp01(m_shared.level.load());
        m_mute = m_shared.mute.load();
        m_age = 0.0f; m_out = 0.0f;
        if (!m_visible) { m_visible = true; m_in = 0.0f; m_shownLevel = m_level; Place(); }
    }
    if (!m_visible) return false;

    m_in = std::min(1.0f, m_in + dt / 0.28f);
    m_age += dt;
    if (m_age > kHold) m_out = std::min(1.0f, m_out + dt / 0.30f);
    m_shownLevel += (m_level - m_shownLevel) * std::min(1.0f, dt * 18.0f);
    if (m_out >= 1.0f) { m_visible = false; ShowWindow(m_hwnd, SW_HIDE); return false; }
    Draw();
    return true;
}

void VolumeOsd::Draw() {
    auto c = m_dcomp->BeginDraw();
    if (!c) return;
    const float s = m_scale;
    const float ein = EaseOut(m_in), eout = EaseOut(m_out);
    const float alpha = Clamp01(ein * 1.5f) * (1.0f - eout);
    const Rect R{kPad * s, (kPad + (1.0f - ein) * 14.0f + eout * 8.0f) * s, kW * s, kH * s};
    const auto& acc = render::glass::kVioletLight;

    D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    c->PushLayer(&lp, nullptr);
    for (int k = 0; k < 3; ++k) {                                   // soft shadow
        const float e = (2.0f + 5.0f * k) * s;
        m_brush->SetColor(D2D1::ColorF(0, 0, 0, 0.17f - 0.05f * k));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(R.x - e, R.y - e + 6 * s, R.Right() + e, R.Bottom() + e + 6 * s), (kRadius + 2) * s + e, (kRadius + 2) * s + e), m_brush.Get());
    }
    if (auto geo = m_dcomp->CreateSquircleGeometry(R, kRadius * s)) render::DrawGlassBody(c, m_dcomp.get(), m_brush.Get(), geo.Get(), R, s, 1.0f, 1.0f);

    const bool muted = m_mute || m_level <= 0.005f;
    wchar_t glyph = kGlyphVol3;
    if (muted) glyph = kGlyphMute;
    else if (m_level < 0.10f) glyph = kGlyphVol0;
    else if (m_level < 0.34f) glyph = kGlyphVol1;
    else if (m_level < 0.67f) glyph = kGlyphVol2;
    if (m_fIcon) {
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, muted ? 0.62f : 0.96f));
        const D2D1_RECT_F r = D2D1::RectF(R.x + 14 * s, R.y, R.x + 54 * s, R.Bottom());
        c->DrawText(&glyph, 1, m_fIcon.Get(), &r, m_brush.Get());
    }

    // level bar
    const float bx = R.x + 64 * s, bw = R.width - 64 * s - 62 * s, bh = 6 * s, by = R.y + R.height * 0.5f - bh * 0.5f;
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.14f));
    c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + bh), bh * 0.5f, bh * 0.5f), m_brush.Get());
    const float shown = muted ? 0.0f : Clamp01(m_shownLevel);
    const float fw = std::max(shown * bw, shown > 0.001f ? bh : 0.0f);
    if (fw > 0.0f) {
        if (auto g = m_dcomp->CreateLinearGradientBrush(render::glass::kVioletDeep, acc, D2D1::Point2F(bx, 0), D2D1::Point2F(bx + bw, 0)))
            c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + fw, by + bh), bh * 0.5f, bh * 0.5f), g.Get());
        const float kx = bx + fw, kr = 7.0f * s;                    // knob
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.98f));
        c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx - kr * 0.35f, by + bh * 0.5f), kr, kr), m_brush.Get());
    }

    if (m_fPct) {
        wchar_t buf[8]; swprintf_s(buf, L"%d", muted ? 0 : static_cast<int>(m_level * 100.0f + 0.5f));
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
        const D2D1_RECT_F r = D2D1::RectF(R.Right() - 58 * s, R.y, R.Right() - 18 * s, R.Bottom());
        c->DrawText(buf, static_cast<UINT32>(wcslen(buf)), m_fPct.Get(), &r, m_brush.Get());
    }
    c->PopLayer();
    m_dcomp->EndDraw();
}

} // namespace aether::bar
