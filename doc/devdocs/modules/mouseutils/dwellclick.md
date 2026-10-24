# Dwell Click

> **Status: in development.** The engine, its unit tests and fuzz target, the module DLL, the
> on-screen overlay (countdown ring and action toolbar), the Settings UI page, GPO policy, DSC
> support, and UI tests are all in the repo today. See
> [Not yet implemented](#not-yet-implemented) for what remains.

Dwell Click issues a mouse click automatically when the pointer is held still for a configurable
time. It exists for people who can move a pointer but cannot reliably click it: tremor, limited fine
motor control, RSI, or a pointer driven by a head tracker, eye tracker, joystick, or track ball.

## Implementation

The decision logic lives in a pure, header-only state machine with no Win32 dependency, so it can be
unit tested deterministically. The module layer stays a thin wrapper: it feeds the engine pointer
events and a monotonic tick, and performs the injections the engine asks for.

### Key Files

- [DwellClickCore.h](/src/modules/MouseUtils/DwellClick/DwellClickCore.h) - the engine, with 50
  tests in [EngineTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/EngineTests.cpp)
- [dllmain.cpp](/src/modules/MouseUtils/DwellClick/dllmain.cpp) - the module DLL
  (`PowertoyModuleIface` plus the Win32 adapter)
- [DwellClickInput.h](/src/modules/MouseUtils/DwellClick/DwellClickInput.h) - what each action
  and keystroke button sends to Windows, as data, with 18 tests in
  [InputTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/InputTests.cpp)
- [DwellClickToolbar.h](/src/modules/MouseUtils/DwellClick/DwellClickToolbar.h) - the toolbar's
  Win32-free state machine and layout, with 37 tests in
  [ToolbarModelTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/ToolbarModelTests.cpp)
- [DwellClickOverlay.cpp](/src/modules/MouseUtils/DwellClick/DwellClickOverlay.cpp) - the layered
  windows for the countdown ring, the action toolbar, and the floating large menu
- [DwellClickActionTests.cs](/src/modules/MouseUtils/MouseUtils.UITests.Next/DwellClickActionTests.cs)
  and [DwellClickSettingsTests.cs](/src/modules/MouseUtils/MouseUtils.UITests.Next/DwellClickSettingsTests.cs) -
  25 end-to-end UI tests (see [UI tests](#ui-tests))

### Why the engine is Win32-free

Two seams keep it testable:

- **Click synthesis sits behind `IClickInjector`.** Production supplies a `SendInput` wrapper; tests
  supply a recorder that also simulates failure (an injection blocked by an elevated window).
- **The clock is a caller-supplied tick.** `Poll(tick, settings)` never reads the clock itself, so a
  test drives a whole dwell in a few microseconds and can replay awkward timing exactly.

Settings are passed per call as a `Settings` snapshot rather than held in the engine, matching the
approach used by the Mouse Button Lock engine, so there is no separate configure step to keep in
sync and no torn read between the sampling thread and a settings update.

### The state machine

The engine tracks an **anchor** (where the pointer came to rest) and the tick the current countdown
began. Callers drive it with three entry points:

| Call | Effect |
|---|---|
| `OnMove(pt, tick, s)` | Updates the position. Re-arms a locked machine, or restarts a running countdown, if the pointer moved far enough. Never fires anything. |
| `Poll(tick, s)` | Advances the countdown. Returns progress for the countdown ring, and fires the action when progress reaches 1. |
| `SetNextAction`, `SetPaused`, `OnPhysicalClick`, `OnPhysicalWheel`, `LockUntilMove`, `ReleaseDrag`, `ResetTransient` | Lifecycle and suppression. |

`PollResult` carries `progress` (0 to 1), `fired`, the `action` that actually fired, and the point it
fired at. Progress is 0 whenever the machine is locked or paused, and also on the poll that fires, so
a countdown indicator clears itself without the caller tracking any state.

### Locking: the Midas touch guard

The dominant failure of any dwell system is the "Midas touch" problem: the machine cannot tell a
pointer being *rested* from a pointer being *aimed*, so it fires clicks the user never intended.
Measured error profiles for dwell-style selection are overwhelmingly unintended activations rather
than missed targets, so the engine is biased toward refusing to fire whenever intent is unclear.

The machine **starts locked** and re-locks:

- after every action that fires,
- after a physical mouse click (the user clicked for themselves; do not stack a dwell click on top),
- after a physical wheel turn (`OnPhysicalWheel`: the content scrolled under a resting pointer,
  so a running countdown is no longer aimed at what the user chose),
- when paused,
- on any live settings change, via `LockUntilMove`,
- on `ResetTransient`, so a pointer left resting across a disable/enable cycle cannot fire the
  instant the module returns.

A lock is cleared only by deliberate pointer movement. Nothing else re-arms it, and no amount of
elapsed time will.

### Two tolerances, not one

Movement is gated by two independent distances. This mirrors macOS Dwell Control, which exposes them
as separate settings, and it is the part of the design most easily got wrong:

- **`moveTolerancePixels`** is the jitter a running countdown survives. Drift within it leaves the
  countdown untouched, which is what lets a hand with a tremor complete a dwell at all. Drift beyond
  it re-anchors and restarts.
- **`postActionTolerancePixels`** is how far the pointer must travel *after* an action before a new
  countdown may start. This is what stops a resting hand from firing a second click on the same spot.

Only a post-action lock uses the second distance. Every other lock uses the ordinary tolerance, so a
settings change does not require an exaggerated movement to recover from.

Both comparisons are strictly greater than the threshold, so a move of exactly the tolerance still
counts as resting. Distances are compared as squared `double`: coordinates are 32-bit and a raw
integer product can overflow for extreme inputs, which the eventual fuzz target will drive.

### Actions

`DwellAction` is `LeftClick`, `RightClick`, `DoubleClick`, `MiddleClick`, `Drag`, `ScrollUp`,
`ScrollDown`, `ScrollLeft`, `ScrollRight`, `ZoomIn`, or `ZoomOut`. The engine lowers each to a
`ClickKind` the injector synthesizes, so the injector stays a dumb `SendInput` wrapper and all
sequencing lives in the engine. The values are persisted (`default_action`) and walked in order
by the cycle shortcut, so new actions are only appended, and `LastDwellAction` names the end.
Horizontal scroll is one `MOUSEEVENTF_HWHEEL` notch; zoom is Ctrl plus one wheel notch, pressed
and released in the same `SendInput` batch so Ctrl is never left down.

**Scroll and zoom are modes, not one-shots** (`IsRepeatingAction`). A scroll dwell injects one wheel notch at the rest point,
then restarts its own countdown instead of locking: another notch lands each repeat interval
(`repeatIntervalMs`, 400 ms by default) for as long as the pointer keeps resting, and the action
never reverts to the default (a single notch
snapping back to clicking would make scrolling useless). Movement re-anchors as usual, and a
failed injection still locks rather than retrying. Selecting another action is the exit; this is
the "scroll lock" behavior of the surveyed dwell products. Only the first notch at a rest point
waits the full dwell time, because that one is the decision; one notch per dwell time (three lines
every 1.2 s at the defaults) was too slow to read a page by. Moving to a new spot, or any lock,
goes back to the full dwell time. Scroll cannot be the persisted default
action (the module clamps `default_action` to the click actions); it is entered from the toolbar.

**Drag is two dwells**, which is how every surveyed dwell product expresses click-and-drag without a
held physical button: the first dwell presses the left button, the user moves, and the second dwell
releases it. The action deliberately does *not* revert between the two halves, or nothing would be
able to release the button. `ReleaseDrag` frees a held button on pause, an action change,
disable, or shutdown, so the pointer is never left stuck. It clears the drag only once the
release is accepted: if the OS rejects it, the engine keeps owning the held button, the next
`ReleaseDrag` retries, and the next dwell finishes the release before it runs any other action.

`revertToDefaultAfterAction` returns the next action to `defaultAction` once an action completes, so
a one-off right click does not silently turn every later dwell into a right click. It defaults on
because that is what shipping dwell tools do: GNOME Hover Click and Dwell Clicker 2 both revert
unconditionally, and macOS exposes it as an "Auto revert to left click" toggle.

Reverting is the engine's only path to `defaultAction`: a new engine's next action is `LeftClick`.
The module therefore seeds the configured default on enable and applies a changed one itself (see
[The module layer](#the-module-layer)). Without that, a non-left default would never be used with
revert off, and the first dwell after enabling would be a left click even with revert on.

`SetNextAction` locks until the pointer moves. Without that, the time already banked against the
previous action would fire the newly chosen one almost immediately, on a target that the user was not
aiming at.

### Pause

`SetPaused` suspends dwelling without losing settings, and releases any in-flight drag. Every dwell
tool surveyed ships this escape hatch (the Dwell Clicker 2 "Rest" button, the macOS Pause dwell
action and hot corners, the Android Pause control, the Tobii pause pop-up), because a user who
cannot click also cannot easily stop a machine that clicks for them. It locks in both directions:
entering pause abandons the running countdown, and leaving it does not resume against time that
elapsed while the user was away.

### Degenerate inputs

The engine stays defined for any input rather than trusting its caller:

- `dwellTimeMs` below 1 is clamped to 1 ms, so progress never divides by zero or becomes NaN.
- Negative pixel tolerances clamp to 0.
- A tick older than the anchor is treated as 0 elapsed. Unsigned subtraction would otherwise wrap to
  a huge value and fire instantly.
- A failed injection still locks, so a blocked injection does not retry on every poll for as long as
  the pointer happens to rest there. It does *not* consume the chosen action, since the user asked
  for a click and never got one.

## The module layer

[dllmain.cpp](/src/modules/MouseUtils/DwellClick/dllmain.cpp) is the Win32 adapter around the
engine, following the Mouse Button Lock module closely:

- **One dedicated thread** owns a `WH_MOUSE_LL` hook and a poll loop. The hook feeds the engine
  `OnMove` for every pointer move, `OnPhysicalClick` for every physical button-down, and
  `OnPhysicalWheel` for every wheel turn on either axis that the module did not inject; the loop
  wakes every 15 ms (`MsgWaitForMultipleObjects` timeout) to pump messages and call `Poll`, which
  is what advances a countdown while the pointer is at rest and no messages arrive.
- **A button held by anything else suppresses dwelling for the whole hold.** The hook tracks
  untagged button presses and releases per button and calls `SetExternalButtonHeld`; while any is held
  the engine stays locked whatever the pointer does, so a dwell can never cut short the user's
  own drag or selection (or another assistive tool's held press). Letting go locks until the
  pointer moves. A hold whose button-up the hook missed is cleared once every button reads up
  with `GetAsyncKeyState`, checked only when no button event has arrived for 500 ms because the
  hook runs before the async key state updates.
- **What each action sends lives in one table.** `StepsFor` (each `ClickKind`) and `KeysFor`
  (each keystroke button) in DwellClickInput.h describe the input as plain data, `SendSteps`
  turns it into one `SendInput` batch, and InputTests.cpp pins every entry. A zoom batch holds
  Ctrl for the instant of its notch; if the process is killed in that instant Windows can leave
  Ctrl down until it is next pressed, which was seen when the UI-test harness killed PowerToys
  mid-zoom.
- **Injection is a tagged `SendInput`.** Every synthetic event carries a `dwExtraInfo` tag so the
  hook can ignore the module's own clicks; without that, every fired click would immediately
  re-lock the machine against its own echo. Only the module's own tag is filtered. `LLMHF_INJECTED`
  is deliberately not: input injected by other software (a head or eye tracker, remote desktop) is
  exactly the input this module serves, so injected moves arm the machine and injected clicks lock
  it, the same as their physical counterparts.
- **The engine stays single-threaded.** All engine calls happen on the hook thread while it runs.
  The runner thread touches the engine only before the thread starts (`enable` seeds the pointer
  position, calls `ResetTransient`, and sets the next action to the configured default) or after
  it joins (`disable`). A live settings change (`set_config`) raises an atomic flag that the poll
  loop consumes into `LockUntilMove`. The ordering matters: `set_config` raises the flag before
  publishing the new values (and again after), and the loop reads its settings snapshot before
  consuming the flag, so a snapshot holding any new value is always followed by the lock that
  value requires before `Poll` runs. Otherwise, a dwell time shortened mid-countdown could fire
  at once.
- **A changed default action applies on the next dwell.** On that same re-lock path the poll loop
  compares the configured default with the one it last applied (`m_appliedDefaultAction`). Only
  when it differs does it release a held drag and call `SetNextAction`, so an unrelated change
  (the dwell time, say) never discards an action that the user picked on the toolbar.
- **Settings are clamped on read.** The dwell time is clamped to 100 ms - 60 s (below 100 ms every
  rest becomes a click, the exact Midas failure the engine is built to avoid), tolerances to
  0 - 10000 px, and an unknown `default_action` value falls back to `LeftClick` rather than
  clamping to whatever enum sits at the range edge.
- **Hotkeys reach the engine through flags.** `get_hotkeys` reports two shortcuts and `on_hotkey`
  runs on the runner thread, so it only raises an atomic flag (`m_pauseToggleRequested`,
  `m_cycleActionRequested`) that the poll loop consumes, exactly like the re-lock. See
  [Hotkeys](#hotkeys).
- **Shutdown never strands a drag.** The hook thread's exit path (disable, destroy, or a failed
  wait) releases a held drag button via `ReleaseDrag`.

The module is registered in the runner's known-modules list, the solution, the ESRP signing list,
the installation verification script, the bug report tool's GPO dump, and telemetry
(`DwellClick_EnableDwellClick` on enable/disable, registered in DATA_AND_PRIVACY.md). The
`ConfigureEnabledUtilityDwellClick` GPO policy is defined in the ADMX/ADML and surfaced through
`GPOWrapper`. It is disabled by default.

## Settings

The module lives on the Mouse Utilities settings page (`MouseUtilsPage.xaml`) with a toggle, a
dwell-time slider (200 ms to 5 s; the module clamps hand-edited values to 100 ms - 60 s), a
scroll and zoom repeat slider (100 ms to 2 s; clamped to 50 ms - 60 s), a
default-action dropdown (left, right, double, middle, drag), a revert-to-default checkbox,
number boxes for the two tolerances (0 - 100 px), and the overlay options: action toolbar on/off,
toolbar side (left, right, top, or bottom edge), toolbar style (docked bar or floating button
with a large menu), position along the edge (0 to 100%, labeled by `PercentLabelConverter`),
countdown ring on/off, overlay size (small 40 px / medium 56 px / large 72 px buttons, with the
ring scaled to match; medium is the default because dwell targets obey Fitts's law and this
audience needs room to land on them), and seventeen checkboxes choosing which buttons the
toolbar carries (see [Placement and style](#placement-and-style)). Scroll and zoom are never
offered as the default action: the module clamps `default_action` to the click actions, so they
are reached from the toolbar or the cycle shortcut. `DwellClickProperties` must keep its
defaults in sync with the engine and dllmain.cpp. DSC v3 can configure every property
(`doc/dsc/modules/DwellClick.md`), and the module appears in OOBE's Mouse Utilities page.

## Hotkeys

Two shortcuts, both parsed with `PowerToysSettings::HotkeyObject::from_json` (a shortcut with no
modifier falls back to its default so a bare key is never swallowed system-wide). Both do nothing
while the module is disabled, because the runner delivers hotkeys regardless.

| Id | Setting | Default | Effect |
|---|---|---|---|
| 0 | `activation_shortcut` | Win+Shift+W | Toggles pause with `SetPaused`, the call the toolbar's pause button makes. |
| 1 | `cycle_action_shortcut` | Win+Shift+A | Releases a held drag, then `SetNextAction` to the next offered `DwellAction` in enum order, wrapping. An action is offered when it is the default action or its toolbar button is switched on (`IsActionOffered`), so an action turned off in Settings never comes up. The toolbar highlight follows because the poll loop pushes `NextAction()` into the toolbar every tick. |

## Click sound

With `play_sound` on, a fired action plays a 40 ms, 1 kHz click. The WAV image is synthesized in
memory by `BuildClickSound` (no binary asset ships), built in `enable` before the hook thread starts,
and played with `PlaySoundW(SND_MEMORY | SND_ASYNC | SND_NODEFAULT)` from the poll loop when
`PollResult.fired` is set. `disable` stops any sound still playing before the buffer is freed.

## Ring color

`indicator_use_accent_color` (default `true`) and `indicator_color` (`"#RRGGBB"`, default
`#0078D4`, parsed with `checkValidRGB`; a malformed string keeps the previous value) travel the same
path as `show_countdown` and `overlay_size`: `OverlaySettings` into `Overlay::ApplySettings`. The
ring's arc is repainted every tick, so a change shows immediately. The accent color is read with
`DwmGetColorizationColor` when a countdown starts.

## The overlay

The interaction model follows Ease Mouse and its open-source clone OpenMouse, which pair a
pointer-attached countdown with an on-screen action menu:

- **The countdown ring** follows the pointer and fills with `PollResult.progress`, so the user
  always sees when a click is about to land. Its window is layered, topmost, and click-through;
  it can never take input or focus. It hides whenever the machine is locked, paused, or the
  pointer is over the toolbar (progress is 0 in all three).
- **The action toolbar** docks to any screen edge: a collapse handle, pause/resume, and a
  Settings-chosen set of buttons. The action buttons choose the next dwell action (left, double,
  right, middle, drag, scroll up/down/left/right, zoom in/out). The system buttons act at once
  instead, as a keystroke into the foreground window, which is still the app the user was in
  because the toolbar never activates: Back (`VK_BROWSER_BACK`), Start menu (Win), Show desktop
  (Win+D), Task view (Win+Tab), and Notifications (Win+N on Windows 11, Win+A on Windows 10).
  These are the Windows counterparts of Ease Mouse's Back, Home, Overview, and Notifications.
  Middle click and every button added after scroll up/down are off by default, so the shipping
  bar keeps its length. Buttons activate by HOVERING
  for the dwell time, with the button filling as feedback; a physical click activates
  immediately. Both paths share the leave-to-rearm rule, so parking on Pause toggles once, not
  once per dwell time. The block names the command, not the button index, and survives a
  layout change, so collapsing or expanding with the pointer parked on the handle (which stays put at
  position 0%) toggles once too. It is cleared on any tick where the pointer is not over that
  command, so a handle that moved away in the new layout can still be dwelled on. And whenever the
  toolbar appears or moves (startup, a settings change, the large menu opening), the button that
  lands under a resting pointer is blocked the same way (`ToolbarModel::BlockAt`): like the
  engine, the toolbar never acts on a pointer that is merely sitting there. The gear exists because a dwell user cannot reach the tray icon: it
  launches the standard `PowerToys.exe --open-settings=MouseUtils` deep link.
- **While the pointer is over the toolbar, the engine stays locked** (the poll loop re-asserts
  `LockUntilMove` each tick). Dwells over the toolbar therefore select buttons instead of firing
  the current action onto them, and, since pause stops the engine but never the toolbar, the
  resume button stays reachable while paused: the answer to "a user who cannot click also cannot
  un-pause".
- Selecting a new action mid-drag releases the held button first (`ReleaseDrag`), so switching
  away from a half-finished drag can never leave the left button stuck.
- The toolbar's hover/hit-test/re-arm logic lives in the Win32-free `ToolbarModel`
  (DwellClickToolbar.h), unit tested and driven by the fuzz target; DwellClickOverlay.cpp is the
  thin layered-window and GDI+ painting shell around it, living entirely on the hook thread so
  the engine stays single-threaded. Glyphs are drawn as shapes (no text), so the overlay
  needs no localized resources.

Both surfaces can be turned off in Settings (`show_toolbar`, `show_countdown`), and
`overlay_size` scales the buttons and the ring together (small / medium / large; medium is the
default because dwell targets obey Fitts's law).

### Placement and style

Placement follows Ease Mouse's dock and button-position settings:

- `toolbar_side` docks the bar to the left, right, top, or bottom edge. The side edges stack
  buttons down; the top and bottom edges run them across (`ToolbarOrientation`). The collapse
  chevron turns to point into whichever edge the bar is on.
- `toolbar_position` centers the bar at 0 to 100% along that edge, clamped so the bar stays fully
  inside the work area. Users move it off content they need to reach.
- A bar longer than its edge wraps into another column (or row) instead of running off screen:
  `ToolbarModel::ButtonsThatFit` sizes a line from the work area, and `SetLayout` flows the
  buttons into as many lines as they need.
- `toolbar_style` 1 replaces the bar with Ease Mouse's "Big" menu: a single floating button (a
  3 x 3 dot grid) at the same edge and position. Dwelling on it opens a near-square grid of
  larger buttons (64 / 80 / 96 px for the three overlay sizes) centered on the work area, with a
  close button in the handle's slot. `ToolbarModel::FitGrid` makes the grid fit the work area in
  both directions, taking more columns or stepping the buttons down (menu size, bar size, 40 px,
  24 px) as needed. If even 24 px overflows, the menu is pinned to the work area's top-left so
  its first row, Close and Pause, is always reachable. It is the same window and `ToolbarModel` as the bar, with
  the handle's collapsed state meaning "menu closed". Any choice closes the menu, and the menu
  was in the middle of the screen, so the pointer is never left resting on the reappearing
  button. While the menu is open the poll loop keeps the engine locked wherever the pointer is
  (`Overlay::IsMenuOpen`), so nothing is clicked behind it.

The toolbar docks to the primary monitor.

## Defaults

| Setting | Default | Source |
|---|---|---|
| `dwellTimeMs` | 1200 | The GNOME Hover Click `dwell-time` default, the closest mouse-driven analogue. Eye-gaze research optima cluster nearer 600 ms, but assume a tracker rather than a hand on a mouse. macOS defaults to 3 s. |
| `moveTolerancePixels` | 10 | The GNOME `dwell-threshold` default. |
| `postActionTolerancePixels` | 10 | No published default found; starts equal to the move tolerance and is intended to be widened independently. |
| `repeatIntervalMs` | 400 | About 7 lines a second at the usual 3 lines per notch: fast enough to read a page by, slow enough to stop on the line you want. |
| `defaultAction` | `LeftClick` | Universal across surveyed products. |
| `revertToDefaultAfterAction` | `true` | GNOME Hover Click and Dwell Clicker 2 behavior. |

The adapter and overlay settings (not part of the engine; `DwellClickProperties` and dllmain.cpp
must agree on all of these):

| Setting | Default | Notes |
|---|---|---|
| `show_toolbar` | `true` | The toolbar is the action-selection surface; hiding it leaves only the default action. |
| `toolbar_side` | `0` (left edge) | 1 = right, 2 = top, 3 = bottom. |
| `toolbar_position` | `50` | Percent along the edge (0 to 100) where the toolbar or floating button is centered. |
| `toolbar_style` | `0` (docked bar) | 1 = floating button that opens a large menu in the middle of the screen. |
| `show_countdown` | `true` | |
| `overlay_size` | `1` (medium, 56 px buttons) | 0 = 40 px, 2 = 72 px; the countdown ring scales to match. |
| `activation_shortcut` | Win+Shift+W | Pause or resume. |
| `cycle_action_shortcut` | Win+Shift+A | Next action. |
| `play_sound` | `false` | Click sound when an action fires. |
| `indicator_use_accent_color` | `true` | Ring uses the Windows accent color. |
| `indicator_color` | `"#0078D4"` | Ring color when the accent is off. |
| `toolbar_button_*` | `true` for left, double, right, drag, scroll up, scroll down, and open settings; `false` for the rest | Seventeen booleans choosing the action buttons; collapse and pause are always shown. Off by default: middle click, scroll left and right, zoom in and out, back, start, show desktop, task view, and notifications. |

## Testing

[EngineTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/EngineTests.cpp) has 39 tests in
eight classes, using a recording fake injector and an explicit tick:

- `DwellCountdown` - starts locked, tolerance boundaries, firing at the threshold, progress
  reporting, and the degenerate inputs above
- `PostActionTolerance` - the two distances are independent, and a non-action lock uses the ordinary
  tolerance
- `ActionSelection` - action-to-click mapping, revert versus sticky, and locking on action change
- `DragGesture` - the two-dwell lifecycle, release on reset, and a failed press not entering drag
  state
- `PauseAndSuppression` - pause, resume, pause mid-drag, physical-click and wheel suppression, and the
  live-settings-change lock
- `InjectionFailure` - failures are reported, not retried per poll, and do not consume the action
- `ScrollActions` - auto-repeat while resting, no revert, movement re-anchoring, and a failed
  scroll locking instead of retrying
- `HorizontalScrollAndZoomActions` - the four newer actions map to their own click kinds,
  auto-repeat, never revert, and `LastDwellAction` names the end of the enum
- `ReviewRegressions` - a rejected cleanup release keeps the drag until a retry succeeds, the
  next dwell finishes a pending release first, and an external button hold suppresses dwelling
  until it ends and the pointer moves
- `RepeatInterval` - the first notch waits the dwell time and later notches the repeat interval,
  movement and locks go back to the full dwell, clicks ignore the interval, and a 0 ms interval
  clamps

(50 tests in ten classes in all.)

[ToolbarModelTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/ToolbarModelTests.cpp) adds
37 tests over the toolbar model in seven classes: layout and hit-testing, the configurable
button set and size, the hover dwell (activation, jitter, re-arm, tick wrap), physical clicks
plus the collapse handle, horizontal flow and wrapping (`SetLayout`, `ButtonsThatFit`), the
command-to-action mapping (every action has a toolbar command; system commands choose none),
and the review regressions: the leave-to-rearm block surviving a layout change without trapping a
moved handle, a button that appears under a resting pointer (`BlockAt`), and `FitGrid` for
roomy, short, small high-DPI, and impossible areas.

[InputTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/InputTests.cpp) has 18 tests in
two classes that pin what each action actually sends to Windows, from
[DwellClickInput.h](/src/modules/MouseUtils/DwellClick/DwellClickInput.h), the one table the
module builds its `SendInput` batches from: one test per dwell action (each click kind, the
full two-dwell drag, both wheels in both directions, and zoom as Ctrl down, wheel, Ctrl up), and
the keys for every toolbar keystroke button (Back, Start menu, Show desktop, Task view, and
Notifications on Windows 11 and 10). A last test in each class fails when an action or command
is added without one.

Build and run:

```
cd src\modules\MouseUtils\DwellClick.UnitTests
..\..\..\..\tools\build\build.ps1 -Platform x64 -Configuration Debug
vstest.console.exe x64\Debug\tests\DwellClick\DwellClick.UnitTests.dll /Platform:x64
```

Beyond the unit tests:

- [DwellClick.FuzzingTest](/src/modules/MouseUtils/DwellClick.FuzzingTest) drives the engine with
  arbitrary event sequences, ticks, coordinates, and settings under ASan + libFuzzer, mirroring
  `MouseButtonLock.FuzzingTest`.
- [DwellClickSettingsTests.cs](/src/modules/MouseUtils/MouseUtils.UITests.Next/DwellClickSettingsTests.cs)
  covers settings persistence and the module's live on/off state, and
  [DwellClickActionTests.cs](/src/modules/MouseUtils/MouseUtils.UITests.Next/DwellClickActionTests.cs)
  covers every action end to end (see [UI tests](#ui-tests)). The settings suite observes the
  module through a Drag-action dwell's held button (`GetAsyncKeyState`), so its enabled/disabled
  state and the two-dwell drag are asserted through it. Both suites lean on an engine guarantee
  for their own safety: settings changes and physical clicks lock the machine until the pointer
  moves, so pattern-driven UIA interactions cannot fire stray dwells.
- The DSC suite has a `DwellClick` module test
  ([SettingsResourceDwellClickModuleTest.cs](/src/dsc/v3/PowerToys.DSC.UnitTests/SettingsResourceTests/SettingsResourceDwellClickModuleTest.cs)).

### UI tests

Two classes, all in the `DwellClick` test category. They share one snapshot of the user's
Dwell Click settings (`DwellClickSettingsSnapshot`) so neither restores the other's test values.

[DwellClickActionTests.cs](/src/modules/MouseUtils/MouseUtils.UITests.Next/DwellClickActionTests.cs)
has 21 end-to-end tests, at least one per toolbar button. Each picks its action the way a user does, by
dwelling on that button of the live toolbar, then rests the pointer on a recorder window
([DwellClickActionRecorder.cs](/src/modules/MouseUtils/MouseUtils.UITests.Next/DwellClickActionRecorder.cs))
that logs every mouse button, wheel, drag-move, and key message it receives, and asserts on what
an application actually saw:

- Left, right, double, and middle click each land once, as the right message, where the pointer
  rested.
- Drag presses at one spot and releases at another with button-down moves in between, and moves
  a window by its title bar as far as the pointer went, both a classic caption and the app-drawn
  WinUI 3 title bar of PowerToys Settings.
- The four scrolls and both zooms land a first notch after the dwell time and then repeat at the
  faster repeat interval (250 ms against a 500 ms dwell in the suite), with the right delta and
  Ctrl state, and zoom never leaves Ctrl down.
- The keystroke buttons are checked by the exact keys the module injects, which the recorder's
  low-level keyboard hook picks out by Dwell Click's injection tag; the shell surfaces they open
  do not reliably take the foreground in time to be observed. Back is also seen arriving at the
  focused window, and Show desktop is also checked by its effect.
- Pause stops clicks until pressed again, Collapse shrinks the bar to its handle and back, and
  Open Settings restores a minimized PowerToys Settings.

Each test starts with the pointer off the toolbar and fails if any modifier key is held before or
after it, releasing it so later tests (and the user's typing) are not affected. Repeating scroll
and zoom are paused before a test ends, because the harness kills PowerToys after every test and
a kill in the middle of a zoom notch was seen to leave Ctrl down.

The four settings tests:

| Test | What it proves |
|---|---|
| `SectionNavigationAndModuleLifecycleAreAvailable` | The settings group is reachable; disabling the module stops a dwell from starting the seeded Drag, and re-enabling restores it. |
| `ActionAndRevertPersistAcrossRestart` | The action, revert, toolbar (edge, style, position, a new button), overlay-size, and countdown options persist to `settings.json` and reload after a Settings restart. |
| `DwellTimeAndTolerancesPersistAtBoundaries` | The dwell time and both tolerances persist at their minimum and maximum, and the dwell time reloads after restart. |
| `ADwellCompletesADragAndASecondDwellReleasesIt` | A dwell presses and holds the left button, a second dwell releases it, and the engine re-arms for another drag. |

All 24 passed on a host desktop against a local x64 Debug build on 2026-10-18 (the Show desktop
test minimizes the user's windows and restores them, but some apps, Windows Settings among them,
may need restoring by hand afterwards). They have not yet
run on the Windows 10/11 VMs from the
[local-VM UI-tests skill](/.github/skills/ui-tests-local-vm/SKILL.md). To run them on a host
desktop:

1. Build the module, the runner, Settings, and the test project. A Debug runner only warns about
   module DLLs that are not built, so these four are enough. Build the runner with
   `/p:SolutionDir=<repo root>\`: built on its own it writes `PowerToys.exe` under `src\runner\x64`,
   and the harness then silently falls back to an installed PowerToys that has no Dwell Click.

   ```
   tools\build\build.ps1 -Platform x64 -Configuration Debug -Path <repo>\src\modules\MouseUtils\DwellClick '/restore' '/p:RestorePackagesConfig=true'
   tools\build\build.ps1 -Platform x64 -Configuration Debug -Path <repo>\src\runner '/restore' '/p:RestorePackagesConfig=true' '/p:SolutionDir=<repo>\'
   tools\build\build.ps1 -Platform x64 -Configuration Debug -Path <repo>\src\settings-ui\Settings.UI '/restore'
   tools\build\build.ps1 -Platform x64 -Configuration Debug -Path <repo>\src\modules\MouseUtils\MouseUtils.UITests.Next '/restore'
   ```

2. Install the pinned winappcli with `.pipelines\InstallWinAppCli.ps1 -Platform x64` and set
   `WINAPP_CLI_PATH` to the `winapp.exe` it prints.
3. Exit any running PowerToys, and back up the `*.json` files under
   `%LOCALAPPDATA%\Microsoft\PowerToys`. The harness restores the files it edits, but the dev
   build's Runner and Settings also migrate other modules' settings and create folders for modules
   an installed PowerToys may not know. Restore the backup afterwards.
4. Run the suite without touching the mouse or keyboard:

   ```
   x64\Debug\tests\MouseUtils.UITests.Next\net10.0-windows10.0.26100.0\MouseUtils.UITests.Next.exe --filter TestCategory=DwellClick --report-trx --results-directory <dir> --timeout 20m
   ```

Harness behavior the suite works around, worth knowing before adding tests:

- `Click`, `ComboBox.Select`, and `CheckBox.SetCheck` click with the real mouse at the control's UIA
  bounds. A Settings control below the fold reports `IsOffscreen=false` with bounds outside the
  window, so the click misses. Call `ScrollIntoView()` and `Find` the control again first.
- winappcli 0.3.2 cannot read a WinUI `Slider`'s value (`get-value` finds none, so `Slider.Value`
  returns 0). The test reads the label beside the dwell-time slider instead
  (`MouseUtils_DwellClickDwellTimeLabelId`), which binds the same property.
- Settings rewrites `settings.json` on every change, so polling it can hit an `IOException` or a
  half-written file. The persistence asserts pass `shouldRetryException` to
  `WaitHelper.WaitForStable`.

## Not yet implemented

Still to build:

- Post-dwell directional gestures (the GNOME Hover Click alternative mode) would need engine work;
  the toolbar covers action selection without them.
- The toolbar docks to the primary monitor only; per-monitor placement (following the pointer's
  monitor) is a possible follow-up.
- Two Ease Mouse options were considered and set aside for now: a filtered physical-button click
  mode (a press counts only when held for the dwell time, and extra presses are ignored for a
  while), which would mean suppressing real clicks for the first time; and scroll buttons parked
  in the screen corners. Ease Mouse's pointer cross is left to the existing Mouse Pointer
  Crosshairs module.
- Killing the PowerToys process while a zoom notch is holding Ctrl can leave Ctrl down (see
  [The module layer](#the-module-layer)); a graceful disable is not affected.

Command Palette is deliberately unwired, matching the Mouse Button Lock decision: the sibling
CmdPal entries fire a module trigger event, and this module has no trigger event; activation is
resting the pointer.

## Design provenance

Behavior was derived from a survey of shipping dwell implementations rather than a port of any one of
them: GNOME Hover Click, macOS Dwell Control, Windows Eye Control, Dwell Clicker 2,
Point-N-Click, ClickAid, Tobii Dynavox Windows Control, Grid 3, and the Android auto click dwell
timing feature, together with the dwell-selection literature on the Midas touch problem, dwell-time
defaults, and countdown feedback.
