#include "aether/ui/settings_pages.hpp"
#include "aether/ui/components.hpp"
#include "aether/ui/animation.hpp"
#include "aether/ui/theme.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include "aether/system/metrics_collector.hpp"
#include "aether/system/gaming_mode.hpp"
#include "aether/system/taskbar_service.hpp"
#include "aether/media/media_service.hpp"
#include "aether/system/autostart.hpp"
#include "aether/system/wallpaper.hpp"
#include "aether/system/icon_pack.hpp"
#include "aether/system/desktop_cfg.hpp"
#include "aether/system/notify_cfg.hpp"
#include "aether/ui/settings_window.hpp"
#include "aether/widgets/widget_window.hpp"
#include <imgui.h>
#include <windows.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cfloat>
#include <algorithm>
#include <cstdio>

#define L10N(en, tr) (aether::i18n::Turkish() ? (tr) : (en))

namespace aether::ui {

namespace {
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void Save() { config::ConfigManager::Instance().Save(); }
ImU32 U32(const core::Color& c, float a = -1.0f) { return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, a < 0 ? c.a : a)); }

// A small coloured bar with a label and a percentage - used by the live telemetry page.
void MeterRow(const char* label, const char* sub, float pct, const char* valueText, const ImVec4& color) {
    const auto& theme = Theme::Instance().Colors();
    UI::SettingRow(label, sub, [&](float w) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bw = w - Sc(70.0f);
        dl->AddRectFilled(ImVec2(p.x, p.y + Sc(11.0f)), ImVec2(p.x + bw, p.y + Sc(19.0f)), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)), Sc(4.0f));
        dl->AddRectFilled(ImVec2(p.x, p.y + Sc(11.0f)), ImVec2(p.x + std::max(Sc(8.0f), bw * std::clamp(pct / 100.0f, 0.0f, 1.0f)), p.y + Sc(19.0f)), ImGui::GetColorU32(color), Sc(4.0f));
        dl->AddText(ImVec2(p.x + bw + Sc(10.0f), p.y + Sc(5.0f)), ImGui::GetColorU32(theme.TextPrimary), valueText);
        ImGui::Dummy(ImVec2(w, Sc(30.0f)));
    });
}
} // namespace

void SettingsPages::Render(SettingsTab activeTab) {
    switch (activeTab) {
        case SettingsTab::General:          RenderGeneral(); break;
        case SettingsTab::DynamicIsland:    RenderDynamicIsland(); break;
        case SettingsTab::Widgets:          RenderWidgets(); break;
        case SettingsTab::Taskbar:          RenderTaskbar(); break;
        case SettingsTab::Desktop:          RenderDesktop(); break;
        case SettingsTab::Appearance:       RenderAppearance(); break;
        case SettingsTab::Animations:       RenderAnimations(); break;
        case SettingsTab::Media:            RenderMedia(); break;
        case SettingsTab::SystemMonitoring: RenderSystemMonitoring(); break;
        case SettingsTab::GamingMode:       RenderGamingMode(); break;
        case SettingsTab::Startup:          RenderStartup(); break;
        case SettingsTab::Performance:      RenderPerformance(); break;
        case SettingsTab::About:            RenderAbout(); break;
    }
}

void SettingsPages::CollectSearchIndex(SettingsTab tab, std::vector<UI::SearchEntry>& out) {
    UI::BeginSearchCapture(&out, static_cast<int>(tab));
    Render(tab);
    UI::EndSearchCapture();
}

bool SettingsPages::WelcomeNeeded() { return !config::ConfigManager::Instance().Get().firstRunDone; }

void SettingsPages::RenderWelcome(float W, float H) {
    auto& root = config::ConfigManager::Instance().GetMutable();
    const auto& theme = Theme::Instance().Colors();
    static bool s_init = false, s_auto = true, s_wall = true;
    if (!s_init) { s_init = true; s_auto = true; s_wall = root.autoWallpaper; }

    const float cw = std::min(Sc(680.0f), W - Sc(48.0f)), ch = std::min(Sc(640.0f), H - Sc(48.0f));
    ImGui::SetCursorPos(ImVec2((W - cw) * 0.5f, std::max(Sc(16.0f), (H - ch) * 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Background);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderSubtle);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Sc(24.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(34.0f), Sc(26.0f)));
    ImGui::BeginChild("Welcome", ImVec2(cw, ch), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    {
        if (FontTitle) ImGui::PushFont(FontTitle);
        ImGui::TextColored(theme.TextPrimary, "%s", L10N("Welcome to Aether", "Aether'e hoş geldin"));
        if (FontTitle) ImGui::PopFont();
        ImGui::TextColored(theme.TextMuted, "%s", L10N("A few quick choices to make it yours. You can change all of them later.", "Sana göre ayarlamak için birkaç hızlı seçim. Hepsini sonra değiştirebilirsin."));
        ImGui::Dummy(ImVec2(0, Sc(14.0f)));

        const float inner = ImGui::GetContentRegionAvail().x;
        auto label = [&](const char* s) { ImGui::TextColored(theme.TextSecondary, "%s", s); ImGui::Dummy(ImVec2(0, Sc(4.0f))); };

        // language
        label(L10N("Language", "Dil"));
        {
            const char* names[3] = {L10N("Follow Windows", "Windows'u izle"), "English", "T\xC3\xBCrk\xC3\xA7" "e"};
            const float gap = Sc(10.0f), bw = (inner - gap * 2.0f) / 3.0f;
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine(0.0f, gap);
                ImGui::PushID(i);
                if (UI::Button(names[i], ImVec2(bw, Sc(38.0f)), root.language == i)) { root.language = i; aether::i18n::SaveLanguage(i); Save(); }
                ImGui::PopID();
            }
        }
        ImGui::Dummy(ImVec2(0, Sc(16.0f)));

        // theme
        label(L10N("Theme", "Tema"));
        {
            int count = 0;
            const render::glass::ThemeDef* themes = render::glass::Themes(&count);
            const int pick[4] = {6, 0, 7, 1};   // Classic (default), Violet, Lavender, OLED Black
            const float gap = Sc(10.0f), tw = (inner - gap * 3.0f) / 4.0f, th = Sc(92.0f);
            const ImVec2 o = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int k = 0; k < 4; ++k) {
                const int i = pick[k];
                if (i >= count) continue;
                const auto& t = themes[i];
                const ImVec2 p0(o.x + k * (tw + gap), o.y), p1(p0.x + tw, p0.y + th);
                ImGui::SetCursorScreenPos(p0);
                char id[16]; snprintf(id, sizeof(id), "##wt%d", i);
                const bool clicked = ImGui::InvisibleButton(id, ImVec2(tw, th));
                const bool hov = ImGui::IsItemHovered();
                dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0.03f, 0.03f, 0.04f, 1.0f)), Sc(12.0f));
                const ImVec2 b0(p0.x + Sc(10.0f), p0.y + Sc(10.0f)), b1(p1.x - Sc(10.0f), p1.y - Sc(30.0f));
                dl->AddRectFilledMultiColor(b0, b1, U32(t.bodyTop, 1.0f), U32(t.bodyTop, 1.0f), U32(t.bodyBot, 1.0f), U32(t.bodyBot, 1.0f));
                dl->AddRect(b0, b1, U32(t.light, 0.55f), Sc(8.0f), 0, 1.2f);
                dl->AddRectFilled(ImVec2(b0.x + 8, b0.y + 9), ImVec2(b0.x + 30, b0.y + 14), U32(t.light, 0.9f), Sc(3.0f));
                dl->AddRectFilled(ImVec2(b0.x + 8, b0.y + 22), ImVec2(b0.x + (b1.x - b0.x) * 0.65f, b0.y + 27), U32(t.accent, 1.0f), Sc(3.0f));
                dl->AddText(ImVec2(p0.x + Sc(10.0f), p1.y - Sc(24.0f)), ImGui::GetColorU32(theme.TextPrimary), WideToUtf8(L10N(t.nameEn, t.nameTr)).c_str());
                if (render::glass::ThemeIndex() == i) dl->AddRect(p0, p1, U32(t.accent, 1.0f), Sc(12.0f), 0, Sc(2.0f));
                else if (hov) dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.30f)), Sc(12.0f), 0, 1.5f);
                if (clicked) { Theme::Instance().UseGlassTheme(i); render::glass::SaveThemeFile(i); }
            }
            ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + th));
            ImGui::Dummy(ImVec2(1.0f, Sc(14.0f)));
        }

        UI::SettingRow(L10N("Launch with Windows", "Windows ile başlat"), L10N("Aether and the bar start when you sign in (one admin prompt)", "Aether ve bar oturum açınca başlar (bir kez yönetici izni ister)"), [&](float) { UI::Toggle("##wauto", &s_auto); });
        UI::SettingRow(L10N("Match wallpaper to theme", "Duvar kağıdını temaya uydur"), L10N("Purple artwork for purple themes, black for dark ones", "Mor temalara mor, koyu temalara siyah duvar kağıdı"), [&](float) { UI::Toggle("##wwall", &s_wall); });

        ImGui::Dummy(ImVec2(0, Sc(14.0f)));
        if (UI::Button(L10N("Get started", "Başla"), ImVec2(inner, Sc(46.0f)), true)) {
            root.launchOnStartup = s_auto;
            root.autoWallpaper = s_wall;
            root.firstRunDone = true;
            if (s_auto) aether::system::autostart::Apply(true);
            if (s_wall) aether::system::wallpaper::ApplyForTheme(render::glass::ThemeIndex());
            Save();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void SettingsPages::RenderGeneral() {
    auto& cfg = config::ConfigManager::Instance().GetMutable();

    UI::Section(L10N("Application", "Uygulama"), L10N("Startup, tray and display preferences", "Başlangıç, tepsi ve ekran tercihleri"), [&]() {
        UI::SettingRow(L10N("Launch with Windows", "Windows ile başlat"), L10N("Start Aether automatically when you sign in", "Oturum açınca Aether otomatik başlasın"), [&](float) {
            if (UI::Toggle("##launchOnStartup", &cfg.launchOnStartup)) {
                Save();
                aether::system::autostart::Apply(cfg.launchOnStartup);
                UI::AddToast(UI::ToastType::Success, L10N("Startup updated", "Başlangıç güncellendi"), L10N("The startup setting was saved.", "Başlangıç ayarı kaydedildi."));
            }
        });
        UI::SettingRow(L10N("Keep running in the tray", "Tepside çalışmaya devam et"), L10N("Closing this window does not quit Aether", "Bu pencereyi kapatmak Aether'i kapatmaz"), [&](float) {
            if (UI::Toggle("##minimizeToTray", &cfg.minimizeToTray)) Save();
        });
        UI::SettingRow(L10N("Language", "Dil"), L10N("Language of Aether's own interface", "Aether arayüzünün dili"), [&](float w) {
            const char* langs[] = {L10N("Follow Windows", "Windows'u izle"), "English", "T\xC3\xBCrk\xC3\xA7" "e"};
            if (UI::Combo("##language", &cfg.language, langs, 3, w)) { aether::i18n::SaveLanguage(cfg.language); Save(); }
        });
        UI::SettingRow(L10N("Target display", "Hedef ekran"), L10N("Display that holds the island and the widgets", "Island ve widget'ların bulunduğu ekran"), [&](float w) {
            const char* monitors[] = {L10N("Primary display", "Ana ekran"), L10N("Display 2", "Ekran 2"), L10N("Display 3", "Ekran 3")};
            if (UI::Combo("##monitorIndex", &cfg.monitorIndex, monitors, 3, w)) Save();
        });
        UI::SettingRow(L10N("Interface scale", "Arayüz ölçeği"), L10N("Scales every Aether surface", "Tüm Aether yüzeylerini ölçekler"), [&](float) {
            if (UI::SliderFloat("##uiScale", &cfg.uiScale, 0.8f, 2.0f, "%.2fx")) Save();
        });
    });

    UI::Section(L10N("Shortcuts", "Kısayollar"), L10N("Global hotkeys", "Genel kısayol tuşları"), [&]() {
        static int s_toggleKey = ImGuiKey_F9;
        static bool s_capturing = false;
        UI::SettingRow(L10N("Toggle the island", "Island'ı aç / kapat"), L10N("Click, then press the key you want", "Tıkla, ardından istediğin tuşa bas"), [&](float) {
            UI::Keybind("toggle_island_hotkey", &s_toggleKey, &s_capturing);
        });
    });
}

void SettingsPages::RenderDynamicIsland() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().island;

    UI::Section(L10N("Island", "Island"), L10N("Position and size of the floating pill", "Yüzen hapın konumu ve boyutu"), [&]() {
        UI::SettingRow(L10N("Show the island", "Island'ı göster"), L10N("The floating pill at the top of the screen", "Ekranın üstündeki yüzen hap"), [&](float) { if (UI::Toggle("##islandEnabled", &cfg.enabled)) Save(); });
        UI::SettingRow(L10N("Horizontal position", "Yatay konum"), L10N("Where the island sits along the top edge: left, centre or right", "Island'ın üst kenar boyunca durduğu yer: sol, orta veya sağ"), [&](float) { if (UI::SliderFloat("##posX", &cfg.posX, 0.0f, 100.0f, "%.0f %%")) Save(); });
        UI::SettingRow(L10N("Vertical position", "Dikey konum"), L10N("Low values keep the island at the top edge; higher values move it down the screen", "Düşük değerler island'ı üst kenarda tutar; yükseldikçe ekranda aşağı iner"), [&](float) { if (UI::SliderFloat("##posY", &cfg.posY, 0.0f, 100.0f, "%.0f %%")) Save(); });
        UI::SettingRow(L10N("Position preset", "Konum ön ayarı"), L10N("Jump to the left, the centre or the right of the screen", "Ekranın soluna, ortasına veya sağına git"), [&](float w) {
            const char* presets[3] = {L10N("Left", "Sol"), L10N("Centre", "Orta"), L10N("Right", "Sağ")};
            int cur = cfg.posX < 30.0f ? 0 : (cfg.posX > 70.0f ? 2 : 1);
            if (UI::Combo("##posPreset", &cur, presets, 3, w)) { cfg.posX = cur == 0 ? 8.0f : (cur == 2 ? 92.0f : 50.0f); Save(); }
        });
        UI::SettingRow(L10N("Top margin", "Üst boşluk"), L10N("Distance from the top edge of the display", "Ekranın üst kenarına uzaklık"), [&](float) { if (UI::SliderFloat("##topOffset", &cfg.topOffset, 4.0f, 60.0f, "%.0f px")) Save(); });
        UI::SettingRow(L10N("Resting width", "Dinlenme genişliği"), L10N("Width of the compact pill", "Kompakt hapın genişliği"), [&](float) { if (UI::SliderFloat("##idleWidth", &cfg.idleWidth, 100.0f, 260.0f, "%.0f px")) Save(); });
        UI::SettingRow(L10N("Resting height", "Dinlenme yüksekliği"), L10N("Height of the compact pill", "Kompakt hapın yüksekliği"), [&](float) { if (UI::SliderFloat("##idleHeight", &cfg.idleHeight, 28.0f, 48.0f, "%.0f px")) Save(); });
    });
    // Aether's own notification pop-ups (drawn by the bar); the choices live in notify.cfg, which the bar watches
    {
        namespace nc = aether::system::notifycfg;
        static nc::Settings ns; static unsigned long long nsStamp = ~0ull;
        const unsigned long long stamp = nc::Stamp();
        if (stamp != nsStamp) { ns = nc::Load(); nsStamp = stamp; }
        UI::Section(L10N("Notifications", "Bildirimler"), L10N("Aether shows your Windows notifications in its own style", "Aether, Windows bildirimlerini kendi tarzında gösterir"), [&]() {
            UI::SettingRow(L10N("Aether notifications", "Aether bildirimleri"), L10N("Show notifications with Aether and hide the Windows pop-ups", "Bildirimleri Aether ile göster, Windows'un kendi pencerelerini gizle"), [&](float) {
                if (UI::Toggle("##ntEnabled", &ns.enabled)) { nc::SetInt(L"enabled", ns.enabled ? 1 : 0); nsStamp = nc::Stamp(); }
            });
            UI::SettingRow(L10N("Do not disturb", "Rahatsız etme"), L10N("Nothing pops up; the notification centre still collects everything", "Hiçbir şey açılmaz; bildirim merkezi hepsini toplamaya devam eder"), [&](float) {
                if (UI::Toggle("##ntDnd", &ns.dnd)) { nc::SetInt(L"dnd", ns.dnd ? 1 : 0); nsStamp = nc::Stamp(); }
            });
            UI::SettingRow(L10N("Position", "Konum"), L10N("Screen corner the notifications slide in from", "Bildirimlerin kaydığı ekran köşesi"), [&](float w) {
                const char* pos[] = {L10N("Top right", "Sağ üst"), L10N("Bottom right", "Sağ alt")};
                if (UI::Combo("##ntPos", &ns.position, pos, 2, w)) { nc::SetInt(L"pos", ns.position); nsStamp = nc::Stamp(); }
            });
            UI::SettingRow(L10N("Duration", "Süre"), L10N("How long a notification stays on screen", "Bildirimin ekranda kalma süresi"), [&](float) {
                if (UI::SliderInt("##ntSecs", &ns.seconds, 3, 20)) { nc::SetInt(L"secs", ns.seconds); nsStamp = nc::Stamp(); }
            });
            UI::SettingRow(L10N("Also in the island", "Island'da da göster"), L10N("The Dynamic Island announces them as well", "Dynamic Island de bildirimleri duyursun"), [&](float) {
                if (UI::Toggle("##ntIsland", &ns.island)) { nc::SetInt(L"island", ns.island ? 1 : 0); nsStamp = nc::Stamp(); }
            });
            UI::SettingRow(L10N("Try it", "Dene"), L10N("Sends a test notification", "Bir deneme bildirimi gönderir"), [&](float) {
                if (UI::Button(L10N("Send a test", "Deneme gönder"), ImVec2(Sc(150.0f), Sc(34.0f)), true)) { nc::SetInt(L"test", static_cast<long long>(GetTickCount64())); nsStamp = nc::Stamp(); }
            });
        });
    }


    UI::Section(L10N("Behaviour", "Davranış"), L10N("How the island opens and what it shows", "Island nasıl açılır ve neyi gösterir"), [&]() {
        UI::SettingRow(L10N("Expand on hover", "Üzerine gelince genişlet"), L10N("Show a preview when the pointer rests on it", "İmleç üzerinde durunca önizleme göster"), [&](float) { if (UI::Toggle("##expandOnHover", &cfg.expandOnHover)) Save(); });
        UI::SettingRow(L10N("Hover delay", "Bekleme süresi"), L10N("Delay before the preview opens", "Önizlemenin açılmasından önceki gecikme"), [&](float) { if (UI::SliderFloat("##hoverDelayMs", &cfg.hoverDelayMs, 0.0f, 400.0f, "%.0f ms")) Save(); });
        UI::SettingRow(L10N("Pin on click", "Tıklayınca sabitle"), L10N("Stay open until you click again", "Tekrar tıklayana kadar açık kalsın"), [&](float) { if (UI::Toggle("##pinOnClick", &cfg.pinOnClick)) Save(); });
        UI::SettingRow(L10N("Clock in the pill", "Hapta saat"), L10N("Show the time in the resting pill", "Dinlenme halinde saati göster"), [&](float) { if (UI::Toggle("##showClockIdle", &cfg.showClockIdle)) Save(); });
        UI::SettingRow(L10N("Audio visualizer", "Ses görselleştirici"), L10N("Animated bars while music plays", "Müzik çalarken hareketli çubuklar"), [&](float) { if (UI::Toggle("##showAudioVisualizer", &cfg.showAudioVisualizer)) Save(); });
        UI::SettingRow(L10N("Hide from screenshots", "Ekran görüntülerinde gizle"), L10N("The island will not appear in captures or recordings", "Island ekran görüntülerinde ve kayıtlarda görünmez"), [&](float) { if (UI::Toggle("##hideFromScreenshots", &cfg.hideFromScreenshots)) Save(); });
    });
}

void SettingsPages::RenderWidgets() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().widgets;

    UI::Section(L10N("Desktop widgets", "Masaüstü widget'ları"), L10N("Cards that live on your desktop", "Masaüstünde duran kartlar"), [&]() {
        UI::SettingRow(L10N("Show widgets", "Widget'ları göster"), L10N("Turns every desktop widget on or off", "Tüm masaüstü widget'larını açar veya kapatır"), [&](float) { if (UI::Toggle("##widgetsEnabled", &cfg.enabled)) Save(); });
        UI::SettingRow(L10N("Snap to grid", "Izgaraya yasla"), L10N("Align widgets while dragging", "Sürüklerken widget'ları hizala"), [&](float) { if (UI::Toggle("##snapToGrid", &cfg.snapToGrid)) Save(); });
        UI::SettingRow(L10N("Grid size", "Izgara boyutu"), L10N("Spacing of the snap grid", "Hizalama ızgarasının aralığı"), [&](float) { if (UI::SliderFloat("##gridSize", &cfg.gridSize, 8.0f, 32.0f, "%.0f px")) Save(); });
    });

    UI::Section(L10N("Size", "Boyut"), L10N("Or drag the corner of a card on the desktop (Ctrl + wheel also works)", "Veya masaüstünde kartın köşesinden sürükle (Ctrl + tekerlek de çalışır)"), [&]() {
        struct Row { const char* label; const char* id; config::DesktopWidgetConfig* c; };
        const Row rows[] = {
            {L10N("System performance", "Sistem performansı"), "##sizeSys", &cfg.sysMonWidget},
            {L10N("Network activity", "Ağ etkinliği"), "##sizeNet", &cfg.netWidget},
            {L10N("Storage", "Depolama"), "##sizeSto", &cfg.storageWidget},
            {L10N("Weather", "Hava durumu"), "##sizeWea", &cfg.weatherWidget},
            {L10N("Now playing", "Çalan müzik"), "##sizeNow", &cfg.nowWidget},
            {L10N("Clock and date", "Saat ve tarih"), "##sizeClk", &cfg.clockWidget},
        };
        for (const Row& r : rows) {
            UI::SettingRow(r.label, nullptr, [&](float) {
                float pct = r.c->scale * 100.0f;
                if (UI::SliderFloat(r.id, &pct, 60.0f, 200.0f, "%.0f %%")) { r.c->scale = std::clamp(pct / 100.0f, 0.6f, 2.0f); widgets::WidgetManager::Instance().SyncSizes(); Save(); }
            });
        }
        UI::SettingRow(L10N("Reset all sizes", "Tüm boyutları sıfırla"), L10N("Back to 100 %", "%100'e döner"), [&](float) {
            if (UI::Button(L10N("Reset", "Sıfırla"), ImVec2(Sc(110.0f), Sc(32.0f)))) {
                for (const Row& r : rows) r.c->scale = 1.0f;
                widgets::WidgetManager::Instance().SyncSizes(); Save();
            }
        });
    });

    UI::Section(L10N("Cards", "Kartlar"), L10N("Choose which widgets are shown", "Hangi widget'ların görüneceğini seç"), [&]() {
        UI::SettingRow(L10N("System performance", "Sistem performansı"), L10N("CPU, RAM and GPU rings", "CPU, RAM ve GPU halkaları"), [&](float) { if (UI::Toggle("##sysMonEnabled", &cfg.sysMonWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Network activity", "Ağ etkinliği"), L10N("Live download and upload speed", "Anlık indirme ve yükleme hızı"), [&](float) { if (UI::Toggle("##netEnabled", &cfg.netWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Storage", "Depolama"), L10N("Used space of every drive", "Tüm sürücülerin kullanımı"), [&](float) { if (UI::Toggle("##storageEnabled", &cfg.storageWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Weather", "Hava durumu"), L10N("Current weather and the next hours", "Anlık hava ve sonraki saatler"), [&](float) { if (UI::Toggle("##weatherEnabled", &cfg.weatherWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Now playing", "Çalan müzik"), L10N("Track, artist and playback controls", "Parça, sanatçı ve oynatma düğmeleri"), [&](float) { if (UI::Toggle("##nowEnabled", &cfg.nowWidget.enabled)) Save(); });
        UI::SettingRow(L10N("Clock and date", "Saat ve tarih"), L10N("Large clock with the long date", "Büyük saat ve uzun tarih"), [&](float) { if (UI::Toggle("##clockEnabled", &cfg.clockWidget.enabled)) Save(); });
    });
}

void SettingsPages::RenderTaskbar() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().taskbar;

    UI::Section(L10N("Windows taskbar", "Windows görev çubuğu"), L10N("Material applied to the stock taskbar", "Standart görev çubuğuna uygulanan malzeme"), [&]() {
        UI::SettingRow(L10N("Customise the taskbar", "Görev çubuğunu özelleştir"), L10N("Apply a modern material to the Windows taskbar", "Windows görev çubuğuna modern bir malzeme uygula"), [&](float) {
            if (UI::Toggle("##taskbarEnabled", &cfg.enabled)) {
                if (cfg.enabled) system::TaskbarService::Instance().ApplyStyle(cfg.style, cfg.tintColor);
                else system::TaskbarService::Instance().RestoreDefaults();
                Save();
            }
        });
        UI::SettingRow(L10N("Material", "Malzeme"), L10N("Composition effect on the taskbar", "Görev çubuğundaki kompozisyon efekti"), [&](float w) {
            const char* styles[] = {L10N("Default", "Varsayılan"), L10N("Clear", "Şeffaf"), L10N("Blur", "Bulanık"), L10N("Acrylic", "Akrilik"), L10N("Tinted", "Renkli")};
            int cur = static_cast<int>(cfg.style);
            if (UI::Combo("##tbStyle", &cur, styles, 5, w)) {
                cfg.style = static_cast<config::TaskbarStyle>(cur);
                system::TaskbarService::Instance().ApplyStyle(cfg.style, cfg.tintColor);
                Save();
            }
        });
        UI::SettingRow(L10N("Tint colour", "Renk tonu"), L10N("Used by the tinted and acrylic materials", "Renkli ve akrilik malzemelerde kullanılır"), [&](float) {
            if (UI::ColorPicker("##tbTintColor", &cfg.tintColor)) {
                system::TaskbarService::Instance().ApplyStyle(cfg.style, cfg.tintColor);
                Save();
            }
        });
    });
}

// ---- Desktop page -------------------------------------------------------------------------------------------------------
// The desktop's look lives in desktop.cfg, which AetherDesktop watches (see desktop_cfg.hpp): everything changed here shows up on the
// real desktop within a second. Edits are written when a slider is let go (or at most every 150 ms while it is dragged).
namespace {
namespace dc = aether::system::desktopcfg;

struct WallTex { std::wstring key; Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv; int w{0}, h{0}; ULONGLONG checked{0}; };
WallTex& WallTexture() { static WallTex t; return t; }

// The current desktop wallpaper as a small GPU texture for the preview (re-checked every two seconds, decoded only when the file changed).
void RefreshWallTexture() {
    ID3D11Device* dev = SettingsWindow::Instance().Device();
    WallTex& t = WallTexture();
    const ULONGLONG now = GetTickCount64();
    if (!dev || (t.checked && now - t.checked < 2000)) return;
    t.checked = now;
    const std::wstring path = system::wallpaper::Current();
    if (path.empty()) { t.srv.Reset(); t.key.clear(); return; }
    WIN32_FILE_ATTRIBUTE_DATA fa{}; GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa);
    wchar_t kb[40]; swprintf(kb, 40, L"|%08x%08x", static_cast<unsigned>(fa.ftLastWriteTime.dwHighDateTime), static_cast<unsigned>(fa.ftLastWriteTime.dwLowDateTime));
    const std::wstring key = path + kb;
    if (key == t.key) return;
    t.key = key; t.srv.Reset();
    Microsoft::WRL::ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return;
    UINT sw = 0, sh = 0; fr->GetSize(&sw, &sh);
    if (!sw || !sh) return;
    const UINT tw = 640, th = std::max<UINT>(1, static_cast<UINT>(static_cast<unsigned long long>(sh) * tw / sw));
    Microsoft::WRL::ComPtr<IWICBitmapScaler> sc;
    if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(fr.Get(), tw, th, WICBitmapInterpolationModeFant))) return;
    Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(sc.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return;
    std::vector<BYTE> px(static_cast<size_t>(tw) * th * 4);
    if (FAILED(conv->CopyPixels(nullptr, tw * 4, static_cast<UINT>(px.size()), px.data()))) return;
    D3D11_TEXTURE2D_DESC td{}; td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = px.data(); sd.SysMemPitch = tw * 4;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) return;
    if (SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &t.srv))) { t.w = static_cast<int>(tw); t.h = static_cast<int>(th); }
}

std::wstring PickImage() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg; std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    COMDLG_FILTERSPEC fs[] = {{L"Images", L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.tif;*.tiff"}};
    dlg->SetFileTypes(1, fs);
    DWORD o = 0; dlg->GetOptions(&o); dlg->SetOptions(o | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(nullptr))) return out;
    Microsoft::WRL::ComPtr<IShellItem> it; PWSTR p = nullptr;
    if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
    return out;
}
std::wstring PickFolder() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg; std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    DWORD o = 0; dlg->GetOptions(&o); dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(nullptr))) return out;
    Microsoft::WRL::ComPtr<IShellItem> it; PWSTR p = nullptr;
    if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
    return out;
}

const ImVec4 kDeskAccents[6] = {{0.55f, 0.30f, 0.95f, 1.0f}, {0.30f, 0.55f, 1.0f, 1.0f}, {0.95f, 0.35f, 0.70f, 1.0f}, {0.20f, 0.78f, 0.52f, 1.0f}, {1.0f, 0.60f, 0.20f, 1.0f}, {0.20f, 0.80f, 0.90f, 1.0f}};
const int kFitCodes[6] = {-1, 10, 6, 2, 0, 100};

// What the preview shows: the wallpaper (or the theme gradient), the dimming, a few icons in the chosen size / glow / label style / accent, the clock and a taskbar.
float DrawDesktopPreview(const dc::Look& l, float pw) {
    const float ph = pw * 9.0f / 16.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + pw, p0.y + ph);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = Sc(14.0f);
    const ImVec4 acc = l.accent == 0 ? ImVec4(render::glass::kViolet.r, render::glass::kViolet.g, render::glass::kViolet.b, 1.0f) : kDeskAccents[l.accent];
    auto C = [](const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, a)); };

    RefreshWallTexture();
    const WallTex& wt = WallTexture();
    dl->AddRectFilled(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), C(acc, 0.20f), rr + 2.0f);
    if (wt.srv && wt.w > 0) {
        // cover the 16:9 frame with the picture (centre crop)
        const float ia = static_cast<float>(wt.w) / static_cast<float>(wt.h), fa = pw / ph;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ia > fa) { const float k = fa / ia; uv0.x = (1.0f - k) * 0.5f; uv1.x = 1.0f - uv0.x; }
        else { const float k = ia / fa; uv0.y = (1.0f - k) * 0.5f; uv1.y = 1.0f - uv0.y; }
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(wt.srv.Get()), p0, p1, uv0, uv1, IM_COL32_WHITE, rr);
    } else {
        dl->AddRectFilledMultiColor(p0, p1, C(ImVec4(acc.x * 0.55f, acc.y * 0.55f, acc.z * 0.55f, 1), 1.0f), C(acc, 0.55f), IM_COL32(8, 4, 24, 255), IM_COL32(8, 4, 24, 255));
    }
    if (l.dim > 0) dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, static_cast<int>(255.0f * l.dim / 100.0f)), rr);
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.16f)), rr, 0, 1.0f);

    // icons
    const float ic = Sc(16.0f + (l.iconSize - 32) / 78.0f * 22.0f), cellW = ic + Sc(34.0f), cellH = ic + Sc(26.0f);
    struct Ic { const char* en; const char* tr; int kind; };
    const Ic icons[] = {{"This PC", "Bu Bilgisayar", 0}, {"Aether", "Aether", 1}, {"Recycle Bin", "Geri Dönüşüm", 2}, {"Projects", "Projeler", 3}};
    ImFont* font = ImGui::GetFont();
    const float fs = Sc(9.5f);
    int shown = 0;
    for (int i = 0; i < 4; ++i) {
        if (!l.icons) break;
        if (i == 0 && !l.pc) continue;
        if (i == 1 && !l.aether) continue;
        if (i == 2 && !l.bin) continue;
        const int col = shown / 3, row = shown % 3; ++shown;
        const float cx = p0.x + Sc(14.0f) + col * cellW, cy = p0.y + Sc(14.0f) + row * cellH;
        const ImVec2 a(cx + (cellW - ic) * 0.5f - Sc(4.0f), cy), b(a.x + ic, a.y + ic);
        const bool main = icons[i].kind == 1;
        if (l.glow > 4) dl->AddRectFilled(ImVec2(a.x - Sc(3.0f), a.y - Sc(3.0f)), ImVec2(b.x + Sc(3.0f), b.y + Sc(3.0f)), C(acc, std::min(0.35f, 0.20f * l.glow / 100.0f)), ic * 0.34f);
        dl->AddRectFilled(a, b, main ? C(acc, 0.95f) : IM_COL32(255, 255, 255, 70), ic * 0.26f);
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 70), ic * 0.26f, 0, 1.0f);
        if (main) { const float g = ic * 0.18f; dl->AddCircle(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), ic * 0.22f, IM_COL32(255, 255, 255, 235), 0, std::max(1.5f, g * 0.5f)); }
        if (l.label != 2) {
            const char* txt = L10N(icons[i].en, icons[i].tr);
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, txt);
            const ImVec2 tp((a.x + b.x) * 0.5f - ts.x * 0.5f, b.y + Sc(4.0f));
            if (l.label == 1) dl->AddRectFilled(ImVec2(tp.x - Sc(5.0f), tp.y - Sc(1.0f)), ImVec2(tp.x + ts.x + Sc(5.0f), tp.y + ts.y + Sc(1.0f)), IM_COL32(0, 0, 0, 150), Sc(8.0f));
            else dl->AddText(font, fs, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 170), txt);
            dl->AddText(font, fs, tp, IM_COL32(255, 255, 255, 235), txt);
        }
    }
    if (l.clock) {
        const float cs = Sc(20.0f); const char* t = "14:32";
        const ImVec2 ts = font->CalcTextSizeA(cs, FLT_MAX, 0.0f, t);
        dl->AddText(font, cs, ImVec2(p1.x - ts.x - Sc(16.0f), p0.y + Sc(10.0f)), IM_COL32(255, 255, 255, 235), t);
        const char* d = L10N("Monday, 16 November", "Pazartesi, 16 Kasım"); const ImVec2 ds = font->CalcTextSizeA(Sc(9.5f), FLT_MAX, 0.0f, d);
        dl->AddText(font, Sc(9.5f), ImVec2(p1.x - ds.x - Sc(16.0f), p0.y + Sc(10.0f) + ts.y + Sc(1.0f)), IM_COL32(255, 255, 255, 170), d);
    }
    // taskbar
    const float tbw = pw * 0.46f, tbh = Sc(14.0f);
    const ImVec2 t0(p0.x + (pw - tbw) * 0.5f, p1.y - tbh - Sc(8.0f)), t1(t0.x + tbw, t0.y + tbh);
    dl->AddRectFilled(t0, t1, IM_COL32(10, 8, 22, 190), tbh * 0.5f);
    for (int i = 0; i < 5; ++i) dl->AddCircleFilled(ImVec2(t0.x + tbw * (0.30f + 0.10f * i), (t0.y + t1.y) * 0.5f), Sc(3.0f), i == 1 ? C(acc, 1.0f) : IM_COL32(255, 255, 255, 140));
    {   // "live" pill: what you see here is what the desktop shows
        const char* t = L10N("LIVE", "CANLI");
        const float fs2 = Sc(9.0f); const ImVec2 ts = font->CalcTextSizeA(fs2, FLT_MAX, 0.0f, t);
        const ImVec2 q0(p1.x - ts.x - Sc(30.0f), p1.y - Sc(26.0f)), q1(p1.x - Sc(10.0f), p1.y - Sc(8.0f));
        if (!l.clock) {
            dl->AddRectFilled(q0, q1, IM_COL32(8, 6, 20, 175), Sc(9.0f));
            dl->AddCircleFilled(ImVec2(q0.x + Sc(9.0f), (q0.y + q1.y) * 0.5f), Sc(3.0f), IM_COL32(60, 220, 130, 255), 12);
            dl->AddText(font, fs2, ImVec2(q0.x + Sc(16.0f), (q0.y + q1.y) * 0.5f - ts.y * 0.5f), IM_COL32(255, 255, 255, 210), t);
        }
    }
    ImGui::Dummy(ImVec2(pw, ph));
    return ph;
}

// A selectable tile that shows how a "fit" mode lays the picture on the screen.
bool FitTile(const char* id, int mode, const char* name, bool sel, const ImVec2& size, const ImVec4& acc) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered();
    const float hv = AnimationEngine::Instance().AnimateFloat(ImGui::GetItemID(), hov ? 1.0f : 0.0f, AnimationSpeed::Fast);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto C = [](const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, a)); };
    const float rad = Sc(12.0f);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), sel ? C(acc, 0.20f) : IM_COL32(255, 255, 255, static_cast<int>(13 + 14 * hv)), rad);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), sel ? C(acc, 0.85f) : IM_COL32(255, 255, 255, static_cast<int>(22 + 30 * hv)), rad, 0, sel ? 1.5f : 1.0f);
    const float in = Sc(9.0f), sw = size.x - in * 2.0f, sh = std::min(sw * 0.56f, size.y - in - Sc(26.0f));
    const ImVec2 s0(p.x + in, p.y + in), s1(s0.x + sw, s0.y + sh);
    dl->AddRectFilled(s0, s1, IM_COL32(8, 6, 22, 255), Sc(4.0f));
    auto pic = [&](float x0, float y0, float x1, float y1) {
        dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), C(acc, 0.95f), C(acc, 0.55f), IM_COL32(40, 30, 110, 255), C(acc, 0.35f));
    };
    const float cx = (s0.x + s1.x) * 0.5f, cy = (s0.y + s1.y) * 0.5f;
    switch (mode) {
    case 10: pic(s0.x + 1, s0.y + 1, s1.x - 1, s1.y - 1); break;                                              // fill: covers everything
    case 6:  pic(s0.x + sw * 0.17f, s0.y + 1, s1.x - sw * 0.17f, s1.y - 1); break;                               // fit: whole picture, bars at the sides
    case 2:  pic(s0.x + 1, s0.y + 1, s1.x - 1, s1.y - 1);                                                       // stretch: filled, with arrows
             dl->AddLine(ImVec2(cx - sw * 0.28f, cy), ImVec2(cx + sw * 0.28f, cy), IM_COL32(255, 255, 255, 230), 1.4f);
             dl->AddTriangleFilled(ImVec2(cx - sw * 0.34f, cy), ImVec2(cx - sw * 0.26f, cy - Sc(3.0f)), ImVec2(cx - sw * 0.26f, cy + Sc(3.0f)), IM_COL32(255, 255, 255, 230));
             dl->AddTriangleFilled(ImVec2(cx + sw * 0.34f, cy), ImVec2(cx + sw * 0.26f, cy - Sc(3.0f)), ImVec2(cx + sw * 0.26f, cy + Sc(3.0f)), IM_COL32(255, 255, 255, 230));
             break;
    case 0:  pic(cx - sw * 0.22f, cy - sh * 0.24f, cx + sw * 0.22f, cy + sh * 0.24f); break;                      // center: small, in the middle
    case 100: {                                                                                                  // tile: a 3 x 2 grid of small copies
        const float tw = sw / 3.0f - Sc(2.0f), th = sh / 2.0f - Sc(2.0f);
        for (int r = 0; r < 2; ++r) for (int c = 0; c < 3; ++c) pic(s0.x + Sc(1.0f) + c * (tw + Sc(2.0f)), s0.y + Sc(1.0f) + r * (th + Sc(2.0f)), s0.x + Sc(1.0f) + c * (tw + Sc(2.0f)) + tw, s0.y + Sc(1.0f) + r * (th + Sc(2.0f)) + th);
        break;
    }
    default: {                                                                                                   // "Windows setting": the four-pane logo
        const float q = Sc(5.0f), g2 = Sc(1.5f);
        for (int r = 0; r < 2; ++r) for (int c = 0; c < 2; ++c) dl->AddRectFilled(ImVec2(cx - q - g2 * 0.5f + c * (q + g2), cy - q - g2 * 0.5f + r * (q + g2)), ImVec2(cx - g2 * 0.5f + c * (q + g2), cy - g2 * 0.5f + r * (q + g2)), IM_COL32(255, 255, 255, 190), 1.0f);
        break;
    }
    }
    dl->AddRect(s0, s1, IM_COL32(255, 255, 255, 46), Sc(4.0f), 0, 1.0f);
    ImFont* font = ImGui::GetFont(); const float fs = Sc(10.5f);
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, name);
    dl->AddText(font, fs, ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + size.y - ts.y - Sc(7.0f)), sel ? IM_COL32(255, 255, 255, 245) : IM_COL32(255, 255, 255, 170), name);
    return clicked;
}

// A selectable tile that shows how icon names are drawn: shadow, capsule or hidden.
bool LabelTile(const char* id, int style, const char* name, bool sel, const ImVec2& size, const ImVec4& acc) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered();
    const float hv = AnimationEngine::Instance().AnimateFloat(ImGui::GetItemID(), hov ? 1.0f : 0.0f, AnimationSpeed::Fast);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto C = [](const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, a)); };
    const float rad = Sc(10.0f);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), sel ? C(acc, 0.20f) : IM_COL32(255, 255, 255, static_cast<int>(13 + 14 * hv)), rad);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), sel ? C(acc, 0.85f) : IM_COL32(255, 255, 255, static_cast<int>(22 + 30 * hv)), rad, 0, sel ? 1.5f : 1.0f);
    const float ic = Sc(15.0f), cx = p.x + size.x * 0.5f, iy = p.y + Sc(8.0f);
    dl->AddRectFilled(ImVec2(cx - ic * 0.5f, iy), ImVec2(cx + ic * 0.5f, iy + ic), C(acc, 0.95f), ic * 0.28f);
    const float ly = iy + ic + Sc(5.0f), lw = ic * 1.7f;
    if (style == 0) {                                  // plain text with a shadow
        dl->AddRectFilled(ImVec2(cx - lw * 0.5f + 1, ly + 1), ImVec2(cx + lw * 0.5f + 1, ly + Sc(3.5f) + 1), IM_COL32(0, 0, 0, 150), 2.0f);
        dl->AddRectFilled(ImVec2(cx - lw * 0.5f, ly), ImVec2(cx + lw * 0.5f, ly + Sc(3.5f)), IM_COL32(255, 255, 255, 220), 2.0f);
    } else if (style == 1) {                           // dark capsule behind the text
        dl->AddRectFilled(ImVec2(cx - lw * 0.5f - Sc(3.0f), ly - Sc(2.0f)), ImVec2(cx + lw * 0.5f + Sc(3.0f), ly + Sc(6.0f)), IM_COL32(0, 0, 0, 160), Sc(4.0f));
        dl->AddRectFilled(ImVec2(cx - lw * 0.5f, ly), ImVec2(cx + lw * 0.5f, ly + Sc(3.5f)), IM_COL32(255, 255, 255, 220), 2.0f);
    } else {                                           // hidden: only a faint dashed hint
        for (int i = 0; i < 4; ++i) dl->AddRectFilled(ImVec2(cx - lw * 0.5f + i * lw * 0.27f, ly + 1), ImVec2(cx - lw * 0.5f + i * lw * 0.27f + lw * 0.16f, ly + Sc(3.0f)), IM_COL32(255, 255, 255, 50), 1.0f);
    }
    ImFont* font = ImGui::GetFont(); const float fs = Sc(10.0f);
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, name);
    dl->AddText(font, fs, ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + size.y - ts.y - Sc(5.0f)), sel ? IM_COL32(255, 255, 255, 245) : IM_COL32(255, 255, 255, 170), name);
    return clicked;
}
} // namespace

void SettingsPages::RenderDesktop() {
    static dc::Look s_look;
    static bool s_init = false;
    static ULONGLONG s_loaded = 0, s_wrote = 0;
    static std::vector<std::pair<std::wstring, std::wstring>> s_pending;
    const ULONGLONG now = GetTickCount64();
    const bool capture = UI::Capturing();
    if (!capture && (!s_init || (s_pending.empty() && !ImGui::IsMouseDown(0) && now - s_loaded > 1000))) { s_look = dc::Load(); s_init = true; s_loaded = now; }
    dc::Look& l = s_look;
    auto queue = [&](const wchar_t* key, const std::wstring& v) {
        for (auto& p : s_pending) if (p.first == key) { p.second = v; return; }
        s_pending.emplace_back(key, v);
    };
    auto qi = [&](const wchar_t* key, int v) { queue(key, std::to_wstring(v)); };
    auto flush = [&]() { for (auto& p : s_pending) dc::Set(p.first, p.second); s_pending.clear(); s_wrote = GetTickCount64(); s_loaded = s_wrote; };

    // a row with a toggle bound to one bool of the desktop config
    auto toggleRow = [&](const char* label, const char* desc, const char* id, bool* v, const wchar_t* key) {
        UI::SettingRow(label, desc, [&](float) { if (UI::Toggle(id, v)) qi(key, *v ? 1 : 0); });
    };
    // a row with a row of equally wide buttons; the selected one is highlighted
    auto segmented = [&](const char* label, const char* desc, const char* id, const std::vector<std::string>& names, int cur, const wchar_t* key) {
        UI::SettingRow(label, desc, [&](float w) {
            const float gap = Sc(6.0f), bw = (w - gap * static_cast<float>(names.size() - 1)) / static_cast<float>(names.size());
            for (size_t i = 0; i < names.size(); ++i) {
                if (i) ImGui::SameLine(0.0f, gap);
                ImGui::PushID(id); ImGui::PushID(static_cast<int>(i));
                if (UI::Button(names[i].c_str(), ImVec2(bw, Sc(32.0f)), static_cast<int>(i) == cur)) qi(key, static_cast<int>(i));
                ImGui::PopID(); ImGui::PopID();
            }
        });
    };

    const ImVec4 accNow = l.accent == 0 ? ImVec4(render::glass::kViolet.r, render::glass::kViolet.g, render::glass::kViolet.b, 1.0f) : kDeskAccents[l.accent];

    // ---- hero: the live preview with the four most visual controls right beside it ----
    if (!capture) {
        const auto& theme = Theme::Instance().Colors();
        const float availW = ImGui::GetContentRegionAvail().x, pad = Sc(20.0f), inner = availW - pad * 2.0f;
        const float leftW = std::clamp(inner * 0.60f, Sc(280.0f), Sc(520.0f)), gapX = Sc(26.0f), rightW = std::max(Sc(190.0f), inner - leftW - gapX);
        const float contentH = std::max(leftW * 9.0f / 16.0f, Sc(268.0f)), heroH = contentH + pad * 2.0f;
        ImGui::Dummy(ImVec2(0, Sc(4.0f)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Surface);
        ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderSubtle);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Sc(18.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Sc(8.0f), Sc(8.0f)));
        ImGui::BeginChild("deskHero", ImVec2(0, heroH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        {
            const ImVec2 base = ImGui::GetCursorPos();
            const float ph = DrawDesktopPreview(l, leftW);
            ImGui::SetCursorPos(ImVec2(base.x, base.y + ph + Sc(10.0f)));
            ImGui::PushTextWrapPos(base.x + leftW);
            if (FontSmall) ImGui::PushFont(FontSmall);
            ImGui::TextColored(theme.TextMuted, "%s", L10N("Live preview of your wallpaper. Changes reach the real desktop within a second.", "Duvar kağıdının canlı önizlemesi. Değişiklikler gerçek masaüstüne bir saniye içinde yansır."));
            if (FontSmall) ImGui::PopFont();
            ImGui::PopTextWrapPos();

            ImGui::SetCursorPos(ImVec2(base.x + leftW + gapX, base.y));
            ImGui::BeginGroup();
            auto cap = [&](const char* t) { if (FontSmall) ImGui::PushFont(FontSmall); ImGui::TextColored(theme.TextMuted, "%s", t); if (FontSmall) ImGui::PopFont(); };
            cap(L10N("ACCENT COLOUR", "VURGU RENGİ"));
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float d = Sc(26.0f), gap = std::max(Sc(6.0f), (rightW - d * 6.0f) / 5.0f);
                for (int k = 0; k < 6; ++k) {
                    if (k) ImGui::SameLine(0.0f, gap);
                    char id[24]; snprintf(id, sizeof(id), "##deskAcc%d", k);
                    const ImVec4 c = k == 0 ? ImVec4(render::glass::kViolet.r, render::glass::kViolet.g, render::glass::kViolet.b, 1.0f) : kDeskAccents[k];
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton(id, ImVec2(d, d))) { l.accent = k; qi(L"accent", k); }
                    const bool hv = ImGui::IsItemHovered();
                    const ImVec2 mid(p.x + d * 0.5f, p.y + d * 0.5f);
                    if (l.accent == k) dl->AddCircleFilled(mid, d * 0.5f + Sc(6.0f), ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.25f)), 28);
                    dl->AddCircleFilled(mid, d * 0.5f - (hv ? 0.0f : 1.0f), ImGui::GetColorU32(c), 28);
                    if (l.accent == k) dl->AddCircle(mid, d * 0.5f + Sc(3.5f), IM_COL32(255, 255, 255, 235), 28, Sc(1.8f));
                    else if (hv) dl->AddCircle(mid, d * 0.5f + Sc(3.0f), IM_COL32(255, 255, 255, 110), 28, 1.4f);
                }
            }
            ImGui::Dummy(ImVec2(0, Sc(4.0f)));
            cap(L10N("ICON LABELS", "SİMGE ETİKETLERİ"));
            {
                const char* names[3] = {L10N("Shadow", "Gölge"), L10N("Capsule", "Kapsül"), L10N("Hidden", "Gizli")};
                const float gap = Sc(8.0f), tw = (rightW - gap * 2.0f) / 3.0f;
                for (int k = 0; k < 3; ++k) {
                    if (k) ImGui::SameLine(0.0f, gap);
                    char id[24]; snprintf(id, sizeof(id), "##deskLbl%d", k);
                    if (LabelTile(id, k, names[k], l.label == k, ImVec2(tw, Sc(62.0f)), accNow)) { l.label = k; qi(L"label", k); }
                }
            }
            ImGui::Dummy(ImVec2(0, Sc(4.0f)));
            cap(L10N("ICON SIZE", "SİMGE BOYUTU"));
            ImGui::SetNextItemWidth(rightW);
            { float v = static_cast<float>(l.iconSize); if (UI::SliderFloat("##deskIcon", &v, 32.0f, 110.0f, "%.0f px")) { l.iconSize = static_cast<int>(v + 0.5f); qi(L"isize", l.iconSize); } }
            cap(L10N("WALLPAPER DIMMING", "DUVAR KAĞIDI KARARTMA"));
            ImGui::SetNextItemWidth(rightW);
            { float v = static_cast<float>(l.dim); if (UI::SliderFloat("##deskDim", &v, 0.0f, 70.0f, "%.0f %%")) { l.dim = static_cast<int>(v + 0.5f); qi(L"dim", l.dim); } }
            ImGui::EndGroup();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(2);
        ImGui::Dummy(ImVec2(0, Sc(10.0f)));
    }

    UI::Section(L10N("Wallpaper", "Duvar kağıdı"), L10N("Picture, fit, dimming and slideshow", "Resim, yerleşim, karartma ve slayt gösterisi"), [&]() {
        UI::SettingRow(L10N("Wallpaper picture", "Duvar kağıdı resmi"), L10N("Pick any image from your PC", "Bilgisayarındaki herhangi bir resmi seç"), [&](float) {
            if (UI::Button(L10N("Choose image...", "Resim seç..."), ImVec2(Sc(150.0f), Sc(32.0f)), true)) {
                const std::wstring p = PickImage();
                if (!p.empty()) { system::wallpaper::SetFile(p); l.slide = false; qi(L"slide", 0); WallTexture().checked = 0; }
            }
        });
        {
            // Wallpaper Engine paints over the Windows wallpaper; while it runs, a picture chosen here is sent to it as well
            static bool weOn = false; static ULONGLONG weAt = 0;
            const ULONGLONG nowMs = GetTickCount64();
            if (!capture && (weAt == 0 || nowMs - weAt > 3000)) { weAt = nowMs; weOn = system::wallpaper::we::Running(); }
            UI::SettingRow(L10N("Wallpaper Engine", "Wallpaper Engine"), L10N("Pictures you choose here are also sent to it while it runs", "Burada seçtiğin resimler, çalışırken Wallpaper Engine'e de gönderilir"), [&](float) {
                const auto& th = Theme::Instance().Colors();
                ImGui::TextColored(weOn ? th.Success : th.TextMuted, "%s", weOn ? L10N("Detected - connected", "Algılandı - bağlı") : L10N("Not running", "Çalışmıyor"));
            });
        }
        if (capture) {
            UI::SettingRow(L10N("Fit", "Yerleşim"), L10N("How the picture fills the screen", "Resmin ekranı nasıl kapladığı"), [&](float) {});
            UI::SettingRow(L10N("Dim", "Karartma"), L10N("A darker wallpaper keeps icon labels readable", "Koyu bir duvar kağıdı simge adlarını okunur tutar"), [&](float) {});
        } else {
            const auto& theme = Theme::Instance().Colors();
            const float availW = ImGui::GetContentRegionAvail().x;
            const ImVec2 line = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(line, ImVec2(line.x + availW, line.y), ImGui::GetColorU32(theme.BorderSubtle), 1.0f);
            ImGui::Dummy(ImVec2(0, Sc(14.0f)));
            ImGui::TextColored(theme.TextPrimary, "%s", L10N("Fit", "Yerleşim"));
            if (FontSmall) ImGui::PushFont(FontSmall);
            ImGui::TextColored(theme.TextMuted, "%s", L10N("How the picture fills the screen", "Resmin ekranı nasıl kapladığı"));
            if (FontSmall) ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, Sc(8.0f)));
            const char* names[6] = {L10N("Windows", "Windows"), L10N("Fill", "Doldur"), L10N("Fit", "Sığdır"), L10N("Stretch", "Uzat"), L10N("Center", "Ortala"), L10N("Tile", "Döşe")};
            const float gap = Sc(10.0f), tw = std::min(Sc(104.0f), (availW - gap * 5.0f) / 6.0f), th = tw * 0.56f + Sc(46.0f);
            for (int i = 0; i < 6; ++i) {
                if (i) ImGui::SameLine(0.0f, gap);
                char id[24]; snprintf(id, sizeof(id), "##deskFit%d", i);
                if (FitTile(id, kFitCodes[i], names[i], l.fit == kFitCodes[i], ImVec2(tw, th), accNow)) { l.fit = kFitCodes[i]; qi(L"fit", l.fit); }
            }
            ImGui::Dummy(ImVec2(0, Sc(14.0f)));
        }
        segmented(L10N("Blur", "Bulanıklık"), L10N("Softens the wallpaper behind the icons", "İkonların arkasındaki duvar kağıdını yumuşatır"), "##deskBlur",
                  {L10N("None", "Yok"), L10N("Low", "Az"), L10N("Medium", "Orta"), L10N("High", "Çok")}, l.blur, L"blur");
        toggleRow(L10N("Wallpaper slideshow", "Duvar kağıdı slaytı"), L10N("Rotate through the pictures of a folder", "Bir klasördeki resimleri sırayla göster"), "##deskSlide", &l.slide, L"slide");
        if (l.slide || !l.slideDir.empty()) {
            UI::SettingRow(L10N("Change every", "Değişim süresi"), L10N("Time between two pictures", "İki resim arasındaki süre"), [&](float) {
                const char* names[4] = {L10N("1 minute", "1 dakika"), L10N("5 minutes", "5 dakika"), L10N("15 minutes", "15 dakika"), L10N("1 hour", "1 saat")};
                static const int mins[4] = {1, 5, 15, 60};
                int cur = 1; for (int i = 0; i < 4; ++i) if (mins[i] == l.slideMin) cur = i;
                if (UI::Combo("##deskSlideMin", &cur, names, 4)) { l.slideMin = mins[cur]; qi(L"slidemin", l.slideMin); }
            });
            UI::SettingRow(L10N("Slideshow folder", "Slayt klasörü"), l.slideDir.empty() ? L10N("No folder chosen yet", "Henüz klasör seçilmedi") : WideToUtf8(l.slideDir).c_str(), [&](float) {
                if (UI::Button(L10N("Choose folder...", "Klasör seç..."), ImVec2(Sc(150.0f), Sc(32.0f)), false)) {
                    const std::wstring p = PickFolder();
                    if (!p.empty()) { l.slideDir = p; queue(L"slidedir", p); }
                }
            });
        }
    });

    UI::Section(L10N("Icons", "Simgeler"), L10N("Glow around icons (size, labels and colour are in the card above)", "Simge parlaması (boyut, etiket ve renk yukarıdaki kartta)"), [&]() {
        if (capture) {   // these controls live in the preview card at the top; the rows only feed the feature search
            UI::SettingRow(L10N("Icon size", "Simge boyutu"), L10N("Ctrl + mouse wheel on the desktop works too", "Masaüstünde Ctrl + fare tekerleği de çalışır"), [&](float) {});
            UI::SettingRow(L10N("Labels", "Etiketler"), L10N("How icon names are drawn", "Simge adlarının çizim biçimi"), [&](float) {});
            UI::SettingRow(L10N("Accent colour", "Vurgu rengi"), L10N("Selection, glow and the Aether icon", "Seçim, parlama ve Aether simgesi"), [&](float) {});
        }
        UI::SettingRow(L10N("Glow", "Parlama"), L10N("Soft light behind selected and hovered icons", "Seçili ve üzerine gelinen simgelerin arkasındaki ışık"), [&](float) {
            float v = static_cast<float>(l.glow);
            if (UI::SliderFloat("##deskGlow", &v, 0.0f, 200.0f, "%.0f %%")) { l.glow = static_cast<int>(v + 0.5f); qi(L"glow", l.glow); }
        });
    });

    UI::Section(L10N("Desktop items", "Masaüstü öğeleri"), L10N("Which icons appear and how they are arranged", "Hangi simgelerin görüneceği ve nasıl dizileceği"), [&]() {
        toggleRow(L10N("Show desktop icons", "Masaüstü simgelerini göster"), L10N("Turns every icon on the desktop on or off", "Masaüstündeki tüm simgeleri açar veya kapatır"), "##deskIcons", &l.icons, L"icons");
        toggleRow(L10N("Aether icon", "Aether simgesi"), L10N("Opens this Settings Center from the desktop", "Bu Ayar Merkezi'ni masaüstünden açar"), "##deskAether", &l.aether, L"aether");
        toggleRow(L10N("This PC", "Bu Bilgisayar"), L10N("Shortcut to your drives", "Sürücülerine kısayol"), "##deskPC", &l.pc, L"pc");
        toggleRow(L10N("Recycle Bin", "Geri Dönüşüm Kutusu"), L10N("Deleted files", "Silinen dosyalar"), "##deskBin", &l.bin, L"bin");
        toggleRow(L10N("User folder", "Kullanıcı klasörü"), L10N("Your personal folder", "Kişisel klasörün"), "##deskUser", &l.user, L"user");
        toggleRow(L10N("Network", "Ağ"), L10N("Computers and devices on your network", "Ağındaki bilgisayarlar ve cihazlar"), "##deskNet", &l.net, L"net");
        toggleRow(L10N("Arrange automatically", "Otomatik düzenle"), L10N("Keeps icons snapped to the grid in order", "Simgeleri sırayla ızgaraya dizili tutar"), "##deskAuto", &l.autoArrange, L"auto");
        UI::SettingRow(L10N("Sort by", "Sıralama"), L10N("Order used by automatic arrange", "Otomatik düzenlemede kullanılan sıra"), [&](float) {
            const char* names[4] = {L10N("Name", "Ad"), L10N("Size", "Boyut"), L10N("Type", "Tür"), L10N("Date modified", "Değiştirilme tarihi")};
            int cur = l.sort;
            if (UI::Combo("##deskSort", &cur, names, 4)) { l.sort = cur; qi(L"sort", cur); }
        });
        toggleRow(L10N("Clock and date", "Saat ve tarih"), L10N("A large clock on the desktop", "Masaüstünde büyük bir saat"), "##deskClock", &l.clock, L"clock");
    });

    UI::Section(L10N("Reset", "Sıfırla"), L10N("Back to the default desktop look", "Varsayılan masaüstü görünümüne dön"), [&]() {
        UI::SettingRow(L10N("Default look", "Varsayılan görünüm"), L10N("Resets wallpaper dimming, blur, icon size, glow, labels, accent, clock and slideshow. Your icons stay where they are.", "Karartma, bulanıklık, simge boyutu, parlama, etiketler, vurgu, saat ve slaytı sıfırlar. Simgelerin yerinde kalır."), [&](float) {
            if (UI::Button(L10N("Reset", "Sıfırla"), ImVec2(Sc(120.0f), Sc(32.0f)), false)) { s_pending.clear(); dc::ResetLook(); l = dc::Look(); s_loaded = GetTickCount64(); UI::AddToast(UI::ToastType::Success, L10N("Desktop look reset", "Masaüstü görünümü sıfırlandı"), ""); }
        });
    });

    if (!capture && !s_pending.empty() && (!ImGui::IsMouseDown(0) || GetTickCount64() - s_wrote > 150)) flush();
}

void SettingsPages::RenderAppearance() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().appearance;
    const auto& theme = Theme::Instance().Colors();

    UI::Section(L10N("Theme", "Tema"), L10N("Applies to the island, the bar, the Start menu, the panels, the widgets and this window", "Island, bar, Başlat menüsü, paneller, widget'lar ve bu pencere için geçerli"), [&]() {
        int count = 0;
        const render::glass::ThemeDef* themes = render::glass::Themes(&count);
        const int current = render::glass::ThemeIndex();
        const float availW = ImGui::GetContentRegionAvail().x;
        const float gap = Sc(14.0f), cardW = std::floor((availW - gap * 3.0f) / 4.0f), cardH = Sc(104.0f);
        ImGui::Dummy(ImVec2(0.0f, Sc(12.0f)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        for (int i = 0; i < count; ++i) {
            const int col = i % 4, row = i / 4;
            const ImVec2 p0(origin.x + col * (cardW + gap), origin.y + row * (cardH + gap));
            const ImVec2 p1(p0.x + cardW, p0.y + cardH);
            ImGui::SetCursorScreenPos(p0);
            char id[24]; snprintf(id, sizeof(id), "##theme%d", i);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(cardW, cardH));
            const bool hovered = ImGui::IsItemHovered();
            const auto& t = themes[i];
            const bool sel = i == current;
            // preview: the panel body with its glow, a pill and a slider in the accent colour
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0.03f, 0.03f, 0.04f, 1.0f)), Sc(12.0f));
            const ImVec2 b0(p0.x + Sc(12.0f), p0.y + Sc(12.0f)), b1(p1.x - Sc(12.0f), p1.y - Sc(34.0f));
            dl->AddRectFilled(ImVec2(b0.x - 3, b0.y - 3), ImVec2(b1.x + 3, b1.y + 3), U32(t.accent, 0.22f * t.glow), Sc(12.0f));
            dl->AddRectFilledMultiColor(b0, b1, U32(t.bodyTop, 1.0f), U32(t.bodyTop, 1.0f), U32(t.bodyBot, 1.0f), U32(t.bodyBot, 1.0f));
            dl->AddRect(b0, b1, U32(t.light, 0.55f), Sc(9.0f), 0, 1.2f);
            dl->AddRectFilled(ImVec2(b0.x + 10, b0.y + 12), ImVec2(b0.x + 36, b0.y + 18), U32(t.light, 0.9f), Sc(3.0f));
            dl->AddRectFilled(ImVec2(b0.x + 10, b0.y + 26), ImVec2(b1.x - 10, b0.y + 31), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.14f)), Sc(3.0f));
            dl->AddRectFilled(ImVec2(b0.x + 10, b0.y + 26), ImVec2(b0.x + (b1.x - b0.x) * 0.6f, b0.y + 31), U32(t.accent, 1.0f), Sc(3.0f));
            dl->AddText(ImVec2(p0.x + Sc(12.0f), p1.y - Sc(26.0f)), ImGui::GetColorU32(theme.TextPrimary), WideToUtf8(L10N(t.nameEn, t.nameTr)).c_str());
            if (sel) {
                dl->AddRect(p0, p1, U32(t.accent, 1.0f), Sc(12.0f), 0, Sc(2.0f));
                dl->AddCircleFilled(ImVec2(p1.x - Sc(18.0f), p1.y - Sc(17.0f)), Sc(7.0f), U32(t.accent, 1.0f));
                dl->AddLine(ImVec2(p1.x - Sc(21.5f), p1.y - Sc(17.0f)), ImVec2(p1.x - Sc(18.5f), p1.y - Sc(14.0f)), IM_COL32(255, 255, 255, 255), 1.8f);
                dl->AddLine(ImVec2(p1.x - Sc(18.5f), p1.y - Sc(14.0f)), ImVec2(p1.x - Sc(14.0f), p1.y - Sc(20.0f)), IM_COL32(255, 255, 255, 255), 1.8f);
            } else if (hovered) {
                dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.30f)), Sc(12.0f), 0, 1.5f);
            }
            if (clicked) {
                Theme::Instance().UseGlassTheme(i);
                render::glass::SaveThemeFile(i);
                cfg.preset = static_cast<config::ThemePreset>(std::min(i, 4));
                if (config::ConfigManager::Instance().Get().autoWallpaper) aether::system::wallpaper::ApplyForTheme(i);
                Save();
                UI::AddToast(UI::ToastType::Success, L10N("Theme applied", "Tema uygulandı"), WideToUtf8(L10N(t.nameEn, t.nameTr)));
            }
        }
        const int rows = (count + 3) / 4;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (cardH + gap)));
        ImGui::Dummy(ImVec2(1.0f, Sc(6.0f)));
    });

    UI::Section(L10N("Wallpaper", "Duvar kağıdı"), L10N("Purple themes get the purple artwork, dark themes the black one", "Mor temalar mor, koyu temalar siyah duvar kağıdı alır"), [&]() {
        auto& root = config::ConfigManager::Instance().GetMutable();
        UI::SettingRow(L10N("Match wallpaper to theme", "Duvar kağıdını temaya uydur"), L10N("Changes the desktop wallpaper whenever you pick a theme", "Tema seçtiğinde masaüstü duvar kağıdını değiştirir"), [&](float) {
            if (UI::Toggle("##autoWallpaper", &root.autoWallpaper)) {
                Save();
                if (root.autoWallpaper) aether::system::wallpaper::ApplyForTheme(render::glass::ThemeIndex());
            }
        });
        UI::SettingRow(L10N("My original wallpaper", "Kendi duvar kağıdım"), L10N("Bring back the wallpaper you had before Aether", "Aether'den önceki duvar kağıdını geri getir"), [&](float) {
            if (UI::Button(L10N("Restore", "Geri yükle"), ImVec2(Sc(110.0f), Sc(32.0f)), false)) aether::system::wallpaper::RestoreOriginal();
        });
    });

    UI::Section(L10N("Icon pack", "İkon paketi"), L10N("Restyle app icons in the taskbar, Start menu and Alt+Tab", "Görev çubuğu, Başlat menüsü ve Alt+Tab'daki uygulama ikonlarını yeniden stillendir"), [&]() {
        namespace ip = aether::system::iconpack;
        static std::vector<std::wstring> packs;
        static std::vector<std::string> names;
        static std::vector<const char*> items;
        static ULONGLONG listedAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (!listedAt || now - listedAt > 3000) {
            listedAt = now;
            packs = ip::ListPacks();
            names.clear(); names.push_back(L10N("System icons", "Sistem ikonları"));
            for (const auto& p : packs) names.push_back(ip::ToUtf8(p));
            items.clear(); for (const auto& n : names) items.push_back(n.c_str());
        }
        UI::SettingRow(L10N("Icon pack", "İkon paketi"), L10N("Programs without an icon in the pack keep their own icon", "Pakette ikonu olmayan programlar kendi ikonunu korur"), [&](float w) {
            const std::wstring active = ip::ActiveName();
            int cur = 0;
            for (size_t i = 0; i < packs.size(); ++i) if (_wcsicmp(packs[i].c_str(), active.c_str()) == 0) cur = static_cast<int>(i) + 1;
            if (UI::Combo("##iconPack", &cur, items.data(), static_cast<int>(items.size()), w)) ip::SetActive(cur <= 0 ? std::wstring() : packs[static_cast<size_t>(cur) - 1]);
        });
        UI::SettingRow(L10N("My own packs", "Kendi paketlerim"), L10N("Drop a folder of PNGs named after programs (chrome.png ...) here and it shows up in the list", "Programların adıyla PNG dosyaları (chrome.png ...) içeren bir klasörü buraya at, listede görünür"), [&](float) {
            if (UI::Button(L10N("Open folder", "Klasörü aç"), ImVec2(Sc(130.0f), Sc(32.0f)), false)) ip::OpenUserFolder();
        });
    });

    UI::Section(L10N("Surfaces", "Yüzeyler"), L10N("Glass strength and depth", "Cam yoğunluğu ve derinlik"), [&]() {
        UI::SettingRow(L10N("Glow", "Parlama"), L10N("Coloured halo around panels", "Panellerin etrafındaki renkli ışıma"), [&](float) { if (UI::Toggle("##enableGlow", &cfg.enableGlow)) Save(); });
        UI::SettingRow(L10N("Shadows", "Gölgeler"), L10N("Soft depth under surfaces", "Yüzeylerin altında yumuşak derinlik"), [&](float) { if (UI::Toggle("##enableShadow", &cfg.enableShadow)) Save(); });
        UI::SettingRow(L10N("Corner radius", "Köşe yuvarlaklığı"), L10N("Roundness of cards and panels", "Kart ve panellerin yuvarlaklığı"), [&](float) { if (UI::SliderFloat("##cornerRadius", &cfg.cornerRadius, 4.0f, 24.0f, "%.0f px")) Save(); });
    });

    RenderAnimations();
}

void SettingsPages::RenderAnimations() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().animation;

    UI::Section(L10N("Motion", "Hareket"), L10N("Spring physics used by the island and the panels", "Island ve panellerin kullandığı yay fiziği"), [&]() {
        UI::SettingRow(L10N("Stiffness", "Sertlik"), L10N("How quickly shapes snap into place", "Şekillerin yerine oturma hızı"), [&](float) { if (UI::SliderFloat("##springK", &cfg.springStiffness, 100.0f, 600.0f, "%.0f")) Save(); });
        UI::SettingRow(L10N("Damping", "Sönümleme"), L10N("Higher values remove the bounce", "Yüksek değerler zıplamayı azaltır"), [&](float) { if (UI::SliderFloat("##springDamp", &cfg.springDamping, 10.0f, 60.0f, "%.0f")) Save(); });
        UI::SettingRow(L10N("Animation speed", "Animasyon hızı"), L10N("Global speed of every transition", "Tüm geçişlerin genel hızı"), [&](float) { if (UI::SliderFloat("##timeScale", &cfg.timeScale, 0.5f, 2.0f, "%.2fx")) Save(); });
        UI::SettingRow(L10N("Reduce motion", "Hareketi azalt"), L10N("Use instant state changes instead of springs", "Yaylar yerine anlık geçişler kullan"), [&](float) { if (UI::Toggle("##reducedMotion", &cfg.reducedMotion)) Save(); });
    });
}

void SettingsPages::RenderMedia() {
    auto track = media::MediaService::Instance().GetTrackLite();
    const auto& theme = Theme::Instance().Colors();

    UI::Section(L10N("Now playing", "Şu an çalan"), L10N("Read from the Windows media session", "Windows medya oturumundan okunur"), [&]() {
        UI::SettingRow(L10N("Session", "Oturum"), L10N("Active player reported by Windows", "Windows'un bildirdiği etkin oynatıcı"), [&](float) {
            if (track.hasMedia) UI::StatusBadge(L10N("ACTIVE", "AKTİF"), theme.Success);
            else UI::StatusBadge(L10N("NO SOURCE", "KAYNAK YOK"), theme.TextMuted);
        });
        UI::SettingRow(L10N("Track", "Parça"), L10N("Title and artist", "Başlık ve sanatçı"), [&](float) {
            const std::string t = WideToUtf8(track.title);
            ImGui::TextColored(theme.TextPrimary, "%s", t.c_str());
        });
        UI::SettingRow(L10N("Controls", "Kontroller"), L10N("Transport commands", "Oynatma komutları"), [&](float) {
            if (UI::Button("<<", ImVec2(Sc(44.0f), Sc(Sizing::Standard)))) media::MediaService::Instance().PreviousTrack();
            ImGui::SameLine();
            if (UI::Button(L10N("Play / Pause", "Oynat / Duraklat"), ImVec2(Sc(110.0f), Sc(Sizing::Standard)), true)) media::MediaService::Instance().PlayPause();
            ImGui::SameLine();
            if (UI::Button(">>", ImVec2(Sc(44.0f), Sc(Sizing::Standard)))) media::MediaService::Instance().NextTrack();
        });
    });
}

void SettingsPages::RenderSystemMonitoring() {
    auto m = system::MetricsCollector::Instance().GetMetrics();
    const auto& theme = Theme::Instance().Colors();
    const double gb = 1024.0 * 1024.0 * 1024.0;

    UI::Section(L10N("Live hardware", "Canlı donanım"), L10N("Measured natively, no extra drivers", "Yerel olarak ölçülür, ek sürücü gerekmez"), [&]() {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f%%", m.cpuUsagePercent);
        MeterRow(L10N("Processor", "İşlemci"), L10N("Total CPU load", "Toplam CPU yükü"), m.cpuUsagePercent, buf, theme.Accent);
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB", m.ramUsedBytes / gb, m.ramTotalBytes / gb);
        MeterRow(L10N("Memory", "Bellek"), L10N("Used / total physical memory", "Kullanılan / toplam fiziksel bellek"), m.ramUsagePercent, buf, theme.Success);
        snprintf(buf, sizeof(buf), "%.0f%%", m.gpuUsagePercent);
        MeterRow(L10N("Graphics", "Ekran kartı"), L10N("3D engine load", "3D motor yükü"), m.gpuUsagePercent, buf, theme.Info);
        UI::SettingRow(L10N("Network", "Ağ"), L10N("Download and upload", "İndirme ve yükleme"), [&](float) {
            ImGui::TextColored(theme.TextPrimary, "%.1f KB/s  /  %.1f KB/s", m.netDownloadBytesPerSec / 1024.0, m.netUploadBytesPerSec / 1024.0);
        });
    });
}

void SettingsPages::RenderGamingMode() {
    auto& cfg = config::ConfigManager::Instance().GetMutable().gaming;
    const bool isGaming = system::GamingModeDetector::Instance().IsGaming();
    const auto& theme = Theme::Instance().Colors();

    UI::Section(L10N("Gaming mode", "Oyun modu"), L10N("Stays out of the way of full-screen games", "Tam ekran oyunların yoluna çıkmaz"), [&]() {
        UI::SettingRow(L10N("Detect full-screen apps", "Tam ekran uygulamaları algıla"), L10N("Automatic full-screen and game detection", "Otomatik tam ekran ve oyun algılama"), [&](float) { if (UI::Toggle("##gamingEnabled", &cfg.enabled)) Save(); });
        UI::SettingRow(L10N("Current state", "Şu anki durum"), L10N("What the detector sees right now", "Algılayıcının şu an gördüğü"), [&](float) {
            if (isGaming) UI::StatusBadge(L10N("GAMING MODE", "OYUN MODU"), theme.Warning);
            else UI::StatusBadge(L10N("DESKTOP", "MASAÜSTÜ"), theme.Info);
        });
        UI::SettingRow(L10N("When a game starts", "Oyun başlayınca"), L10N("What Aether does in full-screen", "Tam ekranda Aether ne yapsın"), [&](float w) {
            const char* actions[] = {L10N("Hide everything", "Hepsini gizle"), L10N("Compact pill only", "Sadece kompakt hap"), L10N("Pause animations", "Animasyonları durdur")};
            int cur = static_cast<int>(cfg.action);
            if (UI::Combo("##gameAction", &cur, actions, 3, w)) { cfg.action = static_cast<config::GamingModeAction>(cur); Save(); }
        });
        UI::SettingRow(L10N("Pause widgets", "Widget'ları duraklat"), L10N("Stop widget updates while gaming", "Oyun sırasında widget güncellemelerini durdur"), [&](float) { if (UI::Toggle("##pauseWidgets", &cfg.pauseWidgets)) Save(); });
        UI::SettingRow(L10N("Mute notifications", "Bildirimleri sustur"), L10N("No island popups while gaming", "Oyun sırasında island bildirimi çıkmaz"), [&](float) { if (UI::Toggle("##suppressNotifs", &cfg.suppressNotifications)) Save(); });
    });
}

void SettingsPages::RenderStartup() { RenderGeneral(); }

void SettingsPages::RenderPerformance() {
    const auto& theme = Theme::Instance().Colors();
    const float fps = ImGui::GetIO().Framerate;
    UI::Section(L10N("Diagnostics", "Tanılama"), L10N("Rendering pipeline status", "Çizim hattı durumu"), [&]() {
        UI::SettingRow(L10N("Settings frame rate", "Ayarlar kare hızı"), L10N("Refresh rate of this window", "Bu pencerenin yenileme hızı"), [&](float) { ImGui::TextColored(theme.Success, "%.0f FPS", fps); });
        UI::SettingRow(L10N("Compositor", "Kompozitör"), L10N("Hardware composition", "Donanım kompozisyonu"), [&](float) { UI::StatusBadge("DIRECTCOMPOSITION", theme.Info); });
    });
}

void SettingsPages::RenderAbout() {
    const auto& theme = Theme::Instance().Colors();

    UI::Section("Aether", L10N("Dynamic Island, taskbar and desktop customisation for Windows", "Windows için Dynamic Island, görev çubuğu ve masaüstü özelleştirme"), [&]() {
        UI::SettingRow(L10N("Version", "Sürüm"), L10N("Native C++20, Direct2D and DirectComposition", "Yerel C++20, Direct2D ve DirectComposition"), [&](float) { ImGui::TextColored(theme.Accent, "Aether 1.0"); });
        UI::SettingRow(L10N("Reset settings", "Ayarları sıfırla"), L10N("Restore every option to its default", "Tüm seçenekleri varsayılana döndür"), [&](float) {
            if (UI::Button(L10N("Reset", "Sıfırla"), ImVec2(Sc(120.0f), Sc(Sizing::Standard)))) {
                config::ConfigManager::Instance().ResetToDefaults();
                Save();
                UI::AddToast(UI::ToastType::Info, L10N("Settings reset", "Ayarlar sıfırlandı"), L10N("Defaults were restored.", "Varsayılanlar geri yüklendi."));
            }
        });
    });
}

} // namespace aether::ui
