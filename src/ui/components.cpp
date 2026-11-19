#include "aether/ui/components.hpp"
#include "aether/ui/animation.hpp"
#include "aether/core/i18n.hpp"
#include <windows.h>
#include <imgui_internal.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::ui {

struct ToastItem {
    UI::ToastType type;
    std::string title;
    std::string message;
    float durationRemaining{4.0f};
    float totalDuration{4.0f};
};

static std::vector<ToastItem> s_toasts;

namespace UI {

namespace {
ImVec4 Lerp4(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }
std::string ToUpperLocaleUtf8(const char* s) {
    if (!s || !s[0]) return {};
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (wn <= 1) return s;
    std::wstring w(static_cast<size_t>(wn - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), wn);
    const wchar_t* loc = aether::i18n::Turkish() ? L"tr-TR" : L"en-US";
    const DWORD flags = LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING;
    int un = LCMapStringEx(loc, flags, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr, 0);
    if (un > 0) {
        std::wstring u(static_cast<size_t>(un), L'\0');
        LCMapStringEx(loc, flags, w.c_str(), static_cast<int>(w.size()), u.data(), un, nullptr, nullptr, 0);
        w.swap(u);
    }
    int bn = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (bn <= 0) return s;
    std::string out(static_cast<size_t>(bn), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), bn, nullptr, nullptr);
    return out;
}
}

// Custom-drawn button: soft translucent fill + hairline border that ease towards the hover state; the primary one is the accent colour with a faint glow.
bool Button(const char* label, const ImVec2& size, bool primary) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 ls = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 sz = ImGui::CalcItemSize(size, ls.x + Sc(30.0f), ls.y + Sc(16.0f));
    const ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + sz.x, window->DC.CursorPos.y + sz.y));
    ImGui::ItemSize(sz, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float hv = AnimationEngine::Instance().AnimateFloat(id, (hovered || held) ? 1.0f : 0.0f, AnimationSpeed::Fast);
    const bool down = held && hovered;
    const auto& theme = Theme::Instance().Colors();
    ImDrawList* dl = window->DrawList;
    const float rad = std::min(Sc(10.0f), sz.y * 0.5f);

    ImVec4 fill, edge, text;
    if (primary) {
        fill = Lerp4(WithAlpha(theme.Accent, 0.46f), WithAlpha(theme.Accent, 0.62f), hv);
        if (down) fill = WithAlpha(theme.Accent, 0.72f);
        edge = WithAlpha(theme.AccentHover, 0.78f + 0.20f * hv);
        text = ImVec4(1, 1, 1, 1);
    } else {
        fill = Lerp4(ImVec4(1, 1, 1, 0.065f), ImVec4(1, 1, 1, 0.135f), hv);
        if (down) fill = ImVec4(1, 1, 1, 0.18f);
        edge = Lerp4(ImVec4(1, 1, 1, 0.13f), WithAlpha(theme.AccentHover, 0.55f), hv);
        text = theme.TextPrimary;
    }
    ImGui::RenderNavCursor(bb, id);
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(fill), rad);
    dl->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(edge), rad, 0, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::RenderTextClipped(ImVec2(bb.Min.x + Sc(8.0f), bb.Min.y), ImVec2(bb.Max.x - Sc(8.0f), bb.Max.y), label, nullptr, &ls, ImVec2(0.5f, 0.5f), &bb);
    ImGui::PopStyleColor();
    return pressed;
}

bool Toggle(const char* id, bool* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID widgetId = window->GetID(id);

    const float width = Sc(44.0f);
    const float height = Sc(24.0f);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));

    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, widgetId)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, widgetId, &hovered, &held);
    if (pressed) {
        *value = !(*value);
        ImGui::MarkItemEdited(widgetId);
    }

    // Micro-animation for toggle knob (0.0 = OFF, 1.0 = ON)
    float targetAnim = *value ? 1.0f : 0.0f;
    float animT = AnimationEngine::Instance().AnimateFloat(widgetId, targetAnim, AnimationSpeed::Fast);

    const auto& theme = Theme::Instance().Colors();
    ImVec4 offBg = ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.22f : 0.15f);
    ImVec4 onBg = WithAlpha(theme.Accent, hovered ? 1.0f : 0.90f);
    ImVec4 currentBg = Lerp4(offBg, onBg, animT);

    // Track (clean Customize-style pill, no heavy frame)
    window->DrawList->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(currentBg), height * 0.5f);
    if (hovered) {
        window->DrawList->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(WithAlpha(theme.AccentHover, 0.45f)), height * 0.5f, 0, 1.0f);
    }

    // Knob
    const float knobRadius = (height - Sc(4.0f)) * 0.5f;
    const float knobMinX = bb.Min.x + Sc(2.0f) + knobRadius;
    const float knobMaxX = bb.Max.x - Sc(2.0f) - knobRadius;
    const float knobX = knobMinX + (knobMaxX - knobMinX) * animT;
    const float knobY = bb.Min.y + height * 0.5f;

    window->DrawList->AddCircleFilled(ImVec2(knobX, knobY), knobRadius, IM_COL32(255, 255, 255, 255), 20);

    return pressed;
}

bool Checkbox(const char* label, bool* value) {
    const auto& theme = Theme::Instance().Colors();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.SurfaceElevated);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme.SurfaceHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme.SurfaceActive);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, theme.Accent);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.Border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::XS);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    bool changed = ImGui::Checkbox(label, value);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return changed;
}

// Custom-drawn slider: a slim track with an accent fill and a round knob that grows on hover, the value printed at the right end.
static bool SliderImpl(const char* id, ImGuiDataType type, void* v, const void* mn, const void* mx, const char* fmt) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID wid = window->GetID(id);
    const float w = ImGui::CalcItemWidth(), h = Sc(30.0f);
    const ImRect frame(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + w, window->DC.CursorPos.y + h));
    ImGui::ItemSize(frame, style.FramePadding.y);
    if (!ImGui::ItemAdd(frame, wid, &frame, 0)) return false;

    const float valueW = Sc(72.0f), knobR = Sc(8.0f);
    const ImRect track(frame.Min, ImVec2(std::max(frame.Min.x + knobR * 4.0f, frame.Max.x - valueW), frame.Max.y));
    const bool hovered = ImGui::ItemHoverable(track, wid, g.LastItemData.ItemFlags);
    if (hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, wid)) {
        ImGui::SetKeyOwner(ImGuiKey_MouseLeft, wid);
        ImGui::SetActiveID(wid, window);
        ImGui::SetFocusID(wid, window);
        ImGui::FocusWindow(window);
        g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
    }
    const float oldGrab = g.Style.GrabMinSize;
    g.Style.GrabMinSize = knobR * 2.0f;                      // the behaviour keeps the knob centre inside [track + r, track - r]
    ImRect grab;
    const bool changed = ImGui::SliderBehavior(track, wid, type, v, mn, mx, fmt, ImGuiSliderFlags_AlwaysClamp, &grab);
    g.Style.GrabMinSize = oldGrab;
    if (changed) ImGui::MarkItemEdited(wid);

    const bool active = g.ActiveId == wid;
    const float hv = AnimationEngine::Instance().AnimateFloat(wid, (hovered || active) ? 1.0f : 0.0f, AnimationSpeed::Fast);
    const auto& theme = Theme::Instance().Colors();
    ImDrawList* dl = window->DrawList;
    const float cy = (track.Min.y + track.Max.y) * 0.5f, th = Sc(6.0f);
    const float x0 = track.Min.x + knobR, x1 = track.Max.x - knobR;
    const float kx = std::clamp((grab.Min.x + grab.Max.x) * 0.5f, x0, x1);
    dl->AddRectFilled(ImVec2(x0 - knobR, cy - th * 0.5f), ImVec2(x1 + knobR, cy + th * 0.5f), IM_COL32(255, 255, 255, 34), th * 0.5f);
    dl->AddRectFilled(ImVec2(x0 - knobR, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), ImGui::GetColorU32(WithAlpha(theme.Accent, 0.96f)), th * 0.5f);
    const float r = knobR * (1.0f + 0.12f * hv);
    if (hv > 0.01f) dl->AddCircleFilled(ImVec2(kx, cy), r + Sc(6.0f), ImGui::GetColorU32(WithAlpha(theme.AccentHover, 0.28f * hv)), 24);
    dl->AddCircleFilled(ImVec2(kx, cy + 1.0f), r + 0.5f, IM_COL32(0, 0, 0, 80), 24);
    dl->AddCircleFilled(ImVec2(kx, cy), r, IM_COL32(255, 255, 255, 255), 24);

    char text[48];
    ImGui::DataTypeFormatString(text, sizeof(text), type, v, fmt);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(frame.Max.x - ts.x, cy - ts.y * 0.5f), ImGui::GetColorU32(active ? theme.TextPrimary : theme.TextSecondary), text);
    return changed;
}

bool SliderFloat(const char* id, float* value, float minVal, float maxVal, const char* format) {
    return SliderImpl(id, ImGuiDataType_Float, value, &minVal, &maxVal, format ? format : "%.2f");
}

bool SliderInt(const char* id, int* value, int minVal, int maxVal) {
    return SliderImpl(id, ImGuiDataType_S32, value, &minVal, &maxVal, "%d");
}

// Custom-drawn drop-down: same soft field as the buttons, a chevron that flips while open and a rounded list with an accent marker on the current entry.
bool Combo(const char* id, int* currentItem, const char* const items[], int itemsCount, float width) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID wid = window->GetID(id);
    const float w = width > 0.0f ? width : ImGui::CalcItemWidth(), h = Sc(32.0f);
    const ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + w, window->DC.CursorPos.y + h));
    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, wid)) return false;

    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, wid, &hovered, &held);
    const ImGuiID popupId = ImHashStr("##ComboPopup", 0, wid);
    bool open = ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None);
    if (pressed && !open) { ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None); open = true; }

    const auto& theme = Theme::Instance().Colors();
    const float hv = AnimationEngine::Instance().AnimateFloat(wid, (hovered || open) ? 1.0f : 0.0f, AnimationSpeed::Fast);
    ImDrawList* dl = window->DrawList;
    const float rad = Sc(10.0f);
    ImGui::RenderNavCursor(bb, wid);
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(Lerp4(ImVec4(1, 1, 1, 0.055f), ImVec4(1, 1, 1, 0.10f), hv)), rad);
    dl->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(open ? WithAlpha(theme.AccentHover, 0.75f) : Lerp4(ImVec4(1, 1, 1, 0.09f), ImVec4(1, 1, 1, 0.20f), hv)), rad, 0, 1.0f);
    const char* preview = (*currentItem >= 0 && *currentItem < itemsCount) ? items[*currentItem] : "";
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextPrimary);
    ImGui::RenderTextClipped(ImVec2(bb.Min.x + Sc(12.0f), bb.Min.y), ImVec2(bb.Max.x - Sc(32.0f), bb.Max.y), preview, nullptr, nullptr, ImVec2(0.0f, 0.5f));
    ImGui::PopStyleColor();
    {   // chevron
        const float cx = bb.Max.x - Sc(17.0f), cy = (bb.Min.y + bb.Max.y) * 0.5f, a = Sc(4.5f), dy = open ? -1.0f : 1.0f;
        const ImU32 cc = ImGui::GetColorU32(open ? theme.AccentHover : theme.TextSecondary);
        dl->AddLine(ImVec2(cx - a, cy - a * 0.45f * dy), ImVec2(cx, cy + a * 0.45f * dy), cc, 1.6f);
        dl->AddLine(ImVec2(cx, cy + a * 0.45f * dy), ImVec2(cx + a, cy - a * 0.45f * dy), cc, 1.6f);
    }
    if (!open) return false;

    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.SurfaceOverlay);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderHover);
    ImGui::PushStyleColor(ImGuiCol_Header, WithAlpha(theme.Accent, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, WithAlpha(theme.Accent, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, WithAlpha(theme.Accent, 0.30f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Sc(12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(6.0f), Sc(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, Sc(2.0f)));
    bool changed = false;
    if (ImGui::BeginComboPopup(popupId, bb, ImGuiComboFlags_HeightLarge)) {
        const float rowH = Sc(30.0f);
        for (int i = 0; i < itemsCount; ++i) {
            ImGui::PushID(i);
            const bool selected = i == *currentItem;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##opt", selected, 0, ImVec2(0.0f, rowH))) { *currentItem = i; changed = true; }
            ImDrawList* pd = ImGui::GetWindowDrawList();
            pd->AddText(ImVec2(p.x + Sc(10.0f), p.y + (rowH - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(selected ? theme.TextPrimary : theme.TextSecondary), items[i]);
            if (selected) pd->AddCircleFilled(ImVec2(p.x + ImGui::GetItemRectSize().x - Sc(14.0f), p.y + rowH * 0.5f), Sc(3.5f), ImGui::GetColorU32(theme.AccentHover), 12);
            if (selected && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
    return changed;
}

bool ColorPicker(const char* id, uint32_t* colorHexRgba) {
    float r = static_cast<float>((*colorHexRgba >> 16) & 0xFF) / 255.0f;
    float g = static_cast<float>((*colorHexRgba >> 8) & 0xFF) / 255.0f;
    float b = static_cast<float>(*colorHexRgba & 0xFF) / 255.0f;
    float col[3] = {r, g, b};

    const auto& theme = Theme::Instance().Colors();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::SM);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.Border);

    bool changed = ImGui::ColorEdit3(id, col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(2);

    if (changed) {
        uint8_t ir = static_cast<uint8_t>(col[0] * 255.0f + 0.5f);
        uint8_t ig = static_cast<uint8_t>(col[1] * 255.0f + 0.5f);
        uint8_t ib = static_cast<uint8_t>(col[2] * 255.0f + 0.5f);
        *colorHexRgba = (0xFF << 24) | (ir << 16) | (ig << 8) | ib;
    }
    return changed;
}

bool Keybind(const char* id, int* currentKey, bool* isCapturing) {
    char buf[64];

    if (*isCapturing) {
        snprintf(buf, sizeof(buf), "[ Press key... ]##%s", id);
        // Check input
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            *isCapturing = false;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            *currentKey = 0;
            *isCapturing = false;
            return true;
        } else {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) {
                    *currentKey = k;
                    *isCapturing = false;
                    return true;
                }
            }
        }
    } else {
        if (*currentKey > 0) {
            snprintf(buf, sizeof(buf), "[ %s ]##%s", ImGui::GetKeyName(static_cast<ImGuiKey>(*currentKey)), id);
        } else {
            snprintf(buf, sizeof(buf), "[ NONE ]##%s", id);
        }
    }

    if (Button(buf, ImVec2(120.0f, Sizing::Standard), *isCapturing)) {
        *isCapturing = !(*isCapturing);
    }
    return false;
}

static bool s_firstRow = true;

// ---- feature search support ----------------------------------------------------------------------------------------
static std::vector<SearchEntry>* s_capture = nullptr;
static int s_captureTab = 0;
static std::string s_curSection;                 // title of the Section being built
static std::string s_focusSection, s_focusLabel; // a search result was clicked: scroll to this row
static std::string s_pulseSection, s_pulseLabel;
static float s_pulse = 0.0f;

void BeginSearchCapture(std::vector<SearchEntry>* out, int tab) { s_capture = out; s_captureTab = tab; s_curSection.clear(); }
void EndSearchCapture() { s_capture = nullptr; }
bool Capturing() { return s_capture != nullptr; }
void FocusSetting(const std::string& section, const std::string& label) { s_focusSection = section; s_focusLabel = label; }
void TickSearchHighlight(float dt) { if (s_pulse > 0.0f) s_pulse = std::max(0.0f, s_pulse - dt); }

void Section(const char* title, const char* description, const std::function<void()>& content) {
    if (s_capture) { s_curSection = title; content(); return; }
    s_curSection = title;
    const auto& theme = Theme::Instance().Colors();

    ImGui::Dummy(ImVec2(0.0f, Sc(8.0f)));
    // Clean Özelleştir-style locale-aware uppercase section eyebrow directly on the obsidian canvas
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Sc(8.0f), Sc(3.0f)));
    if (FontSmall) ImGui::PushFont(FontSmall);
    const std::string upper = ToUpperLocaleUtf8(title);
    ImGui::TextColored(WithAlpha(theme.AccentHover, 0.92f), "%s", upper.c_str());
    if (description && description[0]) {
        ImGui::TextColored(theme.TextMuted, "%s", description);
    }
    if (FontSmall) ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0.0f, Sc(4.0f)));

    s_firstRow = true;
    content();
    ImGui::Dummy(ImVec2(0.0f, Sc(12.0f)));
}

void SettingRow(const char* label, const char* description, const std::function<void(float controlWidth)>& renderControl) {
    if (s_capture) { s_capture->push_back({s_captureTab, s_curSection, label ? label : "", description ? description : ""}); return; }
    const auto& theme = Theme::Instance().Colors();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float controlW = Sc(230.0f);
    const float labelW = std::max(Sc(140.0f), availW - controlW - Sc(28.0f));
    const float padY = Sc(11.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 screen = ImGui::GetCursorScreenPos();
    if (!s_firstRow) dl->AddLine(ImVec2(screen.x, screen.y), ImVec2(screen.x + availW, screen.y), IM_COL32(255, 255, 255, 13), 1.0f);
    s_firstRow = false;

    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x, start.y + padY));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(start.x + labelW);
    ImGui::TextColored(theme.TextPrimary, "%s", label);
    if (description && description[0]) {
        if (FontSmall) ImGui::PushFont(FontSmall);
        ImGui::TextColored(theme.TextMuted, "%s", description);
        if (FontSmall) ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const float labelH = ImGui::GetItemRectSize().y;
    const float rowH = std::max(labelH + padY + padY, Sc(50.0f));

    // Subtle Özelleştir row hover highlight directly on the obsidian canvas
    const ImRect rowRect(ImVec2(screen.x - Sc(10.0f), screen.y + Sc(2.0f)), ImVec2(screen.x + availW + Sc(10.0f), screen.y + rowH - Sc(2.0f)));
    if (ImGui::IsMouseHoveringRect(rowRect.Min, rowRect.Max, true)) {
        dl->AddRectFilled(rowRect.Min, rowRect.Max, IM_COL32(255, 255, 255, 11), Sc(10.0f));
    }

    // arrived here from a search result: bring the row into view and flash it
    if (!s_focusLabel.empty() && s_focusLabel == label && s_focusSection == s_curSection) {
        for (ImGuiWindow* w = ImGui::GetCurrentWindow(); w; w = w->ParentWindow) {
            if (std::strstr(w->Name, "PageScroll")) { ImGui::SetScrollFromPosY(w, screen.y - w->Pos.y, 0.3f); break; }
        }
        s_pulseSection = s_focusSection; s_pulseLabel = s_focusLabel; s_pulse = 1.6f;
        s_focusLabel.clear(); s_focusSection.clear();
    }

    ImGui::SetCursorPos(ImVec2(start.x + availW - controlW, start.y + (rowH - Sc(30.0f)) * 0.5f));
    ImGui::SetNextItemWidth(controlW);
    renderControl(controlW);

    if (s_pulse > 0.0f && s_pulseLabel == label && s_pulseSection == s_curSection) {
        const float a = std::min(1.0f, s_pulse) * (0.5f + 0.5f * std::sin(s_pulse * 9.0f));
        ImVec4 f = theme.Accent; f.w = 0.16f * a;
        ImVec4 e = theme.AccentHover; e.w = 0.72f * std::min(1.0f, s_pulse);
        dl->AddRectFilled(rowRect.Min, rowRect.Max, ImGui::GetColorU32(f), Sc(10.0f));
        dl->AddRect(rowRect.Min, rowRect.Max, ImGui::GetColorU32(e), Sc(10.0f), 0, 1.4f);
    }

    ImGui::SetCursorPos(ImVec2(start.x, start.y + rowH));
}

bool SidebarItem(const char* id, const char* label, bool isSelected, float height, const char* iconUtf8) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID widgetId = window->GetID(id);

    const float availX = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + availX, pos.y + height));

    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, widgetId)) return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, widgetId, &hovered, &held);
    auto& ae = AnimationEngine::Instance();
    const float sel = ae.AnimateFloat(widgetId, isSelected ? 1.0f : 0.0f, AnimationSpeed::Normal);
    const float hov = ae.AnimateFloat(widgetId ^ 0x51ED270Bu, hovered ? 1.0f : 0.0f, AnimationSpeed::Fast);
    const auto& theme = Theme::Instance().Colors();
    ImDrawList* dl = window->DrawList;
    const float R = Sc(11.0f);
    auto withA = [](ImVec4 c, float a) { c.w = a; return ImGui::GetColorU32(c); };

    // Exact Özelleştir pill tab: translucent accent fill + crisp 1px accentLight rim when active; soft white wash on hover.
    if (sel > 0.01f) {
        dl->AddRectFilled(bb.Min, bb.Max, withA(theme.Accent, 0.34f * sel), R);
        dl->AddRect(bb.Min, bb.Max, withA(theme.AccentHover, 0.62f * sel), R, 0, 1.0f);
    }
    if (hov > 0.01f && sel < 0.99f) {
        dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f * hov * (1.0f - sel))), R);
    }

    float textX = bb.Min.x + Sc(14.0f);
    if (iconUtf8 && FontIcons) {
        ImGui::PushFont(FontIcons);
        const ImVec2 isz = ImGui::CalcTextSize(iconUtf8);
        const ImVec4 icIdle = ImLerp(theme.TextSecondary, theme.TextPrimary, hov);
        const ImVec4 ic = ImLerp(icIdle, ImVec4(1, 1, 1, 1), sel);
        dl->AddText(ImVec2(bb.Min.x + Sc(12.0f) + (Sc(22.0f) - isz.x) * 0.5f, bb.Min.y + (height - isz.y) * 0.5f), ImGui::GetColorU32(ic), iconUtf8);
        ImGui::PopFont();
        textX = bb.Min.x + Sc(44.0f);
    }
    if (isSelected && FontBold) ImGui::PushFont(FontBold);
    const ImVec4 textIdle = ImLerp(theme.TextSecondary, theme.TextPrimary, hov);
    const ImVec4 textCol = ImLerp(textIdle, ImVec4(1, 1, 1, 1), sel);
    const ImVec2 textPos(textX, bb.Min.y + (height - ImGui::GetFontSize()) * 0.5f);
    dl->AddText(textPos, ImGui::GetColorU32(textCol), label);
    if (isSelected && FontBold) ImGui::PopFont();
    return pressed;
}

void StatusBadge(const char* label, const ImVec4& color) {
    ImVec2 pad(Spacing::SM, Spacing::XS);
    ImVec2 textSz = ImGui::CalcTextSize(label);
    ImVec2 sz(textSz.x + pad.x * 2.0f, textSz.y + pad.y * 2.0f);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Dim background + subtle border
    ImVec4 bgCol = ImVec4(color.x, color.y, color.z, 0.16f);
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), ImGui::GetColorU32(bgCol), Radius::XL);
    dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), ImGui::GetColorU32(color), Radius::XL, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), ImGui::GetColorU32(color), label);

    ImGui::Dummy(sz);
}

void Tooltip(const char* text) {
    if (ImGui::IsItemHovered()) {
        const auto& theme = Theme::Instance().Colors();
        ImGui::PushStyleColor(ImGuiCol_PopupBg, theme.SurfaceOverlay);
        ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderHover);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Radius::SM);
        ImGui::SetTooltip("%s", text);
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(2);
    }
}

void AddToast(ToastType type, const std::string& title, const std::string& message) {
    s_toasts.push_back({type, title, message, 4.0f, 4.0f});
}

void RenderToasts() {
    if (s_toasts.empty()) return;

    float dt = ImGui::GetIO().DeltaTime;
    for (auto it = s_toasts.begin(); it != s_toasts.end();) {
        it->durationRemaining -= dt;
        if (it->durationRemaining <= 0.0f) {
            it = s_toasts.erase(it);
        } else {
            ++it;
        }
    }

    if (s_toasts.empty()) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float toastW = 300.0f;
    float startX = vp->Pos.x + vp->Size.x - toastW - Spacing::XL;
    float currentY = vp->Pos.y + vp->Size.y - Spacing::XL;

    const auto& theme = Theme::Instance().Colors();

    for (int i = static_cast<int>(s_toasts.size()) - 1; i >= 0; --i) {
        auto& t = s_toasts[i];
        float toastH = 68.0f;
        currentY -= (toastH + Spacing::SM);

        ImGui::SetNextWindowPos(ImVec2(startX, currentY));
        ImGui::SetNextWindowSize(ImVec2(toastW, toastH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.SurfaceOverlay);
        ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderHover);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::MD);

        char toastWinId[64];
        snprintf(toastWinId, sizeof(toastWinId), "##toast_%d", i);

        if (ImGui::Begin(toastWinId, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav)) {
            ImVec4 typeCol = theme.Info;
            if (t.type == ToastType::Success) typeCol = theme.Success;
            else if (t.type == ToastType::Warning) typeCol = theme.Warning;
            else if (t.type == ToastType::Error) typeCol = theme.Error;

            ImGui::TextColored(typeCol, "%s", t.title.c_str());
            ImGui::TextColored(theme.TextSecondary, "%s", t.message.c_str());

            // Progress bar
            float prog = t.durationRemaining / t.totalDuration;
            ImVec2 barP1 = ImVec2(startX + Spacing::MD, currentY + toastH - 4.0f);
            ImVec2 barP2 = ImVec2(startX + Spacing::MD + (toastW - Spacing::MD * 2.0f) * prog, currentY + toastH - 2.0f);
            ImGui::GetForegroundDrawList()->AddRectFilled(barP1, barP2, ImGui::GetColorU32(typeCol), 1.0f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
}

} // namespace UI

} // namespace aether::ui
