#pragma once
// Global keyboard routing for the Aether shell (Alt+Tab / Ctrl+Alt+Tab / Win+Tab switcher, Win+R, Win+M, Win+I).
//
// Used by two processes:
//   * AetherShell owns the switcher and the run box, and routes every key it sees.
//   * AetherBar (always elevated) routes the same keys ONLY while an elevated window has focus. A low-level hook of a
//     non-elevated process is blind to those keys (UIPI), which is why Windows' own Alt+Tab / Win+R used to appear
//     as soon as an administrator console was in front. The two cases never overlap, so a key is never handled twice.
//
// Everything here runs inside a WH_KEYBOARD_LL callback: keep it fast, never block, never touch UI.
#include <windows.h>
#include <atomic>

namespace aether::shellkeys {

constexpr UINT kMsgSwStart = WM_APP + 1, kMsgSwNext = WM_APP + 2, kMsgSwMove = WM_APP + 3, kMsgSwCommit = WM_APP + 4, kMsgSwCancel = WM_APP + 5;
constexpr UINT kMsgRun = WM_APP + 6, kMsgMinAll = WM_APP + 7, kMsgSettings = WM_APP + 8;
constexpr UINT kMsgWinCombo = WM_APP + 9;        // "a Win+<key> shortcut was consumed": the bar must not open Start on the Win release
constexpr UINT kMsgActivate = WM_APP + 10;       // shell -> elevated bar: bring this window (WPARAM = HWND) to the front; a non-elevated process cannot always do it
constexpr UINT kMsgClip = WM_APP + 11;           // Win+V: clipboard history
constexpr UINT kMsgSnap = WM_APP + 12;           // Win+Arrow: WPARAM = arrow vk, LPARAM = 1 when Shift is held (move to the next monitor)
constexpr wchar_t kCtlClass[] = L"AetherShellControl";
constexpr wchar_t kSwitcherClass[] = L"AetherSwitcherWindow";

// Delivers a command to whoever owns the switcher / run box. Returns false when nobody could take it (the key is then left alone).
using PostFn = bool (*)(UINT msg, WPARAM w, LPARAM l);
// Asked before a NEW key sequence is taken over. Returns false to leave the key to the other hook.
using GateFn = bool (*)();

inline std::atomic<bool> g_swActive{false};
inline std::atomic<bool> g_swSticky{false};
inline std::atomic<ULONGLONG> g_swStart{0};
inline std::atomic<ULONGLONG> g_winComboTick{0};       // last time a Win+<key> shortcut was consumed by an Aether hook
inline std::atomic<DWORD> g_swallowedUp{0};            // vk whose key-up still has to be swallowed (its key-down was)

// A dummy key: stops Start / the menu bar from reacting to a lone Win / Alt release.
inline void MaskKeys() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

inline bool KeyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

inline bool SwitcherVisible() {
    HWND h = FindWindowW(kSwitcherClass, nullptr);
    return h && IsWindowVisible(h);
}

// Returns true when the key was consumed (the hook must return 1).
inline bool Route(const KBDLLHOOKSTRUCT* k, WPARAM w, PostFn post, GateFn gate) {
    if (!k || !post || (k->flags & LLKHF_INJECTED)) return false;
    const bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
    const DWORD vk = k->vkCode;
    const bool alt = (k->flags & LLKHF_ALTDOWN) != 0;
    const bool isAlt = vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU;
    const ULONGLONG now = GetTickCount64();

    // ---- a switcher sequence started by this process is running -----------------------------------------------------
    if (g_swActive) {
        const bool sticky = g_swSticky;
        const bool shift = KeyHeld(VK_SHIFT);
        if (now - g_swStart > 60000) { g_swActive = false; post(kMsgSwCancel, 0, 0); return false; }           // can never get stuck
        if (sticky) {
            if (now - g_swStart > 600 && !SwitcherVisible()) { g_swActive = false; return false; }              // closed / clicked away
        } else if (!isAlt && !KeyHeld(VK_MENU)) {                                                               // Alt is up and its release was missed
            g_swActive = false; post(kMsgSwCommit, 0, 0); return false;
        }
        if (vk == VK_TAB) { if (down) post(kMsgSwNext, shift ? static_cast<WPARAM>(-1) : 1, 0); return true; }
        if (vk == VK_ESCAPE) { if (down) { g_swActive = false; post(kMsgSwCancel, 0, 0); } return true; }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) { if (down) post(kMsgSwMove, vk, 0); return true; }
        if (sticky && (vk == VK_RETURN || vk == VK_SPACE)) { if (down) { g_swActive = false; post(kMsgSwCommit, 0, 0); } return true; }
        if (!sticky && !down && isAlt) {                                                                         // Alt released: switch
            g_swActive = false;
            post(kMsgSwCommit, 0, 0);
            MaskKeys();
        }
        return false;
    }

    // ---- the key-up of a key whose key-down we swallowed ------------------------------------------------------------
    if (!down && g_swallowedUp.load() == vk) { g_swallowedUp = 0; return true; }

    const bool win = KeyHeld(VK_LWIN) || KeyHeld(VK_RWIN), ctrl = KeyHeld(VK_CONTROL), shift = KeyHeld(VK_SHIFT);

    // ---- Alt+Tab (quick, closes when Alt is released) and Ctrl+Alt+Tab (stays open) ---------------------------------
    if (down && vk == VK_TAB && alt && !win) {
        if (gate && !gate()) return false;
        if (!post(kMsgSwStart, ctrl ? 1 : 0, shift ? 1 : 0)) return false;
        g_swSticky = ctrl; g_swStart = now; g_swActive = true;
        return true;
    }
    // ---- Win+Tab: the switcher stays open --------------------------------------------------------------------------
    if (down && vk == VK_TAB && win && !ctrl && !alt) {
        if (gate && !gate()) return false;
        if (!post(kMsgSwStart, 1, shift ? 1 : 0)) return false;
        g_winComboTick = now;
        post(kMsgWinCombo, 0, 0);
        MaskKeys();
        g_swSticky = true; g_swStart = now; g_swActive = true;
        return true;
    }
    // ---- Win+R / Win+M / Win+I -------------------------------------------------------------------------------------
    if (down && win && !ctrl && !alt && (vk == 'R' || vk == 'M' || vk == 'I')) {
        if (gate && !gate()) return false;
        const UINT msg = vk == 'R' ? kMsgRun : (vk == 'M' ? kMsgMinAll : kMsgSettings);
        if (!post(msg, vk == 'M' && shift ? 1 : 0, 0)) return false;
        g_winComboTick = now;
        post(kMsgWinCombo, 0, 0);
        MaskKeys();
        g_swallowedUp = vk;
        return true;
    }
    // ---- Win+V (clipboard history) and Win+Arrow (snap / maximize / restore / minimize, Shift: other monitor) ------------
    // Only when Windows' own shell is not running: explorer.exe answers these itself, and answering twice would be worse than not at all.
    // (Ctrl is excluded on purpose: Win+Ctrl+Arrow belongs to the virtual desktops.)
    if (down && win && !ctrl && !alt && GetShellWindow() == nullptr) {
        const bool arrow = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
        if (vk == 'V' || arrow) {
            if (gate && !gate()) return false;
            if (!(vk == 'V' ? post(kMsgClip, 0, 0) : post(kMsgSnap, vk, shift ? 1 : 0))) return false;
            g_winComboTick = now;
            post(kMsgWinCombo, 0, 0);
            MaskKeys();
            g_swallowedUp = vk;
            return true;
        }
    }
    return false;
}

} // namespace aether::shellkeys
