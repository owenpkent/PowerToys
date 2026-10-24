// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pch.h"
#include <shellapi.h>
#include <ShellScalingApi.h>
#include <interface/powertoy_module_interface.h>
#include <common/SettingsAPI/settings_objects.h>
#include <common/utils/logger_helper.h>
#include <common/utils/process_path.h>
#include <common/logger/logger.h>
#include <common/utils/color.h>
#include "trace.h"
#include "resource.h"
#include "DwellClickCore.h"
#include "DwellClickInput.h"
#include "DwellClickOverlay.h"
#include "DwellClickSettingsChange.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib") // PlaySoundW
#pragma comment(lib, "Shcore.lib") // GetDpiForMonitor

// Dwell Click
//
// Issues a mouse click automatically when the pointer is held still for a configurable time,
// for people who can move a pointer but cannot reliably click it. The decision logic lives in
// DwellClickCore.h (Win32-free and unit tested); this file is the thin Win32 adapter around it:
// a low-level mouse hook feeds the engine pointer moves, physical clicks, and wheel turns, a
// polling loop on the same thread advances the countdown, and a SendInput wrapper performs the
// injections the engine asks for. The on-screen surfaces (a countdown ring at the pointer and a dwell-to-select
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
    const wchar_t JSON_KEY_REPEAT_INTERVAL_MS[] = L"repeat_interval_ms";
    const wchar_t JSON_KEY_DEFAULT_ACTION[] = L"default_action";
    const wchar_t JSON_KEY_REVERT_TO_DEFAULT_AFTER_ACTION[] = L"revert_to_default_after_action";
    const wchar_t JSON_KEY_SHOW_TOOLBAR[] = L"show_toolbar";
    const wchar_t JSON_KEY_TOOLBAR_SIDE[] = L"toolbar_side";
    const wchar_t JSON_KEY_SHOW_COUNTDOWN[] = L"show_countdown";
    const wchar_t JSON_KEY_OVERLAY_SIZE[] = L"overlay_size";
    const wchar_t JSON_KEY_INDICATOR_USE_ACCENT_COLOR[] = L"indicator_use_accent_color";
    const wchar_t JSON_KEY_INDICATOR_COLOR[] = L"indicator_color";
    const wchar_t JSON_KEY_PLAY_SOUND[] = L"play_sound";
    const wchar_t JSON_KEY_ACTIVATION_SHORTCUT[] = L"activation_shortcut";
    const wchar_t JSON_KEY_CYCLE_ACTION_SHORTCUT[] = L"cycle_action_shortcut";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_LEFT_CLICK[] = L"toolbar_button_left_click";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_DOUBLE_CLICK[] = L"toolbar_button_double_click";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_RIGHT_CLICK[] = L"toolbar_button_right_click";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_MIDDLE_CLICK[] = L"toolbar_button_middle_click";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_DRAG[] = L"toolbar_button_drag";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_SCROLL_UP[] = L"toolbar_button_scroll_up";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_SCROLL_DOWN[] = L"toolbar_button_scroll_down";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_OPEN_SETTINGS[] = L"toolbar_button_open_settings";
    const wchar_t JSON_KEY_TOOLBAR_POSITION[] = L"toolbar_position";
    const wchar_t JSON_KEY_TOOLBAR_STYLE[] = L"toolbar_style";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_SCROLL_LEFT[] = L"toolbar_button_scroll_left";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_SCROLL_RIGHT[] = L"toolbar_button_scroll_right";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_ZOOM_IN[] = L"toolbar_button_zoom_in";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_ZOOM_OUT[] = L"toolbar_button_zoom_out";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_BACK[] = L"toolbar_button_back";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_START[] = L"toolbar_button_start";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_SHOW_DESKTOP[] = L"toolbar_button_show_desktop";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_TASK_VIEW[] = L"toolbar_button_task_view";
    const wchar_t JSON_KEY_TOOLBAR_BUTTON_NOTIFICATIONS[] = L"toolbar_button_notifications";

    // dwExtraInfo tag stamped on every event we inject via SendInput, so the hook ignores our
    // own synthetic clicks: a dwell-fired click must not register as the user clicking
    // physically, or every fire would immediately re-lock against its own echo. The magic
    // value spells the module's initials in ASCII (D, W, L, C); distinct from MouseButtonLock's
    // tag and the centralized keyboard hook's flag.
    constexpr ULONG_PTR INJECTION_TAG = 0x44574C43;

    // How often the poll loop advances the countdown while the pointer is at rest. Fine enough
    // that a fired click lands within about a frame of the configured dwell time, and that the
    // countdown ring fills smoothly.
    constexpr DWORD POLL_INTERVAL_MS = 15;

    // The poll cadence while nothing can change with time alone: no countdown, nothing on show,
    // and no toolbar button filling. Real input wakes the wait at once (the hook's call arrives
    // as a sent message), and set_config and the hotkeys set m_wakeEvent. But SetCursorPos never
    // reaches a low-level hook, and some assistive software steers the pointer with it, so the
    // toolbar still has to see such a pointer leave a button to re-arm it: this interval bounds
    // how long a SetCursorPos move can go unseen, and any cursor change it does see switches
    // back to the fast cadence. About 10 wake-ups a second instead of 66.
    constexpr DWORD IDLE_POLL_INTERVAL_MS = 100;

    // Default values mirror the engine's Settings defaults (see DwellClickCore.h for their
    // provenance); DwellClickProperties in Settings.UI.Library must match both.
    constexpr int DEFAULT_DWELL_TIME_MS = 1200;
    constexpr int DEFAULT_TOLERANCE_PIXELS = 10;
    constexpr int DEFAULT_REPEAT_INTERVAL_MS = 400;

    // Accepted ranges when reading from settings.json. The Settings UI constrains these, but a
    // hand-edited file could carry out-of-range or non-finite values, so clamp on read.
    // A dwell time under 100 ms fires on virtually every rest (the Midas touch failure the
    // engine is built to avoid); the engine itself stays defined down to 1 ms for tests.
    constexpr int MIN_DWELL_TIME_MS = 100;
    constexpr int MAX_DWELL_TIME_MS = 60000;
    constexpr int MAX_TOLERANCE_PIXELS = 10000;

    // A scroll or zoom repeat faster than 50 ms outruns most apps' wheel handling; the first
    // notch of a repeat always waits the full dwell time, so this bound never makes a rest fire.
    constexpr int MIN_REPEAT_INTERVAL_MS = 50;
    constexpr int MAX_REPEAT_INTERVAL_MS = 60000;

    // Ring color default; matches the PowerToys brand blue the overlay palette uses.
    constexpr COLORREF DEFAULT_INDICATOR_COLOR = RGB(0x00, 0x78, 0xD4);

    // Hotkey ids, in the order get_hotkeys reports them.
    constexpr size_t HOTKEY_ID_PAUSE = 0;
    constexpr size_t HOTKEY_ID_CYCLE_ACTION = 1;

    // Pause or resume: Win+Shift+W. Cycle action: Win+Shift+A.
    constexpr unsigned char DEFAULT_PAUSE_HOTKEY_VK = 0x57;
    constexpr unsigned char DEFAULT_CYCLE_HOTKEY_VK = 0x41;

    // Click sound: synthesized in memory so no binary asset ships. 40 ms of a 1 kHz sine, with a
    // 2 ms linear attack (a hard start into a sine would pop) and an exponential decay.
    constexpr uint32_t SOUND_SAMPLE_RATE = 22050;
    constexpr double SOUND_DURATION_S = 0.040;
    constexpr double SOUND_FREQUENCY_HZ = 1000.0;
    constexpr double SOUND_ATTACK_S = 0.002;
    constexpr double SOUND_DECAY_TAU_S = 0.008;
    constexpr double SOUND_AMPLITUDE = 0.5;

    // Builds a RIFF/WAVE image (16-bit mono PCM) of the click, ready for PlaySoundW(SND_MEMORY).
    std::vector<BYTE> BuildClickSound()
    {
        const uint32_t sampleCount = static_cast<uint32_t>(SOUND_SAMPLE_RATE * SOUND_DURATION_S);
        const uint32_t dataBytes = sampleCount * sizeof(int16_t);

        std::vector<BYTE> wav;
        wav.reserve(44 + dataBytes);
        auto put32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i)
            {
                wav.push_back(static_cast<BYTE>((v >> (8 * i)) & 0xFF));
            }
        };
        auto put16 = [&](uint16_t v) {
            wav.push_back(static_cast<BYTE>(v & 0xFF));
            wav.push_back(static_cast<BYTE>((v >> 8) & 0xFF));
        };
        auto putTag = [&](const char* tag) {
            for (int i = 0; i < 4; ++i)
            {
                wav.push_back(static_cast<BYTE>(tag[i]));
            }
        };

        putTag("RIFF");
        put32(36 + dataBytes);
        putTag("WAVE");
        putTag("fmt ");
        put32(16); // fmt chunk size
        put16(1); // PCM
        put16(1); // mono
        put32(SOUND_SAMPLE_RATE);
        put32(SOUND_SAMPLE_RATE * sizeof(int16_t)); // byte rate
        put16(sizeof(int16_t)); // block align
        put16(16); // bits per sample
        putTag("data");
        put32(dataBytes);

        const double twoPi = 6.283185307179586;
        for (uint32_t i = 0; i < sampleCount; ++i)
        {
            const double t = static_cast<double>(i) / SOUND_SAMPLE_RATE;
            const double attack = (std::min)(1.0, t / SOUND_ATTACK_S);
            const double envelope = attack * std::exp(-t / SOUND_DECAY_TAU_S);
            const double sample = SOUND_AMPLITUDE * envelope * std::sin(twoPi * SOUND_FREQUENCY_HZ * t);
            put16(static_cast<uint16_t>(static_cast<int16_t>(sample * 32767.0)));
        }
        return wav;
    }

    // Send InputSteps (see DwellClickInput.h) as one tagged SendInput batch. The tag lets the
    // hook ignore the module's own events. Returns whether the OS accepted every event.
    bool SendSteps(const std::vector<dwellclick::InputStep>& steps)
    {
        std::vector<INPUT> inputs(steps.size());
        for (size_t i = 0; i < steps.size(); ++i)
        {
            const dwellclick::InputStep& step = steps[i];
            INPUT& input = inputs[i];
            if (step.keyboard)
            {
                input.type = INPUT_KEYBOARD;
                input.ki.wVk = step.vk;
                input.ki.dwFlags = step.keyUp ? KEYEVENTF_KEYUP : 0;
                input.ki.dwExtraInfo = INJECTION_TAG;
            }
            else
            {
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = step.mouseFlag;
                input.mi.mouseData = static_cast<DWORD>(step.wheelDelta);
                input.mi.dwExtraInfo = INJECTION_TAG;
            }
        }
        return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
    }

    // One bit per mouse button for the external-hold mask: left, right, middle, X1, X2.
    uint8_t ButtonBit(WPARAM message, DWORD mouseData)
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return 0x01;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return 0x02;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return 0x04;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            return HIWORD(mouseData) == XBUTTON2 ? 0x10 : 0x08;
        default:
            return 0;
        }
    }

    // Whether any mouse button currently reads down. Used only to clear an external hold whose
    // button-up the hook missed, so it checks every button rather than mapping each bit (the
    // swap-buttons setting makes that mapping ambiguous).
    bool AnyMouseButtonDown()
    {
        for (const int vk : { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 })
        {
            if ((GetAsyncKeyState(vk) & 0x8000) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Windows 11 is Windows 10.0 from build 22000 (the same check common/utils/package.h makes,
    // without pulling in its packaging dependencies).
    bool IsWindows11OrGreater()
    {
        OSVERSIONINFOEXW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        version.dwMajorVersion = HIBYTE(_WIN32_WINNT_WIN10);
        version.dwMinorVersion = LOBYTE(_WIN32_WINNT_WIN10);
        version.dwBuildNumber = 22000;
        DWORDLONG mask = 0;
        VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
        VER_SET_CONDITION(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
        VER_SET_CONDITION(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        return VerifyVersionInfoW(&version, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, mask) != FALSE;
    }

    // Whether a process's token is elevated. A process that cannot be opened or queried counts
    // as not elevated, the same default as common/utils/elevation.h, which this module does not
    // include because it pulls in WIL. GetCurrentProcess() works here too.
    bool IsProcessElevated(HANDLE process)
    {
        HANDLE token = nullptr;
        if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token))
        {
            return false;
        }
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated != 0;
        CloseHandle(token);
        return elevated;
    }

    // Whether Windows will drop input sent to this window from this process: User Interface
    // Privilege Isolation blocks injection into an elevated process unless PowerToys itself runs
    // elevated. SendInput does not reliably report that drop, so check first, the way
    // FancyZones and Always On Top do (IsProcessOfWindowElevated).
    bool IsInputBlockedForWindow(HWND window)
    {
        static const bool selfElevated = IsProcessElevated(GetCurrentProcess());
        if (!window || selfElevated)
        {
            return false;
        }
        const HWND root = GetAncestor(window, GA_ROOT);
        DWORD pid = 0;
        GetWindowThreadProcessId(root ? root : window, &pid);
        if (pid == 0)
        {
            return false;
        }
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
        {
            return false;
        }
        const bool elevated = IsProcessElevated(process);
        CloseHandle(process);
        return elevated;
    }

    // Production injector: a tagged SendInput. Lives behind the engine's IClickInjector so the
    // state machine can be unit tested without Win32.
    class WinInjector : public dwellclick::IClickInjector
    {
    public:
        bool Inject(dwellclick::ClickKind kind, dwellclick::PointL pt) override
        {
            // The point only picks the window that will receive the click. The input itself is
            // injected at the live cursor position, which is the same spot because the engine
            // only fires while the pointer rests there; injecting a MOUSEEVENTF_MOVE instead
            // could fight other software steering the pointer (a head or eye tracker, exactly
            // this module's audience).
            //
            // A drag's release is never refused: the window where the drag started holds mouse
            // capture, so the button-up goes there rather than to whatever is under the
            // pointer, and refusing it would leave the button held down.
            if (kind != dwellclick::ClickKind::LeftUp && IsInputBlockedForWindow(WindowFromPoint(POINT{ pt.x, pt.y })))
            {
                Logger::warn(L"Dwell click not sent: the window under the pointer is elevated above PowerToys, so Windows would drop it.");
                return false;
            }
            if (SendSteps(dwellclick::StepsFor(kind)))
            {
                return true;
            }
            Logger::warn(L"Failed to inject a dwell click, error: {}", GetLastError());
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
    std::atomic<int> m_repeatIntervalMs{ DEFAULT_REPEAT_INTERVAL_MS };
    std::atomic<int> m_defaultAction{ static_cast<int>(dwellclick::DwellAction::LeftClick) };
    std::atomic<bool> m_revertToDefaultAfterAction{ true };
    std::atomic<bool> m_showToolbar{ true };
    std::atomic<int> m_toolbarSide{ 0 };
    std::atomic<bool> m_showCountdown{ true };
    std::atomic<int> m_overlaySize{ 1 }; // 0 = small, 1 = medium, 2 = large
    std::atomic<bool> m_indicatorUseAccentColor{ true };
    std::atomic<COLORREF> m_indicatorColor{ DEFAULT_INDICATOR_COLOR };
    std::atomic<bool> m_playSound{ false };
    std::atomic<bool> m_toolbarButtonLeftClick{ true };
    std::atomic<bool> m_toolbarButtonDoubleClick{ true };
    std::atomic<bool> m_toolbarButtonRightClick{ true };
    std::atomic<bool> m_toolbarButtonMiddleClick{ false };
    std::atomic<bool> m_toolbarButtonDrag{ true };
    std::atomic<bool> m_toolbarButtonScrollUp{ true };
    std::atomic<bool> m_toolbarButtonScrollDown{ true };
    std::atomic<bool> m_toolbarButtonOpenSettings{ true };
    std::atomic<int> m_toolbarPosition{ 50 }; // percent along the edge
    std::atomic<int> m_toolbarStyle{ 0 }; // 0 = docked bar, 1 = floating button with large menu
    std::atomic<bool> m_toolbarButtonScrollLeft{ false };
    std::atomic<bool> m_toolbarButtonScrollRight{ false };
    std::atomic<bool> m_toolbarButtonZoomIn{ false };
    std::atomic<bool> m_toolbarButtonZoomOut{ false };
    std::atomic<bool> m_toolbarButtonBack{ false };
    std::atomic<bool> m_toolbarButtonStart{ false };
    std::atomic<bool> m_toolbarButtonShowDesktop{ false };
    std::atomic<bool> m_toolbarButtonTaskView{ false };
    std::atomic<bool> m_toolbarButtonNotifications{ false };

    // The engine is not thread-safe, so set_config (runner thread) never touches it directly.
    // It publishes the new values through this signal instead, and the hook thread's poll loop
    // consumes it and applies the change (see DwellClickSettingsChange.h for the ordering).
    dwellclick::SettingsChangeSignal m_settingsChange;

    // The hotkeys fire on the runner thread, so like the re-lock they only raise a flag; the
    // hook thread's poll loop consumes it and makes the engine call.
    std::atomic<bool> m_pauseToggleRequested{ false };
    std::atomic<bool> m_cycleActionRequested{ false };

    // The default action last handed to the engine. Written in enable() before the hook thread
    // starts and afterwards only on the hook thread, so it needs no synchronization. The poll loop
    // compares against it so that only a real change of the default replaces the next action; an
    // unrelated settings change must not discard an action picked on the toolbar.
    dwellclick::DwellAction m_appliedDefaultAction = dwellclick::DwellAction::LeftClick;

    // Mouse buttons currently held by something other than this module (ButtonBit per button).
    // Touched only on the hook thread (the hook callback and the poll loop) after enable() resets
    // it before that thread starts, so it needs no synchronization.
    // The effective dpi of the monitor under the pointer, and that monitor. The tolerances are
    // set at 100% scaling and SettingsSnapshot scales them by this, so the same hand movement
    // counts the same on every monitor. Written in enable() before the hook thread starts and
    // afterwards only on the hook thread (UpdatePointerDpi), so it needs no synchronization.
    unsigned m_pointerDpi = 96;
    HMONITOR m_pointerMonitor = nullptr;

    uint8_t m_externalButtonsHeld = 0;
    uint64_t m_lastButtonEventTick = 0;
    static constexpr uint64_t STALE_HOLD_CHECK_MS = 500;

    // The shortcuts. Only touched on the runner thread (constructor, set_config, get_hotkeys),
    // so they need no synchronization.
    Hotkey m_pauseHotkey = { .win = true, .ctrl = false, .shift = true, .alt = false, .key = DEFAULT_PAUSE_HOTKEY_VK };
    Hotkey m_cycleHotkey = { .win = true, .ctrl = false, .shift = true, .alt = false, .key = DEFAULT_CYCLE_HOTKEY_VK };

    // The click sound image, built in enable() before the hook thread starts and cleared in
    // disable() after it joins, so the poll loop never sees it half built.
    std::vector<BYTE> m_clickSound;

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

    // Wakes the hook thread from an idle wait when the runner thread raises a flag (set_config,
    // on_hotkey). Created and closed on the runner thread around the hook thread's lifetime,
    // and every SetEvent also runs on the runner thread, so it cannot race the close.
    HANDLE m_wakeEvent = nullptr;

    // The cursor position the poll loop saw last pass. Hook thread only. A change means the
    // pointer moved, possibly by SetCursorPos, which the hook never reports (see
    // IDLE_POLL_INTERVAL_MS).
    POINT m_lastPolledCursor{};
    std::thread m_hookThread;
    std::atomic<bool> m_listening{ false };

    void init_settings();
    void parse_settings(PowerToysSettings::PowerToyValues& settings);
    bool ParseHotkey(const winrt::Windows::Data::Json::JsonObject& properties, const wchar_t* key, Hotkey& target, unsigned char defaultKey);
    dwellclick::Settings SettingsSnapshot() const;
    void UpdatePointerDpi(POINT pt);

    void HookThreadMain();
    void WakeHookThread();
    void HandleMouseMessage(WPARAM wParam, const MSLLHOOKSTRUCT* data);
    void HandleToolbarCommand(dwellclick::ToolbarCommand command);

    // Whether the cycle shortcut may land on an action: the default action always; otherwise
    // only an action whose toolbar button is switched on in Settings.
    bool IsActionOffered(dwellclick::DwellAction action) const;

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

            // Any live settings change must lock the machine until the pointer moves, or a
            // dwell time shortened mid-countdown would fire at once. The signal's ordering
            // guarantees the hook thread locks before it polls with any new value.
            m_settingsChange.PublishChange([&] { parse_settings(values); });
            WakeHookThread();
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
            UpdatePointerDpi(pt);
            m_engine.OnMove({ pt.x, pt.y }, tick, SettingsSnapshot());
        }
        m_engine.ResetTransient(tick);
        m_externalButtonsHeld = 0;
        m_engine.SetExternalButtonHeld(false);

        // The engine only falls back to the default after an action when revert is on, so seed
        // it here; otherwise the first dwell (or every dwell, with revert off) is a left click.
        m_appliedDefaultAction = SettingsSnapshot().defaultAction;
        m_engine.SetNextAction(m_appliedDefaultAction);

        // Before the hook thread exists, so the poll never sees a half-built buffer.
        m_clickSound = BuildClickSound();

        m_terminateEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_terminateEvent == nullptr)
        {
            // Without the terminate event the hook thread's shutdown wait breaks; don't start it.
            Logger::error(L"Failed to create DwellClick terminate event, error: {}. Hook not started.", GetLastError());
            return;
        }
        m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_wakeEvent == nullptr)
        {
            // Without it the loop cannot back off when idle; it still runs, on the fast cadence.
            Logger::warn(L"Failed to create DwellClick wake event, error: {}. Polling without idle back-off.", GetLastError());
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
        if (m_wakeEvent)
        {
            CloseHandle(m_wakeEvent);
            m_wakeEvent = nullptr;
        }

        // The hook thread is gone, so nothing starts new sounds; stop one still playing before
        // its buffer is freed.
        PlaySoundW(nullptr, nullptr, 0);
        m_clickSound.clear();
    }

    virtual bool is_enabled() override
    {
        return m_enabled;
    }

    virtual bool is_enabled_by_default() const override
    {
        return false;
    }

    virtual size_t get_hotkeys(Hotkey* buffer, size_t buffer_size) override
    {
        constexpr size_t count = 2;
        if (buffer && buffer_size >= count)
        {
            buffer[HOTKEY_ID_PAUSE] = m_pauseHotkey;
            buffer[HOTKEY_ID_CYCLE_ACTION] = m_cycleHotkey;
        }
        return count;
    }

    virtual bool on_hotkey(size_t hotkeyId) override
    {
        // The runner invokes hotkeys even while the module is disabled; there is no hook thread
        // to act on then, so both hotkeys must do nothing (and not swallow the keystroke).
        if (!m_enabled)
        {
            return false;
        }

        switch (hotkeyId)
        {
        case HOTKEY_ID_PAUSE:
            m_pauseToggleRequested.store(true);
            WakeHookThread();
            return true;
        case HOTKEY_ID_CYCLE_ACTION:
            m_cycleActionRequested.store(true);
            WakeHookThread();
            return true;
        default:
            return false;
        }
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
    readInt(JSON_KEY_REPEAT_INTERVAL_MS, m_repeatIntervalMs, MIN_REPEAT_INTERVAL_MS, MAX_REPEAT_INTERVAL_MS);
    readBool(JSON_KEY_REVERT_TO_DEFAULT_AFTER_ACTION, m_revertToDefaultAfterAction);
    readBool(JSON_KEY_SHOW_TOOLBAR, m_showToolbar);
    readInt(JSON_KEY_TOOLBAR_SIDE, m_toolbarSide, 0, 3);
    readInt(JSON_KEY_TOOLBAR_POSITION, m_toolbarPosition, 0, 100);
    readInt(JSON_KEY_TOOLBAR_STYLE, m_toolbarStyle, 0, 1);
    readBool(JSON_KEY_SHOW_COUNTDOWN, m_showCountdown);
    readInt(JSON_KEY_OVERLAY_SIZE, m_overlaySize, 0, 2);
    readBool(JSON_KEY_INDICATOR_USE_ACCENT_COLOR, m_indicatorUseAccentColor);
    readBool(JSON_KEY_PLAY_SOUND, m_playSound);
    readBool(JSON_KEY_TOOLBAR_BUTTON_LEFT_CLICK, m_toolbarButtonLeftClick);
    readBool(JSON_KEY_TOOLBAR_BUTTON_DOUBLE_CLICK, m_toolbarButtonDoubleClick);
    readBool(JSON_KEY_TOOLBAR_BUTTON_RIGHT_CLICK, m_toolbarButtonRightClick);
    readBool(JSON_KEY_TOOLBAR_BUTTON_MIDDLE_CLICK, m_toolbarButtonMiddleClick);
    readBool(JSON_KEY_TOOLBAR_BUTTON_DRAG, m_toolbarButtonDrag);
    readBool(JSON_KEY_TOOLBAR_BUTTON_SCROLL_UP, m_toolbarButtonScrollUp);
    readBool(JSON_KEY_TOOLBAR_BUTTON_SCROLL_DOWN, m_toolbarButtonScrollDown);
    readBool(JSON_KEY_TOOLBAR_BUTTON_OPEN_SETTINGS, m_toolbarButtonOpenSettings);
    readBool(JSON_KEY_TOOLBAR_BUTTON_SCROLL_LEFT, m_toolbarButtonScrollLeft);
    readBool(JSON_KEY_TOOLBAR_BUTTON_SCROLL_RIGHT, m_toolbarButtonScrollRight);
    readBool(JSON_KEY_TOOLBAR_BUTTON_ZOOM_IN, m_toolbarButtonZoomIn);
    readBool(JSON_KEY_TOOLBAR_BUTTON_ZOOM_OUT, m_toolbarButtonZoomOut);
    readBool(JSON_KEY_TOOLBAR_BUTTON_BACK, m_toolbarButtonBack);
    readBool(JSON_KEY_TOOLBAR_BUTTON_START, m_toolbarButtonStart);
    readBool(JSON_KEY_TOOLBAR_BUTTON_SHOW_DESKTOP, m_toolbarButtonShowDesktop);
    readBool(JSON_KEY_TOOLBAR_BUTTON_TASK_VIEW, m_toolbarButtonTaskView);
    readBool(JSON_KEY_TOOLBAR_BUTTON_NOTIFICATIONS, m_toolbarButtonNotifications);

    // Same helper and "#RRGGBB" format as Mouse Highlighter's colors. A malformed string keeps
    // the previous value.
    if (properties.HasKey(JSON_KEY_INDICATOR_COLOR))
    {
        try
        {
            auto colorString = (std::wstring)properties.GetNamedObject(JSON_KEY_INDICATOR_COLOR).GetNamedString(JSON_KEY_VALUE);
            uint8_t r, g, b;
            if (checkValidRGB(colorString, &r, &g, &b))
            {
                m_indicatorColor.store(RGB(r, g, b));
            }
            else
            {
                Logger::error("Indicator color value is invalid. Keeping previous value");
            }
        }
        catch (...)
        {
            Logger::warn("Failed to read the indicator color from settings. Keeping previous value");
        }
    }

    // A missing shortcut is not an error (a fresh install has none); the member defaults cover it.
    ParseHotkey(properties, JSON_KEY_ACTIVATION_SHORTCUT, m_pauseHotkey, DEFAULT_PAUSE_HOTKEY_VK);
    ParseHotkey(properties, JSON_KEY_CYCLE_ACTION_SHORTCUT, m_cycleHotkey, DEFAULT_CYCLE_HOTKEY_VK);

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

// Reads one hotkey object into target. Absent or malformed keeps the previous value; a shortcut
// with no modifier would swallow a bare key system-wide, so that falls back to Win+Shift+defaultKey.
bool DwellClick::ParseHotkey(const winrt::Windows::Data::Json::JsonObject& properties, const wchar_t* key, Hotkey& target, unsigned char defaultKey)
{
    try
    {
        if (!properties.HasKey(key))
        {
            return false;
        }
        auto jsonHotkey = properties.GetNamedObject(key);
        auto hotkey = PowerToysSettings::HotkeyObject::from_json(jsonHotkey);
        Hotkey parsed{};
        parsed.win = hotkey.win_pressed();
        parsed.ctrl = hotkey.ctrl_pressed();
        parsed.shift = hotkey.shift_pressed();
        parsed.alt = hotkey.alt_pressed();
        parsed.key = static_cast<unsigned char>(hotkey.get_code());
        if (!parsed.win && !parsed.ctrl && !parsed.shift && !parsed.alt)
        {
            Logger::info(L"Dwell Click shortcut {} has no modifier; using the default", key);
            parsed = { .win = true, .ctrl = false, .shift = true, .alt = false, .key = defaultKey };
        }
        target = parsed;
        return true;
    }
    catch (...)
    {
        Logger::warn(L"Failed to initialize Dwell Click shortcut {}", key);
        return false;
    }
}

dwellclick::Settings DwellClick::SettingsSnapshot() const
{
    dwellclick::Settings s;
    s.dwellTimeMs = m_dwellTimeMs.load();
    s.moveTolerancePixels = dwellclick::ScaledTolerance(m_moveTolerancePixels.load(), m_pointerDpi);
    s.postActionTolerancePixels = dwellclick::ScaledTolerance(m_postActionTolerancePixels.load(), m_pointerDpi);
    s.repeatIntervalMs = m_repeatIntervalMs.load();
    s.defaultAction = static_cast<dwellclick::DwellAction>(m_defaultAction.load());
    s.revertToDefaultAfterAction = m_revertToDefaultAfterAction.load();
    return s;
}

void DwellClick::UpdatePointerDpi(POINT pt)
{
    // MonitorFromPoint is cheap enough for every move; the dpi is only re-read when the pointer
    // crosses onto another monitor.
    const HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (monitor == m_pointerMonitor)
    {
        return;
    }
    m_pointerMonitor = monitor;
    UINT dpiX = 96;
    UINT dpiY = 96;
    m_pointerDpi = (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) ? dpiX : 96;
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

    HANDLE handles[2] = { m_terminateEvent, m_wakeEvent };
    const DWORD handleCount = m_wakeEvent ? 2 : 1;
    DWORD timeout = POLL_INTERVAL_MS;
    while (m_listening)
    {
        // The timeout doubles as the poll cadence: with the pointer at rest no messages
        // arrive, and the countdown advances on WAIT_TIMEOUT every POLL_INTERVAL_MS. While
        // nothing can change with time alone the loop waits longer (see the end of the loop).
        DWORD res = MsgWaitForMultipleObjects(handleCount, handles, FALSE, timeout, QS_ALLINPUT);
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

        // The snapshot is read BEFORE the change is consumed, so a snapshot holding any new
        // value is always followed by the lock that value requires, before Poll can run against
        // it (see SettingsChangeSignal).
        const dwellclick::Settings snapshot = SettingsSnapshot();
        if (m_settingsChange.Consume())
        {
            dwellclick::ApplySettingsChange(m_engine, snapshot, m_appliedDefaultAction);
        }

        // A button held by the user (or another tool) whose release the hook never saw, say
        // across a secure-desktop switch, must not suppress dwelling forever: once every
        // button reads up, the hold is over.
        // The low-level hook runs before the system updates the async key state, so a press
        // only just seen can still read up; the check waits until the last button event is old.
        if (m_externalButtonsHeld != 0 && GetTickCount64() - m_lastButtonEventTick >= STALE_HOLD_CHECK_MS && !AnyMouseButtonDown())
        {
            m_externalButtonsHeld = 0;
            m_engine.SetExternalButtonHeld(false);
        }

        if (m_pauseToggleRequested.exchange(false))
        {
            // The same engine call the toolbar's pause button makes (see HandleToolbarCommand).
            m_engine.SetPaused(!m_engine.IsPaused());

            // A shortcut gives no feedback of its own; show the result at the pointer when the
            // toolbar is not showing it.
            if (overlayCreated)
            {
                m_overlay.FlashPauseState(m_engine.IsPaused(), GetTickCount64());
            }
        }
        if (m_cycleActionRequested.exchange(false))
        {
            // A drag in progress holds the left button. Release it first, as the toolbar's
            // select does, so changing the action can never orphan the held button. The toolbar
            // highlight follows because SetToolbarState below reads NextAction() every poll.
            if (m_engine.IsDragging())
            {
                m_engine.ReleaseDrag();
            }
            // The next offered action in enum order, wrapping. An action switched off in
            // Settings is skipped, so the shortcut only visits what the toolbar offers plus the
            // default action.
            const int count = static_cast<int>(dwellclick::LastDwellAction) + 1;
            const int current = static_cast<int>(m_engine.NextAction());
            for (int step = 1; step < count; ++step)
            {
                const auto candidate = static_cast<dwellclick::DwellAction>((current + step) % count);
                if (IsActionOffered(candidate))
                {
                    m_engine.SetNextAction(candidate);
                    if (overlayCreated)
                    {
                        m_overlay.FlashAction(candidate, GetTickCount64());
                    }
                    break;
                }
            }
        }

        const uint64_t tick = GetTickCount64();
        POINT cursor{};
        GetCursorPos(&cursor);

        if (overlayCreated)
        {
            dwellclick::OverlaySettings overlaySettings;
            overlaySettings.showToolbar = m_showToolbar.load();
            overlaySettings.toolbarSide = m_toolbarSide.load();
            overlaySettings.toolbarPosition = m_toolbarPosition.load();
            overlaySettings.toolbarStyle = m_toolbarStyle.load();
            overlaySettings.showCountdown = m_showCountdown.load();
            overlaySettings.overlaySize = m_overlaySize.load();
            overlaySettings.indicatorUseAccentColor = m_indicatorUseAccentColor.load();
            overlaySettings.indicatorColor = m_indicatorColor.load();
            overlaySettings.buttonLeftClick = m_toolbarButtonLeftClick.load();
            overlaySettings.buttonDoubleClick = m_toolbarButtonDoubleClick.load();
            overlaySettings.buttonRightClick = m_toolbarButtonRightClick.load();
            overlaySettings.buttonMiddleClick = m_toolbarButtonMiddleClick.load();
            overlaySettings.buttonDrag = m_toolbarButtonDrag.load();
            overlaySettings.buttonScrollUp = m_toolbarButtonScrollUp.load();
            overlaySettings.buttonScrollDown = m_toolbarButtonScrollDown.load();
            overlaySettings.buttonOpenSettings = m_toolbarButtonOpenSettings.load();
            overlaySettings.buttonScrollLeft = m_toolbarButtonScrollLeft.load();
            overlaySettings.buttonScrollRight = m_toolbarButtonScrollRight.load();
            overlaySettings.buttonZoomIn = m_toolbarButtonZoomIn.load();
            overlaySettings.buttonZoomOut = m_toolbarButtonZoomOut.load();
            overlaySettings.buttonBack = m_toolbarButtonBack.load();
            overlaySettings.buttonStartMenu = m_toolbarButtonStart.load();
            overlaySettings.buttonShowDesktop = m_toolbarButtonShowDesktop.load();
            overlaySettings.buttonTaskView = m_toolbarButtonTaskView.load();
            overlaySettings.buttonNotifications = m_toolbarButtonNotifications.load();
            m_overlay.ApplySettings(overlaySettings);
            m_overlay.TickToolbar(cursor, tick, snapshot.dwellTimeMs);
        }

        // While the pointer is over the toolbar, the toolbar's own hover dwell is in charge
        // and the engine stays locked, so a dwell there selects a button instead of firing
        // the current action onto the toolbar. This is also what keeps the resume button
        // reachable while paused: pause stops the engine, never the toolbar.
        // The floating style's large menu counts as the toolbar wherever the pointer is: while
        // it is open the user is choosing, and nothing should be clicked behind it.
        const bool overToolbar = overlayCreated && (m_overlay.IsPointOverToolbar(cursor) || m_overlay.IsMenuOpen());
        if (overToolbar)
        {
            m_engine.LockUntilMove();
        }

        const dwellclick::PollResult result = m_engine.Poll(tick, snapshot);

        if (overlayCreated)
        {
            // Injection failures are logged by the injector; progress drives the ring. The
            // toolbar state goes first because the ring's action badge reads it.
            m_overlay.SetToolbarState({ m_engine.NextAction(), m_engine.IsPaused(), m_engine.IsDragging() });
            m_overlay.UpdateIndicator(cursor, result.progress, snapshot.dwellTimeMs, !overToolbar);
        }

        if (result.failed && overlayCreated)
        {
            // The click did not happen (most often an elevated window under the pointer); say so
            // at the pointer rather than leaving the user to wonder.
            m_overlay.FlashBlocked(tick);
        }

        if (result.fired && m_playSound.load() && !m_clickSound.empty())
        {
            PlaySoundW(reinterpret_cast<LPCWSTR>(m_clickSound.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }

        // Poll fast only while time alone can change something: a countdown (including a
        // repeating scroll), the toolbar under the pointer, anything the overlay is animating,
        // or a pointer that just moved. Otherwise, only input, a settings change, or a hotkey
        // can, and each of those wakes the wait itself; the idle interval covers pointer moves
        // made with SetCursorPos and the stale-hold check.
        const bool cursorMoved = cursor.x != m_lastPolledCursor.x || cursor.y != m_lastPolledCursor.y;
        m_lastPolledCursor = cursor;
        const bool busy = cursorMoved || m_engine.IsCountingDown() || overToolbar || result.failed || (overlayCreated && m_overlay.NeedsTicks());
        timeout = (busy || !m_wakeEvent) ? POLL_INTERVAL_MS : IDLE_POLL_INTERVAL_MS;
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

void DwellClick::WakeHookThread()
{
    if (m_wakeEvent)
    {
        SetEvent(m_wakeEvent);
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

    if (const auto action = dwellclick::ActionFor(command))
    {
        select(*action);
        return;
    }

    switch (command)
    {
    case ToolbarCommand::TogglePause:
        m_engine.SetPaused(!m_engine.IsPaused());
        break;
    case ToolbarCommand::Back:
        // Back goes to the app in the foreground; an elevated one would silently drop it.
        if (IsInputBlockedForWindow(GetForegroundWindow()))
        {
            Logger::warn(L"DwellClick Back not sent: the foreground window is elevated above PowerToys.");
            m_overlay.FlashBlocked(GetTickCount64());
            break;
        }
        [[fallthrough]];
    case ToolbarCommand::StartMenu:
    case ToolbarCommand::ShowDesktop:
    case ToolbarCommand::TaskView:
    case ToolbarCommand::Notifications:
        // A keystroke into the foreground window (see KeysFor in DwellClickInput.h).
        if (!SendSteps(dwellclick::StepsForKeys(dwellclick::KeysFor(command, IsWindows11OrGreater()))))
        {
            Logger::warn(L"DwellClick toolbar key injection was blocked, error: {}", GetLastError());
        }
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

bool DwellClick::IsActionOffered(dwellclick::DwellAction action) const
{
    using dwellclick::DwellAction;
    if (static_cast<int>(action) == m_defaultAction.load())
    {
        return true;
    }
    switch (action)
    {
    case DwellAction::LeftClick:
        return m_toolbarButtonLeftClick.load();
    case DwellAction::RightClick:
        return m_toolbarButtonRightClick.load();
    case DwellAction::DoubleClick:
        return m_toolbarButtonDoubleClick.load();
    case DwellAction::MiddleClick:
        return m_toolbarButtonMiddleClick.load();
    case DwellAction::Drag:
        return m_toolbarButtonDrag.load();
    case DwellAction::ScrollUp:
        return m_toolbarButtonScrollUp.load();
    case DwellAction::ScrollDown:
        return m_toolbarButtonScrollDown.load();
    case DwellAction::ScrollLeft:
        return m_toolbarButtonScrollLeft.load();
    case DwellAction::ScrollRight:
        return m_toolbarButtonScrollRight.load();
    case DwellAction::ZoomIn:
        return m_toolbarButtonZoomIn.load();
    case DwellAction::ZoomOut:
        return m_toolbarButtonZoomOut.load();
    default:
        return false;
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
        UpdatePointerDpi(data->pt);
        m_engine.OnMove({ data->pt.x, data->pt.y }, GetTickCount64(), SettingsSnapshot());
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        // The user clicked for themselves; do not stack a dwell click on top of it. While the
        // button stays down they may be dragging or selecting, so the engine stays locked for
        // the whole hold, not just until the next move.
        m_externalButtonsHeld |= ButtonBit(wParam, data->mouseData);
        m_lastButtonEventTick = GetTickCount64();
        m_engine.OnPhysicalClick();
        m_engine.SetExternalButtonHeld(true);
        break;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        m_externalButtonsHeld &= static_cast<uint8_t>(~ButtonBit(wParam, data->mouseData));
        m_lastButtonEventTick = GetTickCount64();
        if (m_externalButtonsHeld == 0)
        {
            m_engine.SetExternalButtonHeld(false);
        }
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        // The content scrolled under a resting pointer, so a running countdown is now aimed at
        // something the user never chose. The module's own wheel notches carry the injection
        // tag and were filtered above, so dwell scrolling does not stop itself.
        m_engine.OnPhysicalWheel();
        break;
    default:
        break;
    }
}

extern "C" __declspec(dllexport) PowertoyModuleIface* __cdecl powertoy_create()
{
    return new DwellClick();
}
