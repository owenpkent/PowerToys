// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include <windows.h>

#include <vector>

#include "DwellClickCore.h"
#include "DwellClickToolbar.h"

// What each action sends to Windows, as plain data. The module turns these steps into one tagged
// SendInput batch (see WinInjector and SendKeys in dllmain.cpp); keeping the mapping here lets the
// unit tests check every action's exact input without injecting anything.
namespace dwellclick
{
    struct InputStep
    {
        bool keyboard = false;

        // Mouse: a MOUSEEVENTF_* flag, with the wheel delta for the two wheel flags.
        DWORD mouseFlag = 0;
        LONG wheelDelta = 0;

        // Keyboard: the virtual key and whether this step releases it.
        WORD vk = 0;
        bool keyUp = false;

        static constexpr InputStep Mouse(DWORD flag, LONG delta = 0)
        {
            InputStep step;
            step.mouseFlag = flag;
            step.wheelDelta = delta;
            return step;
        }

        static constexpr InputStep Key(WORD vk, bool up)
        {
            InputStep step;
            step.keyboard = true;
            step.vk = vk;
            step.keyUp = up;
            return step;
        }
    };

    // The input for one engine ClickKind. Button events land at the live cursor position, which
    // is the rest point: the engine only fires while the pointer rests there.
    inline std::vector<InputStep> StepsFor(ClickKind kind)
    {
        switch (kind)
        {
        case ClickKind::RightClick:
            return { InputStep::Mouse(MOUSEEVENTF_RIGHTDOWN), InputStep::Mouse(MOUSEEVENTF_RIGHTUP) };
        case ClickKind::MiddleClick:
            return { InputStep::Mouse(MOUSEEVENTF_MIDDLEDOWN), InputStep::Mouse(MOUSEEVENTF_MIDDLEUP) };
        case ClickKind::DoubleClick:
            // Two rapid pairs in one batch; the OS turns them into a double click because they
            // land well inside the double-click time and rectangle.
            return {
                InputStep::Mouse(MOUSEEVENTF_LEFTDOWN),
                InputStep::Mouse(MOUSEEVENTF_LEFTUP),
                InputStep::Mouse(MOUSEEVENTF_LEFTDOWN),
                InputStep::Mouse(MOUSEEVENTF_LEFTUP),
            };
        case ClickKind::LeftDown:
            return { InputStep::Mouse(MOUSEEVENTF_LEFTDOWN) };
        case ClickKind::LeftUp:
            return { InputStep::Mouse(MOUSEEVENTF_LEFTUP) };
        case ClickKind::ScrollUp:
            return { InputStep::Mouse(MOUSEEVENTF_WHEEL, WHEEL_DELTA) };
        case ClickKind::ScrollDown:
            return { InputStep::Mouse(MOUSEEVENTF_WHEEL, -WHEEL_DELTA) };
        case ClickKind::ScrollLeft:
            // A positive horizontal delta scrolls right.
            return { InputStep::Mouse(MOUSEEVENTF_HWHEEL, -WHEEL_DELTA) };
        case ClickKind::ScrollRight:
            return { InputStep::Mouse(MOUSEEVENTF_HWHEEL, WHEEL_DELTA) };
        case ClickKind::ZoomIn:
        case ClickKind::ZoomOut:
            // Ctrl plus one wheel notch, the zoom gesture browsers, Office, Explorer, and most
            // viewers share. Ctrl is released in the same batch so it is never left down.
            return {
                InputStep::Key(VK_CONTROL, false),
                InputStep::Mouse(MOUSEEVENTF_WHEEL, kind == ClickKind::ZoomIn ? WHEEL_DELTA : -WHEEL_DELTA),
                InputStep::Key(VK_CONTROL, true),
            };
        case ClickKind::LeftClick:
        default:
            return { InputStep::Mouse(MOUSEEVENTF_LEFTDOWN), InputStep::Mouse(MOUSEEVENTF_LEFTUP) };
        }
    }

    // The keys a system toolbar command presses, in order (they are released in reverse), or an
    // empty list for every other command. They go to the foreground window, which is still the
    // app the user was in because the toolbar never activates.
    inline std::vector<WORD> KeysFor(ToolbarCommand command, bool windows11)
    {
        switch (command)
        {
        case ToolbarCommand::Back:
            // The browser Back key: Edge, Chrome, Firefox, Explorer, Settings, and most apps
            // with history treat it as Back. A mouse back button would land on the toolbar.
            return { VK_BROWSER_BACK };
        case ToolbarCommand::StartMenu:
            return { VK_LWIN };
        case ToolbarCommand::ShowDesktop:
            return { VK_LWIN, 'D' };
        case ToolbarCommand::TaskView:
            return { VK_LWIN, VK_TAB };
        case ToolbarCommand::Notifications:
            // Windows 11 opens the notification center with Win+N; Windows 10's equivalent is
            // the Action Center on Win+A.
            return { VK_LWIN, static_cast<WORD>(windows11 ? 'N' : 'A') };
        default:
            return {};
        }
    }

    // Keys pressed in order, then released in reverse, as one batch so a modifier is never left
    // down.
    inline std::vector<InputStep> StepsForKeys(const std::vector<WORD>& keys)
    {
        std::vector<InputStep> steps;
        steps.reserve(keys.size() * 2);
        for (const WORD vk : keys)
        {
            steps.push_back(InputStep::Key(vk, false));
        }
        for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        {
            steps.push_back(InputStep::Key(*it, true));
        }
        return steps;
    }
}
