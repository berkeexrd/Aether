#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/animation/spring.hpp"
#include "aether/render/dcomp_layer.hpp"

namespace aether::bar {

using Microsoft::WRL::ComPtr;

// Hover preview of one application's windows, opened above its dock button: a card per window with a live DWM thumbnail,
// the window title and a close button. Click = bring to front, middle click / x = close. Never takes the keyboard focus.
class WindowPreview {
public:
    using ActivateFn = std::function<void(HWND)>;
    WindowPreview();
    ~WindowPreview();

    bool Create(ActivateFn activate);
    // anchorScreenX: centre of the dock button (screen px); dockEdgeScreenY: the dock edge facing the screen (its top for a bottom bar,
    // its bottom for a top bar); below: the preview opens under that edge instead of above it.
    void Show(HMONITOR mon, float anchorScreenX, int dockEdgeScreenY, bool below, const std::wstring& exePath, const std::vector<HWND>& windows);
    void SetWindows(const std::vector<HWND>& windows);          // the app's window list changed while the preview is open
    void Hide();
    bool Frame(float dt);                                       // true while animating
    [[nodiscard]] bool IsOpen() const { return m_open; }
    [[nodiscard]] bool Hovered() const { return m_inside; }
    [[nodiscard]] const std::wstring& ExePath() const { return m_exe; }

private:
    struct Card {
        HWND hwnd{nullptr};
        std::wstring title;
        HTHUMBNAIL thumb{nullptr};
        SIZE src{0, 0};
        bool hasThumb{false};
        core::Rect rect, head, thumbArea, close;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Layout();
    void RegisterThumbs();
    void ReleaseThumbs();
    void UpdateThumbOpacity();
    void Render();
    void Teardown();
    int CardAt(const core::Vec2& p) const;
    void CloseCard(int index);

    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ComPtr<IDWriteTextFormat> m_fTitle, m_fIcon;
    bool m_hasIconFont{false};
    ComPtr<ID2D1Bitmap1> m_icon;
    std::wstring m_iconKey;
    std::vector<Card> m_cards;
    std::wstring m_exe;
    ActivateFn m_activate;
    HMONITOR m_mon{nullptr};
    float m_anchorX{0.0f};
    int m_dockTop{0};
    bool m_below{false};
    float m_scale{1.0f}, m_pad{6.0f}, m_panelW{0.0f}, m_panelH{0.0f};
    bool m_open{false}, m_inside{false}, m_dirty{true}, m_tracking{false};
    int m_hover{-1};
    bool m_hoverClose{false};
    float m_titleTimer{0.0f};
    animation::Spring<float> m_anim;
};

} // namespace aether::bar
