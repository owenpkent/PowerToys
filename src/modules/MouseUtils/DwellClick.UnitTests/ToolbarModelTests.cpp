// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pch.h"
#include "DwellClickToolbar.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace dwellclick;

namespace
{
    constexpr int DWELL_MS = 100;

    PointL CenterOf(const ToolbarModel& model, int index)
    {
        const ToolbarRect r = model.ButtonRect(index);
        return PointL{ r.x + r.w / 2, r.y + r.h / 2 };
    }
}

namespace DwellClickToolbarTests
{
    TEST_CLASS (ToolbarLayout)
    {
    public:
        TEST_METHOD (ExpandedShowsEveryButtonAndCollapsedOnlyTheHandle)
        {
            ToolbarModel model;
            Assert::AreEqual(ToolbarModel::ButtonCount, model.VisibleButtonCount());

            model.SetCollapsed(true);
            Assert::AreEqual(1, model.VisibleButtonCount());
            Assert::IsTrue(model.Height() < ToolbarModel::ButtonCount * ToolbarModel::ButtonSize);
        }

        TEST_METHOD (GapsAndPaddingHitNothing)
        {
            ToolbarModel model;
            // The point between button 0 and button 1 sits in the gap.
            const ToolbarRect first = model.ButtonRect(0);
            Assert::AreEqual(-1, model.HitTest(PointL{ first.x, first.y + first.h + ToolbarModel::Gap / 2 }));
            // The top-left padding corner is outside every button.
            Assert::AreEqual(-1, model.HitTest(PointL{ 0, 0 }));
        }

        TEST_METHOD (CollapsedRejectsHitsOnHiddenButtons)
        {
            ToolbarModel model;
            const PointL pauseCenter = CenterOf(model, ToolbarModel::PauseIndex);
            model.SetCollapsed(true);
            Assert::AreEqual(-1, model.HitTest(pauseCenter));
        }
    };

    TEST_CLASS (ToolbarHoverDwell)
    {
    public:
        TEST_METHOD (HoverForTheDwellTimeActivates)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarModel::PauseIndex);

            Assert::IsFalse(model.OnPointer(pause, true, 0, DWELL_MS).has_value());
            Assert::IsFalse(model.OnPointer(pause, true, DWELL_MS - 1, DWELL_MS).has_value());
            const auto fired = model.OnPointer(pause, true, DWELL_MS, DWELL_MS);
            Assert::IsTrue(fired.has_value());
            Assert::IsTrue(ToolbarCommand::TogglePause == *fired);
        }

        TEST_METHOD (JitterWithinTheButtonKeepsTheCountdown)
        {
            ToolbarModel model;
            const ToolbarRect r = model.ButtonRect(2);

            Assert::IsFalse(model.OnPointer(PointL{ r.x + 2, r.y + 2 }, true, 0, DWELL_MS).has_value());
            // A different point inside the same button does not restart the countdown.
            Assert::IsTrue(model.OnPointer(PointL{ r.x + r.w - 2, r.y + r.h - 2 }, true, DWELL_MS, DWELL_MS).has_value());
        }

        TEST_METHOD (MovingToAnotherButtonRestartsTheCountdown)
        {
            ToolbarModel model;
            Assert::IsFalse(model.OnPointer(CenterOf(model, 2), true, 0, DWELL_MS).has_value());
            // Almost done on button 2, then cross to button 3: the elapsed time does not carry.
            Assert::IsFalse(model.OnPointer(CenterOf(model, 3), true, DWELL_MS - 1, DWELL_MS).has_value());
            Assert::IsFalse(model.OnPointer(CenterOf(model, 3), true, 2 * DWELL_MS - 2, DWELL_MS).has_value());
            Assert::IsTrue(model.OnPointer(CenterOf(model, 3), true, 2 * DWELL_MS - 1, DWELL_MS).has_value());
        }

        TEST_METHOD (ParkingOnAButtonFiresOnceNotOncePerDwellTime)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarModel::PauseIndex);

            model.OnPointer(pause, true, 0, DWELL_MS);
            Assert::IsTrue(model.OnPointer(pause, true, DWELL_MS, DWELL_MS).has_value());

            // Any amount of further parking never refires.
            Assert::IsFalse(model.OnPointer(pause, true, 10 * DWELL_MS, DWELL_MS).has_value());
            Assert::AreEqual(0.0, model.HoverProgress(10 * DWELL_MS, DWELL_MS));
        }

        TEST_METHOD (LeavingAndReturningRearmsTheButton)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarModel::PauseIndex);

            model.OnPointer(pause, true, 0, DWELL_MS);
            model.OnPointer(pause, true, DWELL_MS, DWELL_MS);

            // Leave the toolbar entirely, come back, and it can fire again.
            model.OnPointer(PointL{}, false, DWELL_MS + 10, DWELL_MS);
            Assert::IsFalse(model.OnPointer(pause, true, DWELL_MS + 20, DWELL_MS).has_value());
            Assert::IsTrue(model.OnPointer(pause, true, 2 * DWELL_MS + 20, DWELL_MS).has_value());
        }

        TEST_METHOD (ATickOlderThanTheAnchorDoesNotFire)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarModel::PauseIndex);

            model.OnPointer(pause, true, 1000, DWELL_MS);
            // A wrapped or stale tick reads as 0 elapsed rather than a huge value.
            Assert::IsFalse(model.OnPointer(pause, true, 0, DWELL_MS).has_value());
        }

        TEST_METHOD (ProgressRisesToOneAndClearsAfterActivation)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarModel::PauseIndex);

            model.OnPointer(pause, true, 0, DWELL_MS);
            Assert::AreEqual(0.5, model.HoverProgress(DWELL_MS / 2, DWELL_MS), 0.01);
            model.OnPointer(pause, true, DWELL_MS, DWELL_MS);
            Assert::AreEqual(0.0, model.HoverProgress(DWELL_MS + 1, DWELL_MS));
        }

        TEST_METHOD (ADegenerateDwellTimeStaysDefined)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarModel::PauseIndex);

            model.OnPointer(pause, true, 0, 0);
            Assert::IsTrue(model.OnPointer(pause, true, 1, 0).has_value());
        }
    };

    TEST_CLASS (ToolbarClicksAndCollapse)
    {
    public:
        TEST_METHOD (APhysicalClickActivatesImmediatelyAndBlocksTheFollowingDwell)
        {
            ToolbarModel model;
            const PointL drag = CenterOf(model, 6);

            const auto clicked = model.OnClick(drag);
            Assert::IsTrue(clicked.has_value());
            Assert::IsTrue(ToolbarCommand::SelectDrag == *clicked);

            // The pointer is still parked on the button it clicked; no dwell refire.
            Assert::IsFalse(model.OnPointer(drag, true, 10 * DWELL_MS, DWELL_MS).has_value());
        }

        TEST_METHOD (AClickInAGapDoesNothing)
        {
            ToolbarModel model;
            Assert::IsFalse(model.OnClick(PointL{ 0, 0 }).has_value());
        }

        TEST_METHOD (CollapsedHandleDwellReturnsTheToggleCommand)
        {
            ToolbarModel model;
            model.SetCollapsed(true);
            const PointL handle = CenterOf(model, ToolbarModel::CollapseIndex);

            model.OnPointer(handle, true, 0, DWELL_MS);
            const auto fired = model.OnPointer(handle, true, DWELL_MS, DWELL_MS);
            Assert::IsTrue(fired.has_value());
            Assert::IsTrue(ToolbarCommand::ToggleCollapse == *fired);
        }

        TEST_METHOD (CollapsingResetsAnyRunningHover)
        {
            ToolbarModel model;
            model.OnPointer(CenterOf(model, 2), true, 0, DWELL_MS);
            model.SetCollapsed(true);
            Assert::AreEqual(-1, model.HoveredIndex());
        }
    };
}
