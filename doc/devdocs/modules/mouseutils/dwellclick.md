# Dwell Click

> **Status: in development.** The engine, its unit tests and fuzz target, the module DLL, the
> on-screen overlay (countdown ring and action toolbar), the Settings UI page, GPO policy, DSC
> support, and UI tests are all in the repo today. See
> [Not yet implemented](#not-yet-implemented) for what remains.

Dwell Click issues a mouse click automatically when the pointer is held still for a configurable
time. It exists for people who can move a pointer but cannot reliably click it: tremor, limited fine
motor control, RSI, or a pointer driven by a head tracker, eye tracker, joystick, or trackball.

## Implementation

The decision logic lives in a pure, header-only state machine with no Win32 dependency, so it can be
unit tested deterministically. The module layer stays a thin wrapper: it feeds the engine pointer
events and a monotonic tick, and performs the injections the engine asks for.

### Key Files

- [DwellClickCore.h](/src/modules/MouseUtils/DwellClick/DwellClickCore.h) - the engine
- [EngineTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/EngineTests.cpp) - 30 unit tests
- [dllmain.cpp](/src/modules/MouseUtils/DwellClick/dllmain.cpp) - the module DLL
  (`PowertoyModuleIface` plus the Win32 adapter)
- [DwellClickToolbar.h](/src/modules/MouseUtils/DwellClick/DwellClickToolbar.h) - the toolbar's
  Win32-free state machine, tested by
  [ToolbarModelTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/ToolbarModelTests.cpp)
- [DwellClickOverlay.cpp](/src/modules/MouseUtils/DwellClick/DwellClickOverlay.cpp) - the layered
  windows for the countdown ring and the action toolbar

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
| `SetNextAction`, `SetPaused`, `OnPhysicalClick`, `LockUntilMove`, `ReleaseDrag`, `ResetTransient` | Lifecycle and suppression. |

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

`DwellAction` is `LeftClick`, `RightClick`, `DoubleClick`, `MiddleClick`, `Drag`, `ScrollUp`, or
`ScrollDown`. The engine lowers each to a `ClickKind` the injector synthesizes, so the injector
stays a dumb `SendInput` wrapper and all sequencing lives in the engine.

**Scroll is a mode, not a one-shot.** A scroll dwell injects one wheel notch at the rest point,
then restarts its own countdown instead of locking: another notch lands each dwell period for as
long as the pointer keeps resting, and the action never reverts to the default (a single notch
snapping back to clicking would make scrolling useless). Movement re-anchors as usual, and a
failed injection still locks rather than retrying. Selecting another action is the exit; this is
the "scroll lock" behavior of the surveyed dwell products. Scroll cannot be the persisted default
action (the module clamps `default_action` to the click actions); it is entered from the toolbar.

**Drag is two dwells**, which is how every surveyed dwell product expresses click-and-drag without a
held physical button: the first dwell presses the left button, the user moves, and the second dwell
releases it. The action deliberately does *not* revert between the two halves, or nothing would be
able to release the button. `ReleaseDrag` frees a held button on pause, disable, or shutdown, so the
pointer is never left stuck.

`revertToDefaultAfterAction` returns the next action to `defaultAction` once an action completes, so
a one-off right click does not silently turn every later dwell into a right click. It defaults on
because that is what shipping dwell tools do: GNOME Hover Click and Dwell Clicker 2 both revert
unconditionally, and macOS exposes it as an "Auto revert to left click" toggle.

`SetNextAction` locks until the pointer moves. Without that, the time already banked against the
previous action would fire the newly chosen one almost immediately, on a target the user was not
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
  `OnMove` for every pointer move and `OnPhysicalClick` for every physical button-down; the loop
  wakes every 15 ms (`MsgWaitForMultipleObjects` timeout) to pump messages and call `Poll`, which
  is what advances a countdown while the pointer is at rest and no messages arrive.
- **Injection is a tagged `SendInput`.** Every synthetic event carries a `dwExtraInfo` tag so the
  hook can ignore the module's own clicks; without that, every fired click would immediately
  re-lock the machine against its own echo. Only the module's own tag is filtered. `LLMHF_INJECTED`
  is deliberately not: input injected by other software (a head or eye tracker, remote desktop) is
  exactly the input this module serves, so injected moves arm the machine and injected clicks lock
  it, the same as their physical counterparts.
- **The engine stays single-threaded.** All engine calls happen on the hook thread while it runs.
  The runner thread touches the engine only before the thread starts (`enable` seeds the pointer
  position and calls `ResetTransient`) or after it joins (`disable`). A live settings change
  (`set_config`) raises an atomic flag that the poll loop consumes into `LockUntilMove`.
- **Settings are clamped on read.** The dwell time is clamped to 100 ms - 60 s (below 100 ms every
  rest becomes a click, the exact Midas failure the engine is built to avoid), tolerances to
  0 - 10000 px, and an unknown `default_action` value falls back to `LeftClick` rather than
  clamping to whatever enum sits at the range edge.
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
default-action dropdown (left, right, double, middle, drag), a revert-to-default checkbox,
number boxes for the two tolerances (0 - 100 px), and the overlay options: action toolbar on/off,
toolbar side (left/right edge), countdown ring on/off, and eight checkboxes choosing which
buttons the toolbar carries. `DwellClickProperties` must keep its
defaults in sync with the engine and dllmain.cpp. DSC v3 can configure every property
(`doc/dsc/modules/DwellClick.md`), and the module appears in OOBE's Mouse Utilities page.

## The overlay

The interaction model follows Ease Mouse and its open-source clone OpenMouse, which pair a
pointer-attached countdown with an on-screen action menu:

- **The countdown ring** follows the pointer and fills with `PollResult.progress`, so the user
  always sees when a click is about to land. Its window is layered, topmost, and click-through;
  it can never take input or focus. It hides whenever the machine is locked, paused, or the
  pointer is over the toolbar (progress is 0 in all three).
- **The action toolbar** docks to the left or right screen edge: a collapse handle, pause/resume,
  and a Settings-chosen set of action buttons (left, double, right, middle, drag, scroll up,
  scroll down, and an open-Settings gear; middle is off by default). Buttons activate by HOVERING
  for the dwell time, with the button filling as feedback; a physical click activates
  immediately. Both paths share the leave-to-rearm rule, so parking on Pause toggles once, not
  once per dwell time. The gear exists because a dwell user cannot reach the tray icon: it
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
  the engine stays single-threaded. Glyphs are drawn as pictograms (no text), so the overlay
  needs no localized resources.

Both surfaces can be turned off in Settings (`show_toolbar`, `show_countdown`), and the toolbar
edge is `toolbar_side`. The toolbar currently docks to the primary monitor.

## Defaults

| Setting | Default | Source |
|---|---|---|
| `dwellTimeMs` | 1200 | The GNOME Hover Click `dwell-time` default, the closest mouse-driven analogue. Eye-gaze research optima cluster nearer 600 ms, but assume a tracker rather than a hand on a mouse. macOS defaults to 3 s. |
| `moveTolerancePixels` | 10 | The GNOME `dwell-threshold` default. |
| `postActionTolerancePixels` | 10 | No published default found; starts equal to the move tolerance and is intended to be widened independently. |
| `defaultAction` | `LeftClick` | Universal across surveyed products. |
| `revertToDefaultAfterAction` | `true` | GNOME Hover Click and Dwell Clicker 2 behavior. |

## Testing

[EngineTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/EngineTests.cpp) has 30 tests in six
classes, using a recording fake injector and an explicit tick:

- `DwellCountdown` - starts locked, tolerance boundaries, firing at the threshold, progress
  reporting, and the degenerate inputs above
- `PostActionTolerance` - the two distances are independent, and a non-action lock uses the ordinary
  tolerance
- `ActionSelection` - action-to-click mapping, revert versus sticky, and locking on action change
- `DragGesture` - the two-dwell lifecycle, release on reset, and a failed press not entering drag
  state
- `PauseAndSuppression` - pause, resume, pause mid-drag, physical-click suppression, and the
  live-settings-change lock
- `InjectionFailure` - failures are reported, not retried per poll, and do not consume the action

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
  covers settings persistence and one live behavior: a Drag-action dwell's held button is the one
  externally observable effect (`GetAsyncKeyState`), so the module's enabled/disabled state and the
  two-dwell drag are asserted through it. The suite leans on an engine guarantee for its own
  safety: settings changes and physical clicks lock the machine until the pointer moves, so
  pattern-driven UIA interactions cannot fire stray dwells.
- The DSC suite has a `DwellClick` module test
  ([SettingsResourceDwellClickModuleTest.cs](/src/dsc/v3/PowerToys.DSC.UnitTests/SettingsResourceTests/SettingsResourceDwellClickModuleTest.cs)).

## Not yet implemented

Still to build:

- Post-dwell directional gestures (the GNOME Hover Click alternative mode) would need engine work;
  the toolbar covers action selection without them.
- The toolbar docks to the primary monitor only; per-monitor placement (following the pointer's
  monitor) is a possible follow-up.
- A hotkey to toggle pause without reaching the toolbar.

Command Palette is deliberately unwired, matching the Mouse Button Lock decision: the sibling
CmdPal entries fire a module trigger event, and this module has no trigger event; activation is
resting the pointer.

## Design provenance

Behavior was derived from a survey of shipping dwell implementations rather than a port of any one of
them: GNOME Hover Click, macOS Dwell Control, Windows Eye Control, Dwell Clicker 2,
Point-N-Click, ClickAid, Tobii Dynavox Windows Control, Grid 3, and the Android auto click dwell
timing feature, together with the dwell-selection literature on the Midas touch problem, dwell-time
defaults, and countdown feedback.
