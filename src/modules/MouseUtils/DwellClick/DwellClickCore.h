// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include <cstdint>

// The dwell-click state machine, deliberately decoupled from Win32 so it can be unit tested. The
// caller (the module's cursor poller) feeds it pointer positions and a monotonic millisecond tick
// plus a settings snapshot, and polls it for countdown progress. No Win32 calls live here: click
// synthesis is behind IClickInjector, and the clock is the caller-supplied tick, so tests can drive
// both deterministically.
//
// Dwell clicking auto-issues a click when the pointer is held still over a target. Its dominant
// failure mode is the classic "Midas touch" problem: the machine cannot tell resting the
// pointer from aiming it, so it fires clicks the user never intended. Measured error profiles for
// dwell-style selection are overwhelmingly unintended activations rather than missed targets, so
// every design choice below is biased toward refusing to fire when intent is unclear:
//
//   - The machine starts LOCKED and re-locks after every action, after a physical click, on pause,
//     and on any live settings change. A lock is cleared only by deliberate pointer movement.
//   - Two separate distances gate that movement (this mirrors macOS Dwell Control, which exposes
//     "dwell movement tolerance" and "post-action movement tolerance" as distinct settings):
//     moveTolerancePixels is the jitter a countdown survives, and postActionTolerancePixels is
//     how far the pointer must travel after an action before a new countdown may start. Keeping
//     them separate is what stops a parked pointer from re-firing on the spot.
namespace dwellclick
{
    // What a completed dwell does. Left/Right/Double/Middle fire a click in place; Drag is a
    // two-dwell gesture (first dwell presses the left button, second dwell releases it), which is
    // how every surveyed dwell product expresses click-and-drag without a held physical button.
    enum class DwellAction
    {
        LeftClick,
        RightClick,
        DoubleClick,
        MiddleClick,
        Drag,
    };

    // The primitive the injector is asked to synthesize. Drag is decomposed into LeftDown/LeftUp so
    // the injector stays a dumb SendInput wrapper and all sequencing lives in the engine.
    enum class ClickKind
    {
        LeftClick,
        RightClick,
        DoubleClick,
        MiddleClick,
        LeftDown,
        LeftUp,
    };

    // A plain point so the core does not need <windows.h>.
    struct PointL
    {
        long x = 0;
        long y = 0;
    };

    struct Settings
    {
        // How long the pointer must rest before the action fires. The default matches the GNOME
        // Hover Click dwell time (1200 ms), the closest mouse-driven analogue to this module;
        // eye-gaze research optima cluster lower (around 600 ms) but assume a tracker rather than a
        // hand on a mouse. Every field is overwritten from settings in production, so these
        // defaults only surface in tests.
        int dwellTimeMs = 1200;

        // Jitter the countdown tolerates before it re-anchors and restarts. The default matches the
        // GNOME dwell-threshold default of 10 px.
        int moveTolerancePixels = 10;

        // How far the pointer must move after an action before a new countdown may start. Separate
        // from moveTolerancePixels so it can be widened independently: this is the setting that
        // stops a resting hand from firing a second click on the same spot.
        int postActionTolerancePixels = 10;

        // The action a dwell fires when the user has not chosen another one.
        DwellAction defaultAction = DwellAction::LeftClick;

        // Revert to defaultAction after each action completes, so a one-off right click does not
        // silently turn every later dwell into a right click. On by default because it is the
        // near-universal behavior of shipping dwell tools (GNOME Hover Click and Dwell Clicker 2
        // both revert unconditionally; macOS exposes it as an "Auto revert to left click" toggle).
        bool revertToDefaultAfterAction = true;
    };

    // Abstraction over click synthesis (SendInput in production, a recording fake in tests).
    // Returns true if the OS accepted the synthetic event.
    struct IClickInjector
    {
        virtual ~IClickInjector() = default;
        virtual bool Inject(ClickKind kind, PointL pt) = 0;
    };

    // The result of one Poll. progress drives the countdown ring; it is 0 whenever the machine is
    // locked or paused, so the ring clears itself without the caller tracking state.
    struct PollResult
    {
        double progress = 0.0;
        bool fired = false;
        DwellAction action = DwellAction::LeftClick;
        PointL point{};
    };

    class Engine
    {
    public:
        explicit Engine(IClickInjector& injector) :
            m_injector(injector)
        {
        }

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // Report a new pointer position. Never fires anything itself; it only decides whether the
        // pointer has moved enough to re-arm a locked machine or to restart a running countdown.
        void OnMove(PointL pt, uint64_t tick, const Settings& s)
        {
            m_current = pt;

            if (m_paused)
            {
                // Keep tracking the position so the anchor is current when the user resumes, but
                // never let movement arm the machine while paused.
                return;
            }

            if (m_locked)
            {
                // A post-action lock uses the wider re-arm distance; every other lock (startup,
                // settings change, physical click) uses the ordinary tolerance.
                const int unlockPixels = m_lockedAfterAction
                                             ? Clamp0(s.postActionTolerancePixels)
                                             : Clamp0(s.moveTolerancePixels);
                if (MovedBeyond(pt, m_anchor, unlockPixels))
                {
                    StartCountdown(pt, tick);
                }
                return;
            }

            // Motion beyond the tolerance means the pointer is being aimed, not resting: re-anchor
            // and restart the countdown from here. Drift within the tolerance leaves the countdown
            // untouched, which is what lets a hand with a tremor complete a dwell at all.
            if (MovedBeyond(pt, m_anchor, Clamp0(s.moveTolerancePixels)))
            {
                m_anchor = pt;
                m_restStartTick = tick;
            }
        }

        // Advance the countdown and fire the action if it has completed. Call this on a timer; the
        // returned progress is the countdown ring fill level.
        PollResult Poll(uint64_t tick, const Settings& s)
        {
            PollResult result;
            result.action = m_nextAction;
            result.point = m_current;

            if (m_paused || m_locked)
            {
                return result;
            }

            // A dwell time of 0 would divide by zero, and a negative one is meaningless. Clamp to
            // 1 ms so progress stays finite and such a setting simply fires on the next poll.
            const int dwellMs = s.dwellTimeMs < 1 ? 1 : s.dwellTimeMs;

            // The tick is monotonic in production, but a caller (or the fuzz target) can hand back
            // a tick older than the anchor. Treating that as 0 elapsed keeps the countdown honest;
            // the unsigned subtraction would otherwise wrap and fire instantly.
            const uint64_t elapsed = tick >= m_restStartTick ? tick - m_restStartTick : 0;

            const double progress = static_cast<double>(elapsed) / static_cast<double>(dwellMs);
            if (progress < 1.0)
            {
                result.progress = progress;
                return result;
            }

            // FireAction records which action ran before any revert, so the caller is told what
            // actually fired rather than what the next dwell will do.
            result.fired = FireAction(m_nextAction, s);
            result.action = m_firedAction;

            // Lock regardless of whether the injection succeeded. On success this is the re-arm
            // rule; on failure it stops a blocked injection (against an elevated window, say)
            // from retrying on every poll for as long as the pointer rests there.
            LockAfterAction();

            return result;
        }

        // Choose the action the next dwell will fire. Locks until the pointer moves: the elapsed
        // countdown was started with the previous action in mind, so letting it run on would fire
        // the newly chosen action almost immediately, on a target the user was not aiming at.
        void SetNextAction(DwellAction action)
        {
            m_nextAction = action;
            LockUntilMove();
        }

        DwellAction NextAction() const
        {
            return m_nextAction;
        }

        // Suspend dwelling without losing settings. Every surveyed dwell tool ships this escape
        // hatch (the Dwell Clicker 2 "Rest" button, the macOS Pause dwell action, the Android
        // Pause control), because a user who cannot click also cannot easily stop a machine that
        // clicks for them. Pausing mid-drag releases the held button so the pointer is never left
        // stuck.
        void SetPaused(bool paused)
        {
            if (paused == m_paused)
            {
                return;
            }
            m_paused = paused;
            if (paused)
            {
                ReleaseDrag();
            }
            // Lock in both directions: entering pause must abandon the running countdown, and
            // leaving it must not resume against time that elapsed while the user was away.
            LockUntilMove();
        }

        bool IsPaused() const
        {
            return m_paused;
        }

        // Report that the user clicked a physical mouse button. Someone who can still click
        // sometimes should not get a dwell click stacked on top of the one they just made, so this
        // locks until the pointer moves away.
        void OnPhysicalClick()
        {
            LockUntilMove();
        }

        // Lock until the pointer moves past the ordinary tolerance. Call on any live settings
        // change: shortening the dwell time against an already-running countdown would otherwise
        // fire the moment the new value is applied.
        void LockUntilMove()
        {
            m_locked = true;
            m_lockedAfterAction = false;
            m_anchor = m_current;
        }

        // Release a drag that is still in progress (pause, disable, shutdown). Returns true if a
        // button was actually released, so callers can log it.
        bool ReleaseDrag()
        {
            if (!m_dragging)
            {
                return false;
            }
            m_dragging = false;
            return m_injector.Inject(ClickKind::LeftUp, m_current);
        }

        bool IsDragging() const
        {
            return m_dragging;
        }

        // Clear transient state and lock. Call when (re)enabling so a pointer left resting across a
        // disable/enable cycle cannot fire the instant the module comes back.
        void ResetTransient(uint64_t tick)
        {
            ReleaseDrag();
            m_locked = true;
            m_lockedAfterAction = false;
            m_anchor = m_current;
            m_restStartTick = tick;
        }

    private:
        // Distance comparison in double. Coordinates are 32-bit, so a raw long long product can
        // overflow signed 64-bit for extreme inputs. In production the pointer is screen-bounded so
        // this never triggers, but the engine must stay defined for any input and the fuzz target
        // drives the full coordinate range. double holds these magnitudes without overflow, and its
        // precision is far finer than a pixel tolerance needs.
        static bool MovedBeyond(PointL pt, PointL anchor, int pixels)
        {
            const double dx = static_cast<double>(pt.x) - static_cast<double>(anchor.x);
            const double dy = static_cast<double>(pt.y) - static_cast<double>(anchor.y);
            const double threshold = static_cast<double>(pixels) * static_cast<double>(pixels);
            // Strictly greater: a move of exactly the tolerance still counts as resting, so the
            // boundary belongs to the countdown rather than cancelling it.
            return dx * dx + dy * dy > threshold;
        }

        static int Clamp0(int value)
        {
            return value < 0 ? 0 : value;
        }

        void StartCountdown(PointL pt, uint64_t tick)
        {
            m_anchor = pt;
            m_restStartTick = tick;
            m_locked = false;
            m_lockedAfterAction = false;
        }

        void LockAfterAction()
        {
            m_locked = true;
            m_lockedAfterAction = true;
            m_anchor = m_current;
        }

        // Inject the action. Returns whether the OS accepted it. Drag advances its own two-step
        // state and only reverts once the drop completes, so an interrupted drag cannot leave the
        // action stuck halfway.
        bool FireAction(DwellAction action, const Settings& s)
        {
            m_firedAction = action;

            if (action == DwellAction::Drag)
            {
                if (!m_dragging)
                {
                    if (!m_injector.Inject(ClickKind::LeftDown, m_current))
                    {
                        return false;
                    }
                    m_dragging = true;
                    // Deliberately no revert here: the gesture is only half done, and reverting now
                    // would leave the left button held with no dwell able to release it.
                    return true;
                }

                if (!m_injector.Inject(ClickKind::LeftUp, m_current))
                {
                    return false;
                }
                m_dragging = false;
                RevertIfRequested(s);
                return true;
            }

            if (!m_injector.Inject(ToClickKind(action), m_current))
            {
                return false;
            }
            RevertIfRequested(s);
            return true;
        }

        void RevertIfRequested(const Settings& s)
        {
            if (s.revertToDefaultAfterAction)
            {
                m_nextAction = s.defaultAction;
            }
        }

        static ClickKind ToClickKind(DwellAction action)
        {
            switch (action)
            {
            case DwellAction::RightClick:
                return ClickKind::RightClick;
            case DwellAction::DoubleClick:
                return ClickKind::DoubleClick;
            case DwellAction::MiddleClick:
                return ClickKind::MiddleClick;
            case DwellAction::Drag:
            case DwellAction::LeftClick:
            default:
                return ClickKind::LeftClick;
            }
        }

        IClickInjector& m_injector;

        // The position the countdown is measured from, and the latest reported position. They are
        // separate so drift within the tolerance still updates the click point without disturbing
        // the countdown.
        PointL m_anchor{};
        PointL m_current{};
        uint64_t m_restStartTick = 0;

        // Starts locked so a freshly enabled module never clicks before the user has moved.
        bool m_locked = true;
        bool m_lockedAfterAction = false;
        bool m_paused = false;
        bool m_dragging = false;

        DwellAction m_nextAction = DwellAction::LeftClick;
        DwellAction m_firedAction = DwellAction::LeftClick;
    };
}
