#pragma once
// Aether's own volume pop-up: the small glass pill that slides up above the bar whenever the volume or the mute state of the default
// playback device changes (volume keys, the mixer, an app, the bar's own wheel). Windows' own volume flyout is not used.
#include <windows.h>
#include <wrl/client.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <memory>
#include <string>
#include "aether/render/dcomp_layer.hpp"

namespace aether::bar {

class VolumeOsd {
public:
    VolumeOsd() = default;
    ~VolumeOsd();
    bool Create();
    void Shutdown();
    bool Frame(float dt);                      // true while the pop-up is visible or moving
    static void Nudge(int dir);                // volume key: +1 / -1 = 2 % louder / quieter (and unmutes), 0 = toggle mute

    // written by the audio callback thread
    struct Shared {
        std::atomic<unsigned> stamp{0};        // bumped on every change
        std::atomic<float> level{0.0f};
        std::atomic<bool> mute{false};
    };

private:
    void Attach();                             // (re)connect to the current default playback device
    void Detach();
    void MakeFonts();
    void Place();
    void Draw();

    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fIcon, m_fPct;
    Microsoft::WRL::ComPtr<IAudioEndpointVolume> m_ep;
    Microsoft::WRL::ComPtr<IAudioEndpointVolumeCallback> m_cb;
    std::wstring m_devId;
    Shared m_shared;
    unsigned m_seen{0};
    float m_scale{1.0f}, m_age{0.0f}, m_in{0.0f}, m_out{0.0f}, m_shownLevel{0.0f}, m_devTimer{0.0f};
    float m_level{0.0f};
    bool m_mute{false}, m_visible{false}, m_placed{false};
    ULONGLONG m_attachedAt{0};
};

} // namespace aether::bar
