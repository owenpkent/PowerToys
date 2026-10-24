// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pch.h"
#include "DwellClickOverlay.h"

#include <windowsx.h>
#include <dwmapi.h>

#include <common/logger/logger.h>

// GDI+ needs the COM/OLE types that WIN32_LEAN_AND_MEAN excludes, so pull them in first.
// Warning 4458 (declaration hides class member) is disabled around the GDI+ headers,
// matching the suppression the runner uses for them.
#include <objidl.h>
#pragma warning(push)
#pragma warning(disable : 4458)
#include <gdiplus.h>
#pragma warning(pop)

#include <cmath>

#pragma comment(lib, "dwmapi.lib") // DwmGetColorizationColor

namespace
{
    const wchar_t TOOLBAR_CLASS[] = L"DwellClickToolbarWindow";
    const wchar_t INDICATOR_CLASS[] = L"DwellClickIndicatorWindow";

    // Overlay palette. The toolbar commits to one look that reads over any background
    // (matching the shipping dwell toolbars it is modeled on) rather than following the
    // system theme; the accent matches the PowerToys brand blue used by the module icon.
    const Gdiplus::Color BAR_BACKGROUND{ 235, 32, 32, 32 };
    const Gdiplus::Color BAR_BORDER{ 60, 255, 255, 255 };
    const Gdiplus::Color GLYPH{ 230, 255, 255, 255 };
    const Gdiplus::Color GLYPH_ON_ACCENT{ 255, 255, 255, 255 };
    const Gdiplus::Color ACCENT{ 255, 0, 120, 212 };
    const Gdiplus::Color ACCENT_CHARGE{ 170, 0, 120, 212 };
    const Gdiplus::Color RING_HALO{ 90, 0, 0, 0 };
    const Gdiplus::Color RING_TRACK{ 140, 255, 255, 255 };

    // The overlay-size setting (small / medium / large) maps to these 96-dpi metrics for the
    // toolbar buttons and the countdown ring. The indicator bitmap is square; the ring floats
    // centered inside it with room for the halo stroke.
    constexpr int BUTTON_SIZES[3] = { 40, 56, 72 };

    // The floating style's large menu uses bigger targets than the docked bar: it only shows
    // while the user is choosing, so it can afford the room (Ease Mouse calls this menu "Big").
    constexpr int MENU_BUTTON_SIZES[3] = { 64, 80, 96 };

    // Gap kept between the toolbar and the screen edge, in 96-dpi units.
    constexpr int EDGE_MARGIN = 8;
    constexpr int INDICATOR_SIZES[3] = { 40, 48, 62 };
    constexpr double INDICATOR_RADII[3] = { 11.5, 15.0, 19.5 };

    // How long a shortcut's feedback stays at the pointer.
    constexpr uint64_t FLASH_DURATION_MS = 1200;

    constexpr int ClampSizeIndex(int index)
    {
        return index < 0 ? 0 : (index > 2 ? 2 : index);
    }

    // A 32bpp premultiplied-alpha DIB the GDI+ drawing lands in, committed to the window
    // with UpdateLayeredWindow. Small surfaces repainted at most at the 15 ms poll cadence,
    // so plain GDI+ is plenty.
    struct LayeredSurface
    {
        HDC memDC = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        void* bits = nullptr;
        int width = 0;
        int height = 0;

        bool Init(int w, int h)
        {
            width = w;
            height = h;
            memDC = CreateCompatibleDC(nullptr);
            if (!memDC)
            {
                return false;
            }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = w;
            info.bmiHeader.biHeight = -h; // top-down, so GDI+ rows match window rows
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            bitmap = CreateDIBSection(memDC, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bitmap)
            {
                return false;
            }
            previous = SelectObject(memDC, bitmap);
            return true;
        }

        void Commit(HWND hwnd, POINT position)
        {
            POINT source{ 0, 0 };
            SIZE size{ width, height };
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(hwnd, nullptr, &position, &size, memDC, &source, 0, &blend, ULW_ALPHA);
        }

        ~LayeredSurface()
        {
            if (memDC)
            {
                if (previous)
                {
                    SelectObject(memDC, previous);
                }
                DeleteDC(memDC);
            }
            if (bitmap)
            {
                DeleteObject(bitmap);
            }
        }
    };

    void DrawMouseGlyph(Gdiplus::Graphics& g, Gdiplus::RectF r, const Gdiplus::Color& color, int filledPart)
    {
        // A capsule mouse outline; filledPart selects what lights up:
        // 0 = left half, 1 = right half, 2 = center bar (the wheel).
        Gdiplus::REAL radius = r.Width / 2.0f;
        Gdiplus::GraphicsPath body;
        body.AddArc(r.X, r.Y, r.Width, r.Width, 180.0f, 180.0f);
        body.AddArc(r.X, r.Y + r.Height - r.Width, r.Width, r.Width, 0.0f, 180.0f);
        body.CloseFigure();

        Gdiplus::Region clip(&body);
        Gdiplus::GraphicsContainer container = g.BeginContainer();
        g.SetClip(&clip);
        Gdiplus::SolidBrush fill(color);
        const Gdiplus::REAL splitY = r.Y + r.Height * 0.45f;
        switch (filledPart)
        {
        case 0:
            g.FillRectangle(&fill, r.X, r.Y, r.Width / 2.0f, splitY - r.Y);
            break;
        case 1:
            g.FillRectangle(&fill, r.X + r.Width / 2.0f, r.Y, r.Width / 2.0f, splitY - r.Y);
            break;
        case 2:
        default:
            g.FillRectangle(&fill, r.X + r.Width * 0.38f, r.Y + r.Height * 0.08f, r.Width * 0.24f, splitY - r.Y - r.Height * 0.08f);
            break;
        }
        g.EndContainer(container);

        Gdiplus::Pen outline(color, r.Width * 0.11f);
        g.DrawPath(&outline, &body);
        Gdiplus::Pen split(color, r.Width * 0.08f);
        g.DrawLine(&split, r.X, splitY, r.X + r.Width, splitY);
        (void)radius;
    }

    // Draws a toolbar command's glyph centered in the button rect. Shared by the toolbar and the
    // action badge beside the countdown ring, so both always show the same picture for an
    // action. The collapse handle is not drawn here: its glyph depends on the toolbar's edge and
    // state. backingColor is the color under the glyph (Task view fills one window with it).
    void DrawCommandGlyph(Gdiplus::Graphics& g, dwellclick::ToolbarCommand command, const Gdiplus::RectF& button, const Gdiplus::Color& glyphColor, bool paused, const Gdiplus::Color& backingColor)
    {
        using dwellclick::ToolbarCommand;

        // Glyph metrics ride the button size so every overlay size keeps the same look.
        const Gdiplus::REAL inset = button.Width * 0.275f;
        Gdiplus::RectF glyph{ button.X + inset, button.Y + inset, button.Width - inset * 2, button.Height - inset * 2 };
        Gdiplus::Pen pen(glyphColor, button.Width * 0.055f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::SolidBrush brush(glyphColor);
        const Gdiplus::REAL cx = glyph.X + glyph.Width / 2;
        const Gdiplus::REAL cy = glyph.Y + glyph.Height / 2;

        switch (command)
        {
        case ToolbarCommand::TogglePause:
            if (paused)
            {
                // Resume affordance while paused.
                Gdiplus::PointF triangle[3] = {
                    { glyph.X + glyph.Width * 0.22f, glyph.Y },
                    { glyph.X + glyph.Width * 0.95f, cy },
                    { glyph.X + glyph.Width * 0.22f, glyph.Y + glyph.Height },
                };
                g.FillPolygon(&brush, triangle, 3);
            }
            else
            {
                const Gdiplus::REAL barWidth = glyph.Width * 0.26f;
                g.FillRectangle(&brush, glyph.X + glyph.Width * 0.12f, glyph.Y, barWidth, glyph.Height);
                g.FillRectangle(&brush, glyph.X + glyph.Width * 0.62f, glyph.Y, barWidth, glyph.Height);
            }
            break;
        case ToolbarCommand::SelectLeftClick:
            DrawMouseGlyph(g, glyph, glyphColor, 0);
            break;
        case ToolbarCommand::SelectDoubleClick:
        {
            DrawMouseGlyph(g, glyph, glyphColor, 0);
            Gdiplus::Font font(L"Segoe UI", button.Width * 0.2f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::PointF at{ button.X + button.Width * 0.62f, button.Y + button.Height * 0.58f };
            g.DrawString(L"2", 1, &font, at, &brush);
            break;
        }
        case ToolbarCommand::SelectRightClick:
            DrawMouseGlyph(g, glyph, glyphColor, 1);
            break;
        case ToolbarCommand::SelectMiddleClick:
            DrawMouseGlyph(g, glyph, glyphColor, 2);
            break;
        case ToolbarCommand::SelectScrollUp:
        case ToolbarCommand::SelectScrollDown:
        {
            // An arrow over (or under) two content lines. ScrollDown is the mirror.
            const bool up = command == ToolbarCommand::SelectScrollUp;
            const Gdiplus::REAL head = glyph.Width * 0.24f;
            const Gdiplus::REAL tipY = up ? glyph.Y : glyph.Y + glyph.Height;
            const Gdiplus::REAL tailY = up ? glyph.Y + glyph.Height * 0.62f : glyph.Y + glyph.Height * 0.38f;
            const Gdiplus::REAL headY = up ? tipY + head : tipY - head;
            g.DrawLine(&pen, cx, tipY, cx, tailY);
            g.DrawLine(&pen, cx, tipY, cx - head, headY);
            g.DrawLine(&pen, cx, tipY, cx + head, headY);
            const Gdiplus::REAL line1 = up ? glyph.Y + glyph.Height * 0.82f : glyph.Y + glyph.Height * 0.18f;
            const Gdiplus::REAL line2 = up ? glyph.Y + glyph.Height : glyph.Y;
            g.DrawLine(&pen, glyph.X + glyph.Width * 0.12f, line1, glyph.X + glyph.Width * 0.88f, line1);
            g.DrawLine(&pen, glyph.X + glyph.Width * 0.12f, line2, glyph.X + glyph.Width * 0.88f, line2);
            break;
        }
        case ToolbarCommand::SelectScrollLeft:
        case ToolbarCommand::SelectScrollRight:
        {
            // The scroll glyph turned on its side: an arrow beside two content columns.
            const bool left = command == ToolbarCommand::SelectScrollLeft;
            const Gdiplus::REAL head = glyph.Height * 0.24f;
            const Gdiplus::REAL tipX = left ? glyph.X : glyph.X + glyph.Width;
            const Gdiplus::REAL tailX = left ? glyph.X + glyph.Width * 0.62f : glyph.X + glyph.Width * 0.38f;
            const Gdiplus::REAL headX = left ? tipX + head : tipX - head;
            g.DrawLine(&pen, tipX, cy, tailX, cy);
            g.DrawLine(&pen, tipX, cy, headX, cy - head);
            g.DrawLine(&pen, tipX, cy, headX, cy + head);
            const Gdiplus::REAL col1 = left ? glyph.X + glyph.Width * 0.82f : glyph.X + glyph.Width * 0.18f;
            const Gdiplus::REAL col2 = left ? glyph.X + glyph.Width : glyph.X;
            g.DrawLine(&pen, col1, glyph.Y + glyph.Height * 0.12f, col1, glyph.Y + glyph.Height * 0.88f);
            g.DrawLine(&pen, col2, glyph.Y + glyph.Height * 0.12f, col2, glyph.Y + glyph.Height * 0.88f);
            break;
        }
        case ToolbarCommand::SelectZoomIn:
        case ToolbarCommand::SelectZoomOut:
        {
            // A magnifier with a plus or a minus in the lens.
            const Gdiplus::REAL lensR = glyph.Width * 0.34f;
            const Gdiplus::REAL lx = glyph.X + lensR;
            const Gdiplus::REAL ly = glyph.Y + lensR;
            g.DrawEllipse(&pen, lx - lensR, ly - lensR, lensR * 2, lensR * 2);
            const Gdiplus::REAL handleStart = lensR * 0.72f;
            g.DrawLine(&pen, lx + handleStart, ly + handleStart, glyph.X + glyph.Width, glyph.Y + glyph.Height);
            const Gdiplus::REAL mark = lensR * 0.5f;
            g.DrawLine(&pen, lx - mark, ly, lx + mark, ly);
            if (command == ToolbarCommand::SelectZoomIn)
            {
                g.DrawLine(&pen, lx, ly - mark, lx, ly + mark);
            }
            break;
        }
        case ToolbarCommand::Back:
        {
            // An arrow pointing left.
            const Gdiplus::REAL head = glyph.Width * 0.34f;
            g.DrawLine(&pen, glyph.X, cy, glyph.X + glyph.Width, cy);
            g.DrawLine(&pen, glyph.X, cy, glyph.X + head, cy - head);
            g.DrawLine(&pen, glyph.X, cy, glyph.X + head, cy + head);
            break;
        }
        case ToolbarCommand::StartMenu:
        {
            // Four panes, the Start button's shape.
            const Gdiplus::REAL gap = glyph.Width * 0.08f;
            const Gdiplus::REAL pane = (glyph.Width - gap) / 2;
            g.FillRectangle(&brush, glyph.X, glyph.Y, pane, pane);
            g.FillRectangle(&brush, glyph.X + pane + gap, glyph.Y, pane, pane);
            g.FillRectangle(&brush, glyph.X, glyph.Y + pane + gap, pane, pane);
            g.FillRectangle(&brush, glyph.X + pane + gap, glyph.Y + pane + gap, pane, pane);
            break;
        }
        case ToolbarCommand::ShowDesktop:
        {
            // A monitor on a stand: the desktop.
            const Gdiplus::REAL screenH = glyph.Height * 0.66f;
            g.DrawRectangle(&pen, glyph.X, glyph.Y + glyph.Height * 0.04f, glyph.Width, screenH);
            g.DrawLine(&pen, cx, glyph.Y + glyph.Height * 0.04f + screenH, cx, glyph.Y + glyph.Height * 0.9f);
            g.DrawLine(&pen, glyph.X + glyph.Width * 0.25f, glyph.Y + glyph.Height * 0.95f, glyph.X + glyph.Width * 0.75f, glyph.Y + glyph.Height * 0.95f);
            break;
        }
        case ToolbarCommand::TaskView:
        {
            // Two staggered windows: the open apps side by side.
            const Gdiplus::REAL w = glyph.Width * 0.62f;
            const Gdiplus::REAL h = glyph.Height * 0.62f;
            g.DrawRectangle(&pen, glyph.X + glyph.Width - w, glyph.Y, w, h);
            Gdiplus::SolidBrush backing(backingColor);
            g.FillRectangle(&backing, glyph.X, glyph.Y + glyph.Height - h, w, h);
            g.DrawRectangle(&pen, glyph.X, glyph.Y + glyph.Height - h, w, h);
            break;
        }
        case ToolbarCommand::Notifications:
        {
            // A bell: dome, rim, and clapper.
            Gdiplus::GraphicsPath bell;
            const Gdiplus::REAL rimY = glyph.Y + glyph.Height * 0.74f;
            bell.AddArc(glyph.X + glyph.Width * 0.18f, glyph.Y + glyph.Height * 0.04f, glyph.Width * 0.64f, glyph.Height * 0.64f, 180.0f, 180.0f);
            bell.AddLine(glyph.X + glyph.Width * 0.82f, glyph.Y + glyph.Height * 0.36f, glyph.X + glyph.Width * 0.92f, rimY);
            bell.AddLine(glyph.X + glyph.Width * 0.08f, rimY, glyph.X + glyph.Width * 0.18f, glyph.Y + glyph.Height * 0.36f);
            bell.CloseFigure();
            g.DrawPath(&pen, &bell);
            const Gdiplus::REAL clapper = glyph.Width * 0.1f;
            g.FillEllipse(&brush, cx - clapper, glyph.Y + glyph.Height * 0.82f, clapper * 2, clapper * 1.6f);
            break;
        }
        case ToolbarCommand::OpenSettings:
        {
            // A gear: toothed ring around a hub.
            const Gdiplus::REAL outer = glyph.Width * 0.34f;
            const Gdiplus::REAL tooth = glyph.Width * 0.5f;
            Gdiplus::Pen toothPen(glyphColor, glyph.Width * 0.16f);
            for (int t = 0; t < 8; ++t)
            {
                const double angle = t * 3.14159265 / 4.0;
                const auto dx = static_cast<Gdiplus::REAL>(std::cos(angle));
                const auto dy = static_cast<Gdiplus::REAL>(std::sin(angle));
                g.DrawLine(&toothPen, cx + dx * outer, cy + dy * outer, cx + dx * tooth, cy + dy * tooth);
            }
            g.DrawEllipse(&pen, cx - outer, cy - outer, outer * 2, outer * 2);
            Gdiplus::SolidBrush hub(glyphColor);
            const Gdiplus::REAL hubR = glyph.Width * 0.12f;
            g.FillEllipse(&hub, cx - hubR, cy - hubR, hubR * 2, hubR * 2);
            break;
        }
        case ToolbarCommand::SelectDrag:
        default:
        {
            // A four-direction move glyph.
            const Gdiplus::REAL arm = glyph.Width * 0.42f;
            const Gdiplus::REAL head = glyph.Width * 0.16f;
            g.DrawLine(&pen, cx - arm, cy, cx + arm, cy);
            g.DrawLine(&pen, cx, cy - arm, cx, cy + arm);
            const Gdiplus::PointF heads[4][3] = {
                { { cx - arm, cy }, { cx - arm + head, cy - head }, { cx - arm + head, cy + head } },
                { { cx + arm, cy }, { cx + arm - head, cy - head }, { cx + arm - head, cy + head } },
                { { cx, cy - arm }, { cx - head, cy - arm + head }, { cx + head, cy - arm + head } },
                { { cx, cy + arm }, { cx - head, cy + arm - head }, { cx + head, cy + arm - head } },
            };
            for (const auto& tri : heads)
            {
                g.FillPolygon(&brush, tri, 3);
            }
            break;
        }
        }
    }
}

namespace dwellclick
{
    Overlay::~Overlay()
    {
        Destroy();
    }

    bool Overlay::Create(HINSTANCE instance, CommandCallback onCommand)
    {
        m_instance = instance;
        m_onCommand = std::move(onCommand);

        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok)
        {
            Logger::error(L"DwellClick overlay: GDI+ startup failed; running without the overlay.");
            return false;
        }

        WNDCLASSW toolbarClass{};
        toolbarClass.lpfnWndProc = ToolbarProc;
        toolbarClass.hInstance = instance;
        toolbarClass.lpszClassName = TOOLBAR_CLASS;
        toolbarClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&toolbarClass); // Re-registration across enable cycles fails harmlessly.

        WNDCLASSW indicatorClass{};
        indicatorClass.lpfnWndProc = DefWindowProcW;
        indicatorClass.hInstance = instance;
        indicatorClass.lpszClassName = INDICATOR_CLASS;
        RegisterClassW(&indicatorClass);

        // The toolbar takes input but must never activate or appear in Alt-Tab.
        m_toolbar = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            TOOLBAR_CLASS,
            L"",
            WS_POPUP,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            instance,
            nullptr);
        // The indicator is additionally click-through: input always passes to what is under it.
        m_indicator = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            INDICATOR_CLASS,
            L"",
            WS_POPUP,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            instance,
            nullptr);
        if (!m_toolbar || !m_indicator)
        {
            Logger::error(L"DwellClick overlay: window creation failed, error: {}. Running without the overlay.", GetLastError());
            Destroy();
            return false;
        }
        SetWindowLongPtrW(m_toolbar, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        LayoutToolbar();
        if (m_settings.showToolbar)
        {
            ShowWindow(m_toolbar, SW_SHOWNOACTIVATE);
        }
        return true;
    }

    void Overlay::Destroy()
    {
        if (m_toolbar)
        {
            DestroyWindow(m_toolbar);
            m_toolbar = nullptr;
        }
        if (m_indicator)
        {
            DestroyWindow(m_indicator);
            m_indicator = nullptr;
        }
        if (m_gdiplusToken)
        {
            Gdiplus::GdiplusShutdown(m_gdiplusToken);
            m_gdiplusToken = 0;
        }
    }

    double Overlay::Scale() const
    {
        const UINT dpi = m_toolbar ? GetDpiForWindow(m_toolbar) : 96;
        return dpi > 0 ? dpi / 96.0 : 1.0;
    }

    void Overlay::ApplySettings(const OverlaySettings& settings)
    {
        const bool toolbarChanged = settings.showToolbar != m_settings.showToolbar;
        // The first call always rebuilds: the model's constructor default and the settings
        // defaults are maintained separately, and only the settings are authoritative.
        const bool buttonsChanged = !m_buttonsInitialized || !settings.SameButtons(m_settings);
        const bool sizeChanged = !m_buttonsInitialized || ClampSizeIndex(settings.overlaySize) != ClampSizeIndex(m_settings.overlaySize);
        const bool placementChanged = !m_buttonsInitialized || !settings.SamePlacement(m_settings);
        const bool styleChanged = !m_buttonsInitialized || (settings.toolbarStyle == 1) != (m_settings.toolbarStyle == 1);
        const bool countdownChanged = settings.showCountdown != m_settings.showCountdown;
        m_settings = settings;
        m_buttonsInitialized = true;

        if (!m_toolbar)
        {
            return;
        }
        if (buttonsChanged)
        {
            m_model.SetButtons(m_settings.Buttons());
        }
        if (styleChanged)
        {
            // The floating style starts as its single button; the docked bar starts open.
            m_model.SetCollapsed(IsFloating());
        }
        if (buttonsChanged || sizeChanged || placementChanged || styleChanged)
        {
            LayoutToolbar();
        }
        if (toolbarChanged)
        {
            m_model.ResetHover();
            if (m_settings.showToolbar)
            {
                LayoutToolbar();
                ShowWindow(m_toolbar, SW_SHOWNOACTIVATE);
            }
            else
            {
                ShowWindow(m_toolbar, SW_HIDE);
            }
        }
        if (countdownChanged && !m_settings.showCountdown && m_indicator)
        {
            ShowWindow(m_indicator, SW_HIDE);
            m_indicatorVisible = false;
        }
    }

    bool Overlay::IsPointOverToolbar(POINT screenPt) const
    {
        if (!m_toolbar || !m_settings.showToolbar)
        {
            return false;
        }
        return PtInRect(&m_toolbarRect, screenPt) != FALSE;
    }

    bool Overlay::IsFloating() const
    {
        return m_settings.toolbarStyle == 1;
    }

    bool Overlay::IsMenuOpen() const
    {
        return m_toolbar && m_settings.showToolbar && IsFloating() && !m_model.IsCollapsed();
    }

    void Overlay::TickToolbar(POINT screenPt, uint64_t tick, int dwellMs)
    {
        m_lastTick = tick;
        m_lastDwellMs = dwellMs;
        if (!m_toolbar || !m_settings.showToolbar)
        {
            return;
        }

        const double scale = Scale();
        const bool inside = PtInRect(&m_toolbarRect, screenPt) != FALSE;
        const PointL local{
            static_cast<long>((static_cast<double>(screenPt.x) - m_toolbarRect.left) / scale),
            static_cast<long>((static_cast<double>(screenPt.y) - m_toolbarRect.top) / scale),
        };

        const auto command = m_model.OnPointer(local, inside, tick, dwellMs);

        // Repaint when the hover target or its countdown fill moved visibly.
        const int hover = m_model.HoveredIndex();
        const double progress = m_model.HoverProgress(tick, dwellMs);
        if (hover != m_lastPaintedHover || std::abs(progress - m_lastPaintedProgress) > 0.02)
        {
            PaintToolbar(tick, dwellMs);
        }

        if (command)
        {
            DispatchCommand(*command);
        }
    }

    void Overlay::SetToolbarState(const ToolbarVisualState& state)
    {
        if (state.currentAction == m_state.currentAction && state.paused == m_state.paused && state.dragging == m_state.dragging)
        {
            return;
        }
        m_state = state;
        PaintToolbar(m_lastTick, m_lastDwellMs);
    }

    void Overlay::DispatchCommand(ToolbarCommand command)
    {
        if (command == ToolbarCommand::ToggleCollapse)
        {
            m_model.SetCollapsed(!m_model.IsCollapsed());
            LayoutToolbar();
            return;
        }
        if (m_onCommand)
        {
            m_onCommand(command);
        }
        if (IsMenuOpen())
        {
            // The large menu is a chooser: any choice closes it back to its button, the way
            // Ease Mouse's menu does. The menu was over the middle of the screen, so the pointer
            // is not over the button that reappears at the edge and cannot reopen it at once.
            m_model.SetCollapsed(true);
            LayoutToolbar();
            return;
        }
        PaintToolbar(m_lastTick, m_lastDwellMs);
    }

    void Overlay::LayoutToolbar()
    {
        if (!m_toolbar)
        {
            return;
        }
        const double scale = Scale();

        // The primary monitor's work area, so the toolbar never sits under the taskbar.
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        if (IsMenuOpen())
        {
            LayoutMenu(monitor.rcWork, scale);
        }
        else
        {
            LayoutDocked(monitor.rcWork, scale);
        }

        const int width = static_cast<int>(m_model.Width() * scale);
        const int height = static_cast<int>(m_model.Height() * scale);
        SetWindowPos(m_toolbar, HWND_TOPMOST, m_toolbarRect.left, m_toolbarRect.top, width, height, SWP_NOACTIVATE);
        m_toolbarRect.right = m_toolbarRect.left + width;
        m_toolbarRect.bottom = m_toolbarRect.top + height;

        // The toolbar just appeared or moved. A button that landed under a resting pointer
        // (at startup, after a settings change, or when the floating menu opens) must wait for
        // the pointer to leave it, or it would fire after one dwell with nobody aiming at it.
        POINT cursor{};
        if (GetCursorPos(&cursor) && PtInRect(&m_toolbarRect, cursor))
        {
            m_model.BlockAt(PointL{
                static_cast<long>((static_cast<double>(cursor.x) - m_toolbarRect.left) / scale),
                static_cast<long>((static_cast<double>(cursor.y) - m_toolbarRect.top) / scale),
            });
        }

        m_lastPaintedHover = -2; // force the next paint
        PaintToolbar(m_lastTick, m_lastDwellMs);
    }

    void Overlay::LayoutDocked(const RECT& work, double scale)
    {
        // A bar along its edge (or, in the floating style, the single button that opens the
        // menu). The side edges stack buttons down; the top and bottom edges run them across.
        const int side = m_settings.toolbarSide < 0 || m_settings.toolbarSide > 3 ? 0 : m_settings.toolbarSide;
        const bool vertical = side <= 1;
        const int margin = static_cast<int>(EDGE_MARGIN * scale);
        const int edgeLength = vertical ? work.bottom - work.top : work.right - work.left;

        m_model.SetButtonSize(BUTTON_SIZES[ClampSizeIndex(m_settings.overlaySize)]);
        m_model.SetLayout(
            vertical ? ToolbarOrientation::Vertical : ToolbarOrientation::Horizontal,
            m_model.ButtonsThatFit(static_cast<int>((static_cast<double>(edgeLength) - 2.0 * margin) / scale)));

        const int width = static_cast<int>(m_model.Width() * scale);
        const int height = static_cast<int>(m_model.Height() * scale);
        const int length = vertical ? height : width;

        // Center the bar on the chosen point along the edge, then keep it fully on screen.
        const int position = m_settings.toolbarPosition < 0 ? 0 : (m_settings.toolbarPosition > 100 ? 100 : m_settings.toolbarPosition);
        const int start = (vertical ? work.top : work.left) + margin;
        const int end = (vertical ? work.bottom : work.right) - margin - length;
        int along = (vertical ? work.top : work.left) + static_cast<int>(static_cast<long long>(edgeLength) * position / 100) - length / 2;
        along = along > end ? end : along;
        along = along < start ? start : along;

        int x = 0;
        int y = 0;
        switch (side)
        {
        case 1: // right
            x = work.right - width - margin;
            y = along;
            break;
        case 2: // top
            x = along;
            y = work.top + margin;
            break;
        case 3: // bottom
            x = along;
            y = work.bottom - height - margin;
            break;
        case 0: // left
        default:
            x = work.left + margin;
            y = along;
            break;
        }
        m_toolbarRect = RECT{ x, y, x + width, y + height };
    }

    void Overlay::LayoutMenu(const RECT& work, double scale)
    {
        // The floating style's large menu: a near-square grid of big buttons centered on the
        // work area. It must fit in BOTH directions, so a small or high-DPI screen steps the
        // buttons down (large menu size, then the bar's size, then smaller) until it does.
        const int sizeIndex = ClampSizeIndex(m_settings.overlaySize);
        const int margin = static_cast<int>(EDGE_MARGIN * scale);
        const int availableWidth = static_cast<int>((static_cast<double>(work.right) - work.left - 2.0 * margin) / scale);
        const int availableHeight = static_cast<int>((static_cast<double>(work.bottom) - work.top - 2.0 * margin) / scale);
        const ToolbarModel::GridFit fit = ToolbarModel::FitGrid(
            static_cast<int>(m_model.Buttons().size()),
            availableWidth,
            availableHeight,
            { MENU_BUTTON_SIZES[sizeIndex], BUTTON_SIZES[sizeIndex], BUTTON_SIZES[0], 24 });
        m_model.SetButtonSize(fit.buttonSize);
        m_model.SetLayout(ToolbarOrientation::Horizontal, fit.columns);

        const int width = static_cast<int>(m_model.Width() * scale);
        const int height = static_cast<int>(m_model.Height() * scale);
        int x = work.left + ((work.right - work.left) - width) / 2;
        int y = work.top + ((work.bottom - work.top) - height) / 2;
        // Never start above or left of the work area: if the grid still overflows, its first
        // row (Close and Pause, the way out) stays on screen and the overflow goes off the end.
        x = x < work.left + margin ? work.left + margin : x;
        y = y < work.top + margin ? work.top + margin : y;
        m_toolbarRect = RECT{ x, y, x + width, y + height };
    }

    void Overlay::PaintToolbar(uint64_t tick, int dwellMs)
    {
        if (!m_toolbar || !m_settings.showToolbar)
        {
            return;
        }
        const double scale = Scale();
        const int width = static_cast<int>(m_model.Width() * scale);
        const int height = static_cast<int>(m_model.Height() * scale);

        LayeredSurface surface;
        if (!surface.Init(width, height))
        {
            return;
        }
        Gdiplus::Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(surface.bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const auto s = [scale](double v) { return static_cast<Gdiplus::REAL>(v * scale); };

        // Bar background.
        {
            Gdiplus::GraphicsPath path;
            const Gdiplus::REAL r = s(10);
            const Gdiplus::REAL w = static_cast<Gdiplus::REAL>(width) - 1;
            const Gdiplus::REAL h = static_cast<Gdiplus::REAL>(height) - 1;
            path.AddArc(0.0f, 0.0f, r * 2, r * 2, 180.0f, 90.0f);
            path.AddArc(w - r * 2, 0.0f, r * 2, r * 2, 270.0f, 90.0f);
            path.AddArc(w - r * 2, h - r * 2, r * 2, r * 2, 0.0f, 90.0f);
            path.AddArc(0.0f, h - r * 2, r * 2, r * 2, 90.0f, 90.0f);
            path.CloseFigure();
            Gdiplus::SolidBrush bg(BAR_BACKGROUND);
            g.FillPath(&bg, &path);
            Gdiplus::Pen border(BAR_BORDER, 1.0f);
            g.DrawPath(&border, &path);
        }

        const int hover = m_model.HoveredIndex();
        const double hoverProgress = m_model.HoverProgress(tick, dwellMs);

        for (int i = 0; i < m_model.VisibleButtonCount(); ++i)
        {
            const ToolbarRect rect = m_model.ButtonRect(i);
            const Gdiplus::RectF button{ s(rect.x), s(rect.y), s(rect.w), s(rect.h) };

            const ToolbarCommand command = m_model.CommandFor(i);
            const std::optional<DwellAction> action = ActionFor(command);
            const bool isCurrentAction = action && *action == m_state.currentAction;
            const bool isActive = isCurrentAction || (command == ToolbarCommand::TogglePause && m_state.paused);

            Gdiplus::GraphicsPath buttonPath;
            const Gdiplus::REAL br = s(6);
            buttonPath.AddArc(button.X, button.Y, br * 2, br * 2, 180.0f, 90.0f);
            buttonPath.AddArc(button.X + button.Width - br * 2, button.Y, br * 2, br * 2, 270.0f, 90.0f);
            buttonPath.AddArc(button.X + button.Width - br * 2, button.Y + button.Height - br * 2, br * 2, br * 2, 0.0f, 90.0f);
            buttonPath.AddArc(button.X, button.Y + button.Height - br * 2, br * 2, br * 2, 90.0f, 90.0f);
            buttonPath.CloseFigure();

            if (isActive)
            {
                Gdiplus::SolidBrush active(ACCENT);
                g.FillPath(&active, &buttonPath);
            }
            if (i == hover && hoverProgress > 0.0)
            {
                // The countdown fill rises from the bottom of the button.
                Gdiplus::GraphicsContainer container = g.BeginContainer();
                Gdiplus::Region clip(&buttonPath);
                g.SetClip(&clip);
                const Gdiplus::REAL fillHeight = static_cast<Gdiplus::REAL>(button.Height * hoverProgress);
                Gdiplus::SolidBrush charge(ACCENT_CHARGE);
                g.FillRectangle(&charge, button.X, button.Y + button.Height - fillHeight, button.Width, fillHeight);
                g.EndContainer(container);
            }

            const Gdiplus::Color glyphColor = isActive ? GLYPH_ON_ACCENT : GLYPH;
            if (command != ToolbarCommand::ToggleCollapse)
            {
                DrawCommandGlyph(g, command, button, glyphColor, m_state.paused, BAR_BACKGROUND);
                continue;
            }

            // The collapse handle's glyph depends on the toolbar's edge and state.
            const Gdiplus::REAL inset = button.Width * 0.275f;
            Gdiplus::RectF glyph{ button.X + inset, button.Y + inset, button.Width - inset * 2, button.Height - inset * 2 };
            Gdiplus::Pen pen(glyphColor, button.Width * 0.055f);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            Gdiplus::SolidBrush brush(glyphColor);
            const Gdiplus::REAL cx = glyph.X + glyph.Width / 2;
            const Gdiplus::REAL cy = glyph.Y + glyph.Height / 2;

            if (IsFloating())
            {
                if (m_model.IsCollapsed())
                {
                    // The floating button: a 3 x 3 grid of dots, "open the menu".
                    const Gdiplus::REAL dot = glyph.Width * 0.11f;
                    for (int row = 0; row < 3; ++row)
                    {
                        for (int col = 0; col < 3; ++col)
                        {
                            const Gdiplus::REAL dx = glyph.X + glyph.Width * (0.15f + 0.35f * col);
                            const Gdiplus::REAL dy = glyph.Y + glyph.Height * (0.15f + 0.35f * row);
                            g.FillEllipse(&brush, dx - dot, dy - dot, dot * 2, dot * 2);
                        }
                    }
                }
                else
                {
                    // Close the menu.
                    const Gdiplus::REAL k = glyph.Width * 0.32f;
                    g.DrawLine(&pen, cx - k, cy - k, cx + k, cy + k);
                    g.DrawLine(&pen, cx - k, cy + k, cx + k, cy - k);
                }
                continue;
            }

            // Chevron pointing into the docked edge to collapse, away from it to expand.
            // The side edges point it left or right; the top and bottom edges, up or down.
            const int side = m_settings.toolbarSide;
            const bool horizontalEdge = side == 2 || side == 3;
            const Gdiplus::REAL edgeDir = (side == 0 || side == 2) ? -1.0f : 1.0f;
            const Gdiplus::REAL dir = m_model.IsCollapsed() ? -edgeDir : edgeDir;
            const Gdiplus::REAL span = glyph.Width * 0.28f;
            const Gdiplus::REAL reach = glyph.Height * 0.35f;
            Gdiplus::PointF points[3] = {
                { cx - span * dir * 0.5f, cy - reach },
                { cx + span * dir * 0.9f, cy },
                { cx - span * dir * 0.5f, cy + reach },
            };
            if (horizontalEdge)
            {
                for (auto& point : points)
                {
                    // Turn the chevron by a quarter turn about the button center.
                    const Gdiplus::REAL dx = point.X - cx;
                    const Gdiplus::REAL dy = point.Y - cy;
                    point = Gdiplus::PointF{ cx - dy, cy + dx };
                }
            }
            g.DrawLines(&pen, points, 3);
        }

        surface.Commit(m_toolbar, POINT{ m_toolbarRect.left, m_toolbarRect.top });
        m_lastPaintedHover = hover;
        m_lastPaintedProgress = hoverProgress;
    }

    void Overlay::UpdateIndicator(POINT pt, double progress, int dwellMs, bool show)
    {
        if (!m_indicator)
        {
            return;
        }
        const bool ring = show && m_settings.showCountdown && ShowCountdownRing(progress, dwellMs);
        const bool flash = m_lastTick < m_flashUntil;
        if (!ring && !flash)
        {
            if (m_indicatorVisible)
            {
                ShowWindow(m_indicator, SW_HIDE);
                m_indicatorVisible = false;
            }
            return;
        }
        if (ring && !m_indicatorVisible)
        {
            // A countdown is starting: pick up the current Windows accent color, so a change
            // in Windows settings shows from the next countdown without polling DWM per frame.
            DWORD colorization = 0;
            BOOL opaque = FALSE;
            if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque)))
            {
                m_accentColor = RGB((colorization >> 16) & 0xFF, (colorization >> 8) & 0xFF, colorization & 0xFF);
            }
        }
        PaintIndicator(pt, progress, ring);
        if (!m_indicatorVisible)
        {
            ShowWindow(m_indicator, SW_SHOWNOACTIVATE);
            m_indicatorVisible = true;
        }
    }

    bool Overlay::ToolbarShowsState() const
    {
        // The open bar (or the floating style's open menu) highlights the current action and
        // the pause state; a hidden toolbar, a collapsed handle, or the closed floating button
        // shows neither.
        return m_toolbar && m_settings.showToolbar && !m_model.IsCollapsed();
    }

    void Overlay::StartFlash(ToolbarCommand command, bool pausedGlyph, uint64_t tick)
    {
        if (ToolbarShowsState())
        {
            return;
        }
        m_flashCommand = command;
        m_flashPausedGlyph = pausedGlyph;
        m_flashUntil = tick + FLASH_DURATION_MS;
    }

    void Overlay::FlashPauseState(bool paused, uint64_t tick)
    {
        // Show the state now in effect: pause bars once paused, the play triangle once resumed.
        // The pause glyph draws the triangle when told it is paused (on the toolbar it is the
        // resume affordance), so the flag is inverted here.
        StartFlash(ToolbarCommand::TogglePause, !paused, tick);
    }

    void Overlay::FlashAction(DwellAction action, uint64_t tick)
    {
        StartFlash(SelectCommandFor(action), false, tick);
    }

    void Overlay::PaintIndicator(POINT pt, double progress, bool ring)
    {
        const double scale = Scale();
        const int sizeIndex = ClampSizeIndex(m_settings.overlaySize);
        const int ringSize = static_cast<int>(INDICATOR_SIZES[sizeIndex] * scale);

        // The badge (any action but a left click) and the shortcut feedback sit up and to the
        // left of the pointer, away from the arrow cursor, which hangs down and to the right of
        // its hotspot. They need a canvas twice the ring's size, still centered on the pointer.
        const bool flash = !ring;
        const bool badge = ring && m_state.currentAction != DwellAction::LeftClick;
        const int size = (badge || flash) ? ringSize * 2 : ringSize;

        LayeredSurface surface;
        if (!surface.Init(size, size))
        {
            return;
        }
        Gdiplus::Bitmap canvas(size, size, size * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(surface.bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const Gdiplus::REAL center = size / 2.0f;
        if (ring)
        {
            const Gdiplus::REAL radius = static_cast<Gdiplus::REAL>(INDICATOR_RADII[sizeIndex] * scale);
            const Gdiplus::RectF ringRect{ center - radius, center - radius, radius * 2, radius * 2 };

            // A dark halo keeps the ring readable over light content, the white track over dark;
            // stroke widths ride the radius so every overlay size keeps the same weight.
            Gdiplus::Pen halo(RING_HALO, radius * 0.42f);
            g.DrawEllipse(&halo, ringRect);
            Gdiplus::Pen track(RING_TRACK, radius * 0.24f);
            g.DrawEllipse(&track, ringRect);

            const COLORREF ringColor = m_settings.indicatorUseAccentColor ? m_accentColor : m_settings.indicatorColor;
            Gdiplus::Pen arc(Gdiplus::Color(255, GetRValue(ringColor), GetGValue(ringColor), GetBValue(ringColor)), radius * 0.24f);
            arc.SetStartCap(Gdiplus::LineCapRound);
            arc.SetEndCap(Gdiplus::LineCapRound);
            const Gdiplus::REAL sweep = static_cast<Gdiplus::REAL>(360.0 * (progress > 1.0 ? 1.0 : progress));
            g.DrawArc(&arc, ringRect, -90.0f, sweep);
        }

        if (badge || flash)
        {
            // The shortcut feedback is larger than the badge: it is the only thing on screen.
            const Gdiplus::REAL diameter = ringSize * (flash ? 0.9f : 0.7f);
            const Gdiplus::REAL offset = ringSize * (flash ? 0.55f : 0.6f);
            const Gdiplus::RectF disc{ center - offset - diameter / 2, center - offset - diameter / 2, diameter, diameter };
            Gdiplus::SolidBrush backing(BAR_BACKGROUND);
            g.FillEllipse(&backing, disc);
            Gdiplus::Pen border(BAR_BORDER, 1.0f);
            g.DrawEllipse(&border, disc);
            if (flash)
            {
                DrawCommandGlyph(g, m_flashCommand, disc, GLYPH, m_flashPausedGlyph, BAR_BACKGROUND);
            }
            else
            {
                DrawCommandGlyph(g, SelectCommandFor(m_state.currentAction), disc, GLYPH, false, BAR_BACKGROUND);
            }
        }

        POINT position{ pt.x - size / 2, pt.y - size / 2 };
        SetWindowPos(m_indicator, HWND_TOPMOST, position.x, position.y, size, size, SWP_NOACTIVATE | SWP_NOREDRAW);
        surface.Commit(m_indicator, position);
    }

    LRESULT CALLBACK Overlay::ToolbarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self)
        {
            return self->HandleToolbarMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT Overlay::HandleToolbarMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            // Take the click without ever taking focus from the app being dwelled on.
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        {
            // Physical clicks (from a carer, or a user who can sometimes click) activate
            // immediately. Coordinates arrive in client space, which for this borderless
            // popup equals window space.
            const double scale = Scale();
            const PointL local{
                static_cast<long>(GET_X_LPARAM(lParam) / scale),
                static_cast<long>(GET_Y_LPARAM(lParam) / scale),
            };
            if (const auto command = m_model.OnClick(local))
            {
                DispatchCommand(*command);
            }
            return 0;
        }
        case WM_DPICHANGED:
            LayoutToolbar();
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}
