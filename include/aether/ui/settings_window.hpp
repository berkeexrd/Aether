#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "aether/ui/settings_pages.hpp"
#include <string>
#include <vector>

namespace aether::ui {

using Microsoft::WRL::ComPtr;

class SettingsWindow {
public:
    static SettingsWindow& Instance() {
        static SettingsWindow instance;
        return instance;
    }

    bool Create();
    void Show(bool show = true);
    void ShowTab(SettingsTab tab);          // opens the window on a given page (used by "--settings desktop" and the desktop's own Aether icon)
    [[nodiscard]] bool IsVisible() const { return m_visible; }
    // True once the window has been left alone for a couple of seconds (no mouse, keyboard, resize or show request).
    // The main loop then redraws it at a relaxed rate instead of at the monitor's refresh rate.
    [[nodiscard]] static bool IsIdle();
    static void NoteActivity();
    void Render();
    void Destroy();

    [[nodiscard]] HWND GetHwnd() const { return m_hwnd; }
    [[nodiscard]] ID3D11Device* Device() const { return m_pd3dDevice.Get(); }   // for pages that upload small textures (desktop preview)

private:
    SettingsWindow() = default;
    ~SettingsWindow() { Destroy(); }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateDeviceD3D();
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    HWND m_hwnd{nullptr};
    bool m_visible{false};
    uint32_t m_width{1120};
    uint32_t m_height{760};

    ComPtr<ID3D11Device> m_pd3dDevice;
    ComPtr<ID3D11DeviceContext> m_pd3dDeviceContext;
    ComPtr<IDXGISwapChain> m_pSwapChain;
    ComPtr<ID3D11RenderTargetView> m_mainRenderTargetView;

    SettingsTab m_activeTab{SettingsTab::General};

    // feature search (sidebar box): results replace the page while a query is typed
    char m_query[96]{};
    bool m_focusSearch{false};
    bool m_enterPressed{false};
    float m_searchBoxBottom{0.0f};
    bool m_indexBuilt{false};
    bool m_indexTurkish{false};
    std::vector<UI::SearchEntry> m_index;
    std::vector<std::wstring> m_foldLabel, m_foldAll;      // folded copies of the index, parallel to m_index
    std::string m_lastQuery;
    std::vector<int> m_hits;                                // indices into m_index, best first
    void BuildSearchIndex();
    void UpdateSearchHits();
    void RenderSearchResults();
};

} // namespace aether::ui
