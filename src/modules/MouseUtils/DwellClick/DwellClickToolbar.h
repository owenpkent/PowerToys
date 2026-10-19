// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

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
    // Select* commands choose the next dwell action. The system commands (Back through
    // Notifications) act at once instead, as a keystroke into the window the user was in: the
    // toolbar never takes focus, so that window is still the foreground one.
    enum class ToolbarCommand
    {
        ToggleCollapse,
        TogglePause,
        SelectLeftClick,
        SelectDoubleClick,
        SelectRightClick,
        SelectMiddleClick,
        SelectDrag,
        SelectScrollUp,
        SelectScrollDown,
        OpenSettings,
        SelectScrollLeft,
        SelectScrollRight,
        SelectZoomIn,
        SelectZoomOut,
        Back,
        StartMenu,
        ShowDesktop,
        TaskView,
        Notifications,
    };

    // The dwell action a Select* command chooses, or nullopt for every other command.
    constexpr std::optional<DwellAction> ActionFor(ToolbarCommand command)
    {
        switch (command)
        {
        case ToolbarCommand::SelectLeftClick:
            return DwellAction::LeftClick;
        case ToolbarCommand::SelectDoubleClick:
            return DwellAction::DoubleClick;
        case ToolbarCommand::SelectRightClick:
            return DwellAction::RightClick;
        case ToolbarCommand::SelectMiddleClick:
            return DwellAction::MiddleClick;
        case ToolbarCommand::SelectDrag:
            return DwellAction::Drag;
        case ToolbarCommand::SelectScrollUp:
            return DwellAction::ScrollUp;
        case ToolbarCommand::SelectScrollDown:
            return DwellAction::ScrollDown;
        case ToolbarCommand::SelectScrollLeft:
            return DwellAction::ScrollLeft;
        case ToolbarCommand::SelectScrollRight:
            return DwellAction::ScrollRight;
        case ToolbarCommand::SelectZoomIn:
            return DwellAction::ZoomIn;
        case ToolbarCommand::SelectZoomOut:
            return DwellAction::ZoomOut;
        default:
            return std::nullopt;
        }
    }

    // Buttons flow along the toolbar's long axis: down a bar docked to the left or right edge,
    // across a bar docked to the top or bottom edge or the floating menu's grid. A line that would
    // outgrow the screen wraps into another column (or row) beside it.
    enum class ToolbarOrientation
    {
        Vertical,
        Horizontal,
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
        // scale before creating the window and divides pointer input back down. The button
        // size is a setting (the overlay-size choice): dwell targets obey Fitts's law, and
        // the audience for this module needs them big.
        static constexpr int DefaultButtonSize = 56;
        static constexpr int Padding = 4;
        static constexpr int Gap = 4;

        int ButtonSize() const
        {
            return m_buttonSize;
        }

        // Every button moves when the size changes, so any running hover is meaningless.
        void SetButtonSize(int size)
        {
            m_buttonSize = size < 24 ? 24 : (size > 96 ? 96 : size);
            ResetHover();
        }

        // Index 0 is the collapse handle; it is the only button shown while collapsed.
        static constexpr int CollapseIndex = 0;

        // The button list is configurable (Settings chooses which action buttons appear).
        // The default construction carries the shipping default set; callers replace it with
        // SetButtons. The first entry must stay ToggleCollapse: it is the collapsed handle.
        ToolbarModel()
        {
            m_buttons = {
                ToolbarCommand::ToggleCollapse,
                ToolbarCommand::TogglePause,
                ToolbarCommand::SelectLeftClick,
                ToolbarCommand::SelectDoubleClick,
                ToolbarCommand::SelectRightClick,
                ToolbarCommand::SelectMiddleClick,
                ToolbarCommand::SelectDrag,
                ToolbarCommand::SelectScrollUp,
                ToolbarCommand::SelectScrollDown,
                ToolbarCommand::OpenSettings,
            };
        }

        // Replace the button list. Every button moves, so any running hover is meaningless.
        void SetButtons(std::vector<ToolbarCommand> buttons)
        {
            m_buttons = std::move(buttons);
            ResetHover();
        }

        const std::vector<ToolbarCommand>& Buttons() const
        {
            return m_buttons;
        }

        ToolbarCommand CommandFor(int index) const
        {
            if (index < 0 || index >= static_cast<int>(m_buttons.size()))
            {
                return ToolbarCommand::ToggleCollapse;
            }
            return m_buttons[static_cast<size_t>(index)];
        }

        // The index of a command in the current list, or -1 when it is not shown.
        int IndexOf(ToolbarCommand command) const
        {
            for (size_t i = 0; i < m_buttons.size(); ++i)
            {
                if (m_buttons[i] == command)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
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
            return m_collapsed ? 1 : static_cast<int>(m_buttons.size());
        }

        // Set the flow direction and how many buttons fit on one line before wrapping. A line
        // length below 1 means unlimited. Every button may move, so any running hover is
        // meaningless.
        void SetLayout(ToolbarOrientation orientation, int buttonsPerLine)
        {
            m_orientation = orientation;
            m_buttonsPerLine = buttonsPerLine < 1 ? 0 : buttonsPerLine;
            ResetHover();
        }

        ToolbarOrientation Orientation() const
        {
            return m_orientation;
        }

        // The longest line length that fits in available units (unscaled) along the long axis.
        // Always at least 1, so a tiny screen still shows a usable column of buttons.
        int ButtonsThatFit(int available) const
        {
            return Fit(available, m_buttonSize);
        }

        static constexpr int Fit(int available, int buttonSize)
        {
            // 64-bit so an extreme available length cannot overflow; the result is at most
            // INT_MAX / step, which fits back in an int.
            const long long step = static_cast<long long>(buttonSize < 1 ? 1 : buttonSize) + Gap;
            const long long fit = (static_cast<long long>(available) - 2 * Padding + Gap) / step;
            return fit < 1 ? 1 : static_cast<int>(fit);
        }

        // How the floating style's large menu fills an area: the button size and column count.
        // fits is false only when even the smallest candidate size overflows; the caller then
        // pins the menu to the area's top-left so its first row (Close and Pause) stays reachable.
        struct GridFit
        {
            int buttonSize = DefaultButtonSize;
            int columns = 1;
            bool fits = true;
        };

        // Try each candidate size, largest first, and take the first at which all count buttons
        // fit in availableWidth x availableHeight (unscaled units). The grid stays as square as
        // the area allows: at least ceil(sqrt(count)) columns, more if the rows would not fit.
        static GridFit FitGrid(int count, int availableWidth, int availableHeight, std::initializer_list<int> sizes)
        {
            const int n = count < 1 ? 1 : count;
            GridFit result;
            for (const int size : sizes)
            {
                const int maxColumns = Fit(availableWidth, size);
                const int maxRows = Fit(availableHeight, size);
                result.buttonSize = size;
                result.columns = maxColumns;
                result.fits = static_cast<long long>(maxColumns) * maxRows >= n;
                if (result.fits)
                {
                    int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
                    const int forRows = (n + maxRows - 1) / maxRows;
                    columns = columns < forRows ? forRows : columns;
                    result.columns = columns > maxColumns ? maxColumns : columns;
                    return result;
                }
            }
            return result;
        }

        // Buttons on one line, and the number of lines, for what is currently visible.
        int ButtonsPerLine() const
        {
            const int n = VisibleButtonCount();
            return m_buttonsPerLine < 1 || m_buttonsPerLine > n ? n : m_buttonsPerLine;
        }

        int LineCount() const
        {
            const int perLine = ButtonsPerLine();
            return perLine < 1 ? 1 : (VisibleButtonCount() + perLine - 1) / perLine;
        }

        int Width() const
        {
            return m_orientation == ToolbarOrientation::Vertical ? Span(LineCount()) : Span(ButtonsPerLine());
        }

        int Height() const
        {
            return m_orientation == ToolbarOrientation::Vertical ? Span(ButtonsPerLine()) : Span(LineCount());
        }

        ToolbarRect ButtonRect(int index) const
        {
            const int perLine = ButtonsPerLine() < 1 ? 1 : ButtonsPerLine();
            const int along = index % perLine;
            const int line = index / perLine;
            const long step = m_buttonSize + Gap;
            const long alongOffset = Padding + along * step;
            const long lineOffset = Padding + line * step;
            return m_orientation == ToolbarOrientation::Vertical
                       ? ToolbarRect{ lineOffset, alongOffset, m_buttonSize, m_buttonSize }
                       : ToolbarRect{ alongOffset, lineOffset, m_buttonSize, m_buttonSize };
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
            }

            // Leaving the button that last activated re-arms it. The block follows the command,
            // not the index, so a relayout that puts the same button back under a parked pointer
            // (collapsing at position 0%, say) does not re-arm it. It is checked on every tick,
            // not only when the hovered index changes: a relayout resets the hover, and a pointer
            // that then sits outside would otherwise never clear the block, leaving the moved
            // handle unable to fire again.
            if (m_blockedCommand && (hit < 0 || CommandFor(hit) != *m_blockedCommand))
            {
                m_blockedCommand.reset();
            }

            if (m_hoverIndex < 0 || IsBlocked(m_hoverIndex))
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

            m_blockedCommand = CommandFor(m_hoverIndex);
            return m_blockedCommand;
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
            m_blockedCommand = CommandFor(hit);
            return m_blockedCommand;
        }

        // The countdown fill for painting: 0 when nothing is charging, 1 at activation.
        double HoverProgress(uint64_t tick, int dwellMs) const
        {
            if (m_hoverIndex < 0 || IsBlocked(m_hoverIndex))
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

        // Forget the running hover (the buttons moved). The leave-to-rearm block is kept: it
        // names a command, and only the pointer leaving that command's button clears it.
        void ResetHover()
        {
            m_hoverIndex = -1;
            m_hoverStartTick = 0;
        }

        // Block the button under a toolbar-local point until the pointer leaves it. The overlay
        // calls this whenever the toolbar appears or moves: a button that lands under a resting
        // pointer must not fire just because the pointer happens to be there, the same way the
        // engine starts locked until the pointer moves.
        void BlockAt(PointL local)
        {
            const int hit = HitTest(local);
            if (hit >= 0)
            {
                m_blockedCommand = CommandFor(hit);
            }
        }

        // The command that activated last and has not been left yet, if any.
        std::optional<ToolbarCommand> BlockedCommand() const
        {
            return m_blockedCommand;
        }

    private:
        bool IsBlocked(int index) const
        {
            return m_blockedCommand && CommandFor(index) == *m_blockedCommand;
        }

        // The extent of count buttons in a row, with the outer padding.
        int Span(int count) const
        {
            const int n = count < 1 ? 1 : count;
            return Padding + n * m_buttonSize + (n - 1) * Gap + Padding;
        }

        std::vector<ToolbarCommand> m_buttons;
        ToolbarOrientation m_orientation = ToolbarOrientation::Vertical;
        int m_buttonsPerLine = 0;
        int m_buttonSize = DefaultButtonSize;
        bool m_collapsed = false;
        int m_hoverIndex = -1;
        std::optional<ToolbarCommand> m_blockedCommand;
        uint64_t m_hoverStartTick = 0;
    };
}
