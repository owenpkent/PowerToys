// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include <cstdint>
#include <optional>

#include "DwellClickCore.h"

// The action toolbar's state machine, deliberately decoupled from Win32 so it can be unit
// tested like the engine. The overlay window feeds it pointer positions (in toolbar-local
// coordinates) and a monotonic tick; the model decides layout, hit-testing, and when a
// hover has lasted long enough to activate a button.
//
// The interaction model follows shipping dwell toolbars (Ease Mouse and its open-source
// clone OpenMouse, Dwell Clicker 2): buttons activate by HOVERING for the dwell time, with
// the toolbar running its own countdown rather than receiving synthetic clicks. The engine
// is kept locked while the pointer is over the toolbar, which is what lets a paused user
// still reach the resume button: pausing stops the engine, never the toolbar.
//
// Activation rules mirror the engine's Midas-touch bias:
//   - Entering a button starts its countdown; jitter WITHIN the button never resets it
//     (the button rect is the tolerance), but crossing into a gap or another button does.
//   - An activated button will not fire again until the pointer leaves it, so parking on
//     Pause toggles once, not once per dwell time.
//   - A physical click activates immediately and applies the same leave-to-rearm rule.
namespace dwellclick
{
    enum class ToolbarCommand
    {
        ToggleCollapse,
        TogglePause,
        SelectLeftClick,
        SelectDoubleClick,
        SelectRightClick,
        SelectMiddleClick,
        SelectDrag,
    };

    struct ToolbarRect
    {
        long x = 0;
        long y = 0;
        long w = 0;
        long h = 0;

        constexpr bool Contains(PointL pt) const
        {
            return pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h;
        }
    };

    class ToolbarModel
    {
    public:
        // Geometry in unscaled 96-dpi units; the overlay multiplies by the monitor's DPI
        // scale before creating the window and divides pointer input back down.
        static constexpr int ButtonSize = 40;
        static constexpr int Padding = 4;
        static constexpr int Gap = 4;

        // Index 0 is the collapse handle; it is the only button shown while collapsed.
        static constexpr int ButtonCount = 7;
        static constexpr int CollapseIndex = 0;
        static constexpr int PauseIndex = 1;

        static constexpr ToolbarCommand CommandFor(int index)
        {
            switch (index)
            {
            case 1:
                return ToolbarCommand::TogglePause;
            case 2:
                return ToolbarCommand::SelectLeftClick;
            case 3:
                return ToolbarCommand::SelectDoubleClick;
            case 4:
                return ToolbarCommand::SelectRightClick;
            case 5:
                return ToolbarCommand::SelectMiddleClick;
            case 6:
                return ToolbarCommand::SelectDrag;
            case 0:
            default:
                return ToolbarCommand::ToggleCollapse;
            }
        }

        bool IsCollapsed() const
        {
            return m_collapsed;
        }

        // Collapsing or expanding moves every button, so any running hover is meaningless.
        void SetCollapsed(bool collapsed)
        {
            m_collapsed = collapsed;
            ResetHover();
        }

        int VisibleButtonCount() const
        {
            return m_collapsed ? 1 : ButtonCount;
        }

        int Width() const
        {
            return Padding + ButtonSize + Padding;
        }

        int Height() const
        {
            const int n = VisibleButtonCount();
            return Padding + n * ButtonSize + (n - 1) * Gap + Padding;
        }

        ToolbarRect ButtonRect(int index) const
        {
            return ToolbarRect{
                Padding,
                Padding + index * (ButtonSize + Gap),
                ButtonSize,
                ButtonSize,
            };
        }

        // The button under a toolbar-local point, or -1 for the padding and gaps.
        int HitTest(PointL local) const
        {
            for (int i = 0; i < VisibleButtonCount(); ++i)
            {
                if (ButtonRect(i).Contains(local))
                {
                    return i;
                }
            }
            return -1;
        }

        // Report the pointer. inside is whether the pointer is over the toolbar window at
        // all; when it is not, local is ignored. Returns the command to run when a hover
        // completes.
        std::optional<ToolbarCommand> OnPointer(PointL local, bool inside, uint64_t tick, int dwellMs)
        {
            const int hit = inside ? HitTest(local) : -1;

            if (hit != m_hoverIndex)
            {
                m_hoverIndex = hit;
                m_hoverStartTick = tick;
                // Leaving the button that last activated re-arms it.
                if (m_blockedIndex >= 0 && hit != m_blockedIndex)
                {
                    m_blockedIndex = -1;
                }
            }

            if (m_hoverIndex < 0 || m_hoverIndex == m_blockedIndex)
            {
                return std::nullopt;
            }

            // Same clamps as the engine: a degenerate dwell time stays defined, and a tick
            // older than the anchor reads as 0 elapsed rather than wrapping.
            const int clampedDwellMs = dwellMs < 1 ? 1 : dwellMs;
            const uint64_t elapsed = tick >= m_hoverStartTick ? tick - m_hoverStartTick : 0;
            if (elapsed < static_cast<uint64_t>(clampedDwellMs))
            {
                return std::nullopt;
            }

            m_blockedIndex = m_hoverIndex;
            return CommandFor(m_hoverIndex);
        }

        // A physical click activates immediately, with the same leave-to-rearm rule.
        std::optional<ToolbarCommand> OnClick(PointL local)
        {
            const int hit = HitTest(local);
            if (hit < 0)
            {
                return std::nullopt;
            }
            m_hoverIndex = hit;
            m_blockedIndex = hit;
            return CommandFor(hit);
        }

        // The countdown fill for painting: 0 when nothing is charging, 1 at activation.
        double HoverProgress(uint64_t tick, int dwellMs) const
        {
            if (m_hoverIndex < 0 || m_hoverIndex == m_blockedIndex)
            {
                return 0.0;
            }
            const int clampedDwellMs = dwellMs < 1 ? 1 : dwellMs;
            const uint64_t elapsed = tick >= m_hoverStartTick ? tick - m_hoverStartTick : 0;
            const double progress = static_cast<double>(elapsed) / static_cast<double>(clampedDwellMs);
            return progress > 1.0 ? 1.0 : progress;
        }

        int HoveredIndex() const
        {
            return m_hoverIndex;
        }

        void ResetHover()
        {
            m_hoverIndex = -1;
            m_blockedIndex = -1;
            m_hoverStartTick = 0;
        }

    private:
        bool m_collapsed = false;
        int m_hoverIndex = -1;
        int m_blockedIndex = -1;
        uint64_t m_hoverStartTick = 0;
    };
}
