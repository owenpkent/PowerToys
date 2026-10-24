// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "DwellClickCore.h"
#include "DwellClickToolbar.h"

// The module's on-screen surfaces, modeled on shipping dwell tools (Ease Mouse, OpenMouse):
//
//   - A countdown ring that follows the pointer and fills with PollResult.progress, so the
//     user always sees when a click is about to land. The window is layered, topmost, and
//     click-through: it can never receive input or steal focus.
//   - An action toolbar docked to a screen edge: a collapse handle, pause/resume, and a
//     Settings-chosen set of action buttons (clicks, drag, scroll modes, open Settings). It
//     accepts input (hover dwells and physical clicks) but never activates, and its
//     decisions live in the Win32-free ToolbarModel.
//
// Both windows are created, painted, and destroyed on the module's hook thread, which
// already pumps messages; commands therefore reach the caller's callback on that same
// thread, so the engine stays single-threaded.
namespace dwellclick
{
    struct OverlaySettings
    {
        bool showToolbar = true;
        int toolbarSide = 0; // 0 = left edge, 1 = right edge, 2 = top edge, 3 = bottom edge

        // Where the toolbar is centered along its edge, in percent from the start of the edge
        // (top for the side edges, left for the top and bottom edges). 50 centers it.
        int toolbarPosition = 50;

        // 0 = a bar docked to the edge. 1 = a single floating button at that spot which opens a
        // large grid menu in the middle of the screen (the Ease Mouse "Big" menu); the menu
        // closes after any choice.
        int toolbarStyle = 0;

        bool showCountdown = true;

        // 0 = small, 1 = medium (default), 2 = large. Scales the toolbar buttons and the
        // countdown ring together: dwell targets obey Fitts's law, and the audience for
        // this module needs room to land on them.
        int overlaySize = 1;

        // The countdown ring's fill color: the Windows accent color, or indicatorColor (a
        // COLORREF, default the brand blue) when the accent is switched off.
        bool indicatorUseAccentColor = true;
        COLORREF indicatorColor = RGB(0x00, 0x78, 0xD4);

        // Which action buttons the toolbar carries. The collapse handle and pause are always
        // present: collapse is the handle itself, and pause is the safety escape every
        // surveyed dwell tool ships.
        bool buttonLeftClick = true;
        bool buttonDoubleClick = true;
        bool buttonRightClick = true;
        bool buttonMiddleClick = false;
        bool buttonDrag = true;
        bool buttonScrollUp = true;
        bool buttonScrollDown = true;
        bool buttonOpenSettings = true;

        // The Ease Mouse style extras, all off by default so the shipping bar keeps its length.
        bool buttonScrollLeft = false;
        bool buttonScrollRight = false;
        bool buttonZoomIn = false;
        bool buttonZoomOut = false;
        bool buttonBack = false;
        bool buttonStartMenu = false;
        bool buttonShowDesktop = false;
        bool buttonTaskView = false;
        bool buttonNotifications = false;

        bool SameButtons(const OverlaySettings& other) const
        {
            return buttonLeftClick == other.buttonLeftClick &&
                   buttonDoubleClick == other.buttonDoubleClick &&
                   buttonRightClick == other.buttonRightClick &&
                   buttonMiddleClick == other.buttonMiddleClick &&
                   buttonDrag == other.buttonDrag &&
                   buttonScrollUp == other.buttonScrollUp &&
                   buttonScrollDown == other.buttonScrollDown &&
                   buttonScrollLeft == other.buttonScrollLeft &&
                   buttonScrollRight == other.buttonScrollRight &&
                   buttonZoomIn == other.buttonZoomIn &&
                   buttonZoomOut == other.buttonZoomOut &&
                   buttonBack == other.buttonBack &&
                   buttonStartMenu == other.buttonStartMenu &&
                   buttonShowDesktop == other.buttonShowDesktop &&
                   buttonTaskView == other.buttonTaskView &&
                   buttonNotifications == other.buttonNotifications &&
                   buttonOpenSettings == other.buttonOpenSettings;
        }

        bool SamePlacement(const OverlaySettings& other) const
        {
            return toolbarSide == other.toolbarSide &&
                   toolbarPosition == other.toolbarPosition &&
                   toolbarStyle == other.toolbarStyle;
        }

        // The toolbar commands these settings put on the bar, in display order. Collapse and
        // pause always lead: collapse is the handle itself (the floating button, in that style),
        // and pause is the safety escape every surveyed dwell tool ships.
        std::vector<ToolbarCommand> Buttons() const
        {
            std::vector<ToolbarCommand> buttons{ ToolbarCommand::ToggleCollapse, ToolbarCommand::TogglePause };
            const std::pair<bool, ToolbarCommand> optional[] = {
                { buttonLeftClick, ToolbarCommand::SelectLeftClick },
                { buttonDoubleClick, ToolbarCommand::SelectDoubleClick },
                { buttonRightClick, ToolbarCommand::SelectRightClick },
                { buttonMiddleClick, ToolbarCommand::SelectMiddleClick },
                { buttonDrag, ToolbarCommand::SelectDrag },
                { buttonScrollUp, ToolbarCommand::SelectScrollUp },
                { buttonScrollDown, ToolbarCommand::SelectScrollDown },
                { buttonScrollLeft, ToolbarCommand::SelectScrollLeft },
                { buttonScrollRight, ToolbarCommand::SelectScrollRight },
                { buttonZoomIn, ToolbarCommand::SelectZoomIn },
                { buttonZoomOut, ToolbarCommand::SelectZoomOut },
                { buttonBack, ToolbarCommand::Back },
                { buttonStartMenu, ToolbarCommand::StartMenu },
                { buttonShowDesktop, ToolbarCommand::ShowDesktop },
                { buttonTaskView, ToolbarCommand::TaskView },
                { buttonNotifications, ToolbarCommand::Notifications },
                { buttonOpenSettings, ToolbarCommand::OpenSettings },
            };
            for (const auto& [shown, command] : optional)
            {
                if (shown)
                {
                    buttons.push_back(command);
                }
            }
            return buttons;
        }
    };

    struct ToolbarVisualState
    {
        DwellAction currentAction = DwellAction::LeftClick;
        bool paused = false;
        bool dragging = false;
    };

    class Overlay
    {
    public:
        using CommandCallback = std::function<void(ToolbarCommand)>;

        Overlay() = default;
        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;
        ~Overlay();

        // Create both windows on the calling thread. Returns false (and logs) on failure;
        // the module keeps working without its overlay in that case.
        bool Create(HINSTANCE instance, CommandCallback onCommand);
        void Destroy();

        // Apply the overlay-related settings snapshot; cheap when nothing changed.
        void ApplySettings(const OverlaySettings& settings);

        // Drive the toolbar's hover dwell from the poll loop. screenPt is the live cursor
        // position; dwellMs is the user's dwell time (the toolbar shares it).
        void TickToolbar(POINT screenPt, uint64_t tick, int dwellMs);

        // Reflect engine state (selected action, paused, mid-drag) in the toolbar visuals.
        void SetToolbarState(const ToolbarVisualState& state);

        // Show the countdown ring centered on pt at the given fill, or hide it when the
        // machine is idle (progress 0), the ring is disabled, or the countdown is still too
        // young to be a real rest (see ShowCountdownRing).
        void UpdateIndicator(POINT pt, double progress, int dwellMs, bool show);

        // Whether the point is over the visible toolbar; the caller keeps the engine locked
        // there so dwells over the toolbar select buttons instead of clicking through them.
        bool IsPointOverToolbar(POINT screenPt) const;

        // Whether the floating style's large menu is open. The caller keeps the engine locked
        // while it is, so nothing is clicked behind the menu while the user reaches for it.
        bool IsMenuOpen() const;

    private:
        bool IsFloating() const;
        void LayoutDocked(const RECT& work, double scale);
        void LayoutMenu(const RECT& work, double scale);
        static LRESULT CALLBACK ToolbarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleToolbarMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        void LayoutToolbar();
        void PaintToolbar(uint64_t tick, int dwellMs);
        void PaintIndicator(POINT pt, double progress);
        void DispatchCommand(ToolbarCommand command);
        double Scale() const;

        HINSTANCE m_instance = nullptr;
        HWND m_toolbar = nullptr;
        HWND m_indicator = nullptr;
        CommandCallback m_onCommand;
        ToolbarModel m_model;
        OverlaySettings m_settings;
        ToolbarVisualState m_state;
        ULONG_PTR m_gdiplusToken = 0;
        RECT m_toolbarRect{};
        bool m_buttonsInitialized = false;
        bool m_indicatorVisible = false;
        COLORREF m_accentColor = RGB(0x00, 0x78, 0xD4); // Windows accent, refreshed when a countdown starts
        double m_lastPaintedProgress = -1.0;
        int m_lastPaintedHover = -2;
        int m_lastDwellMs = 1;
        uint64_t m_lastTick = 0;
    };
}
