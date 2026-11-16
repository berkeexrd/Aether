#include <atomic>
#include "aether/ui/settings_window.hpp"
#include "aether/ui/theme.hpp"
#include "aether/ui/components.hpp"
#include "aether/ui/animation.hpp"
#include "aether/core/log.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#define L10N(en, tr) (aether::i18n::Turkish() ? (tr) : (en))

namespace aether::ui {

static const wchar_t* SETTINGS_CLASS_NAME = L"AetherSettingsWindowClass";

bool SettingsWindow::CreateDeviceD3D() {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &m_pSwapChain,
        &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext
    );

    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &m_pSwapChain,
            &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext
        );
    }

    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void SettingsWindow::CleanupDeviceD3D() {
    CleanupRenderTarget();
    m_pSwapChain.Reset();
    m_pd3dDeviceContext.Reset();
    m_pd3dDevice.Reset();
}

void SettingsWindow::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> pBackBuffer;
    m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    m_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &m_mainRenderTargetView);
}

void SettingsWindow::CleanupRenderTarget() {
    m_mainRenderTargetView.Reset();
}

namespace { std::atomic<ULONGLONG> g_lastActivityMs{0}; }
void SettingsWindow::NoteActivity() { g_lastActivityMs = GetTickCount64(); }
bool SettingsWindow::IsIdle() { return GetTickCount64() - g_lastActivityMs.load() > 2000; }

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {   // anything the user does counts as activity (checked before ImGui sees the message)
        case WM_MOUSEMOVE: case WM_NCMOUSEMOVE: case WM_MOUSELEAVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SIZE: case WM_SETFOCUS: case WM_KILLFOCUS:
        case WM_ACTIVATE: case WM_DPICHANGED: case WM_SHOWWINDOW: case WM_SETCURSOR: case WM_NCLBUTTONDOWN: case WM_DISPLAYCHANGE:
            NoteActivity(); break;
        default: break;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }

    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool SettingsWindow::Create() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = SETTINGS_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    Scale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    m_width = std::min(static_cast<uint32_t>(1120.0f * Scale), static_cast<uint32_t>(screenW - 60));
    m_height = std::min(static_cast<uint32_t>(760.0f * Scale), static_cast<uint32_t>(screenH - 80));
    int posX = (screenW - static_cast<int>(m_width)) / 2;
    int posY = (screenH - static_cast<int>(m_height)) / 2;

    m_hwnd = CreateWindowExW(
        0,
        SETTINGS_CLASS_NAME,
        L"Aether Control Center",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU,
        posX, posY, m_width, m_height,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!m_hwnd) return false;

    // Windows 11 Dark Mode Titlebar & Mica/Dark preference
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20, &darkMode, sizeof(darkMode)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    const int corner = 2;   // DWMWCP_ROUND: rounded window corners (Windows 11)
    DwmSetWindowAttribute(m_hwnd, 33, &corner, sizeof(corner));
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);   // frameless: we draw the caption

    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D();
        return false;
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    {   // Segoe UI (+ Semibold for headings, Fluent Icons for the sidebar) with Latin Extended so Turkish letters render properly
        static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2013, 0x2026, 0x2190, 0x2193, 0};
        static const ImWchar iconRanges[] = {0xE700, 0xF8FF, 0};
        const char* reg = "C:\\Windows\\Fonts\\segoeui.ttf";
        const char* semi = "C:\\Windows\\Fonts\\seguisb.ttf";
        const char* bold = "C:\\Windows\\Fonts\\segoeuib.ttf";
        const char* icons = "C:\\Windows\\Fonts\\SegoeIcons.ttf";
        auto exists = [](const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; };
        ImFontConfig fc; fc.OversampleH = 3; fc.OversampleV = 2; fc.PixelSnapH = false;
        if (exists(reg)) {
            const char* head = exists(semi) ? semi : (exists(bold) ? bold : reg);
            io.FontDefault = io.Fonts->AddFontFromFileTTF(reg, 15.5f * Scale, &fc, ranges);
            FontSmall = io.Fonts->AddFontFromFileTTF(reg, 13.0f * Scale, &fc, ranges);
            FontBold = io.Fonts->AddFontFromFileTTF(head, 17.0f * Scale, &fc, ranges);
            FontTitle = io.Fonts->AddFontFromFileTTF(head, 26.0f * Scale, &fc, ranges);
            if (exists(icons)) FontIcons = io.Fonts->AddFontFromFileTTF(icons, 16.0f * Scale, &fc, iconRanges);
        }
    }
    render::glass::SyncTheme();
    Theme::Instance().UseGlassTheme(render::glass::ThemeIndex());

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice.Get(), m_pd3dDeviceContext.Get());

    AETHER_LOG_INFO("SettingsWindow created successfully");
    return true;
}

void SettingsWindow::Show(bool show) {
    m_visible = show;
    if (show) NoteActivity();
    if (m_hwnd) {
        ShowWindow(m_hwnd, show ? SW_SHOW : SW_HIDE);
        if (show) {
            SetForegroundWindow(m_hwnd);
        }
    }
}

void SettingsWindow::ShowTab(SettingsTab tab) {
    m_activeTab = tab;
    m_query[0] = '\0'; m_lastQuery.clear();
    Show(true);
}

void SettingsWindow::Destroy() {
    if (m_hwnd) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

namespace {
// Lower-case + fold Turkish letters ("ayar" finds "Ayarlar", "gorunum" finds "Görünüm").
std::wstring FoldUtf8(const std::string& in) {
    if (in.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), w.data(), n);
    std::wstring low(w.size() * 2 + 2, L'\0');
    const int m = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), static_cast<int>(w.size()), low.data(), static_cast<int>(low.size()), nullptr, nullptr, 0);
    if (m > 0) low.resize(static_cast<size_t>(m)); else low = w;
    std::wstring out;
    out.reserve(low.size());
    for (wchar_t c : low) {
        switch (c) {
            case L'\u0131': out += L'i'; break;
            case L'\u0307': break;
            case L'\u011F': out += L'g'; break;
            case L'\u015F': out += L's'; break;
            case L'\u00F6': out += L'o'; break;
            case L'\u00FC': out += L'u'; break;
            case L'\u00E7': out += L'c'; break;
            default: out += c;
        }
    }
    return out;
}

const char* TabName(int tab) {
    switch (static_cast<SettingsTab>(tab)) {
        case SettingsTab::General:          return L10N("General", "Genel");
        case SettingsTab::Appearance:       return L10N("Appearance & Themes", "Görünüm ve Temalar");
        case SettingsTab::DynamicIsland:    return "Dynamic Island";
        case SettingsTab::Widgets:          return L10N("Widgets", "Widget'lar");
        case SettingsTab::Media:            return L10N("Media", "Medya");
        case SettingsTab::SystemMonitoring: return L10N("Hardware Monitor", "Donanım İzleme");
        case SettingsTab::GamingMode:       return L10N("Gaming Mode", "Oyun Modu");
        case SettingsTab::About:            return L10N("About", "Hakkında");
        default:                            return "";
    }
}
} // namespace

// Records every row of the pages reachable from the sidebar. Rows are only recorded (their controls never run) and the page is run inside
// a 1 px, input-less child, so nothing becomes visible or clickable.
void SettingsWindow::BuildSearchIndex() {
    m_index.clear();
    static const SettingsTab kTabs[] = {SettingsTab::General, SettingsTab::Appearance, SettingsTab::DynamicIsland, SettingsTab::Widgets,
                                        SettingsTab::Media, SettingsTab::SystemMonitoring, SettingsTab::GamingMode, SettingsTab::About};
    ImGui::BeginChild("##searchindex", ImVec2(1.0f, 1.0f), 0, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    for (SettingsTab t : kTabs) SettingsPages::CollectSearchIndex(t, m_index);
    ImGui::EndChild();
    m_foldLabel.clear(); m_foldAll.clear();
    for (const auto& e : m_index) {
        m_foldLabel.push_back(FoldUtf8(e.label));
        m_foldAll.push_back(FoldUtf8(e.label + " " + e.desc + " " + e.section + " " + TabName(e.tab)));
    }
    m_indexBuilt = true;
    m_indexTurkish = aether::i18n::Turkish();
    m_lastQuery.clear();
}

void SettingsWindow::UpdateSearchHits() {
    if (m_lastQuery == m_query) return;
    m_lastQuery = m_query;
    m_hits.clear();
    std::vector<std::wstring> words;
    {
        const std::wstring q = FoldUtf8(m_query);
        for (size_t b = 0; b < q.size();) {
            const size_t e = q.find(L' ', b);
            const size_t len = (e == std::wstring::npos ? q.size() : e) - b;
            if (len) words.push_back(q.substr(b, len));
            if (e == std::wstring::npos) break;
            b = e + 1;
        }
    }
    if (words.empty()) return;
    std::vector<std::pair<int, int>> scored;
    for (int i = 0; i < static_cast<int>(m_index.size()); ++i) {
        int score = 0;
        bool all = true;
        for (const auto& w : words) {
            const size_t inLabel = m_foldLabel[i].find(w);
            if (inLabel != std::wstring::npos) { score += inLabel == 0 ? 0 : 1; continue; }
            if (m_foldAll[i].find(w) == std::wstring::npos) { all = false; break; }
            score += 3;                                              // only in the description / section / page name
        }
        if (all) scored.emplace_back(score, i);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& s : scored) m_hits.push_back(s.second);
}

void SettingsWindow::RenderSearchResults() {
    const auto& theme = Theme::Instance().Colors();
    if (m_hits.empty()) {
        ImGui::Dummy(ImVec2(0.0f, Sc(30.0f)));
        ImGui::TextColored(theme.TextSecondary, "%s", L10N("No setting matches your search.", "Aramanla eşleşen bir ayar yok."));
        ImGui::TextColored(theme.TextMuted, "%s", L10N("Try a shorter word, e.g. \"theme\", \"startup\" or \"hover\".", "Daha kısa bir sözcük dene: \"tema\", \"başlangıç\" veya \"hover\" gibi."));
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x, rowH = Sc(66.0f);
    for (size_t k = 0; k < m_hits.size(); ++k) {
        const UI::SearchEntry& e = m_index[static_cast<size_t>(m_hits[k])];
        ImGui::PushID(static_cast<int>(k));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(w, rowH - Sc(4.0f)));
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 q(p.x + w, p.y + rowH - Sc(4.0f));
        dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(1, 1, 1, hov ? 0.075f : 0.035f)), Sc(12.0f));
        if (hov) dl->AddRect(p, q, ImGui::GetColorU32(ImVec4(theme.Accent.x, theme.Accent.y, theme.Accent.z, 0.45f)), Sc(12.0f), 0, 1.0f);

        std::string crumb = std::string(TabName(e.tab)) + "  >  " + e.section;
        if (FontSmall) ImGui::PushFont(FontSmall);
        const float cw = std::min(ImGui::CalcTextSize(crumb.c_str()).x, w * 0.45f);
        dl->PushClipRect(ImVec2(q.x - Sc(16.0f) - cw, p.y), ImVec2(q.x - Sc(8.0f), q.y), true);
        dl->AddText(ImVec2(q.x - Sc(16.0f) - cw, p.y + Sc(11.0f)), ImGui::GetColorU32(theme.AccentHover), crumb.c_str());
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(p.x + Sc(14.0f), p.y + Sc(30.0f)), ImVec2(q.x - Sc(14.0f), q.y), true);
        dl->AddText(ImVec2(p.x + Sc(16.0f), p.y + Sc(34.0f)), ImGui::GetColorU32(theme.TextMuted), e.desc.c_str());
        dl->PopClipRect();
        if (FontSmall) ImGui::PopFont();
        dl->PushClipRect(ImVec2(p.x + Sc(14.0f), p.y), ImVec2(q.x - Sc(24.0f) - cw, q.y), true);
        dl->AddText(ImVec2(p.x + Sc(16.0f), p.y + Sc(9.0f)), ImGui::GetColorU32(theme.TextPrimary), e.label.c_str());
        dl->PopClipRect();

        if (clicked) { m_activeTab = static_cast<SettingsTab>(e.tab); UI::FocusSetting(e.section, e.label); m_query[0] = '\0'; m_lastQuery.clear(); }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, Sc(4.0f)));
    }
}

namespace {

ImU32 Col(const ImVec4& c, float aMul = 1.0f) { ImVec4 v = c; v.w *= aMul; return ImGui::GetColorU32(v); }

// A soft radial light: a triangle fan whose vertex colours fade out in three rings. Smooth, no banding, one draw call.
void Glow(ImDrawList* dl, ImVec2 c, float rx, float ry, const ImVec4& col, float alpha) {
    if (alpha <= 0.002f || rx < 2.0f || ry < 2.0f) return;
    constexpr int N = 40;
    static const float ringT[3] = {0.40f, 0.74f, 1.0f};
    static const float ringA[4] = {1.0f, 0.46f, 0.13f, 0.0f};
    static float cs[N], sn[N];
    static bool init = false;
    if (!init) { init = true; for (int i = 0; i < N; ++i) { const float a = 6.2831853f * static_cast<float>(i) / N; cs[i] = std::cos(a); sn[i] = std::sin(a); } }
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(15 * N, 1 + 3 * N);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(c, uv, Col(col, alpha * ringA[0]));
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < N; ++i)
            dl->PrimWriteVtx(ImVec2(c.x + cs[i] * rx * ringT[k], c.y + sn[i] * ry * ringT[k]), uv, Col(col, alpha * ringA[k + 1]));
    auto V = [&](int ring, int i) { return static_cast<ImDrawIdx>(base + 1 + ring * N + (i % N)); };
    for (int i = 0; i < N; ++i) { dl->PrimWriteIdx(base); dl->PrimWriteIdx(V(0, i)); dl->PrimWriteIdx(V(0, i + 1)); }
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < N; ++i) {
            const ImDrawIdx a = V(k, i), b = V(k, i + 1), c2 = V(k + 1, i), d = V(k + 1, i + 1);
            dl->PrimWriteIdx(a); dl->PrimWriteIdx(c2); dl->PrimWriteIdx(b);
            dl->PrimWriteIdx(b); dl->PrimWriteIdx(c2); dl->PrimWriteIdx(d);
        }
}

// The whole window surface: a vertical gradient from the theme's lit colour to its deep colour, two big accent lights (top left, bottom right),
// a smaller highlight at the top, and a hairline edge. Static, so it costs nothing when the window is idle.
void PaintBackdrop(ImDrawList* dl, ImVec2 p, ImVec2 sz, const ThemeColors& th) {
    const ImVec2 q(p.x + sz.x, p.y + sz.y);
    dl->AddRectFilledMultiColor(p, q, Col(th.SurfaceElevated), Col(th.SurfaceElevated), Col(th.Background), Col(th.Background));
    const int ti = render::glass::ThemeIndex();
    const auto& def = render::glass::CurrentTheme();
    const float k = ti == 1 ? 0.30f : (def.flat ? 0.55f : 1.0f);          // OLED stays nearly black, flat themes get a quieter light
    const ImVec4 a2(render::glass::kAccent2.r, render::glass::kAccent2.g, render::glass::kAccent2.b, 1.0f);
    const ImVec4 a1 = th.Accent, a3 = th.AccentHover;
    Glow(dl, ImVec2(p.x + sz.x * 0.12f, p.y + sz.y * 0.02f), sz.x * 0.70f, sz.y * 0.80f, a1, 0.30f * k);
    Glow(dl, ImVec2(p.x + sz.x * 0.98f, p.y + sz.y * 1.02f), sz.x * 0.60f, sz.y * 0.78f, a2, 0.24f * k);
    Glow(dl, ImVec2(p.x + sz.x * 0.72f, p.y + sz.y * -0.04f), sz.x * 0.34f, sz.y * 0.34f, a3, 0.10f * k);
    dl->AddRect(ImVec2(p.x + 0.5f, p.y + 0.5f), ImVec2(q.x - 0.5f, q.y - 0.5f), IM_COL32(255, 255, 255, 26), Sc(8.0f), 0, 1.0f);   // matches the DWM corner radius
}

} // namespace

void SettingsWindow::Render() {
    if (!m_visible || !m_hwnd) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    AnimationEngine::Instance().Update(std::min(ImGui::GetIO().DeltaTime, 0.034f));   // the first frame after an idle stretch must not skip the start of a hover/click animation
    UI::TickSearchHighlight(ImGui::GetIO().DeltaTime);
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) m_focusSearch = true;     // Ctrl+F: jump to the search box

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (render::glass::SyncTheme()) Theme::Instance().UseGlassTheme(render::glass::ThemeIndex());
    const auto& theme = Theme::Instance().Colors();

    const ImVec4 winBg(theme.Background.x, theme.Background.y, theme.Background.z, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, winBg);
    if (ImGui::Begin("AetherMainWindow", nullptr, windowFlags)) {
        const float W = viewport->Size.x, H = viewport->Size.y;
        ImDrawList* bg = ImGui::GetWindowDrawList();
        PaintBackdrop(bg, ImGui::GetWindowPos(), ImVec2(W, H), theme);

        auto utf8 = [](unsigned cp) { char b[5] = {}; if (cp < 0x80) b[0] = static_cast<char>(cp); else if (cp < 0x800) { b[0] = static_cast<char>(0xC0 | (cp >> 6)); b[1] = static_cast<char>(0x80 | (cp & 0x3F)); } else { b[0] = static_cast<char>(0xE0 | (cp >> 12)); b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); b[2] = static_cast<char>(0x80 | (cp & 0x3F)); } return std::string(b); };

        if (SettingsPages::WelcomeNeeded()) {
            SettingsPages::RenderWelcome(W, H);
        } else {
        const float sbW = Sc(262.0f);
        const ImVec2 w0 = ImGui::GetWindowPos();

        // ---- SIDEBAR: a darker glass strip that belongs to the window (no floating box) ----------------------------
        {
            const ImVec2 a = w0, b(w0.x + sbW, w0.y + H), mid(b.x, w0.y + H * 0.5f);
            bg->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 38), IM_COL32(0, 0, 0, 38), IM_COL32(0, 0, 0, 92), IM_COL32(0, 0, 0, 92));
            bg->AddRectFilledMultiColor(ImVec2(b.x - 1.0f, a.y), ImVec2(b.x, mid.y), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 30));
            bg->AddRectFilledMultiColor(ImVec2(b.x - 1.0f, mid.y), b, IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
        }
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(16.0f), 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("SidebarRegion", ImVec2(sbW, H), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        {
            const ImVec2 o = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // brand: rounded gradient tile with a soft glow, name and product line
            const float ts0 = Sc(44.0f);
            const ImVec2 b0(o.x + Sc(4.0f), o.y + Sc(22.0f)), b1(b0.x + ts0, b0.y + ts0);
            dl->AddRectFilled(ImVec2(b0.x - Sc(7.0f), b0.y - Sc(5.0f)), ImVec2(b1.x + Sc(7.0f), b1.y + Sc(9.0f)), Col(theme.Accent, 0.10f), Sc(22.0f));
            dl->AddRectFilled(ImVec2(b0.x - Sc(3.0f), b0.y - Sc(2.0f)), ImVec2(b1.x + Sc(3.0f), b1.y + Sc(4.0f)), Col(theme.Accent, 0.16f), Sc(18.0f));
            dl->AddRectFilled(b0, b1, Col(theme.Accent), Sc(14.0f));
            dl->AddRectFilled(b0, ImVec2(b1.x, b0.y + ts0 * 0.52f), IM_COL32(255, 255, 255, 46), Sc(14.0f), ImDrawFlags_RoundCornersTop);
            dl->AddRect(b0, b1, IM_COL32(255, 255, 255, 70), Sc(14.0f), 0, 1.0f);
            if (FontTitle) ImGui::PushFont(FontTitle);
            const ImVec2 ls = ImGui::CalcTextSize("A");
            dl->AddText(ImVec2(b0.x + (ts0 - ls.x) * 0.5f, b0.y + (ts0 - ls.y) * 0.5f - 1.0f), IM_COL32(255, 255, 255, 255), "A");
            if (FontTitle) ImGui::PopFont();
            if (FontBold) ImGui::PushFont(FontBold);
            dl->AddText(ImVec2(b1.x + Sc(14.0f), b0.y + Sc(2.0f)), Col(theme.TextPrimary), "Aether");
            if (FontBold) ImGui::PopFont();
            if (FontSmall) ImGui::PushFont(FontSmall);
            dl->AddText(ImVec2(b1.x + Sc(14.0f), b0.y + Sc(24.0f)), Col(theme.TextMuted), L10N("Control Center", "Kontrol Merkezi"));
            if (FontSmall) ImGui::PopFont();

            // feature search (pill)
            ImGui::SetCursorPosY(Sc(92.0f));
            {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.065f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.09f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme.TextPrimary);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Sc(15.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Sc(36.0f), Sc(9.0f)));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (m_focusSearch) { ImGui::SetKeyboardFocusHere(); m_focusSearch = false; }
                const bool entered = ImGui::InputTextWithHint("##featuresearch", L10N("Search settings  (Ctrl+F)", "Ayar ara  (Ctrl+F)"), m_query, sizeof(m_query), ImGuiInputTextFlags_EnterReturnsTrue);
                const bool boxActive = ImGui::IsItemActive();
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(3);
                if (boxActive) {
                    dl->AddRect(a, b, Col(theme.Accent, 0.85f), Sc(15.0f), 0, 1.5f);
                    dl->AddRect(ImVec2(a.x - 2.0f, a.y - 2.0f), ImVec2(b.x + 2.0f, b.y + 2.0f), Col(theme.Accent, 0.22f), Sc(17.0f), 0, 2.0f);
                }
                if (boxActive && ImGui::IsKeyPressed(ImGuiKey_Escape)) m_query[0] = '\0';
                if (FontIcons) {
                    ImGui::PushFont(FontIcons);
                    const std::string g = utf8(0xE721);
                    const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
                    dl->AddText(ImVec2(a.x + Sc(13.0f), a.y + (b.y - a.y - gs.y) * 0.5f), Col(boxActive ? theme.AccentHover : theme.TextMuted), g.c_str());
                    ImGui::PopFont();
                }
                if (m_query[0]) {
                    ImGui::SetCursorScreenPos(ImVec2(b.x - Sc(32.0f), a.y));
                    if (ImGui::InvisibleButton("##clearsearch", ImVec2(Sc(28.0f), b.y - a.y))) m_query[0] = '\0';
                    if (FontIcons) {
                        ImGui::PushFont(FontIcons);
                        const std::string g = utf8(0xE711);
                        const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
                        dl->AddText(ImVec2(b.x - Sc(32.0f) + (Sc(28.0f) - gs.x) * 0.5f, a.y + (b.y - a.y - gs.y) * 0.5f),
                                    Col(ImGui::IsItemHovered() ? theme.TextPrimary : theme.TextMuted), g.c_str());
                        ImGui::PopFont();
                    }
                }
                if (entered) m_enterPressed = true;
                m_searchBoxBottom = b.y;
            }
            ImGui::SetCursorPosY(Sc(92.0f) + Sc(38.0f) + Sc(6.0f));

            auto group = [&](const char* title) {
                ImGui::Dummy(ImVec2(0.0f, Sc(14.0f)));
                if (FontSmall) ImGui::PushFont(FontSmall);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float availW = ImGui::GetContentRegionAvail().x, padX = Sc(8.0f), fs = ImGui::GetFontSize();
                const float tw = ImGui::CalcTextSize(title).x;
                dl->AddText(ImVec2(p.x + padX, p.y), Col(theme.TextMuted, 0.80f), title);
                dl->AddRectFilledMultiColor(ImVec2(p.x + padX + tw + Sc(10.0f), p.y + fs * 0.5f), ImVec2(p.x + availW, p.y + fs * 0.5f + 1.0f),
                                            IM_COL32(255, 255, 255, 26), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 26));
                ImGui::Dummy(ImVec2(availW, fs + Sc(6.0f)));
                if (FontSmall) ImGui::PopFont();
            };
            auto nav = [&](SettingsTab tab, const char* id, const char* label, unsigned icon) {
                const std::string ic = utf8(icon);
                if (UI::SidebarItem(id, label, m_activeTab == tab, Sc(42.0f), ic.c_str())) m_activeTab = tab;
                ImGui::Dummy(ImVec2(0.0f, Sc(2.0f)));
            };
            group(L10N("GENERAL", "GENEL"));
            nav(SettingsTab::General,       "nav_gen",    L10N("General", "Genel"), 0xE713);
            nav(SettingsTab::Appearance,    "nav_app",    L10N("Appearance & Themes", "Görünüm ve Temalar"), 0xE790);
            group(L10N("COMPONENTS", "BİLEŞENLER"));
            nav(SettingsTab::DynamicIsland, "nav_island", "Dynamic Island", 0xE8B2);
            nav(SettingsTab::Widgets,       "nav_widg",   L10N("Widgets", "Widget'lar"), 0xE8A1);
            nav(SettingsTab::Desktop,       "nav_desk",   L10N("Desktop", "Masaüstü"), 0xE7F4);
            group(L10N("SYSTEM", "SİSTEM"));
            nav(SettingsTab::Media,            "nav_med",  L10N("Media", "Medya"), 0xE8D6);
            nav(SettingsTab::SystemMonitoring, "nav_sys",  L10N("Hardware Monitor", "Donanım İzleme"), 0xEEA1);
            nav(SettingsTab::GamingMode,       "nav_game", L10N("Gaming Mode", "Oyun Modu"), 0xE7FC);
            nav(SettingsTab::About,            "nav_abt",  L10N("About", "Hakkında"), 0xE946);

            // footer: status card (hidden on very short windows so it never overlaps the list)
            const float cardH = Sc(52.0f), fy = H - cardH - Sc(16.0f);
            if (ImGui::GetCursorPosY() + Sc(8.0f) < fy) {
                const ImVec2 cw0 = ImGui::GetWindowPos();
                const ImVec2 p0(cw0.x + Sc(16.0f), cw0.y + fy), p1(cw0.x + sbW - Sc(16.0f), cw0.y + fy + cardH);
                dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 13), Sc(15.0f));
                dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 20), Sc(15.0f), 0, 1.0f);
                const ImVec2 dot(p0.x + Sc(22.0f), (p0.y + p1.y) * 0.5f);
                dl->AddCircleFilled(dot, Sc(9.0f), Col(theme.Success, 0.16f));
                dl->AddCircleFilled(dot, Sc(4.5f), Col(theme.Success));
                if (FontSmall) ImGui::PushFont(FontSmall);
                const float fh = ImGui::GetFontSize();
                dl->AddText(ImVec2(p0.x + Sc(42.0f), (p0.y + p1.y) * 0.5f - fh - 1.0f), Col(theme.TextPrimary, 0.92f), L10N("Running", "Çalışıyor"));
                dl->AddText(ImVec2(p0.x + Sc(42.0f), (p0.y + p1.y) * 0.5f + 1.0f), Col(theme.TextMuted), L10N("Version 1.0", "Sürüm 1.0"));
                if (FontSmall) ImGui::PopFont();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // ---- HEADER + PAGE ----------------------------------------------------------------------------------------------
        const float cx0 = sbW, cw = W - sbW, headH = Sc(92.0f);
        struct Head { const char* title; const char* sub; };
        const Head heads[] = {
            {L10N("General", "Genel"), L10N("Startup, display and shortcuts", "Başlangıç, ekran ve kısayollar")},
            {"Dynamic Island", L10N("The floating pill at the top of the screen", "Ekranın üstündeki yüzen hap")},
            {L10N("Widgets", "Widget'lar"), L10N("Live cards on your desktop", "Masaüstündeki canlı kartlar")},
            {L10N("Taskbar", "Görev Çubuğu"), L10N("Material of the Windows taskbar", "Windows görev çubuğunun malzemesi")},
            {L10N("Desktop", "Masaüstü"), L10N("Wallpaper, icons and desktop items", "Duvar kağıdı, simgeler ve masaüstü öğeleri")},
            {L10N("Appearance & Themes", "Görünüm ve Temalar"), L10N("Pick the look of the whole shell", "Tüm arayüzün görünümünü seç")},
            {L10N("Motion", "Hareket"), L10N("Spring physics", "Yay fiziği")},
            {L10N("Media", "Medya"), L10N("Music and video playback", "Müzik ve video oynatma")},
            {L10N("Hardware Monitor", "Donanım İzleme"), L10N("Live CPU, memory, graphics and network", "Canlı CPU, bellek, ekran kartı ve ağ")},
            {L10N("Gaming Mode", "Oyun Modu"), L10N("Stay out of the way of games", "Oyunların yoluna çıkma")},
            {L10N("Shortcuts", "Kısayollar"), L10N("Global hotkeys", "Genel kısayol tuşları")},
            {L10N("Diagnostics", "Tanılama"), L10N("Rendering status", "Çizim durumu")},
            {L10N("About", "Hakkında"), L10N("About Aether", "Aether hakkında")},
        };
        static const unsigned kTabIcon[13] = {0xE713, 0xE8B2, 0xE8A1, 0xE700, 0xE7F4, 0xE790, 0xE916, 0xE8D6, 0xEEA1, 0xE7FC, 0xE765, 0xE9D9, 0xE946};
        const bool searching = m_query[0] != '\0';
        if (searching) {
            if (!m_indexBuilt || m_indexTurkish != aether::i18n::Turkish()) BuildSearchIndex();
            UpdateSearchHits();
            if (m_enterPressed && !m_hits.empty()) {                       // Enter in the search box opens the best match
                const UI::SearchEntry& top = m_index[static_cast<size_t>(m_hits.front())];
                m_activeTab = static_cast<SettingsTab>(top.tab);
                UI::FocusSetting(top.section, top.label);
                m_query[0] = '\0'; m_lastQuery.clear();
            }
        }
        m_enterPressed = false;
        char subBuf[96];
        snprintf(subBuf, sizeof(subBuf), L10N("%d results", "%d sonuç"), static_cast<int>(m_hits.size()));
        const Head searchHead{L10N("Search", "Arama"), subBuf};
        const bool showSearch = m_query[0] != '\0';
        const Head& h = showSearch ? searchHead : heads[static_cast<int>(m_activeTab)];
        const unsigned headIcon = showSearch ? 0xE721u : kTabIcon[static_cast<int>(m_activeTab)];

        // page title: accent tile with the page glyph, then title and subtitle
        const float tx = cx0 + Sc(36.0f);
        {
            const float tsz = Sc(46.0f);
            const ImVec2 a(w0.x + tx, w0.y + Sc(22.0f)), b(a.x + tsz, a.y + tsz);
            bg->AddRectFilled(ImVec2(a.x - Sc(4.0f), a.y - Sc(3.0f)), ImVec2(b.x + Sc(4.0f), b.y + Sc(6.0f)), Col(theme.Accent, 0.13f), Sc(18.0f));
            bg->AddRectFilled(a, b, Col(theme.Accent, 0.22f), Sc(14.0f));
            bg->AddRect(a, b, Col(theme.Accent, 0.55f), Sc(14.0f), 0, 1.0f);
            if (FontIcons) {
                const std::string g = utf8(headIcon);
                ImGui::PushFont(FontIcons);
                const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
                bg->AddText(ImVec2(a.x + (tsz - gs.x) * 0.5f, a.y + (tsz - gs.y) * 0.5f), Col(theme.AccentHover), g.c_str());
                ImGui::PopFont();
            }
            ImGui::SetCursorPos(ImVec2(tx + tsz + Sc(16.0f), Sc(17.0f)));
            if (FontTitle) ImGui::PushFont(FontTitle);
            ImGui::TextColored(theme.TextPrimary, "%s", h.title);
            if (FontTitle) ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(tx + tsz + Sc(17.0f), Sc(54.0f)));
            ImGui::TextColored(theme.TextMuted, "%s", h.sub);
        }
        {   // caption buttons: 0 = minimise, 1 = close
            const float bw = Sc(38.0f), bh = Sc(32.0f), by = Sc(20.0f);
            for (int k = 0; k < 2; ++k) {
                const float bx = W - Sc(18.0f) - (2 - k) * (bw + Sc(4.0f)) + Sc(4.0f);
                ImGui::SetCursorPos(ImVec2(bx, by));
                char id[16]; snprintf(id, sizeof(id), "##cap%d", k);
                const bool clicked = ImGui::InvisibleButton(id, ImVec2(bw, bh));
                const bool hov = ImGui::IsItemHovered();
                const float ha = AnimationEngine::Instance().AnimateFloat(ImGui::GetID(id) ^ 0x2F1Bu, hov ? 1.0f : 0.0f, AnimationSpeed::Fast);
                const ImVec2 a(w0.x + bx, w0.y + by), b(a.x + bw, a.y + bh);
                if (ha > 0.01f) bg->AddRectFilled(a, b, k == 1 ? IM_COL32(232, 72, 85, static_cast<int>(235 * ha)) : IM_COL32(255, 255, 255, static_cast<int>(26 * ha)), Sc(11.0f));
                if (FontIcons) {
                    const std::string g = utf8(k == 0 ? 0xE921 : 0xE8BB);
                    ImGui::PushFont(FontIcons);
                    const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
                    bg->AddText(ImVec2(a.x + (bw - gs.x) * 0.5f, a.y + (bh - gs.y) * 0.5f), k == 1 && hov ? IM_COL32(255, 255, 255, 255) : Col(theme.TextSecondary), g.c_str());
                    ImGui::PopFont();
                }
                if (clicked) { if (k == 0) ShowWindow(m_hwnd, SW_MINIMIZE); else Show(false); }
            }
        }
        bg->AddRectFilledMultiColor(ImVec2(w0.x + tx, w0.y + headH), ImVec2(w0.x + W - Sc(36.0f), w0.y + headH + 1.0f),
                                    IM_COL32(255, 255, 255, 38), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 38));

        // page transition: a new page fades in while rising a few pixels, and starts at the top
        static SettingsTab s_tab = m_activeTab;
        static bool s_search = false, s_first = true;
        static float s_t = 0.0f;
        bool resetScroll = false;
        if (s_first || s_tab != m_activeTab || s_search != showSearch) { s_first = false; s_tab = m_activeTab; s_search = showSearch; s_t = 0.0f; resetScroll = true; }
        s_t = std::min(1.0f, s_t + std::min(ImGui::GetIO().DeltaTime, 0.034f) / 0.30f);
        const float e = 1.0f - std::pow(1.0f - s_t, 3.0f);
        const float rise = (1.0f - e) * Sc(18.0f);

        ImGui::SetCursorPos(ImVec2(cx0, headH + Sc(2.0f) + rise));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(1, 1, 1, 0.13f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1, 1, 1, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, theme.Accent);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(36.0f), Sc(16.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Sc(7.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, Sc(4.0f));
        if (resetScroll) ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("PageScroll", ImVec2(cw, H - headH - Sc(2.0f) - rise), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        if (showSearch) RenderSearchResults(); else SettingsPages::Render(m_activeTab);
        ImGui::Dummy(ImVec2(0.0f, Sc(28.0f)));
        ImGui::EndChild();
        ImGui::PopStyleVar(5);
        ImGui::PopStyleColor(4);
        }

        // Overlay Toasts
        UI::RenderToasts();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGui::Render();
    const auto& bgc = Theme::Instance().Colors().Background;
    const float clearColor[4] = {bgc.x, bgc.y, bgc.z, 1.0f};
    m_pd3dDeviceContext->OMSetRenderTargets(1, m_mainRenderTargetView.GetAddressOf(), nullptr);
    m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    m_pSwapChain->Present(1, 0);
}

LRESULT SettingsWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam) return 0;   // the whole window is client area: we draw our own caption
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
            mm->ptMinTrackSize.x = static_cast<LONG>(900.0f * Scale);
            mm->ptMinTrackSize.y = static_cast<LONG>(600.0f * Scale);
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            RECT rc; GetClientRect(hwnd, &rc);
            const int e = static_cast<int>(6.0f * Scale);
            const bool L = pt.x < e, R = pt.x >= rc.right - e, T = pt.y < e, B = pt.y >= rc.bottom - e;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (pt.y < static_cast<int>(84.0f * Scale) && pt.x < rc.right - static_cast<int>(120.0f * Scale)) return HTCAPTION;   // drag by the header
            return HTCLIENT;
        }
        case WM_SIZE: {
            if (m_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                m_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        }
        case WM_CLOSE: {
            Show(false);
            return 0;
        }
        case WM_DESTROY: {
            m_hwnd = nullptr;
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace aether::ui
