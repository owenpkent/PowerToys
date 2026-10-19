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

    PointL CenterOf(const ToolbarModel& model, ToolbarCommand command)
    {
        return CenterOf(model, model.IndexOf(command));
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
            Assert::AreEqual(static_cast<int>(model.Buttons().size()), model.VisibleButtonCount());

            model.SetCollapsed(true);
            Assert::AreEqual(1, model.VisibleButtonCount());
            Assert::AreEqual(ToolbarModel::Padding * 2 + model.ButtonSize(), model.Height());
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
            const PointL pauseCenter = CenterOf(model, ToolbarCommand::TogglePause);
            model.SetCollapsed(true);
            Assert::AreEqual(-1, model.HitTest(pauseCenter));
        }

        TEST_METHOD (TheDefaultSetCarriesEveryCommandOnce)
        {
            ToolbarModel model;
            Assert::AreEqual(0, model.IndexOf(ToolbarCommand::ToggleCollapse));
            Assert::IsTrue(model.IndexOf(ToolbarCommand::TogglePause) > 0);
            Assert::IsTrue(model.IndexOf(ToolbarCommand::SelectDoubleClick) > 0);
            Assert::IsTrue(model.IndexOf(ToolbarCommand::SelectScrollUp) > 0);
            Assert::IsTrue(model.IndexOf(ToolbarCommand::SelectScrollDown) > 0);
            Assert::IsTrue(model.IndexOf(ToolbarCommand::OpenSettings) > 0);
        }
    };

    TEST_CLASS (ToolbarOrientationAndWrapping)
    {
    public:
        TEST_METHOD (HorizontalLayoutSwapsWidthAndHeight)
        {
            ToolbarModel model;
            const int vWidth = model.Width();
            const int vHeight = model.Height();

            model.SetLayout(ToolbarOrientation::Horizontal, 0);
            Assert::AreEqual(vHeight, model.Width());
            Assert::AreEqual(vWidth, model.Height());

            // Buttons now run left to right on one row.
            const ToolbarRect first = model.ButtonRect(0);
            const ToolbarRect second = model.ButtonRect(1);
            Assert::AreEqual(first.y, second.y);
            Assert::AreEqual(first.x + model.ButtonSize() + ToolbarModel::Gap, second.x);
        }

        TEST_METHOD (ALongBarWrapsIntoASecondColumn)
        {
            ToolbarModel model; // 10 buttons
            model.SetLayout(ToolbarOrientation::Vertical, 4);

            Assert::AreEqual(4, model.ButtonsPerLine());
            Assert::AreEqual(3, model.LineCount());
            const int step = model.ButtonSize() + ToolbarModel::Gap;
            Assert::AreEqual(ToolbarModel::Padding * 2 + 3 * model.ButtonSize() + 2 * ToolbarModel::Gap, model.Width());
            Assert::AreEqual(ToolbarModel::Padding * 2 + 4 * model.ButtonSize() + 3 * ToolbarModel::Gap, model.Height());

            // Button 4 starts the second column, level with button 0.
            Assert::AreEqual(model.ButtonRect(0).y, model.ButtonRect(4).y);
            Assert::AreEqual(model.ButtonRect(0).x + step, model.ButtonRect(4).x);

            // Every visible button is still hit at its own center.
            for (int i = 0; i < model.VisibleButtonCount(); ++i)
            {
                Assert::AreEqual(i, model.HitTest(CenterOf(model, i)));
            }
        }

        TEST_METHOD (ButtonsThatFitNeverDropsBelowOne)
        {
            ToolbarModel model;
            const int step = model.ButtonSize() + ToolbarModel::Gap;
            Assert::AreEqual(1, model.ButtonsThatFit(0));
            Assert::AreEqual(1, model.ButtonsThatFit(-500));
            // Exactly three buttons with their padding fit; one unit less fits two.
            const int three = ToolbarModel::Padding * 2 + 3 * model.ButtonSize() + 2 * ToolbarModel::Gap;
            Assert::AreEqual(3, model.ButtonsThatFit(three));
            Assert::AreEqual(2, model.ButtonsThatFit(three - 1));
            Assert::AreEqual(3, model.ButtonsThatFit(three + step - 1));
        }

        TEST_METHOD (ACollapsedBarIsOneButtonWhateverTheLayout)
        {
            ToolbarModel model;
            model.SetLayout(ToolbarOrientation::Horizontal, 3);
            model.SetCollapsed(true);
            Assert::AreEqual(1, model.ButtonsPerLine());
            Assert::AreEqual(1, model.LineCount());
            Assert::AreEqual(ToolbarModel::Padding * 2 + model.ButtonSize(), model.Width());
            Assert::AreEqual(ToolbarModel::Padding * 2 + model.ButtonSize(), model.Height());
        }

        TEST_METHOD (SetLayoutResetsARunningHover)
        {
            ToolbarModel model;
            model.OnPointer(CenterOf(model, 2), true, 0, 100);
            Assert::AreEqual(2, model.HoveredIndex());
            model.SetLayout(ToolbarOrientation::Horizontal, 0);
            Assert::AreEqual(-1, model.HoveredIndex());
        }
    };

    TEST_CLASS (ToolbarReviewRegressions)
    {
    public:
        TEST_METHOD (AParkedPointerTogglesCollapseOnceThroughALayoutChange)
        {
            // Replays the overlay's sequence at toolbar position 0%, where the handle sits at the
            // same spot collapsed and expanded: dispatch the toggle, then lay out again (SetCollapsed,
            // SetButtonSize, SetLayout), with the pointer never moving.
            ToolbarModel model;
            const PointL handle = CenterOf(model, 0);
            int toggles = 0;
            for (uint64_t tick = 0; tick <= 6000; tick += 15)
            {
                if (const auto command = model.OnPointer(handle, true, tick, 1200))
                {
                    Assert::IsTrue(*command == ToolbarCommand::ToggleCollapse);
                    ++toggles;
                    model.SetCollapsed(!model.IsCollapsed());
                    model.SetButtonSize(ToolbarModel::DefaultButtonSize);
                    model.SetLayout(ToolbarOrientation::Vertical, 0);
                }
            }
            Assert::AreEqual(1, toggles);

            // Leaving the handle and coming back re-arms it.
            model.OnPointer(PointL{ -100, -100 }, false, 6100, 1200);
            model.OnPointer(handle, true, 6200, 1200);
            Assert::IsTrue(model.OnPointer(handle, true, 7400, 1200).has_value());
        }

        TEST_METHOD (AHandleThatMovedInALayoutChangeCanBeDwelledOnAgain)
        {
            // Collapse by dwelling; the new layout moves the handle out from under the pointer
            // (any position other than 0%). The pointer is then outside the window, and dwelling
            // on the handle in its new place must expand the bar again.
            ToolbarModel model;
            const PointL handle = CenterOf(model, 0);
            model.OnPointer(handle, true, 0, 1200);
            Assert::IsTrue(model.OnPointer(handle, true, 1200, 1200).has_value());
            model.SetCollapsed(true);
            model.SetButtonSize(ToolbarModel::DefaultButtonSize);
            model.SetLayout(ToolbarOrientation::Vertical, 0);

            model.OnPointer(PointL{ -100, -100 }, false, 2500, 1200);
            model.OnPointer(handle, true, 2600, 1200);
            const auto expanded = model.OnPointer(handle, true, 3800, 1200);
            Assert::IsTrue(expanded.has_value(), L"The moved handle stayed blocked after the pointer left and returned.");
            Assert::IsTrue(*expanded == ToolbarCommand::ToggleCollapse);
        }

        TEST_METHOD (AButtonThatAppearsUnderARestingPointerWaitsForItToLeave)
        {
            // The toolbar appears (startup, a settings change) with the pointer already resting
            // on Pause: the overlay blocks that button, so resting there never toggles pause.
            ToolbarModel model;
            const int pause = model.IndexOf(ToolbarCommand::TogglePause);
            const PointL onPause = CenterOf(model, pause);
            model.BlockAt(onPause);
            for (uint64_t tick = 0; tick <= 5000; tick += 15)
            {
                Assert::IsFalse(model.OnPointer(onPause, true, tick, 500).has_value());
            }

            // Moving off and back is a deliberate choice, and works as usual.
            model.OnPointer(PointL{ -50, -50 }, false, 5100, 500);
            model.OnPointer(onPause, true, 5200, 500);
            Assert::IsTrue(model.OnPointer(onPause, true, 5700, 500).has_value());
        }

        TEST_METHOD (BlockAtOnAGapBlocksNothing)
        {
            ToolbarModel model;
            model.BlockAt(PointL{ 0, 0 }); // padding corner
            Assert::IsFalse(model.BlockedCommand().has_value());
        }

        TEST_METHOD (APhysicalClickOnTheHandleStaysBlockedThroughALayoutChange)
        {
            ToolbarModel model;
            const PointL handle = CenterOf(model, 0);
            Assert::IsTrue(model.OnClick(handle).has_value());
            model.SetCollapsed(true);
            model.SetLayout(ToolbarOrientation::Vertical, 0);
            Assert::IsFalse(model.OnPointer(handle, true, 0, 100).has_value());
            Assert::IsFalse(model.OnPointer(handle, true, 5000, 100).has_value());
        }

        TEST_METHOD (TheLargeMenuFitsASmallHighDpiWorkArea)
        {
            // The review's case: 19 buttons, large size, a 1024 x 668 work area at 250%, so
            // 409 x 267 unscaled units less the 8-unit margins.
            const auto fit = ToolbarModel::FitGrid(19, 393, 251, { 96, 72, 40, 24 });
            Assert::IsTrue(fit.fits);
            ToolbarModel model;
            std::vector<ToolbarCommand> buttons(19, ToolbarCommand::SelectLeftClick);
            buttons[0] = ToolbarCommand::ToggleCollapse;
            model.SetButtons(buttons);
            model.SetButtonSize(fit.buttonSize);
            model.SetLayout(ToolbarOrientation::Horizontal, fit.columns);
            Assert::IsTrue(model.Width() <= 393);
            Assert::IsTrue(model.Height() <= 251);
        }

        TEST_METHOD (TheLargeMenuKeepsItsBigSizeWhenThereIsRoom)
        {
            const auto fit = ToolbarModel::FitGrid(19, 2544, 1360, { 96, 72, 40, 24 });
            Assert::IsTrue(fit.fits);
            Assert::AreEqual(96, fit.buttonSize);
            Assert::AreEqual(5, fit.columns); // ceil(sqrt(19))
        }

        TEST_METHOD (AShortAreaGetsMoreColumnsBeforeSmallerButtons)
        {
            // Two rows of 96 fit; 19 buttons then need 10 columns, which also fit.
            const int twoRows = ToolbarModel::Padding * 2 + 2 * 96 + ToolbarModel::Gap;
            const auto fit = ToolbarModel::FitGrid(19, 2000, twoRows, { 96, 72 });
            Assert::IsTrue(fit.fits);
            Assert::AreEqual(96, fit.buttonSize);
            Assert::AreEqual(10, fit.columns);
        }

        TEST_METHOD (AnAreaTooSmallForAnySizeReportsNoFit)
        {
            const auto fit = ToolbarModel::FitGrid(19, 60, 60, { 96, 24 });
            Assert::IsFalse(fit.fits);
            Assert::AreEqual(24, fit.buttonSize); // the smallest candidate, for the pinned fallback
            Assert::IsTrue(fit.columns >= 1);
        }
    };

    TEST_CLASS (ToolbarCommandActions)
    {
    public:
        TEST_METHOD (SelectCommandsMapToTheirActions)
        {
            Assert::IsTrue(ActionFor(ToolbarCommand::SelectScrollLeft) == DwellAction::ScrollLeft);
            Assert::IsTrue(ActionFor(ToolbarCommand::SelectScrollRight) == DwellAction::ScrollRight);
            Assert::IsTrue(ActionFor(ToolbarCommand::SelectZoomIn) == DwellAction::ZoomIn);
            Assert::IsTrue(ActionFor(ToolbarCommand::SelectZoomOut) == DwellAction::ZoomOut);
            Assert::IsTrue(ActionFor(ToolbarCommand::SelectDrag) == DwellAction::Drag);
            Assert::IsTrue(ActionFor(ToolbarCommand::SelectLeftClick) == DwellAction::LeftClick);
        }

        TEST_METHOD (SystemAndChromeCommandsChooseNoAction)
        {
            for (const ToolbarCommand command : { ToolbarCommand::ToggleCollapse, ToolbarCommand::TogglePause, ToolbarCommand::OpenSettings, ToolbarCommand::Back, ToolbarCommand::StartMenu, ToolbarCommand::ShowDesktop, ToolbarCommand::TaskView, ToolbarCommand::Notifications })
            {
                Assert::IsFalse(ActionFor(command).has_value());
            }
        }

        TEST_METHOD (EveryActionHasASelectCommand)
        {
            // Every DwellAction can be chosen from the toolbar, so none is reachable only by
            // the cycle shortcut.
            for (int a = 0; a <= static_cast<int>(LastDwellAction); ++a)
            {
                bool found = false;
                for (int c = 0; c <= static_cast<int>(ToolbarCommand::Notifications); ++c)
                {
                    const auto action = ActionFor(static_cast<ToolbarCommand>(c));
                    found = found || (action && static_cast<int>(*action) == a);
                }
                Assert::IsTrue(found);
            }
        }
    };

    TEST_CLASS (ToolbarCustomization)
    {
    public:
        TEST_METHOD (SetButtonsChangesLayoutAndHitTargets)
        {
            ToolbarModel model;
            const int fullHeight = model.Height();

            model.SetButtons({
                ToolbarCommand::ToggleCollapse,
                ToolbarCommand::TogglePause,
                ToolbarCommand::SelectLeftClick,
            });

            Assert::AreEqual(3, model.VisibleButtonCount());
            Assert::IsTrue(model.Height() < fullHeight);
            Assert::AreEqual(-1, model.IndexOf(ToolbarCommand::SelectDrag));
            // The point where the fourth button used to be is now padding.
            Assert::AreEqual(-1, model.HitTest(CenterOf(model, 2 + 1)));
        }

        TEST_METHOD (AHiddenCommandCannotActivate)
        {
            ToolbarModel model;
            const PointL dragCenterInFullSet = CenterOf(model, ToolbarCommand::SelectDrag);

            model.SetButtons({
                ToolbarCommand::ToggleCollapse,
                ToolbarCommand::TogglePause,
                ToolbarCommand::SelectLeftClick,
            });

            // The old drag position is empty space now; a dwell there never fires anything.
            model.OnPointer(dragCenterInFullSet, true, 0, DWELL_MS);
            Assert::IsFalse(model.OnPointer(dragCenterInFullSet, true, 10 * DWELL_MS, DWELL_MS).has_value());
        }

        TEST_METHOD (SetButtonsResetsARunningHover)
        {
            ToolbarModel model;
            model.OnPointer(CenterOf(model, ToolbarCommand::SelectLeftClick), true, 0, DWELL_MS);
            model.SetButtons({ ToolbarCommand::ToggleCollapse, ToolbarCommand::TogglePause });
            Assert::AreEqual(-1, model.HoveredIndex());
        }

        TEST_METHOD (ButtonSizeScalesTheLayoutAndClampsAndResetsHover)
        {
            ToolbarModel model;
            const int mediumHeight = model.Height();

            model.OnPointer(CenterOf(model, ToolbarCommand::SelectLeftClick), true, 0, DWELL_MS);
            model.SetButtonSize(72);
            Assert::AreEqual(-1, model.HoveredIndex());
            Assert::IsTrue(model.Height() > mediumHeight);
            Assert::AreEqual(ToolbarModel::Padding * 2 + 72, model.Width());

            // Out-of-range sizes clamp rather than producing an unusable layout.
            model.SetButtonSize(-100);
            Assert::AreEqual(24, model.ButtonSize());
            model.SetButtonSize(100000);
            Assert::AreEqual(96, model.ButtonSize());
        }
    };

    TEST_CLASS (ToolbarHoverDwell)
    {
    public:
        TEST_METHOD (HoverForTheDwellTimeActivates)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarCommand::TogglePause);

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
            const PointL pause = CenterOf(model, ToolbarCommand::TogglePause);

            model.OnPointer(pause, true, 0, DWELL_MS);
            Assert::IsTrue(model.OnPointer(pause, true, DWELL_MS, DWELL_MS).has_value());

            // Any amount of further parking never refires.
            Assert::IsFalse(model.OnPointer(pause, true, 10 * DWELL_MS, DWELL_MS).has_value());
            Assert::AreEqual(0.0, model.HoverProgress(10 * DWELL_MS, DWELL_MS));
        }

        TEST_METHOD (LeavingAndReturningRearmsTheButton)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarCommand::TogglePause);

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
            const PointL pause = CenterOf(model, ToolbarCommand::TogglePause);

            model.OnPointer(pause, true, 1000, DWELL_MS);
            // A wrapped or stale tick reads as 0 elapsed rather than a huge value.
            Assert::IsFalse(model.OnPointer(pause, true, 0, DWELL_MS).has_value());
        }

        TEST_METHOD (ProgressRisesToOneAndClearsAfterActivation)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarCommand::TogglePause);

            model.OnPointer(pause, true, 0, DWELL_MS);
            Assert::AreEqual(0.5, model.HoverProgress(DWELL_MS / 2, DWELL_MS), 0.01);
            model.OnPointer(pause, true, DWELL_MS, DWELL_MS);
            Assert::AreEqual(0.0, model.HoverProgress(DWELL_MS + 1, DWELL_MS));
        }

        TEST_METHOD (ADegenerateDwellTimeStaysDefined)
        {
            ToolbarModel model;
            const PointL pause = CenterOf(model, ToolbarCommand::TogglePause);

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
            const PointL drag = CenterOf(model, ToolbarCommand::SelectDrag);

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
