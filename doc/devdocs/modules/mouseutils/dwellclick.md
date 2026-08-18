# Dwell Click

> **Status: in development.** Only the engine and its unit tests are in the repo today. There is no
> module DLL, settings, or UI yet. See [Not yet implemented](#not-yet-implemented) for what remains.

Dwell Click issues a mouse click automatically when the pointer is held still for a configurable
time. It exists for people who can move a pointer but cannot reliably click it: tremor, limited fine
motor control, RSI, or a pointer driven by a head tracker, eye tracker, joystick, or trackball.

## Implementation

The decision logic lives in a pure, header-only state machine with no Win32 dependency, so it can be
unit tested deterministically. The eventual module layer stays a thin wrapper: it samples the cursor,
supplies a monotonic tick, and performs the injections the engine asks for.

### Key Files

- [DwellClickCore.h](/src/modules/MouseUtils/DwellClick/DwellClickCore.h) - the engine
- [EngineTests.cpp](/src/modules/MouseUtils/DwellClick.UnitTests/EngineTests.cpp) - 30 unit tests

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

`DwellAction` is `LeftClick`, `RightClick`, `DoubleClick`, `MiddleClick`, or `Drag`. The engine
lowers each to a `ClickKind` the injector synthesizes, so the injector stays a dumb `SendInput`
wrapper and all sequencing lives in the engine.

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

## Not yet implemented

The engine is only the decision layer. Still to build:

- The module DLL (`PowertoyModuleIface`): cursor sampling thread, a `SendInput` injector, settings
  load and parse, and telemetry. Mouse Button Lock is the closest template.
- A countdown indicator at the cursor. This is not cosmetic: without visible feedback a user cannot
  tell when a click is about to land, and feedback design is tied directly to error rates in the
  dwell literature. Feedback should stay simple at short dwell times, where richer multi-level cues
  were found confusing.
- Action-selection UX. The engine already supports a settings dropdown, a hotkey cycle, or a floating
  toolbar without changes. Post-dwell directional gestures, the GNOME alternative mode, would need
  engine work.
- Settings registration: `MouseUtilsPage.xaml` and its view model, `Settings.UI.Library` classes,
  `ModuleType`, GPO policy and ADMX, OOBE, and DSC.
- A fuzz target over the engine, mirroring `MouseButtonLock.FuzzingTest`.
- UI tests in `MouseUtils.UITests`.

## Design provenance

Behavior was derived from a survey of shipping dwell implementations rather than a port of any one of
them: GNOME Hover Click, macOS Dwell Control, Windows Eye Control, Dwell Clicker 2,
Point-N-Click, ClickAid, Tobii Dynavox Windows Control, Grid 3, and the Android auto click dwell
timing feature, together with the dwell-selection literature on the Midas touch problem, dwell-time
defaults, and countdown feedback.
