// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pch.h"
#include "DwellClickInput.h"

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace dwellclick;

namespace
{
    // Records what the engine asks for, so a test can follow one dwell all the way to the input
    // the module would send for it.
    class RecordingInjector : public IClickInjector
    {
    public:
        std::vector<ClickKind> kinds;

        bool Inject(ClickKind kind, PointL) override
        {
            kinds.push_back(kind);
            return true;
        }
    };

    Settings FastSettings()
    {
        Settings s;
        s.dwellTimeMs = 100;
        s.revertToDefaultAfterAction = false;
        return s;
    }

    // Choose the action, arm the engine with a deliberate move, rest for one dwell, and return
    // what the module would send for it.
    std::vector<InputStep> OneDwell(DwellAction action, RecordingInjector& injector)
    {
        Engine e(injector);
        const Settings s = FastSettings();
        e.SetNextAction(action);
        e.OnMove(PointL{ 200, 200 }, 0, s);
        Assert::IsTrue(e.Poll(100, s).fired, L"The dwell did not fire.");
        Assert::AreEqual(static_cast<size_t>(1), injector.kinds.size());
        return StepsFor(injector.kinds[0]);
    }

    InputStep M(DWORD flag, LONG delta = 0)
    {
        return InputStep::Mouse(flag, delta);
    }

    InputStep K(WORD vk, bool up)
    {
        return InputStep::Key(vk, up);
    }

    void AssertSteps(const std::vector<InputStep>& expected, const std::vector<InputStep>& actual, const wchar_t* what)
    {
        Assert::AreEqual(expected.size(), actual.size(), what);
        for (size_t i = 0; i < expected.size(); ++i)
        {
            const std::wstring at = std::wstring(what) + L" step " + std::to_wstring(i);
            Assert::AreEqual(expected[i].keyboard, actual[i].keyboard, at.c_str());
            Assert::AreEqual(static_cast<unsigned long>(expected[i].mouseFlag), static_cast<unsigned long>(actual[i].mouseFlag), at.c_str());
            Assert::AreEqual(static_cast<long>(expected[i].wheelDelta), static_cast<long>(actual[i].wheelDelta), at.c_str());
            Assert::AreEqual(static_cast<int>(expected[i].vk), static_cast<int>(actual[i].vk), at.c_str());
            Assert::AreEqual(expected[i].keyUp, actual[i].keyUp, at.c_str());
        }
    }
}

namespace DwellClickInputTests
{
    // One test per dwell action: choosing it and resting once sends exactly this input.
    TEST_CLASS (EveryDwellActionSendsTheRightInput)
    {
    public:
        TEST_METHOD (LeftClick)
        {
            RecordingInjector injector;
            AssertSteps({ M(MOUSEEVENTF_LEFTDOWN), M(MOUSEEVENTF_LEFTUP) }, OneDwell(DwellAction::LeftClick, injector), L"Left click");
        }

        TEST_METHOD (RightClick)
        {
            RecordingInjector injector;
            AssertSteps({ M(MOUSEEVENTF_RIGHTDOWN), M(MOUSEEVENTF_RIGHTUP) }, OneDwell(DwellAction::RightClick, injector), L"Right click");
        }

        TEST_METHOD (DoubleClick)
        {
            RecordingInjector injector;
            AssertSteps(
                { M(MOUSEEVENTF_LEFTDOWN), M(MOUSEEVENTF_LEFTUP), M(MOUSEEVENTF_LEFTDOWN), M(MOUSEEVENTF_LEFTUP) },
                OneDwell(DwellAction::DoubleClick, injector),
                L"Double-click");
        }

        TEST_METHOD (MiddleClick)
        {
            RecordingInjector injector;
            AssertSteps({ M(MOUSEEVENTF_MIDDLEDOWN), M(MOUSEEVENTF_MIDDLEUP) }, OneDwell(DwellAction::MiddleClick, injector), L"Middle click");
        }

        TEST_METHOD (DragPressesOnTheFirstDwellAndReleasesOnTheSecond)
        {
            RecordingInjector injector;
            Engine e(injector);
            const Settings s = FastSettings();
            e.SetNextAction(DwellAction::Drag);
            e.OnMove(PointL{ 200, 200 }, 0, s);
            Assert::IsTrue(e.Poll(100, s).fired);
            AssertSteps({ M(MOUSEEVENTF_LEFTDOWN) }, StepsFor(injector.kinds.back()), L"Drag press");

            // The button stays down while the pointer moves to the drop point...
            e.OnMove(PointL{ 260, 230 }, 150, s);
            e.OnMove(PointL{ 400, 300 }, 200, s);
            Assert::IsTrue(e.IsDragging());
            Assert::AreEqual(static_cast<size_t>(1), injector.kinds.size());

            // ...and the next dwell there releases it.
            Assert::IsTrue(e.Poll(300, s).fired);
            AssertSteps({ M(MOUSEEVENTF_LEFTUP) }, StepsFor(injector.kinds.back()), L"Drag release");
            Assert::IsFalse(e.IsDragging());
        }

        TEST_METHOD (ScrollUp)
        {
            RecordingInjector injector;
            AssertSteps({ M(MOUSEEVENTF_WHEEL, WHEEL_DELTA) }, OneDwell(DwellAction::ScrollUp, injector), L"Scroll up");
        }

        TEST_METHOD (ScrollDown)
        {
            RecordingInjector injector;
            AssertSteps({ M(MOUSEEVENTF_WHEEL, -WHEEL_DELTA) }, OneDwell(DwellAction::ScrollDown, injector), L"Scroll down");
        }

        TEST_METHOD (ScrollLeft)
        {
            RecordingInjector injector;
            AssertSteps({ M(MOUSEEVENTF_HWHEEL, -WHEEL_DELTA) }, OneDwell(DwellAction::ScrollLeft, injector), L"Scroll left");
        }

        TEST_METHOD (ScrollRight)
        {
            RecordingInjector injector;
            AssertSteps({ M(MOUSEEVENTF_HWHEEL, WHEEL_DELTA) }, OneDwell(DwellAction::ScrollRight, injector), L"Scroll right");
        }

        TEST_METHOD (ZoomInIsCtrlPlusWheelUpWithCtrlReleased)
        {
            RecordingInjector injector;
            AssertSteps(
                { K(VK_CONTROL, false), M(MOUSEEVENTF_WHEEL, WHEEL_DELTA), K(VK_CONTROL, true) },
                OneDwell(DwellAction::ZoomIn, injector),
                L"Zoom in");
        }

        TEST_METHOD (ZoomOutIsCtrlPlusWheelDownWithCtrlReleased)
        {
            RecordingInjector injector;
            AssertSteps(
                { K(VK_CONTROL, false), M(MOUSEEVENTF_WHEEL, -WHEEL_DELTA), K(VK_CONTROL, true) },
                OneDwell(DwellAction::ZoomOut, injector),
                L"Zoom out");
        }

        TEST_METHOD (EveryActionIsCovered)
        {
            // A new DwellAction must get its own test above; this fails until LastDwellAction
            // is reviewed here.
            Assert::AreEqual(static_cast<int>(DwellAction::ZoomOut), static_cast<int>(LastDwellAction));
        }
    };

    // The toolbar's keystroke buttons, and that every other command sends no keys.
    TEST_CLASS (EveryToolbarCommandSendsTheRightKeys)
    {
    public:
        TEST_METHOD (BackSendsTheBrowserBackKey)
        {
            AssertSteps({ K(VK_BROWSER_BACK, false), K(VK_BROWSER_BACK, true) }, StepsForKeys(KeysFor(ToolbarCommand::Back, true)), L"Back");
        }

        TEST_METHOD (StartMenuTapsTheWindowsKey)
        {
            AssertSteps({ K(VK_LWIN, false), K(VK_LWIN, true) }, StepsForKeys(KeysFor(ToolbarCommand::StartMenu, true)), L"Start menu");
        }

        TEST_METHOD (ShowDesktopIsWinD)
        {
            AssertSteps(
                { K(VK_LWIN, false), K('D', false), K('D', true), K(VK_LWIN, true) },
                StepsForKeys(KeysFor(ToolbarCommand::ShowDesktop, true)),
                L"Show desktop");
        }

        TEST_METHOD (TaskViewIsWinTab)
        {
            AssertSteps(
                { K(VK_LWIN, false), K(VK_TAB, false), K(VK_TAB, true), K(VK_LWIN, true) },
                StepsForKeys(KeysFor(ToolbarCommand::TaskView, true)),
                L"Task view");
        }

        TEST_METHOD (NotificationsSendsTheNotificationCenterKeysForEachWindowsVersion)
        {
            AssertSteps(
                { K(VK_LWIN, false), K('N', false), K('N', true), K(VK_LWIN, true) },
                StepsForKeys(KeysFor(ToolbarCommand::Notifications, true)),
                L"Notifications on Windows 11");
            AssertSteps(
                { K(VK_LWIN, false), K('A', false), K('A', true), K(VK_LWIN, true) },
                StepsForKeys(KeysFor(ToolbarCommand::Notifications, false)),
                L"Notifications on Windows 10");
        }

        TEST_METHOD (EveryCommandIsAnActionAKeystrokeOrToolbarChrome)
        {
            // Each toolbar command does exactly one kind of thing: choose a dwell action, send
            // keys, or act on the toolbar or Settings itself. None falls through doing nothing.
            for (int c = 0; c <= static_cast<int>(ToolbarCommand::Notifications); ++c)
            {
                const auto command = static_cast<ToolbarCommand>(c);
                const bool chooses = ActionFor(command).has_value();
                const bool keys = !KeysFor(command, true).empty();
                const bool chrome = command == ToolbarCommand::ToggleCollapse ||
                                    command == ToolbarCommand::TogglePause ||
                                    command == ToolbarCommand::OpenSettings;
                Assert::AreEqual(1, static_cast<int>(chooses) + static_cast<int>(keys) + static_cast<int>(chrome));
            }
        }
    };
}
