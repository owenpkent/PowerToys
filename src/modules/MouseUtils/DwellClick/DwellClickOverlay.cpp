// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pch.h"
#include "DwellClickOverlay.h"

#include <windowsx.h>

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

    // The indicator bitmap is square; the ring floats centered inside it with room for the
    // halo stroke.
    constexpr int INDICATOR_SIZE = 48;
    constexpr double INDICATOR_RADIUS = 15.0;

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
        const bool toolbarChanged = settings.showToolbar != m_settings.showToolbar || settings.toolbarSide != m_settings.toolbarSide;
        // The first call always rebuilds: the model's constructor default and the settings
        // defaults are maintained separately, and only the settings are authoritative.
        const bool buttonsChanged = !m_buttonsInitialized || !settings.SameButtons(m_settings);
        const bool countdownChanged = settings.showCountdown != m_settings.showCountdown;
        m_settings = settings;
        m_buttonsInitialized = true;

        if (!m_toolbar)
        {
            return;
        }
        if (buttonsChanged)
        {
            std::vector<ToolbarCommand> buttons{ ToolbarCommand::ToggleCollapse, ToolbarCommand::TogglePause };
            if (m_settings.buttonLeftClick)
            {
                buttons.push_back(ToolbarCommand::SelectLeftClick);
            }
            if (m_settings.buttonDoubleClick)
            {
                buttons.push_back(ToolbarCommand::SelectDoubleClick);
            }
            if (m_settings.buttonRightClick)
            {
                buttons.push_back(ToolbarCommand::SelectRightClick);
            }
            if (m_settings.buttonMiddleClick)
            {
                buttons.push_back(ToolbarCommand::SelectMiddleClick);
            }
            if (m_settings.buttonDrag)
            {
                buttons.push_back(ToolbarCommand::SelectDrag);
            }
            if (m_settings.buttonScrollUp)
            {
                buttons.push_back(ToolbarCommand::SelectScrollUp);
            }
            if (m_settings.buttonScrollDown)
            {
                buttons.push_back(ToolbarCommand::SelectScrollDown);
            }
            if (m_settings.buttonOpenSettings)
            {
                buttons.push_back(ToolbarCommand::OpenSettings);
            }
            m_model.SetButtons(std::move(buttons));
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
        PaintToolbar(m_lastTick, m_lastDwellMs);
    }

    void Overlay::LayoutToolbar()
    {
        if (!m_toolbar)
        {
            return;
        }
        const double scale = Scale();
        const int width = static_cast<int>(m_model.Width() * scale);
        const int height = static_cast<int>(m_model.Height() * scale);

        // Dock to the primary monitor's work area, vertically centered.
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const int margin = static_cast<int>(8 * scale);
        const int x = m_settings.toolbarSide == 0 ? monitor.rcWork.left + margin : monitor.rcWork.right - width - margin;
        const int y = monitor.rcWork.top + ((monitor.rcWork.bottom - monitor.rcWork.top) - height) / 2;

        SetWindowPos(m_toolbar, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
        m_toolbarRect = RECT{ x, y, x + width, y + height };
        m_lastPaintedHover = -2; // force the next paint
        PaintToolbar(m_lastTick, m_lastDwellMs);
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
            const bool isCurrentAction =
                (command == ToolbarCommand::SelectLeftClick && m_state.currentAction == DwellAction::LeftClick) ||
                (command == ToolbarCommand::SelectDoubleClick && m_state.currentAction == DwellAction::DoubleClick) ||
                (command == ToolbarCommand::SelectRightClick && m_state.currentAction == DwellAction::RightClick) ||
                (command == ToolbarCommand::SelectMiddleClick && m_state.currentAction == DwellAction::MiddleClick) ||
                (command == ToolbarCommand::SelectDrag && m_state.currentAction == DwellAction::Drag) ||
                (command == ToolbarCommand::SelectScrollUp && m_state.currentAction == DwellAction::ScrollUp) ||
                (command == ToolbarCommand::SelectScrollDown && m_state.currentAction == DwellAction::ScrollDown);
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
            const Gdiplus::REAL inset = s(11);
            Gdiplus::RectF glyph{ button.X + inset, button.Y + inset, button.Width - inset * 2, button.Height - inset * 2 };
            Gdiplus::Pen pen(glyphColor, s(2.2));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            Gdiplus::SolidBrush brush(glyphColor);
            const Gdiplus::REAL cx = glyph.X + glyph.Width / 2;
            const Gdiplus::REAL cy = glyph.Y + glyph.Height / 2;

            switch (command)
            {
            case ToolbarCommand::ToggleCollapse:
            {
                // Chevron pointing into the docked edge to collapse, away from it to expand.
                const Gdiplus::REAL edgeDir = m_settings.toolbarSide == 0 ? -1.0f : 1.0f;
                const Gdiplus::REAL dir = m_model.IsCollapsed() ? -edgeDir : edgeDir;
                const Gdiplus::REAL span = glyph.Width * 0.28f;
                Gdiplus::PointF points[3] = {
                    { cx - span * dir * 0.5f, cy - glyph.Height * 0.35f },
                    { cx + span * dir * 0.9f, cy },
                    { cx - span * dir * 0.5f, cy + glyph.Height * 0.35f },
                };
                g.DrawLines(&pen, points, 3);
                break;
            }
            case ToolbarCommand::TogglePause:
                if (m_state.paused)
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
                Gdiplus::Font font(L"Segoe UI", s(8), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::PointF at{ button.X + button.Width - s(14), button.Y + button.Height - s(15) };
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
            case ToolbarCommand::OpenSettings:
            {
                // A gear: toothed ring around a hub.
                const Gdiplus::REAL outer = glyph.Width * 0.34f;
                const Gdiplus::REAL tooth = glyph.Width * 0.5f;
                Gdiplus::Pen toothPen(glyphColor, s(3.0));
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

        surface.Commit(m_toolbar, POINT{ m_toolbarRect.left, m_toolbarRect.top });
        m_lastPaintedHover = hover;
        m_lastPaintedProgress = hoverProgress;
    }

    void Overlay::UpdateIndicator(POINT pt, double progress, bool show)
    {
        if (!m_indicator)
        {
            return;
        }
        if (!show || !m_settings.showCountdown || progress <= 0.0)
        {
            if (m_indicatorVisible)
            {
                ShowWindow(m_indicator, SW_HIDE);
                m_indicatorVisible = false;
            }
            return;
        }
        PaintIndicator(pt, progress);
        if (!m_indicatorVisible)
        {
            ShowWindow(m_indicator, SW_SHOWNOACTIVATE);
            m_indicatorVisible = true;
        }
    }

    void Overlay::PaintIndicator(POINT pt, double progress)
    {
        const double scale = Scale();
        const int size = static_cast<int>(INDICATOR_SIZE * scale);

        LayeredSurface surface;
        if (!surface.Init(size, size))
        {
            return;
        }
        Gdiplus::Bitmap canvas(size, size, size * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(surface.bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const Gdiplus::REAL center = size / 2.0f;
        const Gdiplus::REAL radius = static_cast<Gdiplus::REAL>(INDICATOR_RADIUS * scale);
        const Gdiplus::RectF ring{ center - radius, center - radius, radius * 2, radius * 2 };

        // A dark halo keeps the ring readable over light content, the white track over dark.
        Gdiplus::Pen halo(RING_HALO, static_cast<Gdiplus::REAL>(6.0 * scale));
        g.DrawEllipse(&halo, ring);
        Gdiplus::Pen track(RING_TRACK, static_cast<Gdiplus::REAL>(3.5 * scale));
        g.DrawEllipse(&track, ring);

        Gdiplus::Pen arc(ACCENT, static_cast<Gdiplus::REAL>(3.5 * scale));
        arc.SetStartCap(Gdiplus::LineCapRound);
        arc.SetEndCap(Gdiplus::LineCapRound);
        const Gdiplus::REAL sweep = static_cast<Gdiplus::REAL>(360.0 * (progress > 1.0 ? 1.0 : progress));
        g.DrawArc(&arc, ring, -90.0f, sweep);

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
