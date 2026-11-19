// AetherIntro: the greeting animation shown when Aether starts (logon / first run).
//   AetherIntro.exe           short version (~3.4 s)
//   AetherIntro.exe --first   welcome version (~5.2 s), launched by the installer
// Design: quiet and still. The theme's deepest colour, one soft light from above, the logo easing in, the wordmark rising out of a mask
// and a hairline progress bar. No particles, no moving glows, no slogans.
// Any click, Esc, Enter or Space skips it. A file %APPDATA%\Aether\no-intro disables the logon intro (AetherShell checks it).
#include <windows.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "aether/core/single_instance.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/mini_gfx.hpp"

using namespace aether;
using aether::mg::Color;
using aether::mg::Rect;
using Microsoft::WRL::ComPtr;

namespace {

bool g_first = false;
float g_T = 3.4f;

float Clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
float Smooth(float x) { x = Clamp01(x); return x * x * (3.0f - 2.0f * x); }
float EaseOutCubic(float x) { x = Clamp01(x); const float u = 1.0f - x; return 1.0f - u * u * u; }
float EaseInOut(float x) { x = Clamp01(x); return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f; }

bool Turkish() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH; }

// The Aether logo is embedded as RCDATA resource 301 (aether.png); decode it with WIC.
ComPtr<ID2D1Bitmap1> LoadLogoBitmap(ID2D1DeviceContext* ctx) {
    ComPtr<ID2D1Bitmap1> out;
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC hr = FindResourceW(mod, MAKEINTRESOURCEW(301), RT_RCDATA);
    HGLOBAL hg = hr ? LoadResource(mod, hr) : nullptr;
    const void* data = hg ? LockResource(hg) : nullptr;
    const DWORD size = hr ? SizeofResource(mod, hr) : 0;
    if (!ctx || !data || !size) return out;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) return out;
    ComPtr<IWICStream> st;
    if (FAILED(wic->CreateStream(&st)) ||
        FAILED(st->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<void*>(data)), size))) return out;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return out;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return out;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return out;
    ctx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &out);
    return out;
}

class Intro {
public:
    bool Create(HINSTANCE inst) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"AetherIntroWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Aether", WS_POPUP,
                                 mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, nullptr, nullptr, inst, this);
        if (!m_hwnd) return false;
        m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
        RECT rc; GetClientRect(m_hwnd, &rc);
        m_dc = std::make_unique<render::DCompLayer>();
        if (!m_dc->Initialize(m_hwnd, rc.right, rc.bottom)) return false;
        m_g.Init(m_dc.get());
        render::glass::SyncTheme();                      // follow the theme chosen in Aether settings (%APPDATA%\Aether\theme.txt)
        m_W = rc.right / m_scale; m_H = rc.bottom / m_scale;
        QueryPerformanceFrequency(&m_freq); QueryPerformanceCounter(&m_t0);
        ShowWindow(m_hwnd, SW_SHOWNA);
        SetForegroundWindow(m_hwnd);
        SetTimer(m_hwnd, 1, 8, nullptr);
        return true;
    }
    void Run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }

private:
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    float m_scale{1.0f}, m_W{0}, m_H{0};
    ComPtr<ID2D1Bitmap1> m_logo;
    LARGE_INTEGER m_freq{}, m_t0{};
    bool m_armed{false};

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Intro* self = nullptr;
        if (m == WM_NCCREATE) { self = static_cast<Intro*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        else self = reinterpret_cast<Intro*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
        case WM_TIMER: self->Frame(); return 0;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
        case WM_LBUTTONDOWN: case WM_KEYDOWN: DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    void Radial(float cx, float cy, float r, const Color& c0, const Color& c1) {
        if (c0.a <= 0.002f && c1.a <= 0.002f) return;
        auto* ctx = m_g.ctx;
        D2D1_GRADIENT_STOP st[2] = {{0.0f, D2D1::ColorF(c0.r, c0.g, c0.b, c0.a)}, {1.0f, D2D1::ColorF(c1.r, c1.g, c1.b, c1.a)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        if (FAILED(ctx->CreateGradientStopCollection(st, 2, &sc))) return;
        ComPtr<ID2D1RadialGradientBrush> br;
        if (FAILED(ctx->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), r, r), sc.Get(), &br))) return;
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br.Get());
    }

    void Frame() {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const float t = static_cast<float>(now.QuadPart - m_t0.QuadPart) / static_cast<float>(m_freq.QuadPart);
        if (t >= g_T) { DestroyWindow(m_hwnd); return; }
        // skip: Esc / Enter / Space, or a click (armed once the buttons were released, so the click that started us does not skip)
        const bool pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000) || (GetAsyncKeyState(VK_SPACE) & 0x8000);
        if (!pressed) m_armed = true;
        if (pressed && m_armed && t > 0.4f) { DestroyWindow(m_hwnd); return; }
        if (t > 0.3f && GetForegroundWindow() != m_hwnd) SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        auto* ctx = m_g.Begin();
        if (!ctx) return;
        if (!m_logo) m_logo = LoadLogoBitmap(ctx);
        render::glass::SyncTheme();                      // pick up a theme change mid-intro too
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
        const float W = m_W, H = m_H, cx = W * 0.5f, cy = H * 0.5f - 30.0f;
        const float fadeIn = Smooth(t / 0.45f), fadeOut = 1.0f - Smooth((t - (g_T - 0.55f)) / 0.55f);
        const float A = fadeIn * fadeOut;
        const auto& th = render::glass::CurrentTheme();
        const Color accent{render::glass::kViolet}, light{render::glass::kVioletLight};
        const bool tr = Turkish();

        // backdrop: the theme's deepest colour, one still light from above and a soft vignette
        m_g.FillRect(Rect{0, 0, W, H}, Color{th.bodyBot.r * 0.55f, th.bodyBot.g * 0.55f, th.bodyBot.b * 0.55f, A});
        const float R = std::max(W, H);
        Radial(cx, cy - H * 0.16f, R * 0.60f, mg::WithA(accent, 0.15f * A * Smooth(t / 1.4f)), mg::WithA(accent, 0.0f));
        Radial(cx, H * 0.5f, std::sqrt(W * W + H * H) * 0.5f, Color{0, 0, 0, 0.0f}, Color{0, 0, 0, 0.50f * A});

        // the logo: fades in and settles from 94 % to 100 %, nothing bounces or pulses
        const float lk = EaseOutCubic((t - 0.12f) / 0.95f), la = lk * A;
        const float logoSize = 84.0f, s = logoSize * (0.94f + 0.06f * lk);
        if (m_logo && la > 0.002f) {
            Radial(cx, cy, logoSize * 1.30f, mg::WithA(accent, 0.20f * la), mg::WithA(accent, 0.0f));
            const D2D1_RECT_F dst = D2D1::RectF(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f);
            ctx->DrawBitmap(m_logo.Get(), &dst, la, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }

        // wordmark: small, light, evenly tracked; each letter rises out of a mask a hair after the one before
        const std::wstring title = L"AETHER";
        const float fs = 19.0f, gap = 8.0f, baseY = cy + logoSize * 0.5f + 36.0f;
        float total = 0; std::vector<float> wd;
        for (wchar_t ch : title) { const float w = m_g.TextWidth(std::wstring(1, ch), fs, 500); wd.push_back(w); total += w + gap; }
        total -= gap;
        {
            const D2D1_RECT_F clip = D2D1::RectF(cx - total * 0.5f - 12.0f, baseY - 2.0f, cx + total * 0.5f + 12.0f, baseY + fs + 14.0f);
            ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            float x = cx - total * 0.5f;
            for (size_t i = 0; i < title.size(); ++i) {
                const float k = EaseOutCubic((t - 0.80f - 0.045f * static_cast<float>(i)) / 0.70f);
                if (k > 0.003f)
                    m_g.Text(std::wstring(1, title[i]), Rect{x - 6, baseY + (1.0f - k) * (fs + 6.0f), wd[i] + 12, fs + 12}, fs, 500, Color{1, 1, 1, 0.94f * A}, DWRITE_TEXT_ALIGNMENT_CENTER);
                x += wd[i] + gap;
            }
            ctx->PopAxisAlignedClip();
        }

        // hairline progress bar and one quiet status line
        const float barW = 168.0f, barY = baseY + fs + 34.0f;
        const float ba = Smooth((t - 1.05f) / 0.5f) * A;
        if (ba > 0.003f) {
            const float p = EaseInOut((t - 1.10f) / std::max(0.5f, g_T - 1.10f - 0.60f));
            const Rect track{cx - barW * 0.5f, barY, barW, 2.0f};
            m_g.FillRR(track, 1.0f, Color{1, 1, 1, 0.10f * ba});
            if (p > 0.002f) m_g.FillRR(Rect{track.x, track.y, std::max(2.0f, barW * p), 2.0f}, 1.0f, Color{light.r, light.g, light.b, 0.95f * ba});
            const std::wstring status = g_first ? (tr ? L"Masaüstün hazırlanıyor" : L"Setting up your desktop") : (tr ? L"Başlatılıyor" : L"Starting");
            m_g.Text(status, Rect{0, barY + 14.0f, W, 20}, 11.5f, 400, Color{1, 1, 1, 0.48f * ba}, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (g_first) m_g.Text(tr ? L"Atlamak için tıkla" : L"Click to skip", Rect{0, H - 52, W, 20}, 11.0f, 400, Color{1, 1, 1, 0.28f * ba}, DWRITE_TEXT_ALIGNMENT_CENTER);
        m_g.End();
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!aether::core::AcquireSingleInstance(L"Aether.Intro.SingleInstance")) return 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (cmd && wcsstr(cmd, L"--first")) { g_first = true; g_T = 5.2f; }
    Intro intro;
    if (!intro.Create(inst)) return 1;
    intro.Run();
    return 0;
}
