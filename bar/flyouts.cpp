#include <winsock2.h>
#include <ws2tcpip.h>
#include "aether/bar/flyouts.hpp"
#include "aether/bar/bar_edge.hpp"
#include "aether/bar/tray_host.hpp"
#include "aether/bar/netstate.hpp"
#include "aether/bar/ui_common.hpp"
#include "aether/core/monitors.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include <iphlpapi.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wlanapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;
namespace gl = ui;
namespace i18n = aether::i18n;

namespace {
inline D2D1_RECT_F RF(const Rect& r) { return D2D1::RectF(r.Left(), r.Top(), r.Right(), r.Bottom()); }
inline float C01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline Color glass_accent() { return render::glass::kVioletLight; }
#define kAccent glass_accent()
inline Color W(float a) { return Color{1.0f, 1.0f, 1.0f, a}; }

ComPtr<IAudioEndpointVolume> Endpoint(std::wstring* name = nullptr) {
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev; ComPtr<IAudioEndpointVolume> v;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(v.GetAddressOf()));
        if (name) {
            ComPtr<IPropertyStore> ps;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) *name = pv.pwszVal;
                PropVariantClear(&pv);
            }
        }
    }
    return v;
}
void Open(const wchar_t* what) { ShellExecuteW(nullptr, L"open", what, nullptr, nullptr, SW_SHOWNORMAL); }
} // namespace

// ===== Flyout base ===========================================================================
Flyout::~Flyout() { if (m_hwnd) DestroyWindow(m_hwnd); }

LRESULT CALLBACK Flyout::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Flyout* self = nullptr;
    if (m == WM_NCCREATE) { self = static_cast<Flyout*>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams); SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    else self = reinterpret_cast<Flyout*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

bool Flyout::Create(const wchar_t* cls, float w, float h) {
    m_baseW = w; m_baseH = h;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    const int ww = static_cast<int>(w + m_pad * 2), wh = static_cast<int>(h + m_pad * 2);
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls, L"Aether Panel", WS_POPUP,
                             0, 0, ww, wh, nullptr, nullptr, wc.hInstance, this);
    if (!m_hwnd) return false;
    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, ww, wh)) return false;
    m_brush = m_dcomp->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1.0f});
    MakeFonts();
    return true;
}

void Flyout::MakeFonts() {
    auto dw = m_dcomp->GetDWriteFactory();
    if (!dw) return;
    ComPtr<IDWriteFontCollection> col; dw->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL e = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &e)) && e; };
    const wchar_t* disp = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* text = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* ico = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
    m_hasIcons = ico != nullptr;
    auto mk = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* fam, DWRITE_FONT_WEIGHT wt, float sz, DWRITE_TEXT_ALIGNMENT al) {
        f.Reset(); if (!fam) return;
        dw->CreateTextFormat(fam, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sz * m_scale, L"en-us", &f);
        if (!f) return;
        f->SetTextAlignment(al); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; ComPtr<IDWriteInlineObject> e;
        dw->CreateEllipsisTrimmingSign(f.Get(), &e); f->SetTrimming(&t, e.Get());
    };
    mk(m_fTitle, disp, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15, DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(m_fBody,  text, DWRITE_FONT_WEIGHT_NORMAL, 13, DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(m_fSmall, text, DWRITE_FONT_WEIGHT_NORMAL, 11.5f, DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(m_fBig,   disp, DWRITE_FONT_WEIGHT_LIGHT, 46, DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(m_fIcon,  ico,  DWRITE_FONT_WEIGHT_NORMAL, 17, DWRITE_TEXT_ALIGNMENT_CENTER);
    mk(m_fIconBig, ico, DWRITE_FONT_WEIGHT_NORMAL, 24, DWRITE_TEXT_ALIGNMENT_CENTER);
    mk(m_fCenter, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, DWRITE_TEXT_ALIGNMENT_CENTER);
    mk(m_fCTitle, disp, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15, DWRITE_TEXT_ALIGNMENT_CENTER);
    mk(m_fCBody,  text, DWRITE_FONT_WEIGHT_NORMAL, 13, DWRITE_TEXT_ALIGNMENT_CENTER);
    mk(m_fCSmall, text, DWRITE_FONT_WEIGHT_NORMAL, 11.5f, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Flyout::Show(HMONITOR mon, int reservedPx, float anchorCenterX) {
    if (!m_hwnd) return;
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    const float s = core::MonitorScale(mon);
    const bool rescale = s != m_scale;
    m_scale = s; m_pad = 40.0f * s;
    const float ph = PanelHeight();
    const int ww = static_cast<int>(m_baseW * s + m_pad * 2), wh = static_cast<int>(ph * s + m_pad * 2);
    SetWindowPos(m_hwnd, nullptr, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    m_dcomp->Resize(ww, wh);
    if (rescale || !m_fTitle) MakeFonts();
    int x = static_cast<int>(anchorCenterX) - ww / 2;
    x = std::clamp(x, static_cast<int>(mi.rcMonitor.left) + 8, static_cast<int>(mi.rcMonitor.right) - ww - 8);
    const int y = g_barAtTop ? mi.rcMonitor.top + reservedPx - static_cast<int>(m_pad) + static_cast<int>(6 * s)
                             : mi.rcMonitor.bottom - reservedPx - wh + static_cast<int>(m_pad) - static_cast<int>(6 * s);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, ww, wh, SWP_NOACTIVATE);
    OnShow();
    m_prevDown = true; m_dismissTick = 0;
    m_wantOpen = true; m_open.SnapTo(0.0f); m_open.SetTarget(1.0f);
    if (!(IsWindowVisible(m_hwnd) && m_backdrop)) {   // still fading out from the last open: keep that snapshot
        render::CaptureBackdrop(m_dcomp->GetContext(), x + static_cast<int>(m_pad), y + static_cast<int>(m_pad),
                                static_cast<int>(m_baseW * s), static_cast<int>(ph * s), 4, m_backdrop);
    }
    ShowWindow(m_hwnd, SW_SHOW);
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
    if (fg && fg != me) AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(m_hwnd); SetFocus(m_hwnd);
    if (fg && fg != me) AttachThreadInput(me, fg, FALSE);
}

void Flyout::Hide() {
    if (m_wantOpen) { m_wantOpen = false; m_open.SetTarget(0.0f); }
    if (m_hwnd) { LONG ex = GetWindowLongW(m_hwnd, GWL_EXSTYLE); if (ex & WS_EX_TRANSPARENT) SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT); }
}

bool Flyout::Frame(float dt) {
    render::glass::SyncTheme();
    if (!m_hwnd) return false;
    m_time += dt; m_open.Update(dt);
    if (!m_wantOpen && m_open.IsResting() && IsWindowVisible(m_hwnd)) { ShowWindow(m_hwnd, SW_HIDE); return false; }
    if (!IsWindowVisible(m_hwnd)) return false;
    // Outside-click dismissal + click-through: only the panel itself takes the mouse, the transparent glow
    // margin lets clicks fall through to whatever is below (dock icons, windows). A press outside closes it.
    if (m_wantOpen && IsWindowVisible(m_hwnd)) {
        RECT wr; GetWindowRect(m_hwnd, &wr);
        POINT cp; GetCursorPos(&cp);
        const int pad = static_cast<int>(m_pad);
        const bool inside = cp.x >= wr.left + pad && cp.x < wr.right - pad && cp.y >= wr.top + pad && cp.y < wr.bottom - pad;
        LONG ex = GetWindowLongW(m_hwnd, GWL_EXSTYLE);
        const bool transparent = (ex & WS_EX_TRANSPARENT) != 0;
        if (!inside && !transparent) SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
        else if (inside && transparent) SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        if (down && !m_prevDown && !inside && !(false)) { m_dismissTick = GetTickCount64(); Hide(); }
        m_prevDown = down;
    }
    // Frame pacing: the open/close spring runs at full rate; a resting panel is capped at ~60 fps, so a static panel is
    // not redrawn hundreds of times a second (hover and ambient animations stay smooth).
    if (m_wantOpen && m_open.IsResting()) {
        const ULONGLONG nowMs = GetTickCount64();
        if (m_lastDrawMs && nowMs - m_lastDrawMs < 15) {
            m_skipDt += dt;
            MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(16 - (nowMs - m_lastDrawMs)), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            return true;
        }
        m_lastDrawMs = nowMs;
    }
    dt += m_skipDt; m_skipDt = 0.0f;
    OnTick(dt);
    const float s = m_scale, o = C01(m_open.GetValue());
    const Rect panel{m_pad, m_pad, m_baseW * s, PanelHeight() * s};
    auto body = m_dcomp->CreateSquircleGeometry(panel, 28 * s);
    auto silhouette = render::RecordSilhouette(m_dcomp.get(), {body.Get()}, s, 0.85f);   // before BeginDraw
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return true;
    m_hits.clear();
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(0.95f + 0.05f * o, 0.95f + 0.05f * o, D2D1::Point2F(panel.Center().x, panel.Bottom())) *
                      D2D1::Matrix3x2F::Translation(0, (1 - o) * 18 * s));
    D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), o, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    ctx->PushLayer(&lp, nullptr);
    render::DrawGlassGlow(ctx, silhouette.Get(), s, 0.85f);
    if (body && m_backdrop) render::FillBackdrop(ctx, m_backdrop, body.Get(), panel, 0.95f);
    if (body) render::DrawGlassBody(ctx, m_dcomp.get(), m_brush.Get(), body.Get(), panel, s, 1.0f, m_backdrop ? 0.55f : 1.0f);
    OnDraw(ctx, panel);
    ctx->PopLayer();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dcomp->EndDraw();
    return true;
}

void Flyout::Text(ID2D1DeviceContext* c, const std::wstring& t, IDWriteTextFormat* f, const Rect& r, const Color& col) {
    if (!f || t.empty()) return;
    m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a));
    const D2D1_RECT_F rc = RF(r);
    c->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, &rc, m_brush.Get());
}
void Flyout::Glyph(ID2D1DeviceContext* c, wchar_t g, const Rect& r, const Color& col, bool big) {
    if (!m_hasIcons) return;
    wchar_t b[2] = {g, 0};
    m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a));
    const D2D1_RECT_F rc = RF(r);
    c->DrawText(b, 1, big ? m_fIconBig.Get() : m_fIcon.Get(), &rc, m_brush.Get());
}
void Flyout::Fill(ID2D1DeviceContext* c, const Rect& r, float rad, const Color& col) {
    if (auto g = m_dcomp->CreateSquircleGeometry(r, rad)) { m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a)); c->FillGeometry(g.Get(), m_brush.Get()); }
}
void Flyout::Stroke(ID2D1DeviceContext* c, const Rect& r, float rad, const Color& col, float w) {
    if (auto g = m_dcomp->CreateSquircleGeometry(r, rad)) { m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a)); c->DrawGeometry(g.Get(), m_brush.Get(), w * m_scale); }
}
int Flyout::HitAt(const Vec2& p) const { for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) if (it->r.Contains(p)) return it->id; return 0; }

LRESULT Flyout::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    const Vec2 p{static_cast<float>(GET_X_LPARAM(l)), static_cast<float>(GET_Y_LPARAM(l))};
    switch (m) {
        case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) Hide(); return 0;
        case WM_MOUSEMOVE: m_mouse = p; if (m_down && m_dragId) OnDrag(m_dragId, p, false); return 0;
        case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) { gl::ApplyCursor(HitAt(m_mouse) != 0); return TRUE; } break;
        case WM_LBUTTONDOWN: { m_down = true; m_dragId = HitAt(p); SetCapture(h); if (m_dragId) OnDrag(m_dragId, p, true); return 0; }
        case WM_LBUTTONUP: {
            const bool was = m_down; m_down = false; if (GetCapture() == h) ReleaseCapture();
            const int id = HitAt(p);
            if (was && id && id == m_dragId) { m_rightButton = false; OnClick(id, p); }
            m_dragId = 0; return 0;
        }
        case WM_RBUTTONUP: { const int id = HitAt(p); if (id) { m_rightButton = true; OnClick(id, p); m_rightButton = false; } return 0; }
        case WM_MOUSEWHEEL: OnWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA); return 0;
        case WM_KEYDOWN: if (w == VK_ESCAPE) Hide(); else OnKey(w); return 0;
        case WM_CHAR: OnChar(static_cast<wchar_t>(w)); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ===== Quick settings ========================================================================
namespace {
// Undocumented but stable COM interface behind "set default audio device" (used by every sound switcher).
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfigAether : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
const CLSID kCLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

bool SetDefaultDevice(const std::wstring& id) {
    ComPtr<IPolicyConfigAether> pc;
    if (FAILED(CoCreateInstance(kCLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfigAether), reinterpret_cast<void**>(pc.GetAddressOf()))) || !pc) return false;
    bool ok = false;
    for (ERole r : {eConsole, eMultimedia, eCommunications}) ok |= SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), r));
    return ok;
}

ComPtr<IAudioEndpointVolume> FlowEndpoint(EDataFlow flow) {
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev; ComPtr<IAudioEndpointVolume> v;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eMultimedia, &dev)))
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(v.GetAddressOf()));
    return v;
}
std::wstring FriendlyName(IMMDevice* dev) {
    std::wstring n; ComPtr<IPropertyStore> ps;
    if (dev && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) n = pv.pwszVal;
        PropVariantClear(&pv);
    }
    return n;
}
} // namespace

void QuickPanel::OnShow() { m_view = 0; Refresh(); m_refreshTimer = 0; }
void QuickPanel::OnTick(float dt) { m_refreshTimer += dt; if (m_refreshTimer > 2.0f && !m_down) { m_refreshTimer = 0; Refresh(); } }

void QuickPanel::RefreshDevices() {
    m_devices.clear();
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) || !en) return;
    for (int f = 0; f < 2; ++f) {
        const EDataFlow flow = f == 0 ? eRender : eCapture;
        std::wstring defId;
        { ComPtr<IMMDevice> d; if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eMultimedia, &d)) && d) { LPWSTR id = nullptr; if (SUCCEEDED(d->GetId(&id)) && id) { defId = id; CoTaskMemFree(id); } } }
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) || !col) continue;
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n && i < 6; ++i) {
            ComPtr<IMMDevice> d; if (FAILED(col->Item(i, &d)) || !d) continue;
            LPWSTR id = nullptr; if (FAILED(d->GetId(&id)) || !id) continue;
            Dev dv; dv.id = id; CoTaskMemFree(id);
            dv.name = FriendlyName(d.Get()); dv.input = f == 1; dv.isDefault = dv.id == defId;
            m_devices.push_back(std::move(dv));
        }
    }
}

void QuickPanel::Refresh() {
    if (auto ev = FlowEndpoint(eRender)) { float v = 0; BOOL mu = FALSE; if (SUCCEEDED(ev->GetMasterVolumeLevelScalar(&v))) m_volume = v; if (SUCCEEDED(ev->GetMute(&mu))) m_muted = mu; }
    { ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> d;
      if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &d))) m_device = FriendlyName(d.Get());
      else m_device.clear();
      ComPtr<IMMDevice> m; if (en && SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eMultimedia, &m))) m_micName = FriendlyName(m.Get()); else m_micName.clear(); }
    if (auto mv = FlowEndpoint(eCapture)) { float v = 0; BOOL mu = FALSE; m_hasMic = true; if (SUCCEEDED(mv->GetMasterVolumeLevelScalar(&v))) m_micVolume = v; if (SUCCEEDED(mv->GetMute(&mu))) m_micMuted = mu; } else m_hasMic = false;
    if (m_view == 1) RefreshDevices();

    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100) { m_battery = ps.BatteryLifePercent; m_charging = ps.ACLineStatus == 1; } else m_battery = -1;

    const NetState ns = QueryNetState();
    m_netUp = ns.connected; m_wifi = ns.wifi; m_signal = ns.signal;
    if (!ns.connected) { m_netName = i18n::Pick(L"Not connected", L"Bağlı değil"); m_netDetail = i18n::Pick(L"No network connection", L"Ağ bağlantısı yok"); }
    else {
        m_netName = ns.name.empty() ? std::wstring(ns.wifi ? L"Wi-Fi" : L"Ethernet") : ns.name;
        std::wstring d = ns.internet ? i18n::Pick(L"Connected, internet access", L"Bağlı, internet erişimi var") : i18n::Pick(L"Connected, no internet", L"Bağlı, internet yok");
        if (ns.wifi && ns.signal > 0) d += L"  -  " + std::to_wstring(ns.signal) + L"%";
        m_netDetail = d;
    }
}

void QuickPanel::OnWheel(float n) {
    if (m_view != 0) return;
    const int under = HitAt(m_mouse);
    if (under == 5 || under == 6) {   // cursor is on the microphone row: scroll changes the microphone, not the speakers
        if (auto mv = FlowEndpoint(eCapture)) {
            m_micVolume = C01(m_micVolume + n * 0.03f);
            mv->SetMasterVolumeLevelScalar(m_micVolume, nullptr);
            if (m_micVolume > 0) { mv->SetMute(FALSE, nullptr); m_micMuted = false; }
        }
        return;
    }
    if (auto ev = FlowEndpoint(eRender)) { m_volume = C01(m_volume + n * 0.03f); ev->SetMasterVolumeLevelScalar(m_volume, nullptr); if (m_volume > 0) ev->SetMute(FALSE, nullptr); m_muted = false; }
}

void QuickPanel::OnDrag(int id, const Vec2& p, bool) {
    if (id == 2) {
        m_volume = C01((p.x - m_slider.x) / std::max(1.0f, m_slider.width));
        if (auto ev = FlowEndpoint(eRender)) { ev->SetMasterVolumeLevelScalar(m_volume, nullptr); if (m_volume > 0) { ev->SetMute(FALSE, nullptr); m_muted = false; } }
    } else if (id == 6) {
        m_micVolume = C01((p.x - m_micSlider.x) / std::max(1.0f, m_micSlider.width));
        if (auto ev = FlowEndpoint(eCapture)) { ev->SetMasterVolumeLevelScalar(m_micVolume, nullptr); if (m_micVolume > 0) { ev->SetMute(FALSE, nullptr); m_micMuted = false; } }
    }
}

void QuickPanel::OnClick(int id, const Vec2&) {
    if (id >= 200 && id < 260) {      // device list
        const size_t k = static_cast<size_t>(id - 200);
        if (k < m_devices.size()) { SetDefaultDevice(m_devices[k].id); RefreshDevices(); Refresh(); }
        return;
    }
    switch (id) {
        case 1: if (auto ev = FlowEndpoint(eRender)) { m_muted = !m_muted; ev->SetMute(m_muted, nullptr); } break;
        case 3: Open(L"ms-settings:network"); Hide(); break;
        case 4: m_view = 1; RefreshDevices(); break;                       // open the device list
        case 5: if (auto ev = FlowEndpoint(eCapture)) { m_micMuted = !m_micMuted; ev->SetMute(m_micMuted, nullptr); } break;
        case 7: m_view = 0; break;                                         // back
        case 8: Open(L"ms-settings:sound"); Hide(); break;
        case 10: m_view = 1; RefreshDevices(); break;
        case 11: Open(L"ms-settings:display"); Hide(); break;
        case 12: Open(L"ms-settings:bluetooth"); Hide(); break;
        case 13: Open(L"ms-settings:powersleep"); Hide(); break;
        case 14: Open(L"ms-settings:nightlight"); Hide(); break;
        case 15: Open(L"ms-settings:"); Hide(); break;
        case 16: LockWorkStation(); Hide(); break;
        case 17: Open(L"taskmgr.exe"); Hide(); break;
        case 18: Hide(); ShellExecuteW(nullptr, L"open", L"ms-settings:network-airplanemode", nullptr, nullptr, SW_SHOWNORMAL); break;   // airplane mode
    }
}

void QuickPanel::OnDraw(ID2D1DeviceContext* c, const Rect& P) {
    const float s = S(); const float x = P.x + 22 * s, w = P.width - 44 * s;
    const Color acc = glass_accent();
    if (m_view == 1) {                                  // ---- audio devices ----
        const Rect back{P.x + 12 * s, P.y + 12 * s, 36 * s, 36 * s};
        if (back.Contains(m_mouse)) Fill(c, back, 12 * s, W(0.10f));
        Glyph(c, gl::kGlyphChevronLeft, back, W(0.9f)); AddHit(back, 7);
        Text(c, i18n::Pick(L"Sound devices", L"Ses cihazları"), m_fTitle.Get(), Rect{x + 30 * s, P.y + 14 * s, 240 * s, 32 * s}, W(0.95f));
        Rect set{P.Right() - 120 * s, P.y + 14 * s, 100 * s, 32 * s}; AddHit(set, 8);
        Text(c, i18n::Pick(L"Settings", L"Ayarlar"), m_fCenter.Get(), set, set.Contains(m_mouse) ? acc : W(0.55f));
        float y = P.y + 62 * s;
        for (int f = 0; f < 2; ++f) {
            Text(c, f == 0 ? i18n::Pick(L"OUTPUT", L"ÇIKIŞ") : i18n::Pick(L"INPUT", L"GİRİŞ"), m_fSmall.Get(), Rect{x + 4 * s, y, w, 22 * s}, W(0.40f));
            y += 26 * s; int count = 0;
            for (size_t k = 0; k < m_devices.size(); ++k) {
                const Dev& d = m_devices[k];
                if (d.input != (f == 1)) continue;
                ++count;
                Rect r{x, y, w, 42 * s};
                const bool hot = r.Contains(m_mouse);
                Fill(c, r, 14 * s, d.isDefault ? Color{acc.r, acc.g, acc.b, 0.18f} : (hot ? W(0.09f) : W(0.05f)));
                if (d.isDefault) Stroke(c, r, 14 * s, Color{acc.r, acc.g, acc.b, 0.55f});
                AddHit(r, 200 + static_cast<int>(k));
                Glyph(c, f == 0 ? gl::kGlyphVolume : gl::kGlyphMic, Rect{r.x + 6 * s, r.y, 36 * s, r.height}, d.isDefault ? acc : W(0.7f));
                Text(c, d.name, m_fBody.Get(), Rect{r.x + 48 * s, r.y, r.width - 90 * s, r.height}, W(d.isDefault ? 0.97f : 0.80f));
                if (d.isDefault) Glyph(c, gl::kGlyphCheck, Rect{r.Right() - 38 * s, r.y, 30 * s, r.height}, acc);
                y += 46 * s;
            }
            if (!count) { Text(c, i18n::Pick(L"No device found", L"Cihaz bulunamadı"), m_fSmall.Get(), Rect{x + 6 * s, y, w, 26 * s}, W(0.4f)); y += 30 * s; }
            y += 8 * s;
        }
        return;
    }

    Text(c, i18n::Pick(L"Quick settings", L"Hızlı ayarlar"), m_fTitle.Get(), Rect{x, P.y + 18 * s, 200 * s, 28 * s}, W(0.95f));
    if (m_battery >= 0) {
        wchar_t b[32]; swprintf_s(b, L"%ls%d%%", m_charging ? L"+ " : L"", m_battery);
        Text(c, b, m_fCenter.Get(), Rect{P.Right() - 120 * s, P.y + 18 * s, 98 * s, 28 * s},
             m_charging ? Color{0.3f, 0.9f, 0.45f, 1.0f} : (m_battery <= 20 ? Color{1.0f, 0.35f, 0.3f, 1.0f} : W(0.8f)));
    }
    // volume + output device
    Rect vc{x, P.y + 56 * s, w, 112 * s};
    Fill(c, vc, 20 * s, W(0.06f)); Stroke(c, vc, 20 * s, W(0.07f));
    const Rect mute{vc.x + 12 * s, vc.y + 12 * s, 44 * s, 44 * s};
    AddHit(mute, 1);
    Fill(c, mute, 14 * s, m_muted ? Color{1.0f, 0.35f, 0.3f, 0.25f} : W(0.08f));
    Glyph(c, (m_muted || m_volume <= 0.005f) ? gl::kGlyphMute : gl::kGlyphVolume, mute, W(0.95f), true);
    m_slider = Rect{vc.x + 70 * s, vc.y + 26 * s, vc.width - 150 * s, 16 * s};
    const float th = 8 * s, cy = m_slider.Center().y;
    Fill(c, Rect{m_slider.x, cy - th / 2, m_slider.width, th}, th / 2, W(0.16f));
    Fill(c, Rect{m_slider.x, cy - th / 2, std::max(th, m_slider.width * m_volume), th}, th / 2, acc);
    const float kx = m_slider.x + m_slider.width * m_volume;
    Fill(c, Rect{kx - 9 * s, cy - 9 * s, 18 * s, 18 * s}, 9 * s, Color{1.0f, 1.0f, 1.0f, 1.0f});
    AddHit(Rect{m_slider.x - 8 * s, m_slider.y - 14 * s, m_slider.width + 16 * s, m_slider.height + 28 * s}, 2);
    wchar_t pc[16]; swprintf_s(pc, L"%d%%", static_cast<int>(m_volume * 100 + 0.5f));
    Text(c, pc, m_fTitle.Get(), Rect{vc.Right() - 70 * s, vc.y + 14 * s, 56 * s, 40 * s}, W(0.95f));
    Rect dev{vc.x + 8 * s, vc.y + 66 * s, vc.width - 16 * s, 38 * s};
    Fill(c, dev, 12 * s, dev.Contains(m_mouse) ? W(0.10f) : W(0.04f)); AddHit(dev, 4);
    Text(c, m_device.empty() ? std::wstring(i18n::Pick(L"Default output", L"Varsayılan çıkış")) : m_device, m_fSmall.Get(), Rect{dev.x + 12 * s, dev.y, dev.width - 52 * s, dev.height}, W(0.62f));
    Glyph(c, gl::kGlyphChevronRight, Rect{dev.Right() - 36 * s, dev.y, 28 * s, dev.height}, W(0.55f));

    // microphone
    Rect mc{x, vc.Bottom() + 10 * s, w, 64 * s};
    Fill(c, mc, 20 * s, W(0.06f)); Stroke(c, mc, 20 * s, W(0.07f));
    const Rect mm{mc.x + 12 * s, mc.y + 10 * s, 44 * s, 44 * s};
    Fill(c, mm, 14 * s, m_micMuted ? Color{1.0f, 0.35f, 0.3f, 0.25f} : W(0.08f)); AddHit(mm, 5);
    Glyph(c, gl::kGlyphMic, mm, m_micMuted ? Color{1.0f, 0.5f, 0.45f, 1.0f} : W(0.95f), true);
    m_micSlider = Rect{mc.x + 70 * s, mc.y + 24 * s, mc.width - 150 * s, 16 * s};
    const float mcy = m_micSlider.Center().y;
    Fill(c, Rect{m_micSlider.x, mcy - th / 2, m_micSlider.width, th}, th / 2, W(0.16f));
    Fill(c, Rect{m_micSlider.x, mcy - th / 2, std::max(th, m_micSlider.width * (m_hasMic ? m_micVolume : 0.0f)), th}, th / 2, m_micMuted ? W(0.35f) : acc);
    const float mkx = m_micSlider.x + m_micSlider.width * (m_hasMic ? m_micVolume : 0.0f);
    Fill(c, Rect{mkx - 9 * s, mcy - 9 * s, 18 * s, 18 * s}, 9 * s, Color{1.0f, 1.0f, 1.0f, 1.0f});
    AddHit(Rect{m_micSlider.x - 8 * s, m_micSlider.y - 14 * s, m_micSlider.width + 16 * s, m_micSlider.height + 28 * s}, 6);
    if (!m_hasMic) Text(c, i18n::Pick(L"No mic", L"Mikrofon yok"), m_fSmall.Get(), Rect{mc.Right() - 90 * s, mc.y, 80 * s, mc.height}, W(0.4f));
    else { wchar_t mp[16]; swprintf_s(mp, L"%d%%", static_cast<int>(m_micVolume * 100 + 0.5f)); Text(c, mp, m_fBody.Get(), Rect{mc.Right() - 70 * s, mc.y, 56 * s, mc.height}, W(0.8f)); }

    // network
    Rect nc{x, mc.Bottom() + 10 * s, w, 70 * s};
    Fill(c, nc, 20 * s, W(0.06f)); Stroke(c, nc, 20 * s, W(0.07f)); AddHit(nc, 3);
    const Rect ni{nc.x + 12 * s, nc.y + 13 * s, 44 * s, 44 * s};
    Fill(c, ni, 14 * s, m_netUp ? Color{acc.r, acc.g, acc.b, 0.28f} : W(0.08f));
    Glyph(c, m_wifi ? gl::kGlyphWifi : (m_netUp ? gl::kGlyphEthernet : gl::kGlyphGlobe), ni, m_netUp ? acc : W(0.6f), true);
    Text(c, m_netName, m_fTitle.Get(), Rect{nc.x + 70 * s, nc.y + 11 * s, nc.width - 110 * s, 26 * s}, W(0.95f));
    Text(c, m_netDetail, m_fSmall.Get(), Rect{nc.x + 70 * s, nc.y + 37 * s, nc.width - 110 * s, 22 * s}, W(0.5f));
    Glyph(c, gl::kGlyphChevronRight, Rect{nc.Right() - 40 * s, nc.y, 30 * s, nc.height}, W(0.5f));

    struct T { wchar_t g; const wchar_t* label; int id; };
    const T tiles[] = {{gl::kGlyphVolume, i18n::Pick(L"Sound", L"Ses"), 10}, {gl::kGlyphDisplay, i18n::Pick(L"Display", L"Ekran"), 11}, {gl::kGlyphBluetooth, L"Bluetooth", 12},
                       {gl::kGlyphSettings, i18n::Pick(L"Power", L"Güç"), 13}, {gl::kGlyphInfo, i18n::Pick(L"Night light", L"Gece ışığı"), 14}, {gl::kGlyphSettings, i18n::Pick(L"Settings", L"Ayarlar"), 15},
                       {gl::kGlyphLock, i18n::Pick(L"Lock", L"Kilitle"), 16}, {static_cast<wchar_t>(0xE709), i18n::Pick(L"Airplane", L"Uçak modu"), 18}, {static_cast<wchar_t>(0xE9D9), i18n::Pick(L"Task Mgr", L"Görev Yön."), 17}};
    const float gy = nc.Bottom() + 12 * s, tw = (w - 20 * s) / 3, th2 = 62 * s;
    for (int i = 0; i < 9; ++i) {
        Rect r{x + (i % 3) * (tw + 10 * s), gy + (i / 3) * (th2 + 8 * s), tw, th2};
        const bool hot = r.Contains(m_mouse);
        Fill(c, r, 18 * s, hot ? W(0.12f) : W(0.06f)); AddHit(r, tiles[i].id);
        Glyph(c, tiles[i].g, Rect{r.x, r.y + 6 * s, r.width, 28 * s}, hot ? acc : W(0.9f));
        Text(c, tiles[i].label, m_fCenter.Get(), Rect{r.x, r.y + 36 * s, r.width, 22 * s}, W(0.7f));
    }
}

// ===== Calendar ==============================================================================
namespace {
// Cheap deterministic noise in [0,1): every particle's parameters come from its index, so the effects need no stored state.
float Hash01(unsigned n, unsigned salt) {
    n = n * 747796405u + salt * 2891336453u + 1u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    n = (n >> 22u) ^ n;
    return static_cast<float>(n & 0xFFFFFFu) / 16777216.0f;
}
float Ease(float x) { x = std::clamp(x, 0.0f, 1.0f); const float u = 1.0f - x; return 1.0f - u * u * u; }
}

void CalendarPanel::OnShow() {
    SYSTEMTIME st; GetLocalTime(&st);
    m_year = st.wYear; m_month = st.wMonth; m_selDay = st.wDay; m_selMonth = st.wMonth; m_selYear = st.wYear;
    m_prevYear = m_year; m_prevMonth = m_month; m_anim = 1.0f;
    m_season = m_prevSeason = SeasonOf(m_month); m_seasonT = 1.0f;
}

void CalendarPanel::NavigateTo(int year, int month) {
    while (month < 1) { month += 12; --year; }
    while (month > 12) { month -= 12; ++year; }
    const int diff = (year * 12 + month) - (m_year * 12 + m_month);
    if (diff == 0) return;
    m_prevYear = m_year; m_prevMonth = m_month; m_dir = diff > 0 ? 1 : -1; m_anim = 0.0f;     // the old month slides out while the new one slides in
    m_year = year; m_month = month;
    const int ns = SeasonOf(m_month);
    if (ns != m_season) { m_prevSeason = m_season; m_season = ns; m_seasonT = 0.0f; }
}

void CalendarPanel::OnTick(float dt) {
    if (m_anim < 1.0f) m_anim = std::min(1.0f, m_anim + dt / 0.34f);
    if (m_seasonT < 1.0f) m_seasonT = std::min(1.0f, m_seasonT + dt / 0.80f);
}

void CalendarPanel::OnWheel(float n) { const int d = -static_cast<int>(n); if (d) NavigateTo(m_year, m_month + d); }

void CalendarPanel::OnClick(int id, const Vec2&) {
    if (id == 1) OnWheel(1); else if (id == 2) OnWheel(-1);
    else if (id == 3) { SYSTEMTIME st; GetLocalTime(&st); NavigateTo(st.wYear, st.wMonth); m_selDay = st.wDay; m_selMonth = st.wMonth; m_selYear = st.wYear; }
    else if (id == 4) { Open(L"ms-clock:"); Hide(); }
    else if (id == 5) { Open(L"ms-settings:dateandtime"); Hide(); }
    else if (id >= 100 && id < 142) {
        SYSTEMTIME first = {}; first.wYear = static_cast<WORD>(m_year); first.wMonth = static_cast<WORD>(m_month); first.wDay = 1;
        FILETIME ft; SystemTimeToFileTime(&first, &ft); SYSTEMTIME f2; FileTimeToSystemTime(&ft, &f2);
        int firstDow = (f2.wDayOfWeek + 6) % 7;
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        u.QuadPart += static_cast<ULONGLONG>(id - 100 - firstDow) * 864000000000ULL;
        ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart; FileTimeToSystemTime(&ft, &f2);
        m_selDay = f2.wDay; m_selMonth = f2.wMonth; m_selYear = f2.wYear;
        if (f2.wMonth != m_month) NavigateTo(f2.wYear, f2.wMonth);
    }
}

// One month: the 6 x 7 day grid. Used twice while sliding (old month fading out, new month fading in); only the settled one takes clicks.
void CalendarPanel::DrawMonthGrid(ID2D1DeviceContext* c, float x, float gy, float cw, int year, int month, const SYSTEMTIME& now, float yOff, float alpha, bool interactive) {
    const float s = S();
    if (alpha <= 0.004f) return;
    auto A = [&](Color col) { col.a *= alpha; return col; };
    SYSTEMTIME m = {}; m.wYear = static_cast<WORD>(year); m.wMonth = static_cast<WORD>(month); m.wDay = 1;
    FILETIME ft; SystemTimeToFileTime(&m, &ft); SYSTEMTIME f2; FileTimeToSystemTime(&ft, &f2);
    const int firstDow = (f2.wDayOfWeek + 6) % 7;
    ULARGE_INTEGER base; base.LowPart = ft.dwLowDateTime; base.HighPart = ft.dwHighDateTime;
    base.QuadPart -= static_cast<ULONGLONG>(firstDow) * 864000000000ULL;
    for (int i = 0; i < 42; ++i) {
        ULARGE_INTEGER u = base; u.QuadPart += static_cast<ULONGLONG>(i) * 864000000000ULL;
        FILETIME g; g.dwLowDateTime = u.LowPart; g.dwHighDateTime = u.HighPart; SYSTEMTIME d; FileTimeToSystemTime(&g, &d);
        const Rect cell{x + (i % 7) * cw + 2 * s, gy + 28 * s + (i / 7) * 40 * s + yOff, cw - 4 * s, 36 * s};
        const bool cur = d.wMonth == month, isToday = d.wDay == now.wDay && d.wMonth == now.wMonth && d.wYear == now.wYear;
        const bool sel = d.wDay == m_selDay && d.wMonth == m_selMonth && d.wYear == m_selYear;
        const bool hot = interactive && cell.Contains(m_mouse);
        if (isToday) Fill(c, cell, 18 * s, A(kAccent));
        else if (sel) Fill(c, cell, 18 * s, A(Color{0.62f, 0.42f, 1.0f, 0.28f}));
        else if (hot) Fill(c, cell, 18 * s, A(W(0.08f)));
        if (interactive) AddHit(Rect{cell.x, cell.y, cell.width, cell.height}, 100 + i);
        wchar_t n[4]; swprintf_s(n, L"%d", d.wDay);
        Text(c, n, m_fCenter.Get(), cell, A(isToday ? Color{0.04f, 0.05f, 0.08f, 1.0f} : (cur ? W(0.92f) : W(0.28f))));
    }
}

// Seasonal ambience behind the calendar: winter snow, spring petals, summer sun and warm dust, autumn leaves.
// Every particle is a pure function of (index, time), so nothing is stored or allocated per frame.
void CalendarPanel::DrawSeason(ID2D1DeviceContext* c, const Rect& P, int season, float a) {
    if (a <= 0.004f) return;
    const float s = S(), t = m_time;
    c->PushAxisAlignedClip(D2D1::RectF(P.x + 4 * s, P.y + 4 * s, P.Right() - 4 * s, P.Bottom() - 4 * s), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_MATRIX_3X2_F old; c->GetTransform(&old);
    auto disc = [&](float cx, float cy, float rx, float ry, const Color& col) {
        m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a));
        c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), m_brush.Get());
    };
    auto fade = [&](float y) { return std::clamp(std::min(y - P.y, P.Bottom() - y) / (34.0f * s), 0.0f, 1.0f); };   // particles melt in and out at the top and bottom edge
    auto wash = [&](const Color& col, float cx, float cy, float rx, float ry) {                                       // a soft glow built from stacked ellipses
        for (int k = 0; k < 4; ++k) { const float f = 1.0f - 0.22f * k; disc(cx, cy, rx * f, ry * f, Color{col.r, col.g, col.b, col.a * a * 0.34f}); }
    };

    if (season == 0) {                                                   // ---- winter: slow snowfall under a cold light
        wash(Color{0.55f, 0.75f, 1.0f, 0.16f}, P.x + P.width * 0.5f, P.y, P.width * 0.85f, P.height * 0.36f);
        for (unsigned i = 0; i < 28; ++i) {
            const float sp = (13.0f + Hash01(i, 2) * 26.0f) * s, r = (1.0f + Hash01(i, 3) * 2.5f) * s, cyc = P.height + 24 * s;
            const float y = P.y + std::fmod(t * sp + Hash01(i, 6) * cyc, cyc) - 12 * s;
            const float x = P.x + Hash01(i, 1) * P.width + std::sin(t * (0.5f + Hash01(i, 7) * 0.8f) + Hash01(i, 4) * 6.28f) * (8.0f + Hash01(i, 5) * 16.0f) * s;
            const float al = (0.28f + 0.5f * Hash01(i, 8)) * a * fade(y);
            disc(x, y, r * 2.4f, r * 2.4f, Color{0.85f, 0.92f, 1.0f, al * 0.18f});
            disc(x, y, r, r, Color{1.0f, 1.0f, 1.0f, al});
        }
    } else if (season == 1) {                                            // ---- spring: cherry petals drifting down and sideways
        wash(Color{1.0f, 0.70f, 0.85f, 0.14f}, P.x + P.width * 0.25f, P.y, P.width * 0.8f, P.height * 0.34f);
        static const Color pal[3] = {{1.0f, 0.72f, 0.84f, 1.0f}, {1.0f, 0.90f, 0.94f, 1.0f}, {0.98f, 0.60f, 0.78f, 1.0f}};
        for (unsigned i = 0; i < 18; ++i) {
            const float sp = (16.0f + Hash01(i, 2) * 22.0f) * s, cyc = P.height + 30 * s, k = 0.8f + Hash01(i, 3) * 0.6f;
            const float y = P.y + std::fmod(t * sp + Hash01(i, 6) * cyc, cyc) - 15 * s;
            const float x = P.x + Hash01(i, 1) * P.width + std::sin(t * (0.45f + Hash01(i, 7) * 0.5f) + Hash01(i, 4) * 6.28f) * (12.0f + Hash01(i, 5) * 18.0f) * s + t * 5 * s;
            const float xw = P.x + std::fmod(x - P.x + P.width * 4.0f, P.width);
            const float ang = std::sin(t * 0.9f + Hash01(i, 9) * 6.28f) * 55.0f + Hash01(i, 10) * 90.0f;
            const Color pc = pal[i % 3];
            c->SetTransform(D2D1::Matrix3x2F::Rotation(ang, D2D1::Point2F(xw, y)) * old);
            disc(xw, y, 3.6f * s * k, 2.0f * s * k, Color{pc.r, pc.g, pc.b, (0.5f + 0.25f * Hash01(i, 8)) * a * fade(y)});
            disc(xw - 0.8f * s, y - 0.4f * s, 1.5f * s * k, 0.8f * s * k, Color{1.0f, 1.0f, 1.0f, 0.35f * a * fade(y)});
            c->SetTransform(old);
        }
    } else if (season == 2) {                                            // ---- summer: a warm sun with slow rays, dust floating up in the heat
        const float sx = P.Right() - 66 * s, sy = P.y + 52 * s;
        wash(Color{1.0f, 0.78f, 0.30f, 0.30f}, sx, sy, 112 * s, 104 * s);
        for (int i = 0; i < 12; ++i) {
            const float ang = (t * 5.0f + i * 30.0f) * 0.0174533f, r0 = 30 * s, r1 = (62.0f + 22.0f * std::sin(t * 0.8f + i * 1.7f)) * s;
            m_brush->SetColor(D2D1::ColorF(1.0f, 0.86f, 0.45f, 0.075f * a));
            c->DrawLine(D2D1::Point2F(sx + std::cos(ang) * r0, sy + std::sin(ang) * r0), D2D1::Point2F(sx + std::cos(ang) * r1, sy + std::sin(ang) * r1), m_brush.Get(), 3.0f * s);
        }
        disc(sx, sy, 17 * s, 17 * s, Color{1.0f, 0.88f, 0.55f, 0.20f * a});
        disc(sx, sy, 11 * s, 11 * s, Color{1.0f, 0.93f, 0.70f, 0.30f * a});
        for (unsigned i = 0; i < 16; ++i) {
            const float sp = (8.0f + Hash01(i, 2) * 16.0f) * s, cyc = P.height + 20 * s;
            const float y = P.Bottom() + 10 * s - std::fmod(t * sp + Hash01(i, 6) * cyc, cyc);
            const float x = P.x + Hash01(i, 1) * P.width + std::sin(t * (0.4f + Hash01(i, 7) * 0.6f) + Hash01(i, 4) * 6.28f) * 10 * s;
            const float tw = 0.5f + 0.5f * std::sin(t * (1.2f + Hash01(i, 3)) + Hash01(i, 5) * 6.28f);
            const float r = (1.0f + Hash01(i, 9) * 1.6f) * s;
            disc(x, y, r * 2.6f, r * 2.6f, Color{1.0f, 0.88f, 0.5f, 0.07f * tw * a * fade(y)});
            disc(x, y, r, r, Color{1.0f, 0.93f, 0.68f, 0.55f * tw * a * fade(y)});
        }
    } else {                                                             // ---- autumn: tumbling leaves
        wash(Color{1.0f, 0.55f, 0.20f, 0.14f}, P.x + P.width * 0.7f, P.y, P.width * 0.8f, P.height * 0.34f);
        static const Color pal[4] = {{0.95f, 0.55f, 0.15f, 1.0f}, {0.85f, 0.30f, 0.12f, 1.0f}, {0.98f, 0.78f, 0.22f, 1.0f}, {0.70f, 0.25f, 0.10f, 1.0f}};
        for (unsigned i = 0; i < 13; ++i) {
            const float sp = (20.0f + Hash01(i, 2) * 24.0f) * s, cyc = P.height + 36 * s, k = 0.8f + Hash01(i, 3) * 0.6f;
            const float y = P.y + std::fmod(t * sp + Hash01(i, 6) * cyc, cyc) - 18 * s;
            const float x = P.x + Hash01(i, 1) * P.width + std::sin(t * (0.55f + Hash01(i, 7) * 0.5f) + Hash01(i, 4) * 6.28f) * (16.0f + Hash01(i, 5) * 22.0f) * s;
            const float ang = std::sin(t * 1.1f + Hash01(i, 9) * 6.28f) * 70.0f + Hash01(i, 10) * 60.0f;
            const Color pc = pal[i % 4]; const float al = (0.50f + 0.25f * Hash01(i, 8)) * a * fade(y);
            c->SetTransform(D2D1::Matrix3x2F::Rotation(ang, D2D1::Point2F(x, y)) * old);
            disc(x, y, 5.6f * s * k, 2.9f * s * k, Color{pc.r, pc.g, pc.b, al});
            m_brush->SetColor(D2D1::ColorF(pc.r * 0.6f, pc.g * 0.5f, pc.b * 0.4f, al * 0.8f));
            c->DrawLine(D2D1::Point2F(x - 5.6f * s * k, y), D2D1::Point2F(x + 5.0f * s * k, y), m_brush.Get(), 0.9f * s);
            c->SetTransform(old);
        }
    }
    c->PopAxisAlignedClip();
}

void CalendarPanel::OnDraw(ID2D1DeviceContext* c, const Rect& P) {
    const float s = S(); const float x = P.x + 24 * s, w = P.width - 48 * s;
    SYSTEMTIME now; GetLocalTime(&now);

    // seasonal ambience first, so every label and day number stays on top of it
    if (m_seasonT < 1.0f) DrawSeason(c, P, m_prevSeason, 1.0f - Ease(m_seasonT));
    DrawSeason(c, P, m_season, m_seasonT < 1.0f ? Ease(m_seasonT) : 1.0f);

    wchar_t t[16]; swprintf_s(t, L"%02d:%02d", now.wHour, now.wMinute);
    Text(c, t, m_fBig.Get(), Rect{x, P.y + 14 * s, 190 * s, 64 * s}, W(0.97f));
    wchar_t sec[8]; swprintf_s(sec, L":%02d", now.wSecond);
    Text(c, sec, m_fTitle.Get(), Rect{x + 150 * s, P.y + 34 * s, 50 * s, 36 * s}, kAccent);
    const std::wstring date = i18n::LongDate(now);
    Text(c, date, m_fBody.Get(), Rect{x, P.y + 78 * s, w, 24 * s}, W(0.6f));

    const float e = Ease(m_anim), hy = P.y + 118 * s;
    {   // month and year: the old title slides out, the new one slides in from the direction you are scrolling to
        c->PushAxisAlignedClip(D2D1::RectF(x, hy - 2 * s, x + 200 * s, hy + 34 * s), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto title = [&](int y, int mo, float yOff, float al) { Text(c, std::wstring(i18n::Month(mo)) + L" " + std::to_wstring(y), m_fTitle.Get(), Rect{x, hy + yOff, 200 * s, 32 * s}, W(0.95f * al)); };
        if (e < 1.0f) { title(m_prevYear, m_prevMonth, -m_dir * e * 22 * s, 1.0f - e); title(m_year, m_month, m_dir * (1.0f - e) * 22 * s, e); }
        else title(m_year, m_month, 0.0f, 1.0f);
        c->PopAxisAlignedClip();
    }
    Rect prev{P.Right() - 130 * s, hy, 34 * s, 32 * s}, today{P.Right() - 92 * s, hy, 34 * s, 32 * s}, next{P.Right() - 56 * s, hy, 34 * s, 32 * s};
    for (auto& r : {prev, today, next}) if (r.Contains(m_mouse)) Fill(c, r, 10 * s, W(0.1f));
    Glyph(c, gl::kGlyphChevronUp, prev, W(0.85f)); Glyph(c, gl::kGlyphHome, today, W(0.85f)); Glyph(c, 0xE70D, next, W(0.85f));
    AddHit(prev, 1); AddHit(next, 2); AddHit(today, 3);

    const float cw = w / 7, gy = hy + 44 * s;
    for (int i = 0; i < 7; ++i) {
        const std::wstring nm = std::wstring(i18n::Weekday((1 + i) % 7)).substr(0, 3);   // 2024-01-01 is a Monday
        Text(c, nm, m_fCenter.Get(), Rect{x + i * cw, gy, cw, 24 * s}, W(0.4f));
    }
    {   // the grid slides vertically inside its own window while the month changes
        c->PushAxisAlignedClip(D2D1::RectF(x - 2 * s, gy + 26 * s, x + w + 2 * s, gy + 28 * s + 6 * 40 * s), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float travel = 34 * s;
        if (e < 1.0f) {
            DrawMonthGrid(c, x, gy, cw, m_prevYear, m_prevMonth, now, -m_dir * e * travel, 1.0f - e, false);
            DrawMonthGrid(c, x, gy, cw, m_year, m_month, now, m_dir * (1.0f - e) * travel, e, true);
        } else DrawMonthGrid(c, x, gy, cw, m_year, m_month, now, 0.0f, 1.0f, true);
        c->PopAxisAlignedClip();
    }
    const float by = P.Bottom() - 56 * s;
    Rect b1{x, by, (w - 10 * s) / 2, 38 * s}, b2{x + (w + 10 * s) / 2, by, (w - 10 * s) / 2, 38 * s};
    Fill(c, b1, 14 * s, b1.Contains(m_mouse) ? W(0.12f) : W(0.07f)); Fill(c, b2, 14 * s, b2.Contains(m_mouse) ? W(0.12f) : W(0.07f));
    AddHit(b1, 4); AddHit(b2, 5);
    Text(c, i18n::Pick(L"Alarms & clock", L"Alarmlar ve saat"), m_fCenter.Get(), b1, W(0.85f)); Text(c, i18n::Pick(L"Date & time settings", L"Tarih ve saat ayarları"), m_fCenter.Get(), b2, W(0.85f));
}

// ===== Tray ==================================================================================
float TrayPanel::PanelHeight() {
    const auto& icons = TrayHost::Instance().Icons();
    const int n = static_cast<int>(std::count_if(icons.begin(), icons.end(), [](const TrayIcon& i) { return !i.hidden && i.icon; }));
    const int rows = std::max(1, (n + 5) / 6);
    return 84.0f + rows * 52.0f;
}
void TrayPanel::OnShow() { TrayHost::Instance().Prune(); }

void TrayPanel::OnClick(int id, const Vec2&) {
    const auto& icons = TrayHost::Instance().Icons();
    int k = 0;
    for (const auto& i : icons) {
        if (i.hidden || !i.icon) continue;
        if (k++ != id - 1) continue;
        const ULONGLONG now = GetTickCount64();
        const bool dbl = !m_rightButton && id == m_lastId && now - m_lastClick < 400;
        m_lastId = id; m_lastClick = now;
        TrayHost::Instance().Click(i, m_rightButton, dbl);
        if (!m_rightButton) Hide();
        return;
    }
}

void TrayPanel::OnDraw(ID2D1DeviceContext* c, const Rect& P) {
    const float s = S();
    Text(c, i18n::Pick(L"System tray", L"Sistem tepsisi"), m_fTitle.Get(), Rect{P.x + 22 * s, P.y + 14 * s, 200 * s, 28 * s}, W(0.95f));
    const auto& icons = TrayHost::Instance().Icons();
    if (!TrayHost::Instance().Active()) { Text(c, i18n::Pick(L"Tray host is not running", L"Tepsi servisi çalışmıyor"), m_fBody.Get(), Rect{P.x + 22 * s, P.y + 56 * s, P.width - 44 * s, 28 * s}, W(0.5f)); return; }
    int k = 0; const float cell = 48 * s;
    for (const auto& i : icons) {
        if (i.hidden || !i.icon) continue;
        Rect r{P.x + 20 * s + (k % 6) * (cell + 2 * s), P.y + 56 * s + (k / 6) * (cell + 4 * s), cell, cell};
        auto& e = m_cache[{i.hwnd, i.id}];
        if (!e.bmp || e.serial != i.serial) {
            // tray HICONs are mostly 16 px: show the owning exe's crisp icon instead, like the taskbar does
            ComPtr<ID2D1Bitmap1> bmp;
            if (gl::IconNativeSize(i.icon) < 28) {
                std::wstring exe;
                DWORD pid = 0; GetWindowThreadProcessId(i.hwnd, &pid);
                if (HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) {
                    wchar_t buf[MAX_PATH * 2] = {}; DWORD len = MAX_PATH * 2;
                    if (QueryFullProcessImageNameW(h, 0, buf, &len)) exe.assign(buf, len);
                    CloseHandle(h);
                }
                bmp = gl::ExeIconBitmap(m_dcomp->GetContext(), exe, 128);
            }
            if (!bmp) bmp = gl::IconToBitmap(m_dcomp->GetContext(), i.icon, static_cast<int>(64 * s));
            e.bmp = bmp; e.serial = i.serial;
        }
        const bool hot = r.Contains(m_mouse);
        Fill(c, r, 14 * s, hot ? W(0.12f) : W(0.04f));
        if (e.bmp) { const D2D1_RECT_F d = D2D1::RectF(r.Center().x - 14 * s, r.Center().y - 14 * s, r.Center().x + 14 * s, r.Center().y + 14 * s); c->DrawBitmap(e.bmp.Get(), &d, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC); }
        AddHit(r, ++k);
        if (hot && !i.tip.empty()) {
            Text(c, i.tip, m_fSmall.Get(), Rect{P.x + 22 * s, P.Bottom() - 30 * s, P.width - 44 * s, 24 * s}, W(0.6f));
        }
    }
    if (k == 0) Text(c, i18n::Pick(L"No apps are using the tray right now", L"Şu an tepsiyi kullanan uygulama yok"), m_fBody.Get(), Rect{P.x + 22 * s, P.y + 56 * s, P.width - 44 * s, 28 * s}, W(0.5f));
}

} // namespace aether::bar
