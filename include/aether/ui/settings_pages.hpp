#pragma once

#include "aether/config/config_manager.hpp"
#include "aether/ui/components.hpp"
#include <vector>

namespace aether::ui {

enum class SettingsTab {
    General,
    DynamicIsland,
    Widgets,
    Taskbar,
    Desktop,
    Appearance,
    Animations,
    Media,
    SystemMonitoring,
    GamingMode,
    Startup,
    Performance,
    About
};

class SettingsPages {
public:
    static void Render(SettingsTab activeTab);
    // Runs a page without drawing it and records its sections / rows (used by the feature search).
    static void CollectSearchIndex(SettingsTab tab, std::vector<UI::SearchEntry>& out);
    // First-run welcome / setup page (language, theme, autostart, wallpaper). Replaces the normal layout until finished.
    static bool WelcomeNeeded();
    static void RenderWelcome(float width, float height);

private:
    static void RenderGeneral();
    static void RenderDynamicIsland();
    static void RenderWidgets();
    static void RenderTaskbar();
    static void RenderDesktop();
    static void RenderAppearance();
    static void RenderAnimations();
    static void RenderMedia();
    static void RenderSystemMonitoring();
    static void RenderGamingMode();
    static void RenderStartup();
    static void RenderPerformance();
    static void RenderAbout();
};

} // namespace aether::ui
