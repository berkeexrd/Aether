#include "aether/ui/theme.hpp"
#include "aether/render/glass.hpp"
#include <algorithm>

namespace aether::ui {

Theme::Theme() {
    Apply();
}

void Theme::SetAccentColor(uint32_t hexRgba) {
    float r = static_cast<float>((hexRgba >> 16) & 0xFF) / 255.0f;
    float g = static_cast<float>((hexRgba >> 8) & 0xFF) / 255.0f;
    float b = static_cast<float>(hexRgba & 0xFF) / 255.0f;
    float a = static_cast<float>((hexRgba >> 24) & 0xFF) / 255.0f;
    if (a <= 0.0f) a = 1.0f;

    m_colors.Accent = ImVec4(r, g, b, a);
    m_colors.AccentHover = ImVec4(
        std::min(r * 1.15f, 1.0f),
        std::min(g * 1.15f, 1.0f),
        std::min(b * 1.15f, 1.0f),
        a
    );
    m_colors.AccentActive = ImVec4(r * 0.85f, g * 0.85f, b * 0.85f, a);
    m_colors.AccentMuted = ImVec4(r, g, b, 0.20f);
    m_colors.AccentGlow = ImVec4(r, g, b, 0.35f);
    m_colors.BorderFocus = m_colors.Accent;

    Apply();
}

void Theme::UseGlassTheme(int index) {
    render::glass::SetTheme(index);
    m_themeIndex = render::glass::ThemeIndex();
    const auto& t = render::glass::CurrentTheme();
    const float ar = t.accent.r, ag = t.accent.g, ab = t.accent.b;
    const float lr = t.light.r, lg = t.light.g, lb = t.light.b;
    const bool oled = index == 1;

    // Deep obsidian-glass palette matching AetherDesktop's Customize ("Özelleştir") window:
    // clean dark navy-obsidian base so the top accent wash and crisp hairline rims breathe without muddy purple cards.
    const float baseR = oled ? 0.018f : (0.036f + ar * 0.018f);
    const float baseG = oled ? 0.018f : (0.032f + ag * 0.016f);
    const float baseB = oled ? 0.024f : (0.072f + ab * 0.024f);
    auto lift = [&](float add, float accMix, float alpha = 1.0f) {
        return ImVec4(std::min(baseR + add + ar * accMix, 1.0f),
                      std::min(baseG + add + ag * accMix, 1.0f),
                      std::min(baseB + add + ab * accMix, 1.0f), alpha);
    };

    m_colors.Background      = ImVec4(baseR, baseG, baseB, 1.0f);
    m_colors.Shell           = lift(0.008f, 0.004f);
    m_colors.Surface         = lift(0.032f, 0.008f);
    m_colors.SurfaceElevated = lift(0.056f, 0.012f);
    m_colors.SurfaceHover    = lift(0.092f, 0.020f);
    m_colors.SurfaceActive   = lift(0.125f, 0.030f);
    m_colors.SurfaceOverlay  = ImVec4(baseR + 0.016f, baseG + 0.014f, baseB + 0.024f, 0.97f);
    m_colors.Border          = ImVec4(1.0f, 1.0f, 1.0f, 0.13f);
    m_colors.BorderSubtle    = ImVec4(1.0f, 1.0f, 1.0f, 0.075f);
    m_colors.BorderHover     = ImVec4(lr, lg, lb, 0.55f);
    m_colors.TextPrimary     = ImVec4(0.955f, 0.955f, 0.975f, 1.0f);
    m_colors.TextSecondary   = ImVec4(0.72f, 0.74f, 0.81f, 1.0f);
    m_colors.TextMuted       = ImVec4(0.52f, 0.55f, 0.64f, 1.0f);
    m_colors.TextDisabled    = ImVec4(0.34f, 0.36f, 0.44f, 1.0f);

    m_colors.Accent          = ImVec4(ar, ag, ab, 1.0f);
    m_colors.AccentHover     = ImVec4(lr, lg, lb, 1.0f);
    m_colors.AccentActive    = ImVec4(ar * 0.82f, ag * 0.82f, ab * 0.82f, 1.0f);
    m_colors.AccentMuted     = ImVec4(ar, ag, ab, 0.28f);
    m_colors.AccentGlow      = ImVec4(lr, lg, lb, 0.42f);
    m_colors.BorderFocus     = ImVec4(lr, lg, lb, 0.85f);
    Apply();
}

void Theme::Apply() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Sizing & Spacing
    style.WindowPadding     = ImVec2(Spacing::LG, Spacing::LG);
    style.FramePadding      = ImVec2(Spacing::MD, Spacing::SM);
    style.CellPadding       = ImVec2(Spacing::SM, Spacing::SM);
    style.ItemSpacing       = ImVec2(Spacing::MD, Spacing::MD);
    style.ItemInnerSpacing  = ImVec2(Spacing::SM, Spacing::SM);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing     = Spacing::XL;
    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 12.0f;

    // Radius hierarchy: Window > Popup/Card > Control > Badge
    style.WindowRounding    = Radius::LG;
    style.ChildRounding     = 14.0f;
    style.FrameRounding     = 8.0f;
    style.PopupRounding     = Radius::MD;
    style.ScrollbarRounding = Radius::SM;
    style.GrabRounding      = Radius::XS;
    style.TabRounding       = Radius::SM;

    // Borders
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;

    // Colors mapping to tokens
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = m_colors.TextPrimary;
    colors[ImGuiCol_TextDisabled]          = m_colors.TextDisabled;
    colors[ImGuiCol_WindowBg]              = m_colors.Background;
    colors[ImGuiCol_ChildBg]               = m_colors.Surface;
    colors[ImGuiCol_PopupBg]               = m_colors.SurfaceOverlay;
    colors[ImGuiCol_Border]                = m_colors.Border;
    colors[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg]               = m_colors.SurfaceElevated;
    colors[ImGuiCol_FrameBgHovered]        = m_colors.SurfaceHover;
    colors[ImGuiCol_FrameBgActive]         = m_colors.SurfaceActive;
    colors[ImGuiCol_TitleBg]               = m_colors.Shell;
    colors[ImGuiCol_TitleBgActive]         = m_colors.Shell;
    colors[ImGuiCol_TitleBgCollapsed]      = m_colors.Shell;
    colors[ImGuiCol_MenuBarBg]             = m_colors.Shell;
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab]         = m_colors.SurfaceHover;
    colors[ImGuiCol_ScrollbarGrabHovered]  = m_colors.SurfaceActive;
    colors[ImGuiCol_ScrollbarGrabActive]   = m_colors.Accent;
    colors[ImGuiCol_CheckMark]             = m_colors.Accent;
    colors[ImGuiCol_SliderGrab]            = m_colors.Accent;
    colors[ImGuiCol_SliderGrabActive]      = m_colors.AccentHover;
    colors[ImGuiCol_Button]                = m_colors.SurfaceElevated;
    colors[ImGuiCol_ButtonHovered]         = m_colors.SurfaceHover;
    colors[ImGuiCol_ButtonActive]          = m_colors.SurfaceActive;
    colors[ImGuiCol_Header]                = m_colors.SurfaceElevated;
    colors[ImGuiCol_HeaderHovered]         = m_colors.SurfaceHover;
    colors[ImGuiCol_HeaderActive]          = m_colors.SurfaceActive;
    colors[ImGuiCol_Separator]             = m_colors.BorderSubtle;
    colors[ImGuiCol_SeparatorHovered]      = m_colors.BorderHover;
    colors[ImGuiCol_SeparatorActive]       = m_colors.Accent;
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGripHovered]     = m_colors.AccentMuted;
    colors[ImGuiCol_ResizeGripActive]      = m_colors.Accent;
    colors[ImGuiCol_Tab]                   = m_colors.SurfaceElevated;
    colors[ImGuiCol_TabHovered]            = m_colors.SurfaceHover;
    colors[ImGuiCol_TabActive]             = m_colors.SurfaceActive;
    colors[ImGuiCol_TabUnfocused]          = m_colors.SurfaceElevated;
    colors[ImGuiCol_TabUnfocusedActive]    = m_colors.SurfaceActive;
    colors[ImGuiCol_ModalWindowDimBg]      = m_colors.BackdropDim;
    style.ScaleAllSizes(Scale);
}

} // namespace aether::ui
