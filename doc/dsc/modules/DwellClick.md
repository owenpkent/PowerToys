---
description: DSC configuration reference for PowerToys DwellClick module
ms.date:     09/14/2026
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

Sets the pointer drift, in pixels, that a running countdown tolerates.
Larger values let a trembling hand complete a dwell; drift beyond the
tolerance restarts the countdown at the new position.

**Type:** integer  
**Default:** `10`

### PostActionTolerancePixels

Sets how far the pointer must move after an action, in pixels, before a
new countdown may start. This is the setting that stops a resting
pointer from clicking the same spot repeatedly.

**Type:** integer  
**Default:** `10`

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

Sets which screen edge the action toolbar docks to.

**Type:** integer  
**Values:** `0` = left edge, `1` = right edge  
**Default:** `0`

### ShowCountdown

Controls whether the countdown ring is drawn at the pointer while a
dwell is in progress.

**Type:** boolean  
**Default:** `true`

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
