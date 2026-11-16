#pragma once
// Aether's own notification pop-ups ("toasts"). Windows' banners are moved off screen (NativeToastHider) and the toasts that
// Windows collected are drawn here instead: glass cards that slide in at a screen corner, stack, pause while hovered and open
// the sending app when clicked. The history stays in the notification centre (NotifyPanel).
#include <windows.h>
#include <wrl/client.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <memory>
#include <string>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/bar/notifications.hpp"
#include "aether/system/notify_cfg.hpp"

namespace aether::bar {

class ToastHost {
public:
    ToastHost() = default;
    ~ToastHost();
    bool Create();
    void Shutdown();
    bool Frame(float dt);                              // true while a toast is visible or moving
    void PushText(const std::wstring& app, const std::wstring& title, const std::wstring& body);   // for Aether's own messages and the test button

private:
    struct Toast {
        long long id{0};
        std::wstring app, appId, title, body;
        float life{7.0f}, age{0.0f}, in{0.0f}, out{0.0f}, y{0.0f}, targetY{0.0f}, cardH{62.0f};
        int bodyLines{0};
        bool closing{false}, hover{false}, placed{false};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> icon;
        std::wstring label;                            // resolved app name
    };
    struct Hit { core::Rect card, close; long long id; };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Push(const NotifItem& n);
    void Prepare(Toast& t);
    void MakeFonts();
    void Place();                                      // window position for the chosen corner
    void Draw();
    void DrawToast(ID2D1DeviceContext* c, Toast& t);
    void CloseToast(long long id);
    void Activate(const Toast& t);
    void PollConfig(float dt);
    static bool Quiet();

    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fApp, m_fTitle, m_fBody, m_fIcon, m_fInitial;
    std::vector<Toast> m_toasts;                       // newest first
    std::vector<Hit> m_hits;
    system::notifycfg::Settings m_cfg;
    unsigned long long m_cfgStamp{~0ull};
    float m_cfgTimer{0.0f}, m_scale{1.0f}, m_time{0.0f};
    int m_placedPos{-1};
    long long m_lastTest{-1};
    core::Vec2 m_mouse{-1, -1};
    bool m_shown{false}, m_trackLeave{false};
    long long m_nextLocalId{-1};
};

// While Aether's notifications are on, Windows' own banner windows are moved off screen the moment they appear (they are hosted by the
// shell experience host); Windows keeps collecting the notifications, which is where Aether reads them from.
class NativeToastHider {
public:
    static void Start();
    static void Stop();
    static void Enable(bool on);
};

} // namespace aether::bar
