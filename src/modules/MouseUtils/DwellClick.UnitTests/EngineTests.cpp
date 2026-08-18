// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pch.h"
#include "DwellClickCore.h"

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace dwellclick;

namespace
{
    // Records every injection with the point it was issued at, and can be configured to report
    // failure (e.g. an injection blocked by an elevated window).
    class FakeInjector : public IClickInjector
    {
    public:
        struct Call
        {
            ClickKind kind;
            PointL pt;
        };

        std::vector<Call> calls;
        bool succeed = true;

        bool Inject(ClickKind kind, PointL pt) override
        {
            calls.push_back({ kind, pt });
            return succeed;
        }

        size_t Count() const
        {
            return calls.size();
        }
    };

    // Test baseline: a 100 ms dwell so the arithmetic in the timing tests stays obvious, with the
    // shipping 10 px tolerance and re-arm distance. The shipping dwell default is 1200 ms (see
    // DwellClickCore.h); tests that pivot on a specific duration set it themselves.
    Settings DefaultSettings()
    {
        Settings s;
        s.dwellTimeMs = 100;
        return s;
    }

    // The engine starts locked with its anchor at the origin, so every test must first move the
    // pointer far enough to arm it. This helper does that and leaves the countdown started at tick.
    void Arm(Engine& e, const Settings& s, PointL pt = PointL{ 100, 100 }, uint64_t tick = 0)
    {
        e.OnMove(pt, tick, s);
    }
}

namespace DwellClickEngineTests
{
    TEST_CLASS (DwellCountdown)
    {
    public:
        TEST_METHOD (StartsLockedAndDoesNotFireWithoutMovement)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            // No movement at all: the machine must never fire, however long the caller polls for.
            const PollResult r = e.Poll(1000000, s);
            Assert::IsFalse(r.fired);
            Assert::AreEqual(0.0, r.progress);
            Assert::AreEqual(static_cast<size_t>(0), injector.Count());
        }

        TEST_METHOD (MovementWithinToleranceDoesNotArm)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings(); // 10 px tolerance, anchor starts at the origin

            e.OnMove(PointL{ 10, 0 }, 0, s); // exactly 10 px: still resting, so still locked
            Assert::IsFalse(e.Poll(1000, s).fired);

            e.OnMove(PointL{ 11, 0 }, 0, s); // 11 px clears the tolerance and arms the machine
            Assert::IsTrue(e.Poll(1000, s).fired);
        }

        TEST_METHOD (FiresWhenDwellCompletes)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);

            const PollResult half = e.Poll(50, s);
            Assert::IsFalse(half.fired);
            Assert::AreEqual(0.5, half.progress, 0.0001);

            const PollResult done = e.Poll(100, s); // exactly at the threshold
            Assert::IsTrue(done.fired);
            Assert::IsTrue(done.action == DwellAction::LeftClick);
            Assert::AreEqual(static_cast<size_t>(1), injector.Count());
            Assert::IsTrue(injector.calls[0].kind == ClickKind::LeftClick);
            Assert::AreEqual(100L, injector.calls[0].pt.x);
        }

        TEST_METHOD (FiringPollReportsZeroProgressSoTheRingClears)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            const PollResult done = e.Poll(100, s);
            Assert::IsTrue(done.fired);
            // The countdown is over, so the ring must be told to clear rather than sit full.
            Assert::AreEqual(0.0, done.progress);
        }

        TEST_METHOD (LocksAfterFiringUntilThePointerMoves)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            Assert::IsTrue(e.Poll(100, s).fired);

            // A parked pointer must not re-fire, however long it rests. This is the core Midas
            // touch guard: without it a resting hand machine-guns clicks at whatever is under it.
            Assert::IsFalse(e.Poll(200, s).fired);
            Assert::IsFalse(e.Poll(100000, s).fired);
            Assert::AreEqual(static_cast<size_t>(1), injector.Count());
        }

        TEST_METHOD (DriftWithinToleranceDoesNotRestartTheCountdown)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            e.OnMove(PointL{ 104, 104 }, 50, s); // ~5.7 px of drift, under the 10 px tolerance

            const PollResult done = e.Poll(100, s);
            Assert::IsTrue(done.fired); // countdown survived the tremor
            // The click still lands where the pointer actually is, not at the stale anchor.
            Assert::AreEqual(104L, done.point.x);
            Assert::AreEqual(104L, injector.calls[0].pt.x);
        }

        TEST_METHOD (MovementBeyondToleranceRestartsTheCountdown)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            e.OnMove(PointL{ 200, 200 }, 50, s); // re-anchors and restarts from tick 50

            Assert::IsFalse(e.Poll(100, s).fired); // only 50 ms into the new countdown
            Assert::IsTrue(e.Poll(150, s).fired);
        }

        TEST_METHOD (ZeroDwellTimeDoesNotDivideByZero)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.dwellTimeMs = 0;

            Arm(e, s);

            const PollResult immediate = e.Poll(0, s);
            Assert::IsFalse(immediate.fired);
            // The guard clamps the divisor to 1 ms, so progress stays a real number rather than NaN.
            Assert::AreEqual(0.0, immediate.progress);
            Assert::IsTrue(e.Poll(1, s).fired);
        }

        TEST_METHOD (NegativeSettingsAreClampedRatherThanUndefined)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.dwellTimeMs = -500;
            s.moveTolerancePixels = -5;
            s.postActionTolerancePixels = -5;

            // A negative tolerance clamps to 0, so any movement at all arms the machine.
            e.OnMove(PointL{ 1, 0 }, 0, s);
            Assert::IsTrue(e.Poll(1, s).fired);
        }

        TEST_METHOD (ATickOlderThanTheAnchorDoesNotFire)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s, PointL{ 100, 100 }, 1000);

            // Unsigned subtraction would wrap to a huge elapsed value and fire instantly.
            const PollResult r = e.Poll(500, s);
            Assert::IsFalse(r.fired);
            Assert::AreEqual(0.0, r.progress);
        }
    };

    TEST_CLASS (PostActionTolerance)
    {
    public:
        TEST_METHOD (PostActionToleranceIsSeparateFromTheDwellTolerance)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.moveTolerancePixels = 10;
            s.postActionTolerancePixels = 50; // deliberately wider than the dwell tolerance

            Arm(e, s);
            Assert::IsTrue(e.Poll(100, s).fired);

            // 20 px clears the dwell tolerance but not the re-arm distance, so the machine stays
            // locked. This is the setting that keeps a shaky hand from double-firing in place.
            e.OnMove(PointL{ 120, 100 }, 110, s);
            Assert::IsFalse(e.Poll(300, s).fired);

            // 100 px clears the re-arm distance and starts a fresh countdown.
            e.OnMove(PointL{ 200, 100 }, 400, s);
            Assert::IsFalse(e.Poll(450, s).fired); // countdown restarted at tick 400
            Assert::IsTrue(e.Poll(500, s).fired);
            Assert::AreEqual(static_cast<size_t>(2), injector.Count());
        }

        TEST_METHOD (ExactPostActionToleranceDoesNotUnlock)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.postActionTolerancePixels = 50;

            Arm(e, s);
            Assert::IsTrue(e.Poll(100, s).fired);

            e.OnMove(PointL{ 150, 100 }, 110, s); // exactly 50 px: still parked
            Assert::IsFalse(e.Poll(300, s).fired);

            e.OnMove(PointL{ 151, 100 }, 310, s); // 51 px clears it
            Assert::IsTrue(e.Poll(500, s).fired);
        }

        TEST_METHOD (NonActionLocksUseTheOrdinaryTolerance)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.moveTolerancePixels = 10;
            s.postActionTolerancePixels = 500; // must not apply to a plain lock

            Arm(e, s);
            e.LockUntilMove();

            // A settings-change lock is not a post-action lock, so 20 px is enough to re-arm.
            e.OnMove(PointL{ 120, 100 }, 50, s);
            Assert::IsTrue(e.Poll(150, s).fired);
        }
    };

    TEST_CLASS (ActionSelection)
    {
    public:
        TEST_METHOD (ChosenActionFiresAndThenRevertsToTheDefault)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.defaultAction = DwellAction::LeftClick;
            s.revertToDefaultAfterAction = true;

            e.SetNextAction(DwellAction::RightClick);
            Arm(e, s);

            const PollResult r = e.Poll(100, s);
            Assert::IsTrue(r.fired);
            // The caller is told what actually fired, not what the next dwell will do.
            Assert::IsTrue(r.action == DwellAction::RightClick);
            Assert::IsTrue(injector.calls[0].kind == ClickKind::RightClick);
            Assert::IsTrue(e.NextAction() == DwellAction::LeftClick);
        }

        TEST_METHOD (ActionStaysStickyWhenRevertIsDisabled)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.revertToDefaultAfterAction = false;

            e.SetNextAction(DwellAction::RightClick);
            Arm(e, s);
            Assert::IsTrue(e.Poll(100, s).fired);
            Assert::IsTrue(e.NextAction() == DwellAction::RightClick);

            e.OnMove(PointL{ 400, 400 }, 200, s);
            Assert::IsTrue(e.Poll(300, s).fired);
            Assert::IsTrue(injector.calls[1].kind == ClickKind::RightClick);
        }

        TEST_METHOD (ChoosingAnActionLocksUntilThePointerMoves)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            e.Poll(50, s); // countdown half spent

            // Picking an action mid-countdown must not let the remaining time fire the new action
            // on a target the user was not aiming at.
            e.SetNextAction(DwellAction::DoubleClick);
            Assert::IsFalse(e.Poll(100, s).fired);
            Assert::IsFalse(e.Poll(10000, s).fired);

            e.OnMove(PointL{ 300, 300 }, 200, s);
            Assert::IsTrue(e.Poll(300, s).fired);
            Assert::IsTrue(injector.calls[0].kind == ClickKind::DoubleClick);
        }

        TEST_METHOD (EachActionMapsToItsOwnClickKind)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.revertToDefaultAfterAction = false;

            const DwellAction actions[] = { DwellAction::LeftClick, DwellAction::RightClick, DwellAction::DoubleClick, DwellAction::MiddleClick };
            const ClickKind expected[] = { ClickKind::LeftClick, ClickKind::RightClick, ClickKind::DoubleClick, ClickKind::MiddleClick };

            for (int i = 0; i < 4; i++)
            {
                FakeInjector each;
                Engine engine(each);
                engine.SetNextAction(actions[i]);
                engine.OnMove(PointL{ 100, 100 }, 0, s);
                Assert::IsTrue(engine.Poll(100, s).fired);
                Assert::AreEqual(static_cast<size_t>(1), each.Count());
                Assert::IsTrue(each.calls[0].kind == expected[i]);
            }
        }
    };

    TEST_CLASS (DragGesture)
    {
    public:
        TEST_METHOD (DragIsTwoDwellsPressThenRelease)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.defaultAction = DwellAction::LeftClick;

            e.SetNextAction(DwellAction::Drag);
            Arm(e, s);

            // First dwell picks the item up.
            Assert::IsTrue(e.Poll(100, s).fired);
            Assert::IsTrue(e.IsDragging());
            Assert::AreEqual(static_cast<size_t>(1), injector.Count());
            Assert::IsTrue(injector.calls[0].kind == ClickKind::LeftDown);
            // Mid-gesture the action must not revert, or nothing could release the button.
            Assert::IsTrue(e.NextAction() == DwellAction::Drag);

            // Carry it somewhere else and dwell again to drop it.
            e.OnMove(PointL{ 300, 300 }, 200, s);
            Assert::IsTrue(e.Poll(300, s).fired);
            Assert::IsFalse(e.IsDragging());
            Assert::AreEqual(static_cast<size_t>(2), injector.Count());
            Assert::IsTrue(injector.calls[1].kind == ClickKind::LeftUp);
            Assert::AreEqual(300L, injector.calls[1].pt.x);
            // Only now, with the gesture complete, does the action revert.
            Assert::IsTrue(e.NextAction() == DwellAction::LeftClick);
        }

        TEST_METHOD (ReleaseDragFreesAHeldButton)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            e.SetNextAction(DwellAction::Drag);
            Arm(e, s);
            Assert::IsTrue(e.Poll(100, s).fired);
            Assert::IsTrue(e.IsDragging());

            Assert::IsTrue(e.ReleaseDrag());
            Assert::IsFalse(e.IsDragging());
            Assert::IsTrue(injector.calls[1].kind == ClickKind::LeftUp);

            // Releasing again is a no-op rather than a stray injection.
            Assert::IsFalse(e.ReleaseDrag());
            Assert::AreEqual(static_cast<size_t>(2), injector.Count());
        }

        TEST_METHOD (ResetTransientReleasesAnInFlightDrag)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            e.SetNextAction(DwellAction::Drag);
            Arm(e, s);
            Assert::IsTrue(e.Poll(100, s).fired);

            // A disable/enable cycle must never leave the left button physically stuck down.
            e.ResetTransient(500);
            Assert::IsFalse(e.IsDragging());
            Assert::IsTrue(injector.calls[1].kind == ClickKind::LeftUp);
        }

        TEST_METHOD (AFailedPressDoesNotEnterDragState)
        {
            FakeInjector injector;
            injector.succeed = false;
            Engine e(injector);
            const Settings s = DefaultSettings();

            e.SetNextAction(DwellAction::Drag);
            Arm(e, s);

            const PollResult r = e.Poll(100, s);
            Assert::IsFalse(r.fired);
            // Believing we are dragging when the press never landed would make the next dwell
            // inject an unpaired button-up.
            Assert::IsFalse(e.IsDragging());
        }
    };

    TEST_CLASS (PauseAndSuppression)
    {
    public:
        TEST_METHOD (PauseStopsFiring)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            e.SetPaused(true);
            Assert::IsTrue(e.IsPaused());

            Assert::IsFalse(e.Poll(100, s).fired);
            e.OnMove(PointL{ 500, 500 }, 200, s); // movement must not arm while paused
            Assert::IsFalse(e.Poll(400, s).fired);
            Assert::AreEqual(static_cast<size_t>(0), injector.Count());
        }

        TEST_METHOD (ResumingRequiresMovementBeforeFiring)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            e.SetPaused(true);
            e.SetPaused(false);
            Assert::IsFalse(e.IsPaused());

            // Time passed while paused must not count toward a countdown.
            Assert::IsFalse(e.Poll(10000, s).fired);

            e.OnMove(PointL{ 300, 300 }, 10000, s);
            Assert::IsTrue(e.Poll(10100, s).fired);
        }

        TEST_METHOD (PausingMidDragReleasesTheHeldButton)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            e.SetNextAction(DwellAction::Drag);
            Arm(e, s);
            Assert::IsTrue(e.Poll(100, s).fired);
            Assert::IsTrue(e.IsDragging());

            e.SetPaused(true);
            Assert::IsFalse(e.IsDragging());
            Assert::AreEqual(static_cast<size_t>(2), injector.Count());
            Assert::IsTrue(injector.calls[1].kind == ClickKind::LeftUp);
        }

        TEST_METHOD (RedundantPauseCallsAreNoOps)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            e.SetPaused(false); // already running: must not disturb the countdown
            Assert::IsTrue(e.Poll(100, s).fired);
        }

        TEST_METHOD (APhysicalClickSuppressesTheNextDwell)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            e.Poll(50, s);

            // The user clicked for themselves; do not stack a dwell click on top of it.
            e.OnPhysicalClick();
            Assert::IsFalse(e.Poll(100, s).fired);
            Assert::IsFalse(e.Poll(5000, s).fired);

            e.OnMove(PointL{ 300, 300 }, 200, s);
            Assert::IsTrue(e.Poll(300, s).fired);
        }

        TEST_METHOD (LockUntilMoveProtectsAgainstLiveSettingsChanges)
        {
            FakeInjector injector;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.dwellTimeMs = 1000;

            Arm(e, s);
            e.Poll(900, s); // 900 ms of rest already banked

            // The user drags the dwell-time slider down to 100 ms. Without the re-lock the banked
            // 900 ms would instantly satisfy the new threshold and fire.
            s.dwellTimeMs = 100;
            e.LockUntilMove();
            Assert::IsFalse(e.Poll(901, s).fired);

            e.OnMove(PointL{ 300, 300 }, 1000, s);
            Assert::IsTrue(e.Poll(1100, s).fired);
        }

        TEST_METHOD (ResetTransientLocksSoAReEnableCannotFireImmediately)
        {
            FakeInjector injector;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);
            e.ResetTransient(50);

            Assert::IsFalse(e.Poll(1000, s).fired);
            e.OnMove(PointL{ 300, 300 }, 1000, s);
            Assert::IsTrue(e.Poll(1100, s).fired);
        }
    };

    TEST_CLASS (InjectionFailure)
    {
    public:
        TEST_METHOD (AFailedInjectionIsReportedAndNotRetriedOnEveryPoll)
        {
            FakeInjector injector;
            injector.succeed = false;
            Engine e(injector);
            const Settings s = DefaultSettings();

            Arm(e, s);

            const PollResult r = e.Poll(100, s);
            Assert::IsFalse(r.fired);
            Assert::AreEqual(static_cast<size_t>(1), injector.Count());

            // The engine still locks, so a blocked injection does not spin once per poll for as
            // long as the pointer happens to rest there.
            e.Poll(200, s);
            e.Poll(300, s);
            Assert::AreEqual(static_cast<size_t>(1), injector.Count());
        }

        TEST_METHOD (AFailedInjectionStillConsumesTheAction)
        {
            FakeInjector injector;
            injector.succeed = false;
            Engine e(injector);
            Settings s = DefaultSettings();
            s.revertToDefaultAfterAction = true;

            e.SetNextAction(DwellAction::RightClick);
            Arm(e, s);
            Assert::IsFalse(e.Poll(100, s).fired);

            // The revert is deliberately skipped on failure: the user asked for a right click and
            // never got one, so the next dwell should still be the right click they chose.
            Assert::IsTrue(e.NextAction() == DwellAction::RightClick);
        }
    };
}
