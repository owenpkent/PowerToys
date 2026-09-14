// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>

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
        int toolbarSide = 0; // 0 = left edge, 1 = right edge
        bool showCountdown = true;

        // 0 = small, 1 = medium (default), 2 = large. Scales the toolbar buttons and the
        // countdown ring together: dwell targets obey Fitts's law, and the audience for
        // this module needs room to land on them.
        int overlaySize = 1;

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

        bool SameButtons(const OverlaySettings& other) const
        {
            return buttonLeftClick == other.buttonLeftClick &&
                   buttonDoubleClick == other.buttonDoubleClick &&
                   buttonRightClick == other.buttonRightClick &&
                   buttonMiddleClick == other.buttonMiddleClick &&
                   buttonDrag == other.buttonDrag &&
                   buttonScrollUp == other.buttonScrollUp &&
                   buttonScrollDown == other.buttonScrollDown &&
                   buttonOpenSettings == other.buttonOpenSettings;
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
        // machine is idle (progress 0) or the ring is disabled.
        void UpdateIndicator(POINT pt, double progress, bool show);

        // Whether the point is over the visible toolbar; the caller keeps the engine locked
        // there so dwells over the toolbar select buttons instead of clicking through them.
        bool IsPointOverToolbar(POINT screenPt) const;

    private:
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
        double m_lastPaintedProgress = -1.0;
        int m_lastPaintedHover = -2;
        int m_lastDwellMs = 1;
        uint64_t m_lastTick = 0;
    };
}
