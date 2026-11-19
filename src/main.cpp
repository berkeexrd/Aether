#include <windows.h>
#include <algorithm>
#include <shellapi.h>
#include <timeapi.h>
#include <dwmapi.h>
#include <winrt/base.h>

#pragma comment(lib, "winmm.lib")
#include "aether/core/log.hpp"
#include "aether/core/crash.hpp"
#include "aether/core/single_instance.hpp"
#include "aether/core/timer.hpp"
#include "aether/core/frame_recorder.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include "aether/system/wallpaper.hpp"
#include <functional>
#include <vector>
#include <algorithm>
#include "aether/core/events.hpp"
#include "aether/config/config_manager.hpp"
#include "aether/system/metrics_collector.hpp"
#include "aether/system/gaming_mode.hpp"
#include "aether/system/taskbar_service.hpp"
#include "aether/media/media_service.hpp"
#include "aether/island/island_window.hpp"
#include "aether/system/notify_cfg.hpp"
#include "aether/bar/notifications.hpp"
#include "aether/widgets/widget_window.hpp"
#include "aether/ui/settings_window.hpp"

using namespace aether;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    (void)hInstance;
    aether::core::InstallCrashHandler(L"Aether");   // mini dump + crash.log in %APPDATA%\Aether\crash if the island ever dies

    // Enable 1ms multimedia timer resolution for high refresh rates (144Hz / 240Hz)
    timeBeginPeriod(1);

    // Enable Per-Monitor V2 DPI Awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // One Aether at a time: a second launch would stack a second island and a second set of cards on top of the first.
    // A recording (--record) takes over instead: it asks the running copy to quit, records, then starts a normal copy again.
    const bool recordRun = core::ParseRecordArgs().on;
    bool tookOver = false;
    core::OpenAcl acl;   // open DACL: a copy that runs elevated and one that does not can still find each other's events / mutex
    HANDLE quitEvt = CreateEventW(acl.Get(), TRUE, FALSE, L"Local\\AetherMainQuit");
    // "--settings [desktop]" from another program (the desktop's Aether icon, a shortcut): the running copy opens its Settings Center
    HANDLE settingsEvt = CreateEventW(acl.Get(), FALSE, FALSE, L"Local\\AetherShowSettings");
    HANDLE settingsDeskEvt = CreateEventW(acl.Get(), FALSE, FALSE, L"Local\\AetherShowSettingsDesktop");
    const bool wantSettings = wcsstr(GetCommandLineW(), L"--settings") != nullptr;
    const bool wantDesktopPage = wantSettings && wcsstr(GetCommandLineW(), L"desktop") != nullptr;
    HANDLE once = CreateMutexW(acl.Get(), FALSE, L"Local\\AetherMainInstance");
    {
        const bool exists = GetLastError() == ERROR_ALREADY_EXISTS;
        if (exists) {
            if (!recordRun) {
                if (wantSettings) { HANDLE e = wantDesktopPage ? settingsDeskEvt : settingsEvt; if (e) SetEvent(e); }
                return 0;
            }
            if (quitEvt) SetEvent(quitEvt);
        }
        const DWORD w = once ? WaitForSingleObject(once, exists ? 10000 : 0) : WAIT_FAILED;
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 0;
        if (exists) { if (quitEvt) ResetEvent(quitEvt); tookOver = true; }
    }
    if (core::ParseRecordArgs().on) i18n::Mode() = 1;   // showcase recordings are always in English

    // log level: Warning in Release (see log.hpp), everything in Debug builds
#ifndef NDEBUG
    core::Logger::Instance().SetMinLevel(core::LogLevel::Debug);
#endif
    AETHER_LOG_INFO("=================================================");
    AETHER_LOG_INFO("     PROJECT AETHER - DESKTOP CUSTOMIZATION      ");
    AETHER_LOG_INFO("=================================================");

    try {
        // Initialize WinRT & COM
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        AETHER_LOG_DEBUG("COM / WinRT apartment initialized");

        // 1. Initialize Configuration Manager
        config::ConfigManager::Instance().Initialize();
        const auto& appConfig = config::ConfigManager::Instance().Get();
        AETHER_LOG_DEBUG("ConfigManager initialized");

        // 2. Initialize Hardware Services
        system::TaskbarService::Instance().Initialize();
        if (appConfig.taskbar.enabled) {
            system::TaskbarService::Instance().ApplyStyle(appConfig.taskbar.style, appConfig.taskbar.tintColor);
        }
        AETHER_LOG_DEBUG("TaskbarService initialized");

        system::MetricsCollector::Instance().Start(1000);
        AETHER_LOG_DEBUG("MetricsCollector initialized");

        system::GamingModeDetector::Instance().Start(1500);
        AETHER_LOG_DEBUG("GamingModeDetector initialized");

        media::MediaService::Instance().Initialize();
        AETHER_LOG_DEBUG("MediaService initialized");

        // 3. Create Dynamic Island Window
        auto island = std::make_unique<island::IslandWindow>();
        if (!island->Create()) {
            AETHER_LOG_ERROR("Could not create IslandWindow");
        }
        else island->PlayIntro();
        AETHER_LOG_DEBUG("IslandWindow initialized");

        // 4. Create Desktop Widgets
        // Always create the cards: the master switch (and each card's own switch) only shows or hides them. Skipping creation
        // when it was off at start-up meant switching it on later did nothing until the next restart.
        widgets::WidgetManager::Instance().Initialize();
        AETHER_LOG_DEBUG("WidgetManager initialized");

        // 5. Create Settings Center Window
        if (!ui::SettingsWindow::Instance().Create()) {
            AETHER_LOG_ERROR("Could not create SettingsWindow");
        }
        AETHER_LOG_DEBUG("SettingsWindow initialized");
        if (!config::ConfigManager::Instance().Get().firstRunDone) ui::SettingsWindow::Instance().Show(true);   // first-run setup page

    // Subscribe to Gaming Mode state changes
    core::EventBus::Instance().Subscribe<system::GamingModeChangedEvent>([&](const system::GamingModeChangedEvent& evt) {
        if (evt.isGamingActive) {
            AETHER_LOG_INFO("Game detected! Gracefully sliding out non-essential overlays...");
            const auto& gameCfg = config::ConfigManager::Instance().Get().gaming;
            if (gameCfg.action == config::GamingModeAction::HideCompletely) {
                island->SetGamingSuspended(true);
            }
            if (gameCfg.pauseWidgets) {
                widgets::WidgetManager::Instance().SetVisible(false);
            }
        } else {
            AETHER_LOG_INFO("Exited game! Gracefully sliding in desktop customization...");
            island->SetGamingSuspended(false);
            widgets::WidgetManager::Instance().SetVisible(true);
        }
    });

    // Register Global Hotkey (F9) to toggle Island
    RegisterHotKey(nullptr, 101, MOD_NOREPEAT, VK_F9);

    // Check command line arguments for --settings
    int numArgs = 0;
    LPWSTR* argList = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    if (argList) {
        for (int i = 0; i < numArgs; ++i) {
            if (wcscmp(argList[i], L"--settings") == 0) {
                const bool desk = i + 1 < numArgs && _wcsicmp(argList[i + 1], L"desktop") == 0;
                if (desk) ui::SettingsWindow::Instance().ShowTab(ui::SettingsTab::Desktop); else ui::SettingsWindow::Instance().Show(true);
            }
        }
        LocalFree(argList);
    }

    AETHER_LOG_INFO("Entering high-efficiency 240Hz message loop...");
    timeBeginPeriod(1);
    core::Timer timer;
    bool running = true;
    // --record <folder>: frame-by-frame showcase capture on a virtual clock (see frame_recorder.hpp)
    core::FrameRecorder recorder;
    const core::RecordOptions recOpt = core::ParseRecordArgs();
    const bool recording = recOpt.on && recorder.Start(recOpt);
    if (recording) widgets::WidgetManager::Instance().SetFrameByFrame(true);
    if (recording) {
        // back to a clean desktop: minimise every normal application window (ours, the console, the browser, ...)
        EnumWindows([](HWND h, LPARAM) -> BOOL {
            if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) return TRUE;
            const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE), ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            if ((st & WS_CAPTION) != WS_CAPTION || (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))) return TRUE;
            BOOL cloaked = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (!cloaked) ShowWindowAsync(h, SW_MINIMIZE);
            return TRUE;
        }, 0);
    }
    if (recording) Sleep(3000);   // let transient windows (console minimising, taskbar animations) settle before the first frame
    // Scripted tour. Steps that only show Aether's own windows run frame-locked (every frame = 1/fps); steps that open other
    // programs run in real time and each frame is stored with its real duration, so nothing is ever sped up or dropped.
    struct TourStep { double dur; bool realtime; std::function<void()> begin; std::function<void()> end; std::function<void()> at{}; double atT{0.0}; bool atDone{false}; };   // 'at' runs once atT seconds into the step
    std::vector<TourStep> tour;
    size_t tourIdx = 0; double stepT = 0.0, videoT = 0.0; bool stepStarted = false;
    LARGE_INTEGER qFreq{}, qLast{};
    QueryPerformanceFrequency(&qFreq); QueryPerformanceCounter(&qLast);
    if (recording) {
        render::glass::SyncTheme();
        const int origTheme = render::glass::ThemeIndex();
        const std::wstring origWall = aether::system::wallpaper::Current();
        wchar_t selfPath[MAX_PATH]; GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        std::wstring exeDir = selfPath; exeDir = exeDir.substr(0, exeDir.find_last_of(L'\\'));
        auto launch = [exeDir](const wchar_t* name) {
            const std::wstring p = exeDir + L"\\" + name;
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{}; std::wstring cl = L"\"" + p + L"\"";
            if (CreateProcessW(p.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, exeDir.c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        };
        auto closeWindow = [](const wchar_t* cls) { if (HWND h = FindWindowW(cls, nullptr)) PostMessageW(h, WM_CLOSE, 0, 0); };
        auto key = [](WORD vk, bool up) { INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; SendInput(1, &in, sizeof(in)); };
        auto tap = [key](WORD vk) { key(vk, false); key(vk, true); };
        auto moveToWallpaper = []() {   // empty wallpaper: clear of icons (left), clock (top) and cards (right); real mouse-move first so the window learns the position
            const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
            const int x = static_cast<int>(w * 0.46), y = static_cast<int>(h * 0.60);
            SetCursorPos(x, y);
            INPUT mv{}; mv.type = INPUT_MOUSE; mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            mv.mi.dx = x * 65535 / std::max(1, w - 1); mv.mi.dy = y * 65535 / std::max(1, h - 1);
            SendInput(1, &mv, sizeof(INPUT));
        };
        auto dismissMenu = [key]() {   // Esc is not enough for the desktop's own menu: also click on empty wallpaper well away from it
            // The menu polls the key / button state (GetAsyncKeyState), so a press that is over before its next poll goes unnoticed: hold it a little.
            key(VK_ESCAPE, false); Sleep(120); key(VK_ESCAPE, true);
            const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
            const int x = static_cast<int>(w * 0.25), y = static_cast<int>(h * 0.50);
            SetCursorPos(x, y);
            INPUT mv{}; mv.type = INPUT_MOUSE; mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE; mv.mi.dx = x * 65535 / std::max(1, w - 1); mv.mi.dy = y * 65535 / std::max(1, h - 1);
            SendInput(1, &mv, sizeof(INPUT));
            INPUT dn{}; dn.type = INPUT_MOUSE; dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; SendInput(1, &dn, sizeof(INPUT));
            Sleep(120);
            INPUT up{}; up.type = INPUT_MOUSE; up.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &up, sizeof(INPUT));
        };
        auto rightClick = []() {
            INPUT in[2] = {}; in[0].type = in[1].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            SendInput(2, in, sizeof(INPUT));
        };
        // Pinned while a showcase scene shows an expanded state: without a mouse over the island it would collapse again after a split second.
        auto mode = [&island](island::IslandMode md) { return [&island, md] { if (island) { island->SetPinned(md != island::IslandMode::Idle); island->SetMode(md); } }; };
        // ---- promo-video helpers (the tour is also what tools\record_promo.ps1 films) -------------------------------------------------
        auto demoFlyout = [](int id) {   // every bar gets it (one per monitor); only the primary one answers
            return [id] { for (HWND b = nullptr; (b = FindWindowExW(nullptr, b, L"AetherBarClass", nullptr)) != nullptr;) PostMessageW(b, WM_APP + 30 /*AetherBar::kMsgDemoFlyout*/, static_cast<WPARAM>(id), 0); };
        };
        auto focusWindow = [](const wchar_t* cls) {
            HWND h = FindWindowW(cls, nullptr);
            if (!h) return;
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
            if (fg && fg != me) AttachThreadInput(me, fg, TRUE);
            BringWindowToTop(h); SetForegroundWindow(h);
            if (fg && fg != me) AttachThreadInput(me, fg, FALSE);
        };
        auto typeText = [](const wchar_t* s) {
            for (; *s; ++s) {
                INPUT in[2] = {}; in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wScan = in[1].ki.wScan = *s; in[0].ki.dwFlags = KEYEVENTF_UNICODE; in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
            }
        };
        auto chord = [key](WORD mod, WORD vk) { key(mod, false); key(vk, false); key(vk, true); key(mod, true); };
        auto shellCtl = []() { return FindWindowExW(HWND_MESSAGE, nullptr, L"AetherShellControl", nullptr); };
        tour.push_back({4.0, false, nullptr, nullptr});                                                      // start-up: island drops in, cards rise
        tour.push_back({3.0, false, mode(island::IslandMode::MediaExpanded), nullptr});
        tour.push_back({3.0, false, mode(island::IslandMode::SystemExpanded), nullptr});
        tour.push_back({1.2, false, mode(island::IslandMode::Idle), nullptr});
        tour.push_back({3.4, false, [&island] {
            island::NotificationData nd; nd.title = L"Aether"; nd.message = L"Notifications pop out of the island"; nd.sourceApp = L"Aether"; nd.timeRemaining = 3.0;
            if (island) island->PushNotification(nd); }, nullptr});
        tour.push_back({2.6, true, demoFlyout(1), demoFlyout(0)});                                                   // quick settings
        tour.push_back({2.6, true, demoFlyout(2), demoFlyout(0)});                                                   // calendar
        tour.push_back({2.4, true, demoFlyout(3), demoFlyout(0)});                                                   // tray
        tour.push_back({2.6, true, demoFlyout(4), demoFlyout(0)});                                                   // weather
        tour.push_back({1.7, true, moveToWallpaper, nullptr, demoFlyout(6), 0.5});   // Start menu (opened by message: an injected Windows key would open Windows' own Start)
        { const wchar_t* q = L"settings"; for (const wchar_t* c = q; *c; ++c) { const wchar_t ch = *c; tour.push_back({0.16, true, [ch] { if (HWND s = FindWindowW(L"AetherStartMenuClass", nullptr)) PostMessageW(s, WM_CHAR, ch, 0); }, nullptr}); } }   // typed straight into the Start menu's search box
        tour.push_back({1.5, true, nullptr, demoFlyout(7)});
        tour.push_back({3.6, true, moveToWallpaper, dismissMenu, rightClick, 0.8});
        tour.push_back({0.9, true, nullptr, nullptr});                                                              // let the menu fade out before the next scene                                          // desktop context menu
        // Settings Center: every main page for a moment
        {
            const ui::SettingsTab tabs[] = {ui::SettingsTab::General, ui::SettingsTab::DynamicIsland, ui::SettingsTab::Widgets, ui::SettingsTab::Taskbar, ui::SettingsTab::Desktop, ui::SettingsTab::Appearance};
            for (size_t i = 0; i < std::size(tabs); ++i) {
                const ui::SettingsTab tb = tabs[i];
                std::function<void()> endFn;
                if (i + 1 == std::size(tabs)) endFn = [] { ui::SettingsWindow::Instance().Show(false); };
                tour.push_back({i == 0 ? 2.2 : 1.7, true, [tb] { ui::SettingsWindow::Instance().ShowTab(tb); }, endFn});
            }
        }
        // Run box (Win+R), typed letter by letter
        tour.push_back({1.0, true, [=] { if (HWND c = shellCtl()) PostMessageW(c, WM_APP + 6 /*shellkeys::kMsgRun*/, 0, 0); }, nullptr});
        { const wchar_t* q = L"notepad"; for (const wchar_t* c = q; *c; ++c) { const wchar_t one[2] = {*c, 0}; std::wstring s1 = one; tour.push_back({0.17, true, [=] { typeText(s1.c_str()); }, nullptr}); } }
        tour.push_back({1.1, true, nullptr, [=] { tap(VK_ESCAPE); }});
        // Explorer: thumbnails, grid, tabs and a big copy with the progress card
        {
            wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
            const std::wstring base = std::wstring(tmp) + L"AetherDemo", src = base + L"\\Wallpapers", dst = base + L"\\Backup";
            tour.push_back({2.6, true, [=] {
                CreateDirectoryW(base.c_str(), nullptr); CreateDirectoryW(src.c_str(), nullptr); CreateDirectoryW(dst.c_str(), nullptr);
                WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((exeDir + L"\\wallpapers\\*.jpg").c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE) { do CopyFileW((exeDir + L"\\wallpapers\\" + fd.cFileName).c_str(), (src + L"\\" + fd.cFileName).c_str(), FALSE); while (FindNextFileW(h, &fd)); FindClose(h); }
                HANDLE big = CreateFileW((src + L"\\Project Aether (raw footage).mov").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (big != INVALID_HANDLE_VALUE) { LARGE_INTEGER sz; sz.QuadPart = 3LL * 1024 * 1024 * 1024; SetFilePointerEx(big, sz, nullptr, FILE_BEGIN); SetEndOfFile(big); CloseHandle(big); }
                std::wstring cl = L"\"" + exeDir + L"\\AetherExplorer.exe\" \"" + src + L"\"";
                STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, 0, nullptr, exeDir.c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
            }, nullptr});
            tour.push_back({2.0, true, [=] { focusWindow(L"AetherExplorerWindow"); chord(VK_CONTROL, '2'); }, nullptr});          // grid with thumbnails
            tour.push_back({1.6, true, [=] { chord(VK_CONTROL, '1'); }, nullptr});                                               // back to the list
            tour.push_back({1.4, true, [=] { chord(VK_CONTROL, 'A'); }, nullptr});                                               // select everything
            tour.push_back({0.6, true, [=] { chord(VK_CONTROL, 'C'); chord(VK_CONTROL, 'T'); }, nullptr});                       // copy, new tab
            tour.push_back({0.5, true, [=] { chord(VK_CONTROL, 'L'); }, nullptr});
            tour.push_back({0.9, true, [=] { typeText(dst.c_str()); tap(VK_RETURN); }, nullptr});                                // ... in the Backup folder
            tour.push_back({6.5, true, [=] { chord(VK_CONTROL, 'V'); }, nullptr});                                               // paste: progress card
            tour.push_back({0.8, true, nullptr, [=] { closeWindow(L"AetherExplorerWindow"); }});
            tour.push_back({0.6, true, [=] {
                std::wstring from = base + L"\\"; from.push_back(0); from.push_back(0);
                SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
                SHFileOperationW(&op); }, nullptr});
        }
        tour.push_back({4.0, true, [=] { launch(L"AetherBin.exe"); }, [=] { closeWindow(L"AetherBinWindow"); }});
        int themeCount = 0; render::glass::Themes(&themeCount);
        for (int i = 0; i < themeCount; ++i)                                                                       // every colour theme, to finish
            tour.push_back({2.2, true, [i] {
                render::glass::SetTheme(i); render::glass::SaveThemeFile(i);
                if (config::ConfigManager::Instance().Get().autoWallpaper) aether::system::wallpaper::ApplyForTheme(i); }, nullptr});
        tour.push_back({1.5, true, [=] {                                                                           // back to the user's own look
            render::glass::SetTheme(origTheme); render::glass::SaveThemeFile(origTheme);
            if (!origWall.empty()) aether::system::wallpaper::SetFile(origWall); }, nullptr});
        // AETHER_TOUR=extra (tools\record_promo.ps1 -Tour extra): only the island and the bar's flyouts, for a second pass of the promo footage
        wchar_t tourName[32] = {};
        if (GetEnvironmentVariableW(L"AETHER_TOUR", tourName, 32) && !wcscmp(tourName, L"extra")) {
            tour.clear();
            tour.push_back({1.2, false, nullptr, nullptr});
            tour.push_back({3.2, false, mode(island::IslandMode::MediaExpanded), nullptr});
            tour.push_back({3.2, false, mode(island::IslandMode::SystemExpanded), nullptr});
            tour.push_back({1.6, false, mode(island::IslandMode::Idle), nullptr});
            tour.push_back({3.0, true, demoFlyout(1), demoFlyout(0)});    // quick settings
            tour.push_back({3.0, true, demoFlyout(2), demoFlyout(0)});    // calendar
            tour.push_back({2.6, true, demoFlyout(3), demoFlyout(0)});    // tray
            tour.push_back({3.0, true, demoFlyout(4), demoFlyout(0)});    // weather
            tour.push_back({1.0, true, nullptr, nullptr});
        }
    }
    float frameDur = 0.0f;
    double lastMetricsTick = 0.0;
    double lastClockTick = 0.0;
    double lastNotifPoll = 0.0;
    long long lastNotifId = -1;   // -1: first poll only learns what already exists, nothing old is replayed

    while (running) {
        MSG msg;
        // Process all pending Windows messages
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }

            // Global Hotkey (F9)
            if (msg.message == WM_HOTKEY && msg.wParam == 101) {
                if (island) {
                    if (island->GetMode() == island::IslandMode::Idle) {
                        island->SetMode(island::IslandMode::MediaExpanded);
                    } else if (island->GetMode() == island::IslandMode::MediaExpanded) {
                        island->SetMode(island::IslandMode::SystemExpanded);
                    } else {
                        island->SetMode(island::IslandMode::Idle);
                    }
                }
            }

            // Message from context menu: Open Settings
            if (msg.message == WM_USER + 100) {
                ui::SettingsWindow::Instance().Show(true);
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!recording && quitEvt && WaitForSingleObject(quitEvt, 0) == WAIT_OBJECT_0) break;   // a recording run asked this copy to step aside
        if (settingsEvt && WaitForSingleObject(settingsEvt, 0) == WAIT_OBJECT_0) ui::SettingsWindow::Instance().Show(true);
        if (settingsDeskEvt && WaitForSingleObject(settingsDeskEvt, 0) == WAIT_OBJECT_0) ui::SettingsWindow::Instance().ShowTab(ui::SettingsTab::Desktop);
        if (!running) break;

        float dt = static_cast<float>(timer.Tick());
        double now = timer.Now();
        if (recording) {
            LARGE_INTEGER qNow; QueryPerformanceCounter(&qNow);
            const double real = std::clamp(static_cast<double>(qNow.QuadPart - qLast.QuadPart) / static_cast<double>(qFreq.QuadPart), 0.004, 0.25);
            qLast = qNow;
            if (tourIdx >= tour.size()) {
                recorder.ForceDone();
                dt = recorder.Dt();
            } else {
                TourStep& st = tour[tourIdx];
                if (!stepStarted) { stepStarted = true; stepT = 0.0; if (st.begin) st.begin(); }
                dt = st.realtime ? static_cast<float>(real) : recorder.Dt();
                if (st.at && !st.atDone && stepT >= st.atT) { st.atDone = true; st.at(); }
                stepT += dt;
                if (stepT >= st.dur) { if (st.end) st.end(); ++tourIdx; stepStarted = false; }
            }
            frameDur = dt;
            videoT += dt;
            now = videoT;
        }

        // Advance media track timeline
        media::MediaService::Instance().Update(dt);

        const bool widgetsAnimating = widgets::WidgetManager::Instance().IsAnimating();
        bool isAnimating = recording || (island && !island->IsResting()) || ui::SettingsWindow::Instance().IsVisible() || widgetsAnimating;

        // Update & Render Island:
        // If animating (springs in motion, mouse hovering, music playing), update and render every frame!
        // If resting, update at 1Hz on clock tick to update HH:MM.
        if (island) {
            if (recording || isAnimating || (now - lastClockTick >= 1.0)) {
                if (!isAnimating) lastClockTick = now;
                island->Update(dt);
                island->Render();
            }
        }

        // Windows toasts (messages, calls and other alerts, including phones linked through Phone Link) pop out of the island.
        if (island && !recording && now - lastNotifPoll >= 2.0) {
            lastNotifPoll = now;
            auto& feed = bar::NotificationFeed::Instance();
            feed.RequestRefresh();
            const auto items = feed.Items();
            long long maxId = lastNotifId;
            const bool quiet = system::GamingModeDetector::Instance().IsGaming() &&
                               config::ConfigManager::Instance().Get().gaming.suppressNotifications;
            const bar::NotifItem* fresh = nullptr;
            for (const auto& it : items) {
                maxId = std::max(maxId, it.id);
                if (lastNotifId >= 0 && it.id > lastNotifId && (!fresh || it.id > fresh->id)) fresh = &it;
            }
            if (fresh && !quiet && system::notifycfg::IslandAlso()) {
                island::NotificationData nd;
                nd.sourceApp = fresh->app;
                nd.title = fresh->title.empty() ? fresh->app : fresh->title;
                nd.message = fresh->body;
                nd.timeRemaining = config::ConfigManager::Instance().Get().island.notificationDismissSeconds;
                island->PushNotification(nd);
            }
            if (!items.empty() || lastNotifId >= 0) lastNotifId = std::max(lastNotifId, maxId < 0 ? 0 : maxId);
        }

        // Update widgets at adaptive 1Hz or on demand
        if (recording || widgetsAnimating || now - lastMetricsTick >= 1.0) {
            lastMetricsTick = now;
            widgets::WidgetManager::Instance().Update(dt);
            widgets::WidgetManager::Instance().Render();
        }

        // Render Settings Window if open
        if (ui::SettingsWindow::Instance().IsVisible()) {
            ui::SettingsWindow::Instance().Render();
        }

        if (recording) {
            recorder.Capture(frameDur);
            if (recorder.Done()) { recorder.Finish(); running = false; break; }
            continue;   // no sleeping: the clock is virtual
        }

        // 240Hz Fluid Pacing:
        // While animating: DO NOT SLEEP! The composition swap chain's Present(1, 0)
        // synchronizes directly to the monitor's hardware V-Blank (~4.16ms on 240Hz).
        // Only sleep when 100% idle and resting to yield 0% CPU.
        const bool settingsOnly = ui::SettingsWindow::Instance().IsVisible() && !recording && !widgetsAnimating && !(island && !island->IsResting());
        if (!isAnimating) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else if (settingsOnly && ui::SettingsWindow::Instance().IsIdle()) {
            // The settings window is open but nobody has touched it for a while and nothing else moves:
            // ~12 fps keeps caret blink / live values alive without redrawing at the monitor's refresh rate. Any input wakes the loop at once.
            MsgWaitForMultipleObjectsEx(0, nullptr, 80, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else if (island && !island->IsSpringDriven()) {
            // Only ambient motion (audio bars, scrolling title): 60 fps is plenty and saves GPU/CPU.
            // With the settings window open, ~120 fps keeps it fluid without redrawing at the monitor's full rate.
            MsgWaitForMultipleObjectsEx(0, nullptr, ui::SettingsWindow::Instance().IsVisible() ? 7 : 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    AETHER_LOG_INFO("Shutting down Aether gracefully...");

    // Clean shutdown
    timeEndPeriod(1);
    ui::SettingsWindow::Instance().Destroy();
    widgets::WidgetManager::Instance().Shutdown();
    island.reset();

    media::MediaService::Instance().Shutdown();
    system::GamingModeDetector::Instance().Stop();
    system::MetricsCollector::Instance().Stop();
    system::TaskbarService::Instance().RestoreDefaults();

    UnregisterHotKey(nullptr, 101);
    winrt::uninit_apartment();
    AETHER_LOG_INFO("Aether shutdown complete.");
    if (tookOver) {   // bring the normal Aether back after a recording
        if (once) { ReleaseMutex(once); CloseHandle(once); once = nullptr; }
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring cl = L"\"" + std::wstring(self) + L"\"";
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        if (CreateProcessW(self, cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    return 0;
    } catch (const std::exception& e) {
        AETHER_LOG_ERROR("Unhandled exception in wWinMain: " << e.what());
        return 1;
    } catch (const winrt::hresult_error& he) {
        AETHER_LOG_ERROR("WinRT hresult error: " << winrt::to_string(he.message()));
        return 1;
    } catch (...) {
        AETHER_LOG_ERROR("Unknown fatal crash in wWinMain");
        return 1;
    }
}
