---
description: DSC configuration reference for PowerToys DwellClick module
ms.date:     10/23/2026
ms.topic:    reference
title:       DwellClick Module
---

# DwellClick Module

## Synopsis

Manages configuration for the Dwell Click utility, which issues a mouse
click automatically when the pointer is held still.

## Description

The `DwellClick` module configures PowerToys Dwell Click, a Mouse
Utilities sub-module for people who can move a pointer but cannot
reliably click it. Rest the pointer on a target for the configured dwell
time and the chosen action fires there: a left, right, double, or middle
click, or a two-dwell drag (the first dwell presses the left button, the
second releases it).

## Properties

The DwellClick module supports the following configurable properties:

### DwellTimeMs

Sets how long the pointer must rest, in milliseconds, before the action
fires.

**Type:** integer  
**Range:** `100` to `60000` (the Settings UI slider covers `200` to
`5000`; a hand-edited value is clamped to the full range)  
**Default:** `1200`

### MoveTolerancePixels

Sets the pointer drift, in pixels at 100% display scaling, that a running
countdown tolerates. Larger values let a trembling hand complete a dwell;
drift beyond the tolerance restarts the countdown at the new position. The
module scales the value with the display scaling of the monitor under the
pointer, so it covers the same hand movement on every monitor.

**Type:** integer  
**Default:** `10`

### PostActionTolerancePixels

Sets how far the pointer must move after an action, in pixels at 100%
display scaling, before a new countdown may start. This is the setting that stops a resting
pointer from clicking the same spot repeatedly.

**Type:** integer  
**Default:** `10`

### RepeatIntervalMs

Sets how often a scroll or zoom repeats, in milliseconds, once its first
step has landed. The first step always waits the full dwell time; moving
the pointer to a new spot waits the full dwell time again.

**Type:** integer  
**Range:** `50` to `60000` (the Settings UI slider covers `100` to
`2000`; a hand-edited value is clamped to the full range)  
**Default:** `400`

### DefaultAction

Sets the action a dwell performs when no other action has been chosen.

**Type:** integer  
**Values:** `0` = left click, `1` = right click, `2` = double-click,
`3` = middle click, `4` = drag  
**Default:** `0`

### RevertToDefaultAfterAction

Controls whether the action returns to `DefaultAction` after each
completed action, so a one-off right click does not turn every later
dwell into a right click.

**Type:** boolean  
**Default:** `true`

### ShowToolbar

Controls whether the on-screen action toolbar is shown. The toolbar
docks to a screen edge; hovering one of its buttons for the dwell time
selects that action (or pauses dwelling) with no click needed.

**Type:** boolean  
**Default:** `true`

### ToolbarSide

Sets which edge of the primary monitor the action toolbar docks to. With
the floating button style, the button sits on this edge. The
toolbar stays clear of the taskbar and moves with it, and with any change
of resolution or primary monitor.

**Type:** integer  
**Values:** `0` = left edge, `1` = right edge, `2` = top edge,
`3` = bottom edge  
**Default:** `0`

### ToolbarPosition

Sets where along its edge the toolbar (or the floating button) is
centered, as a percentage from the start of the edge (`0`) to its end
(`100`). The start of the left or right edge is the top; the start of
the top or bottom edge is the left. Move the toolbar away from content
that needs to be reached.

**Type:** integer  
**Range:** `0` to `100` (the Settings UI slider moves in steps of `5`)  
**Default:** `50`

### ToolbarStyle

Sets how the toolbar is presented: a bar docked to the screen edge, or a
single floating button that opens a large menu of actions in the middle
of the screen. The large menu closes after an action is chosen.

**Type:** integer  
**Values:** `0` = docked bar, `1` = floating button with large menu  
**Default:** `0`

### ShowCountdown

Controls whether the countdown ring is drawn at the pointer while a
dwell is in progress. The ring appears a moment into the dwell (after a
fifth of the dwell time, and at most 250 ms), so it does not follow a
pointer that is still moving. When the next action is anything other
than a left click, a small badge beside the ring shows which action will
fire.

**Type:** boolean  
**Default:** `true`

### OverlaySize

Sets how large the toolbar buttons and the countdown ring are. Bigger
targets are easier to rest on. Both scale with display scaling: the
toolbar with its monitor's, and the ring with the scaling of whichever
monitor the pointer is on.

**Type:** integer  
**Values:** `0` = small, `1` = medium, `2` = large  
**Default:** `1`

### ActivationShortcut

Sets the keyboard shortcut that pauses or resumes dwell clicking, so the
pointer can rest without clicking. It does the same thing as the
toolbar's pause button. When the toolbar is hidden or collapsed, the
pointer briefly shows the pause or resume symbol.

**Type:** object
**Properties:**

- `win` (boolean) - Windows key modifier
- `ctrl` (boolean) - Ctrl key modifier
- `alt` (boolean) - Alt key modifier
- `shift` (boolean) - Shift key modifier
- `code` (integer) - Virtual key code
- `key` (string) - Key name

**Default:** `Win+Shift+W`

### CycleActionShortcut

Sets the keyboard shortcut that switches the next dwell action to the
following one (left click, right click, double-click, middle click, drag,
scroll up, scroll down, then back to left click). A drag in progress is
released first. When the toolbar is hidden or collapsed, the pointer
briefly shows the symbol of the action chosen. Takes the same object as
`ActivationShortcut`.

**Type:** object
**Default:** `Win+Shift+A`

### PlaySound

Controls whether a short click sound plays each time a dwell action
fires.

**Type:** boolean
**Default:** `false`

### IndicatorUseAccentColor

Controls whether the countdown ring uses the Windows accent color. When
`false`, the ring uses `IndicatorColor`.

**Type:** boolean
**Default:** `true`

### IndicatorColor

Sets the countdown ring color used when `IndicatorUseAccentColor` is
`false`. A malformed value is ignored and the previous color is kept.

**Type:** string (hex color)
**Format:** `"#RRGGBB"`
**Default:** `"#0078D4"`

### ToolbarButton properties

Seventeen booleans choose which action buttons the toolbar carries; the
collapse handle and pause are always present. Scroll and zoom actions
keep repeating while the pointer rests and stay selected until another
action is chosen. Back, Start menu, Show desktop, Task view, and
Notifications act at once in the window that was in use.
`ToolbarButtonOpenSettings` is a button that opens PowerToys Settings,
for users who cannot reach the system tray.

- `ToolbarButtonLeftClick` (default `true`)
- `ToolbarButtonDoubleClick` (default `true`)
- `ToolbarButtonRightClick` (default `true`)
- `ToolbarButtonMiddleClick` (default `false`)
- `ToolbarButtonDrag` (default `true`)
- `ToolbarButtonScrollUp` (default `true`)
- `ToolbarButtonScrollDown` (default `true`)
- `ToolbarButtonScrollLeft` (default `false`)
- `ToolbarButtonScrollRight` (default `false`)
- `ToolbarButtonZoomIn` (default `false`)
- `ToolbarButtonZoomOut` (default `false`)
- `ToolbarButtonBack` (default `false`)
- `ToolbarButtonStart` (default `false`)
- `ToolbarButtonShowDesktop` (default `false`)
- `ToolbarButtonTaskView` (default `false`)
- `ToolbarButtonNotifications` (default `false`)
- `ToolbarButtonOpenSettings` (default `true`)

## Examples

### Example 1 - Configure dwell timing with direct execution

This example shortens the dwell time and widens the movement tolerance.

```powershell
$config = @{
    settings = @{
        properties = @{
            DwellTimeMs = 800
            MoveTolerancePixels = 15
        }
        name = "DwellClick"
        version = "1.0"
    }
} | ConvertTo-Json -Depth 10 -Compress

PowerToys.DSC.exe set --resource 'settings' --module DwellClick --input $config
```

### Example 2 - Configure the default action with DSC

This example makes each dwell perform a double-click.

```bash
dsc config set --file dwellclick-action.dsc.yaml
```

```yaml
# dwellclick-action.dsc.yaml
$schema: https://aka.ms/dsc/schemas/v3/bundled/config/document.json
resources:
  - name: Configure Dwell Click action
    type: Microsoft.PowerToys/DwellClickSettings
    properties:
      settings:
        properties:
          DefaultAction: 2
        name: DwellClick
        version: 1.0
```

### Example 3 - Install and configure with WinGet

This example installs PowerToys and configures Dwell Click for a slower,
steadier dwell.

```bash
winget configure winget-dwellclick.yaml
```

```yaml
# winget-dwellclick.yaml
$schema: https://raw.githubusercontent.com/PowerShell/DSC/main/schemas/2023/08/config/document.json
metadata:
  winget:
    processor: dscv3
resources:
  - name: Install PowerToys
    type: Microsoft.WinGet.DSC/WinGetPackage
    properties:
      id: Microsoft.PowerToys
      source: winget

  - name: Configure Dwell Click
    type: Microsoft.PowerToys/DwellClickSettings
    properties:
      settings:
        properties:
          DwellTimeMs: 2000
          MoveTolerancePixels: 20
        name: DwellClick
        version: 1.0
```

## Use cases

### Head or eye tracker input

Trackers hold position less steadily than a hand on a mouse, so widen
both tolerances and keep the default dwell time:

```yaml
resources:
  - name: Tracker-friendly configuration
    type: Microsoft.PowerToys/DwellClickSettings
    properties:
      settings:
        properties:
          MoveTolerancePixels: 25
          PostActionTolerancePixels: 30
        name: DwellClick
        version: 1.0
```

### Tremor

Configure a longer dwell time and a wide movement tolerance so a
countdown survives involuntary movement:

```yaml
resources:
  - name: Tremor-friendly configuration
    type: Microsoft.PowerToys/DwellClickSettings
    properties:
      settings:
        properties:
          DwellTimeMs: 2500
          MoveTolerancePixels: 30
        name: DwellClick
        version: 1.0
```

## See also

- [Settings Resource][01]
- [PowerToys DSC Overview][02]
- [FindMyMouse][03]

<!-- Link reference definitions -->
[01]: ../settings-resource.md
[02]: ../overview.md
[03]: ./FindMyMouse.md
