// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pch.h"
#include <shellapi.h>
#include <interface/powertoy_module_interface.h>
#include <common/SettingsAPI/settings_objects.h>
#include <common/utils/logger_helper.h>
#include <common/utils/process_path.h>
#include <common/logger/logger.h>
#include "trace.h"
#include "resource.h"
#include "DwellClickCore.h"
#include "DwellClickOverlay.h"

#include <atomic>
#include <cmath>
#include <thread>

// Dwell Click
//
// Issues a mouse click automatically when the pointer is held still for a configurable time,
// for people who can move a pointer but cannot reliably click it. The decision logic lives in
// DwellClickCore.h (Win32-free and unit tested); this file is the thin Win32 adapter around it:
// a low-level mouse hook feeds the engine pointer moves and physical clicks, a polling loop on
// the same thread advances the countdown, and a SendInput wrapper performs the injections the
// engine asks for. The on-screen surfaces (a countdown ring at the pointer and a dwell-to-select
// action toolbar, modeled on Ease Mouse and OpenMouse) live in DwellClickOverlay.cpp, with their
// decisions in the Win32-free DwellClickToolbar.h.
//
// The hook lives on a dedicated thread with its own message pump, matching the other Mouse
// Utilities (see MouseButtonLock and CursorWrap). All engine calls happen on that thread while
// it runs; the runner thread only touches the engine before the thread starts (enable) or
// after it joins (disable), and requests a re-lock via an atomic flag otherwise (set_config).

extern "C" IMAGE_DOS_HEADER __ImageBase;

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        Trace::RegisterProvider();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        Trace::UnregisterProvider();
        break;
    }
    return TRUE;
}

// Non-localizable strings.
namespace
{
    const wchar_t JSON_KEY_PROPERTIES[] = L"properties";
    const wchar_t JSON_KEY_VALUE[] = L"value";
    const wchar_t JSON_KEY_DWELL_TIME_MS[] = L"dwell_time_ms";
    const wchar_t JSON_KEY_MOVE_TOLERANCE_PIXELS[] = L"move_tolerance_pixels";
    const wchar_t JSON_KEY_POST_ACTION_TOLERANCE_PIXELS[] = L"post_action_tolerance_pixels";
    const wchar_t JSON_KEY_DEFAULT_ACTION[] = L"default_action";
    const wchar_t JSON_KEY_REVERT_TO_DEFAULT_AFTER_ACTION[] = L"revert_to_default_after_action";
    const wchar_t JSON_KEY_SHOW_TOOLBAR[] = L"show_toolbar";
    const wchar_t JSON_KEY_TOOLBAR_SIDE[] = L"toolbar_side";
    const wchar_t JSON_KEY_SHOW_COUNTDOWN[] = L"show_countdown";
    const wchar_t JSON_KEY_OVERLAY_SIZE[] = L"overlay_size";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_LEFT_CLICK[] = L"toolbar_button_left_click";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_DOUBLE_CLICK[] = L"toolbar_button_double_click";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_RIGHT_CLICK[] = L"toolbar_button_right_click";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_MIDDLE_CLICK[] = L"toolbar_button_middle_click";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_DRAG[] = L"toolbar_button_drag";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_SCROLL_UP[] = L"toolbar_button_scroll_up";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_SCROLL_DOWN[] = L"toolbar_button_scroll_down";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_OPEN_SETTINGS[] = L"toolbar_button_open_settings";

    // dwExtraInfo tag stamped on every event we inject via SendInput, so the hook ignores our
    // own synthetic clicks: a dwell-fired click must not register as the user clicking
    // physically, or every fire would immediately re-lock against its own echo. The magic
    // value spells the module's initials in ASCII (D, W, L, C); distinct from MouseButtonLock's
    // tag and the centralized keyboard hook's flag.
    constexpr ULONG_PTR INJECTION_TAG = 0x44574C43;

    // How often the poll loop advances the countdown while the pointer is at rest. Fine enough
    // that a fired click lands within about a frame of the configured dwell time, and the same
    // cadence the future countdown indicator will want for smooth progress.
    constexpr DWORD POLL_INTERVAL_MS = 15;

    // Default values mirror the engine's Settings defaults (see DwellClickCore.h for their
    // provenance); the eventual C# DwellClickProperties must match both.
    constexpr int DEFAULT_DWELL_TIME_MS = 1200;
    constexpr int DEFAULT_TOLERANCE_PIXELS = 10;

    // Accepted ranges when reading from settings.json. The Settings UI will constrain these,
    // but a hand-edited file could carry out-of-range or non-finite values, so clamp on read.
    // A dwell time under 100 ms fires on virtually every rest (the Midas touch failure the
    // engine is built to avoid); the engine itself stays defined down to 1 ms for tests.
    constexpr int MIN_DWELL_TIME_MS = 100;
    constexpr int MAX_DWELL_TIME_MS = 60000;
    constexpr int MAX_TOLERANCE_PIXELS = 10000;

    // Production injector: a tagged SendInput. Lives behind the engine's IClickInjector so the
    // state machine can be unit tested without Win32.
    class WinInjector : public dwellclick::IClickInjector
    {
    public:
        bool Inject(dwellclick::ClickKind kind, dwellclick::PointL /*pt*/) override
        {
            // The point is deliberately unused: the engine only fires while the pointer is
            // resting at that point, so injecting button events at the live cursor position
            // clicks the same spot. Injecting a MOUSEEVENTF_MOVE instead could fight other
            // software steering the pointer (a head or eye tracker, exactly this module's
            // audience).
            INPUT inputs[4] = {};
            UINT count = 0;
            auto add = [&](DWORD flag) {
                inputs[count].type = INPUT_MOUSE;
                inputs[count].mi.dwFlags = flag;
                inputs[count].mi.dwExtraInfo = INJECTION_TAG;
                ++count;
            };

            switch (kind)
            {
            case dwellclick::ClickKind::ScrollUp:
            case dwellclick::ClickKind::ScrollDown:
                // One wheel notch at the pointer; sign selects the direction.
                add(MOUSEEVENTF_WHEEL);
                inputs[0].mi.mouseData = kind == dwellclick::ClickKind::ScrollUp ? WHEEL_DELTA : static_cast<DWORD>(-WHEEL_DELTA);
                break;
            case dwellclick::ClickKind::RightClick:
                add(MOUSEEVENTF_RIGHTDOWN);
                add(MOUSEEVENTF_RIGHTUP);
                break;
            case dwellclick::ClickKind::MiddleClick:
                add(MOUSEEVENTF_MIDDLEDOWN);
                add(MOUSEEVENTF_MIDDLEUP);
                break;
            case dwellclick::ClickKind::DoubleClick:
                // Two rapid pairs in one SendInput batch; the OS turns them into a double
                // click because they land well inside the double-click time and rectangle.
                add(MOUSEEVENTF_LEFTDOWN);
                add(MOUSEEVENTF_LEFTUP);
                add(MOUSEEVENTF_LEFTDOWN);
                add(MOUSEEVENTF_LEFTUP);
                break;
            case dwellclick::ClickKind::LeftDown:
                add(MOUSEEVENTF_LEFTDOWN);
                break;
            case dwellclick::ClickKind::LeftUp:
                add(MOUSEEVENTF_LEFTUP);
                break;
            case dwellclick::ClickKind::LeftClick:
            default:
                add(MOUSEEVENTF_LEFTDOWN);
                add(MOUSEEVENTF_LEFTUP);
                break;
            }

            if (SendInput(count, inputs, sizeof(INPUT)) == count)
            {
                return true;
            }
            Logger::warn(L"Failed to inject a dwell click; the target window may be elevated above this process.");
            return false;
        }
    };
}

// The PowerToy name that will be shown in the settings.
const static wchar_t* MODULE_NAME = L"DwellClick";

// Forward declaration so the static hook proc can reach the singleton instance.
class DwellClick;
// Atomic because the static hook proc (on the hook thread) reads it while the ctor/destroy
// (on the runner thread) write it. destroy() still unhooks and joins the hook thread before
// clearing this and deleting the object, so a non-null load in the proc stays valid.
static std::atomic<DwellClick*> g_instance{ nullptr };

// Implement the PowerToy Module Interface and all the required methods.
class DwellClick : public PowertoyModuleIface
{
private:
    // The PowerToy enabled state (the whole module, driven by the runner). Atomic because the
    // runner's telemetry thread can call is_enabled() while enable()/disable() write it.
    std::atomic<bool> m_enabled{ false };

    // Settings. Read on the hook thread, written by set_config on the runner thread, so atomic.
    std::atomic<int> m_dwellTimeMs{ DEFAULT_DWELL_TIME_MS };
    std::atomic<int> m_moveTolerancePixels{ DEFAULT_TOLERANCE_PIXELS };
    std::atomic<int> m_postActionTolerancePixels{ DEFAULT_TOLERANCE_PIXELS };
    std::atomic<int> m_defaultAction{ static_cast<int>(dwellclick::DwellAction::LeftClick) };
    std::atomic<bool> m_revertToDefaultAfterAction{ true };
    std::atomic<bool> m_showToolbar{ true };
    std::atomic<int> m_toolbarSide{ 0 };
    std::atomic<bool> m_showCountdown{ true };
    std::atomic<int> m_overlaySize{ 1 }; // 0 = small, 1 = medium, 2 = large
    std::atomic<bool> m_toolbarButtonLeftClick{ true };
    std::atomic<bool> m_toolbarButtonDoubleClick{ true };
    std::atomic<bool> m_toolbarButtonRightClick{ true };
    std::atomic<bool> m_toolbarButtonMiddleClick{ false };
    std::atomic<bool> m_toolbarButtonDrag{ true };
    std::atomic<bool> m_toolbarButtonScrollUp{ true };
    std::atomic<bool> m_toolbarButtonScrollDown{ true };
    std::atomic<bool> m_toolbarButtonOpenSettings{ true };

    // The engine is not thread-safe, so set_config (runner thread) never touches it directly.
    // It raises this flag instead, and the hook thread's poll loop consumes it and applies the
    // engine's settings-change rule (lock until the pointer moves) within one poll interval.
    std::atomic<bool> m_relockRequested{ false };

    // The state machine. m_injector must be declared before m_engine (the engine binds a
    // reference to it in its constructor).
    WinInjector m_injector;
    dwellclick::Engine m_engine{ m_injector };

    // The on-screen surfaces (countdown ring + action toolbar). Created, driven, and
    // destroyed exclusively on the hook thread, like every other engine interaction.
    dwellclick::Overlay m_overlay;

    // Hook thread + lifecycle.
    HHOOK m_mouseHook = nullptr;
    HANDLE m_terminateEvent = nullptr;
    std::thread m_hookThread;
    std::atomic<bool> m_listening{ false };

    void init_settings();
    void parse_settings(PowerToysSettings::PowerToyValues& settings);
    dwellclick::Settings SettingsSnapshot() const;

    void HookThreadMain();
    void HandleMouseMessage(WPARAM wParam, const MSLLHOOKSTRUCT* data);
    void HandleToolbarCommand(dwellclick::ToolbarCommand command);

    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);

public:
    DwellClick()
    {
        LoggerHelpers::init_logger(MODULE_NAME, L"ModuleInterface", LogSettings::dwellClickLoggerName);
        init_settings();
        g_instance.store(this);
    }

    virtual void destroy() override
    {
        // Ensure the hook thread is torn down and any held drag released before deletion.
        disable();
        g_instance.store(nullptr);
        delete this;
    }

    virtual const wchar_t* get_name() override
    {
        return MODULE_NAME;
    }

    virtual const wchar_t* get_key() override
    {
        return MODULE_NAME;
    }

    virtual powertoys_gpo::gpo_rule_configured_t gpo_policy_enabled_configuration() override
    {
        return powertoys_gpo::getConfiguredDwellClickEnabledValue();
    }

    virtual bool get_config(wchar_t* buffer, int* buffer_size) override
    {
        HINSTANCE hinstance = reinterpret_cast<HINSTANCE>(&__ImageBase);

        PowerToysSettings::Settings settings(hinstance, get_name());
        settings.set_description(IDS_DWELLCLICK_NAME);

        return settings.serialize_to_buffer(buffer, buffer_size);
    }

    virtual void call_custom_action(const wchar_t* /*action*/) override {}

    virtual void set_config(const wchar_t* config) override
    {
        try
        {
            PowerToysSettings::PowerToyValues values =
                PowerToysSettings::PowerToyValues::from_json_string(config, get_key());

            parse_settings(values);

            // Any live settings change must lock the machine until the pointer moves:
            // shortening the dwell time against an already-running countdown would otherwise
            // fire the moment the new value lands. The hook thread applies it (see the flag).
            m_relockRequested.store(true);
        }
        catch (...)
        {
            // catch(...) because the JSON accessors throw winrt::hresult_error, which does not
            // derive from std::exception; a malformed payload must not escape and abort apply.
            Logger::error("Invalid json when trying to parse DwellClick settings json.");
        }
    }

    virtual void enable() override
    {
        m_enabled = true;
        Trace::EnableDwellClick(true);

        if (m_listening)
        {
            return;
        }

        // Seed the engine with the real cursor position, then clear transient state and lock.
        // Without the seed the anchor starts at (0,0), and the first tiny jiggle anywhere else
        // on screen would read as a deliberate move and arm the machine; with it, a pointer
        // left resting across a disable/enable cycle stays locked until it genuinely moves.
        // Safe to touch the engine here: the hook thread is not running yet.
        const uint64_t tick = GetTickCount64();
        POINT pt{};
        if (GetCursorPos(&pt))
        {
            m_engine.OnMove({ pt.x, pt.y }, tick, SettingsSnapshot());
        }
        m_engine.ResetTransient(tick);

        m_terminateEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_terminateEvent == nullptr)
        {
            // Without the terminate event the hook thread's shutdown wait breaks; don't start it.
            Logger::error(L"Failed to create DwellClick terminate event, error: {}. Hook not started.", GetLastError());
            return;
        }
        m_listening = true;
        m_hookThread = std::thread([this]() { HookThreadMain(); });
    }

    virtual void disable() override
    {
        m_enabled = false;
        Trace::EnableDwellClick(false);

        if (!m_listening)
        {
            return;
        }

        m_listening = false;
        if (m_terminateEvent)
        {
            SetEvent(m_terminateEvent);
        }
        if (m_hookThread.joinable())
        {
            m_hookThread.join();
        }
        if (m_terminateEvent)
        {
            CloseHandle(m_terminateEvent);
            m_terminateEvent = nullptr;
        }
    }

    virtual bool is_enabled() override
    {
        return m_enabled;
    }

    virtual bool is_enabled_by_default() const override
    {
        return false;
    }
};

void DwellClick::init_settings()
{
    try
    {
        PowerToysSettings::PowerToyValues settings =
            PowerToysSettings::PowerToyValues::load_from_settings_file(DwellClick::get_key());
        parse_settings(settings);
    }
    catch (...)
    {
        // catch(...) so winrt::hresult_error from the JSON accessors can't escape the constructor;
        // keep the defaults on any parse error.
        Logger::error("Invalid json when trying to load the DwellClick settings json from file.");
    }
}

void DwellClick::parse_settings(PowerToysSettings::PowerToyValues& settings)
{
    auto settingsObject = settings.get_raw_json();
    if (!settingsObject.GetView().Size() || !settingsObject.HasKey(JSON_KEY_PROPERTIES))
    {
        Logger::info("DwellClick settings are empty; keeping defaults.");
        return;
    }

    auto properties = settingsObject.GetNamedObject(JSON_KEY_PROPERTIES);

    auto readBool = [&](const wchar_t* key, std::atomic<bool>& target) {
        if (!properties.HasKey(key))
        {
            return;
        }
        try
        {
            target.store(properties.GetNamedObject(key).GetNamedBoolean(JSON_KEY_VALUE));
        }
        catch (...)
        {
            Logger::warn(L"Failed to read bool setting; keeping previous value.");
        }
    };

    auto readInt = [&](const wchar_t* key, std::atomic<int>& target, int minValue, int maxValue) {
        if (!properties.HasKey(key))
        {
            return;
        }
        try
        {
            // GetNamedNumber yields a double. NaN compares false against both bounds, so it would
            // slip past the range checks below and make static_cast<int> undefined; reject any
            // non-finite value outright and keep the previous value. Finite out-of-range values are
            // clamped BEFORE the cast so the conversion is always defined.
            double raw = properties.GetNamedObject(key).GetNamedNumber(JSON_KEY_VALUE);
            if (!std::isfinite(raw))
            {
                Logger::warn(L"Ignoring non-finite int setting; keeping previous value.");
                return;
            }
            if (raw < minValue)
            {
                raw = minValue;
            }
            else if (raw > maxValue)
            {
                raw = maxValue;
            }
            target.store(static_cast<int>(raw));
        }
        catch (...)
        {
            Logger::warn(L"Failed to read int setting; keeping previous value.");
        }
    };

    readInt(JSON_KEY_DWELL_TIME_MS, m_dwellTimeMs, MIN_DWELL_TIME_MS, MAX_DWELL_TIME_MS);
    readInt(JSON_KEY_MOVE_TOLERANCE_PIXELS, m_moveTolerancePixels, 0, MAX_TOLERANCE_PIXELS);
    readInt(JSON_KEY_POST_ACTION_TOLERANCE_PIXELS, m_postActionTolerancePixels, 0, MAX_TOLERANCE_PIXELS);
    readBool(JSON_KEY_REVERT_TO_DEFAULT_AFTER_ACTION, m_revertToDefaultAfterAction);
    readBool(JSON_KEY_SHOW_TOOLBAR, m_showToolbar);
    readInt(JSON_KEY_TOOLBAR_SIDE, m_toolbarSide, 0, 1);
    readBool(JSON_KEY_SHOW_COUNTDOWN, m_showCountdown);
    readInt(JSON_KEY_OVERLAY_SIZE, m_overlaySize, 0, 2);
    readBool(JSON_KEY_TOOLBAR_BUTTON_LEFT_CLICK, m_toolbarButtonLeftClick);
    readBool(JSON_KEY_TOOLBAR_BUTTON_DOUBLE_CLICK, m_toolbarButtonDoubleClick);
    readBool(JSON_KEY_TOOLBAR_BUTTON_RIGHT_CLICK, m_toolbarButtonRightClick);
    readBool(JSON_KEY_TOOLBAR_BUTTON_MIDDLE_CLICK, m_toolbarButtonMiddleClick);
    readBool(JSON_KEY_TOOLBAR_BUTTON_DRAG, m_toolbarButtonDrag);
    readBool(JSON_KEY_TOOLBAR_BUTTON_SCROLL_UP, m_toolbarButtonScrollUp);
    readBool(JSON_KEY_TOOLBAR_BUTTON_SCROLL_DOWN, m_toolbarButtonScrollDown);
    readBool(JSON_KEY_TOOLBAR_BUTTON_OPEN_SETTINGS, m_toolbarButtonOpenSettings);

    // The action is an enum stored as a number. An unknown value (a future action arriving via
    // a hand-edited or newer settings file) falls back to LeftClick, the least surprising
    // action, rather than clamping to whatever enum happens to sit at the range edge.
    if (properties.HasKey(JSON_KEY_DEFAULT_ACTION))
    {
        try
        {
            const double raw = properties.GetNamedObject(JSON_KEY_DEFAULT_ACTION).GetNamedNumber(JSON_KEY_VALUE);
            const bool known = raw >= static_cast<double>(dwellclick::DwellAction::LeftClick) &&
                               raw <= static_cast<double>(dwellclick::DwellAction::Drag) &&
                               raw == static_cast<double>(static_cast<int>(raw));
            m_defaultAction.store(known ? static_cast<int>(raw) : static_cast<int>(dwellclick::DwellAction::LeftClick));
        }
        catch (...)
        {
            Logger::warn(L"Failed to read the default action setting; keeping previous value.");
        }
    }
}

dwellclick::Settings DwellClick::SettingsSnapshot() const
{
    dwellclick::Settings s;
    s.dwellTimeMs = m_dwellTimeMs.load();
    s.moveTolerancePixels = m_moveTolerancePixels.load();
    s.postActionTolerancePixels = m_postActionTolerancePixels.load();
    s.defaultAction = static_cast<dwellclick::DwellAction>(m_defaultAction.load());
    s.revertToDefaultAfterAction = m_revertToDefaultAfterAction.load();
    return s;
}

void DwellClick::HookThreadMain()
{
    // WH_MOUSE_LL callbacks are delivered to the thread that installed the hook, so this
    // thread needs a message queue and must pump messages while the hook is active.
    MSG msg;
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    m_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, GetModuleHandle(nullptr), 0);
    if (!m_mouseHook)
    {
        // Keep running for a clean shutdown path, but without moves the machine stays locked,
        // so the module is inert until the next enable.
        Logger::error(L"Failed to install DwellClick mouse hook, error: {}", GetLastError());
    }

    // The overlay lives on this thread so its window procs, the poll loop, and the engine
    // never race. The module keeps working (without visuals) if creation fails.
    const bool overlayCreated = m_overlay.Create(
        reinterpret_cast<HINSTANCE>(&__ImageBase),
        [this](dwellclick::ToolbarCommand command) { HandleToolbarCommand(command); });

    HANDLE handles[1] = { m_terminateEvent };
    while (m_listening)
    {
        // The timeout doubles as the poll cadence: with the pointer at rest no messages
        // arrive, and the countdown advances on WAIT_TIMEOUT every POLL_INTERVAL_MS.
        DWORD res = MsgWaitForMultipleObjects(1, handles, FALSE, POLL_INTERVAL_MS, QS_ALLINPUT);
        if (!m_listening || res == WAIT_OBJECT_0)
        {
            break;
        }
        if (res == WAIT_FAILED)
        {
            // Defensive: shouldn't happen now that the terminate event is validated before start.
            // Bail rather than spin if the wait ever fails.
            Logger::error(L"DwellClick wait failed, error: {}. Stopping hook thread.", GetLastError());
            break;
        }

        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (m_relockRequested.exchange(false))
        {
            m_engine.LockUntilMove();
        }

        const dwellclick::Settings snapshot = SettingsSnapshot();
        const uint64_t tick = GetTickCount64();
        POINT cursor{};
        GetCursorPos(&cursor);

        if (overlayCreated)
        {
            dwellclick::OverlaySettings overlaySettings;
            overlaySettings.showToolbar = m_showToolbar.load();
            overlaySettings.toolbarSide = m_toolbarSide.load();
            overlaySettings.showCountdown = m_showCountdown.load();
            overlaySettings.overlaySize = m_overlaySize.load();
            overlaySettings.buttonLeftClick = m_toolbarButtonLeftClick.load();
            overlaySettings.buttonDoubleClick = m_toolbarButtonDoubleClick.load();
            overlaySettings.buttonRightClick = m_toolbarButtonRightClick.load();
            overlaySettings.buttonMiddleClick = m_toolbarButtonMiddleClick.load();
            overlaySettings.buttonDrag = m_toolbarButtonDrag.load();
            overlaySettings.buttonScrollUp = m_toolbarButtonScrollUp.load();
            overlaySettings.buttonScrollDown = m_toolbarButtonScrollDown.load();
            overlaySettings.buttonOpenSettings = m_toolbarButtonOpenSettings.load();
            m_overlay.ApplySettings(overlaySettings);
            m_overlay.TickToolbar(cursor, tick, snapshot.dwellTimeMs);
        }

        // While the pointer is over the toolbar, the toolbar's own hover dwell is in charge
        // and the engine stays locked, so a dwell there selects a button instead of firing
        // the current action onto the toolbar. This is also what keeps the resume button
        // reachable while paused: pause stops the engine, never the toolbar.
        const bool overToolbar = overlayCreated && m_overlay.IsPointOverToolbar(cursor);
        if (overToolbar)
        {
            m_engine.LockUntilMove();
        }

        const dwellclick::PollResult result = m_engine.Poll(tick, snapshot);

        if (overlayCreated)
        {
            // Injection failures are logged by the injector; progress drives the ring.
            m_overlay.UpdateIndicator(cursor, result.progress, !overToolbar);
            m_overlay.SetToolbarState({ m_engine.NextAction(), m_engine.IsPaused(), m_engine.IsDragging() });
        }
    }

    if (overlayCreated)
    {
        m_overlay.Destroy();
    }
    if (m_mouseHook)
    {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }

    // Crash/shutdown safety: never leave the left button held by an unfinished drag.
    if (m_engine.ReleaseDrag())
    {
        Logger::info(L"Released a drag that was in progress when DwellClick stopped.");
    }
}

// Runs on the hook thread (toolbar window procs and the poll loop both live there), so
// engine calls need no synchronization.
void DwellClick::HandleToolbarCommand(dwellclick::ToolbarCommand command)
{
    using dwellclick::DwellAction;
    using dwellclick::ToolbarCommand;

    // Selecting a new action mid-drag abandons the gesture: release the held button first
    // so nothing is left stuck, then arm the choice.
    const auto select = [this](DwellAction action) {
        if (m_engine.IsDragging())
        {
            m_engine.ReleaseDrag();
        }
        m_engine.SetNextAction(action);
    };

    switch (command)
    {
    case ToolbarCommand::TogglePause:
        m_engine.SetPaused(!m_engine.IsPaused());
        break;
    case ToolbarCommand::SelectLeftClick:
        select(DwellAction::LeftClick);
        break;
    case ToolbarCommand::SelectDoubleClick:
        select(DwellAction::DoubleClick);
        break;
    case ToolbarCommand::SelectRightClick:
        select(DwellAction::RightClick);
        break;
    case ToolbarCommand::SelectMiddleClick:
        select(DwellAction::MiddleClick);
        break;
    case ToolbarCommand::SelectDrag:
        select(DwellAction::Drag);
        break;
    case ToolbarCommand::SelectScrollUp:
        select(DwellAction::ScrollUp);
        break;
    case ToolbarCommand::SelectScrollDown:
        select(DwellAction::ScrollDown);
        break;
    case ToolbarCommand::OpenSettings:
    {
        // The deep link every module uses: a second PowerToys.exe instance hands the request
        // to the running one and exits (see runner/main.cpp). ShellExecute keeps this
        // non-blocking; a dwell user cannot reach the tray icon, which is why the toolbar
        // carries this button at all.
        const std::wstring executable = get_module_folderpath(reinterpret_cast<HMODULE>(&__ImageBase)) + L"\\PowerToys.exe";
        ShellExecuteW(nullptr, L"open", executable.c_str(), L"--open-settings=MouseUtils", nullptr, SW_SHOWNORMAL);
        break;
    }
    case ToolbarCommand::ToggleCollapse:
    default:
        // Collapse is handled inside the overlay; nothing reaches the engine.
        break;
    }
}

LRESULT CALLBACK DwellClick::MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    // Load the singleton once into a local; see the g_instance declaration for why this is safe.
    DwellClick* instance = g_instance.load();
    if (nCode == HC_ACTION && instance)
    {
        instance->HandleMouseMessage(wParam, reinterpret_cast<MSLLHOOKSTRUCT*>(lParam));
    }
    // Dwell Click only observes; it never suppresses an event.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void DwellClick::HandleMouseMessage(WPARAM wParam, const MSLLHOOKSTRUCT* data)
{
    // Ignore only our own injected events. Deliberately NOT filtering on LLMHF_INJECTED:
    // input injected by other software (a head or eye tracker, an on-screen keyboard, remote
    // desktop) is exactly the input this module exists to serve, so a move injected by a
    // tracker must arm the machine and a click injected by another assistive tool must lock
    // it, the same as their physical counterparts.
    if (data->dwExtraInfo == INJECTION_TAG)
    {
        return;
    }

    // GetTickCount64 rather than data->time: the hook's 32-bit timestamp wraps, and mixing it
    // with the 64-bit tick the poll loop uses would break the engine's monotonic clock. Hook
    // delivery latency is negligible against dwell times measured in hundreds of milliseconds.
    switch (wParam)
    {
    case WM_MOUSEMOVE:
        m_engine.OnMove({ data->pt.x, data->pt.y }, GetTickCount64(), SettingsSnapshot());
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        // The user clicked for themselves; do not stack a dwell click on top of it.
        m_engine.OnPhysicalClick();
        break;
    default:
        break;
    }
}

extern "C" __declspec(dllexport) PowertoyModuleIface* __cdecl powertoy_create()
{
    return new DwellClick();
}
