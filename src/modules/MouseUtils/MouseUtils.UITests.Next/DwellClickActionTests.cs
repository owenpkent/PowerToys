// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Drawing;
using System.Runtime.InteropServices;
using Microsoft.PowerToys.UITest.Next;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using static MouseUtils.UITests.DwellClickActionRecorder;

namespace MouseUtils.UITests;

/// <summary>
/// End-to-end tests for every Dwell Click action. Each test picks its action the way a user does,
/// by dwelling on that button of the live toolbar, then rests the pointer on a recorder window and
/// asserts on the messages the window actually received. The keystroke buttons are checked by the
/// exact keys the module injects, and Show desktop also by its effect on the windows.
/// </summary>
[TestClass]
public class DwellClickActionTests : UITestBase
{
    private const string ModuleName = "DwellClick";

    // Short enough to keep the suite quick, long enough that a stepped move never completes a
    // dwell partway through.
    private const int DwellMs = 500;
    private const int SlackMs = 400;

    // Medium overlay size: 56 px buttons, 4 px padding and gap (DwellClickToolbar.h).
    private const int ButtonSize = 56;
    private const int Padding = 4;
    private const int Gap = 4;

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    // Toolbar order with every button switched on (OverlaySettings::Buttons in DwellClickOverlay.h).
    private enum ToolbarButton
    {
        Collapse,
        Pause,
        LeftClick,
        DoubleClick,
        RightClick,
        MiddleClick,
        Drag,
        ScrollUp,
        ScrollDown,
        ScrollLeft,
        ScrollRight,
        ZoomIn,
        ZoomOut,
        Back,
        StartMenu,
        ShowDesktop,
        TaskView,
        Notifications,
        OpenSettings,
    }

    private const int VkTab = 0x09;
    private const int VkLWin = 0x5B;
    private const int VkBrowserBack = 0xA6;
    private const int SwRestore = 9;

    private DwellClickActionRecorder? recorder;

    static DwellClickActionTests() => DwellClickSettingsSnapshot.Take();

    public DwellClickActionTests()
        : base(PowerToysModule.PowerToysSettings, enableModules: new[] { ModuleName })
    {
    }

    [ClassCleanup]
    public static void RestoreModuleSettings() => DwellClickSettingsSnapshot.Restore();

    protected override void PrepareTestState() =>
        MouseUtilsTestHelper.ReplaceModuleSettings(ModuleName, AllButtonsSettings());

    [TestCleanup]
    public async Task CloseRecorder()
    {
        await CaptureFailureArtifactsBeforeCleanupAsync();

        // A failed drag can leave the left button held, and a failed shell test can leave a
        // flyout open. Neither may leak into the next test.
        try
        {
            if ((GetAsyncKeyState(0x01) & 0x8000) != 0)
            {
                MouseHelper.LeftUp();
            }
        }
        catch
        {
        }

        recorder?.Dispose();
        recorder = null;

        // A modifier left down poisons every later test (and the user's typing), so catch it
        // here, release it, and fail the test that left it.
        var stuck = new[] { Key.LCtrl, Key.RCtrl, Key.LShift, Key.Alt, Key.LWin }
            .Where(key => (GetAsyncKeyState((int)key) & 0x8000) != 0)
            .ToList();
        foreach (var key in stuck)
        {
            KeyboardHelper.ReleaseKey(key);
        }

        Assert.AreEqual(0, stuck.Count, $"This test left modifier keys held down: {string.Join(", ", stuck)}.");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void LeftClickLandsOnceAtTheRestPoint()
    {
        var point = StartRecorderAndSelect(ToolbarButton.LeftClick);
        point = RestAt(point);

        WaitFor(() => recorder!.Count(WmLButtonUp) >= 1, "left click");
        Assert.AreEqual(1, recorder!.Count(WmLButtonDown), recorder.Describe());
        Assert.AreEqual(0, recorder.Count(WmLButtonDblClk), recorder.Describe());
        AssertNear(point, recorder.All(WmLButtonDown)[0], "left button down");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void RightClickLandsOnceAtTheRestPoint()
    {
        var point = StartRecorderAndSelect(ToolbarButton.RightClick);
        point = RestAt(point);

        WaitFor(() => recorder!.Count(WmRButtonUp) >= 1, "right click");
        Assert.AreEqual(1, recorder!.Count(WmRButtonDown), recorder.Describe());
        Assert.AreEqual(0, recorder.Count(WmLButtonDown), recorder.Describe());
        AssertNear(point, recorder.All(WmRButtonDown)[0], "right button down");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void DoubleClickArrivesAsADoubleClick()
    {
        var point = StartRecorderAndSelect(ToolbarButton.DoubleClick);
        point = RestAt(point);

        WaitFor(() => recorder!.Count(WmLButtonDblClk) >= 1, "double-click");
        Assert.AreEqual(1, recorder!.Count(WmLButtonDblClk), recorder.Describe());
        AssertNear(point, recorder.All(WmLButtonDblClk)[0], "double-click");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void MiddleClickLandsOnceAtTheRestPoint()
    {
        var point = StartRecorderAndSelect(ToolbarButton.MiddleClick);
        point = RestAt(point);

        WaitFor(() => recorder!.Count(WmMButtonUp) >= 1, "middle click");
        Assert.AreEqual(1, recorder!.Count(WmMButtonDown), recorder.Describe());
        AssertNear(point, recorder.All(WmMButtonDown)[0], "middle button down");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void DragPressesAtOneSpotAndReleasesAtAnother()
    {
        var start = StartRecorderAndSelect(ToolbarButton.Drag);

        // First dwell: the press.
        start = RestAt(start);
        WaitFor(() => recorder!.Count(WmLButtonDown) >= 1, "drag press");
        Assert.AreEqual(0, recorder!.Count(WmLButtonUp), $"The drag released before the second dwell: {recorder.Describe()}");

        // Move with the button held, in steps large enough that no dwell completes on the way.
        MouseHelper.MoveBy(240, 120, steps: 12);
        var (endX, endY) = MouseHelper.GetMousePosition();
        var end = new Point(endX, endY);

        // Second dwell: the release, at the new spot.
        Thread.Sleep(DwellMs + SlackMs);
        WaitFor(() => recorder.Count(WmLButtonUp) >= 1, "drag release");
        Assert.IsTrue(recorder.Count(WmMouseMove) > 0, $"The window saw no movement with the left button held: {recorder.Describe()}");
        AssertNear(start, recorder.All(WmLButtonDown)[0], "drag press");
        AssertNear(end, recorder.All(WmLButtonUp)[0], "drag release");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void DragMovesAWindowByItsTitleBar()
    {
        StartRecorderAndSelect(ToolbarButton.Drag);
        var before = recorder!.Bounds;
        var caption = new Point(before.Left + (before.Width / 2), before.Top + 12);

        // Press on the title bar, move the window, and release. Relative moves are scaled by the
        // pointer speed setting, so the window is compared against how far the pointer went.
        var pressedAt = RestAt(caption);
        Thread.Sleep(SlackMs);
        MouseHelper.MoveBy(200, 100, steps: 10);
        var (releasedX, releasedY) = MouseHelper.GetMousePosition();
        Thread.Sleep(DwellMs + SlackMs + SlackMs);

        var after = recorder.Bounds;
        var pointerDx = releasedX - pressedAt.X;
        var pointerDy = releasedY - pressedAt.Y;
        Assert.IsTrue(pointerDx > 100 && pointerDy > 50, $"The pointer barely moved ({pointerDx}, {pointerDy}); the drag was not exercised.");
        Assert.IsTrue(
            Math.Abs(after.Left - before.Left - pointerDx) <= 6 && Math.Abs(after.Top - before.Top - pointerDy) <= 6,
            $"Dragging the title bar should have moved the window with the pointer by ({pointerDx}, {pointerDy}); it moved by ({after.Left - before.Left}, {after.Top - before.Top}).");
        Assert.IsFalse((GetAsyncKeyState(0x01) & 0x8000) != 0, "The left button was still held after the second dwell.");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void ScrollUpRepeatsWhileThePointerRests() =>
        AssertRepeatingWheel(ToolbarButton.ScrollUp, WmMouseWheel, expectedDelta: 120, expectCtrl: false);

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void ScrollDownRepeatsWhileThePointerRests() =>
        AssertRepeatingWheel(ToolbarButton.ScrollDown, WmMouseWheel, expectedDelta: -120, expectCtrl: false);

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void ScrollLeftRepeatsWhileThePointerRests() =>
        AssertRepeatingWheel(ToolbarButton.ScrollLeft, WmMouseHWheel, expectedDelta: -120, expectCtrl: false);

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void ScrollRightRepeatsWhileThePointerRests() =>
        AssertRepeatingWheel(ToolbarButton.ScrollRight, WmMouseHWheel, expectedDelta: 120, expectCtrl: false);

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void ZoomInIsCtrlWheelUpAndReleasesCtrl() =>
        AssertRepeatingWheel(ToolbarButton.ZoomIn, WmMouseWheel, expectedDelta: 120, expectCtrl: true);

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void ZoomOutIsCtrlWheelDownAndReleasesCtrl() =>
        AssertRepeatingWheel(ToolbarButton.ZoomOut, WmMouseWheel, expectedDelta: -120, expectCtrl: true);

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void BackSendsTheBrowserBackKeyToTheFocusedWindow()
    {
        StartRecorder();
        DwellOnToolbar(ToolbarButton.Back);

        // VK_BROWSER_BACK arrives as a key down, which DefWindowProc also turns into
        // WmAppCommand(APPCOMMAND_BROWSER_BACKWARD).
        AssertKeysSent(VkBrowserBack);
        WaitFor(
            () => recorder!.All(WmKeyDown).Any(m => m.WParam == VkBrowserBack) || recorder.Count(WmAppCommand) > 0,
            "the Back key to reach the focused window");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickShell")]
    public void StartMenuTapsTheWindowsKey()
    {
        StartRecorder();
        DwellOnToolbar(ToolbarButton.StartMenu);
        AssertKeysSent(VkLWin);
        CloseShellSurface();
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickShell")]
    public void TaskViewSendsWinTab()
    {
        StartRecorder();
        DwellOnToolbar(ToolbarButton.TaskView);
        AssertKeysSent(VkLWin, VkTab);
        CloseShellSurface();
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickShell")]
    public void NotificationsSendsWinNOnWindows11AndWinAOnWindows10()
    {
        StartRecorder();
        DwellOnToolbar(ToolbarButton.Notifications);
        AssertKeysSent(VkLWin, Environment.OSVersion.Version.Build >= 22000 ? 'N' : 'A');
        CloseShellSurface();
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickShell")]
    public void ShowDesktopHidesWindowsAndASecondDwellBringsThemBack()
    {
        StartRecorder();
        var handle = recorder!.Handle;

        // Show desktop minimizes every window on the machine; put back anything it leaves
        // minimized (the toggle does not bring back every app, Settings for one).
        var shownBefore = VisibleWindows(minimized: false);
        try
        {
            DwellOnToolbar(ToolbarButton.ShowDesktop);
            AssertKeysSent(VkLWin, 'D');
            WaitFor(() => IsIconic(handle) || IsDesktop(WindowControl.GetForegroundWindowInfo()), "the desktop to show");

            // Show desktop toggles: the same button brings the windows back once the toolbar
            // has seen the pointer leave it. (The recorder is minimized here, so leave toward
            // the middle of the screen.)
            var middle = System.Windows.Forms.Screen.PrimaryScreen!.WorkingArea;
            LeaveToolbar(new Point(middle.Left + (middle.Width / 2), middle.Top + (middle.Height / 2)));
            DwellOnToolbar(ToolbarButton.ShowDesktop);
            WaitFor(() => recorder.InjectedKeys.Count >= 8, $"the second Win+D (keys so far: {recorder.InjectedKeys.Count})");
            WaitFor(() => !IsIconic(handle), "the windows to come back");
        }
        finally
        {
            // The toggle's restore animates; check after it settles, twice, since some apps
            // (Settings among them) minimize again late.
            for (var pass = 0; pass < 2; pass++)
            {
                Thread.Sleep(1_000);
                foreach (var window in shownBefore.Where(IsIconic))
                {
                    ShowWindow(window, SwRestore);
                }
            }
        }
    }

    private static List<IntPtr> VisibleWindows(bool minimized)
    {
        var windows = new List<IntPtr>();
        EnumWindows(
            (hwnd, _) =>
            {
                if (IsWindowVisible(hwnd) && IsIconic(hwnd) == minimized && GetWindowTextLengthW(hwnd) > 0)
                {
                    windows.Add(hwnd);
                }

                return true;
            },
            IntPtr.Zero);
        return windows;
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void PauseStopsClicksUntilPressedAgain()
    {
        var point = StartRecorderAndSelect(ToolbarButton.LeftClick);

        DwellOnToolbar(ToolbarButton.Pause);
        RestAt(point);
        Thread.Sleep(DwellMs);
        Assert.AreEqual(0, recorder!.Count(WmLButtonDown), $"A paused Dwell Click still clicked: {recorder.Describe()}");

        // Leave the pause button so it re-arms, then press it again to resume.
        LeaveToolbar(point);
        DwellOnToolbar(ToolbarButton.Pause);
        RestAt(point);
        WaitFor(() => recorder.Count(WmLButtonUp) >= 1, "a click after resuming");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void CollapseShrinksTheToolbarToItsHandleAndExpandsAgain()
    {
        StartRecorder();
        var toolbar = FindWindowW("DwellClickToolbarWindow", null);
        GetWindowRect(toolbar, out var expanded);

        DwellOnToolbar(ToolbarButton.Collapse);
        GetWindowRect(toolbar, out var collapsed);
        Assert.IsTrue(
            collapsed.Bottom - collapsed.Top < (expanded.Bottom - expanded.Top) / 4,
            $"The toolbar did not collapse: height {expanded.Bottom - expanded.Top} -> {collapsed.Bottom - collapsed.Top}.");

        // Leave and come back to the handle to expand it again.
        LeaveToolbar(new Point(recorder!.ClientBounds.Left + 50, recorder.ClientBounds.Top + 50));
        DwellOnToolbar(ToolbarButton.Collapse);
        GetWindowRect(toolbar, out var reopened);
        Assert.AreEqual(expanded.Bottom - expanded.Top, reopened.Bottom - reopened.Top, "The toolbar did not expand again.");
    }

    [TestMethod]
    [TestCategory("DwellClick")]
    [TestCategory("DwellClickActions")]
    public void OpenSettingsBringsBackPowerToysSettings()
    {
        StartRecorder(); // minimizes Settings
        var settings = new IntPtr(Session.WindowHandle);
        Assert.IsTrue(IsIconic(settings), "Settings should start minimized for this test.");

        DwellOnToolbar(ToolbarButton.OpenSettings);
        WaitFor(() => !IsIconic(settings), "PowerToys Settings to be restored");
    }

    private static bool IsDesktop(WindowControl.ForegroundWindowInfo info) =>
        info.ClassName is "WorkerW" or "Progman";

    private void AssertRepeatingWheel(ToolbarButton button, int message, int expectedDelta, bool expectCtrl)
    {
        var point = StartRecorderAndSelect(button);
        RestAt(point);

        // Scroll and zoom repeat one notch per dwell period while the pointer keeps resting.
        Thread.Sleep((DwellMs * 2) + SlackMs);
        WaitFor(() => recorder!.Count(message) >= 2, $"at least two notches from {button}");

        foreach (var notch in recorder!.All(message))
        {
            var delta = (short)((notch.WParam >> 16) & 0xFFFF);
            var keys = notch.WParam & 0xFFFF;
            Assert.AreEqual(expectedDelta, delta, $"{button} sent the wrong wheel delta: {recorder.Describe()}");
            Assert.AreEqual(expectCtrl, (keys & MkControl) != 0, $"{button} had the wrong Ctrl state on the wheel: {recorder.Describe()}");
        }

        Assert.AreEqual(0, recorder.Count(WmLButtonDown), $"{button} also clicked: {recorder.Describe()}");

        // Stop the repeat before the test ends: PowerToys is torn down after each test, and it
        // must not be killed in the middle of a notch.
        DwellOnToolbar(ToolbarButton.Pause);
        if (expectCtrl)
        {
            // Zoom keeps repeating while the pointer rests, and Ctrl is down for the instant of
            // each notch, so require Ctrl to read up steadily rather than at one sample.
            var released = WaitHelper.WaitForStable(
                () => (GetAsyncKeyState(0x11) & 0x8000) == 0,
                up => up,
                timeoutMS: 3_000,
                requiredConsecutiveMatches: 5,
                pollIntervalMS: 20);
            Assert.IsTrue(released.Succeeded, "Zoom left Ctrl held down.");
        }
    }

    private void StartRecorder()
    {
        // Every test must start with no modifier held, or wheel and key assertions are meaningless.
        var held = new[] { Key.LCtrl, Key.RCtrl, Key.LShift, Key.Alt, Key.LWin }
            .Where(key => (GetAsyncKeyState((int)key) & 0x8000) != 0)
            .ToList();
        foreach (var key in held)
        {
            KeyboardHelper.ReleaseKey(key);
        }

        Assert.AreEqual(0, held.Count, $"Modifier keys were already held when this test started: {string.Join(", ", held)}.");

        WindowHelper.MinimizeWindow(new IntPtr(Session.WindowHandle));

        var screen = System.Windows.Forms.Screen.PrimaryScreen!.WorkingArea;
        var width = Math.Min(900, screen.Width - 400);
        var height = Math.Min(640, screen.Height - 160);
        var bounds = new Rectangle(screen.Left + ((screen.Width - width) / 2), screen.Top + ((screen.Height - height) / 2), width, height);
        recorder = new DwellClickActionRecorder(bounds);

        // Keystroke actions go to the foreground window, so the recorder must hold it.
        var handle = recorder.Handle;
        var focused = WaitHelper.WaitForStable(
            () => WindowControl.TryBringToForeground(handle) && WindowControl.GetForegroundWindowHandle() == handle,
            isForeground => isForeground,
            timeoutMS: 10_000,
            requiredConsecutiveMatches: 2);
        if (!focused.Succeeded)
        {
            // Windows refuses SetForegroundWindow while a shell flyout holds the foreground; a
            // real click on the recorder's title bar always takes it.
            var outer = recorder.Bounds;
            MouseHelper.LeftClickAt(outer.Left + (outer.Width / 2), outer.Top + 12);
            focused = WaitHelper.WaitForStable(
                () => WindowControl.GetForegroundWindowHandle() == handle,
                isForeground => isForeground,
                timeoutMS: 5_000,
                requiredConsecutiveMatches: 2);
        }

        Assert.IsTrue(focused.Succeeded, $"The recorder did not become the foreground window: {WindowControl.GetForegroundWindowInfo()}.");

        // Start with the pointer off the toolbar: a button that appeared under a resting pointer
        // stays blocked until the pointer leaves it, by design.
        var client = recorder.ClientBounds;
        LeaveToolbar(new Point(client.Left + (client.Width / 2), client.Top + (client.Height / 2)));
        recorder.Clear();
    }

    /// <summary>Open the recorder, choose the action on the toolbar, and return a rest point on the recorder.</summary>
    private Point StartRecorderAndSelect(ToolbarButton button)
    {
        StartRecorder();
        DwellOnToolbar(button);
        recorder!.Clear();
        var client = recorder.ClientBounds;
        return new Point(client.Left + (client.Width / 3), client.Top + (client.Height / 3));
    }

    /// <summary>
    /// Hover a toolbar button for the dwell time. The toolbar reads the cursor position itself, so
    /// placing the pointer is enough; the engine stays locked while the pointer is over it.
    /// </summary>
    private static void DwellOnToolbar(ToolbarButton button)
    {
        var center = ToolbarButtonCenter((int)button);
        MouseHelper.MoveTo(center.X, center.Y);
        Thread.Sleep(DwellMs + SlackMs);
    }

    /// <summary>
    /// Approach the point in steps (deliberate movement is what arms the engine), then rest for the
    /// dwell time.
    /// </summary>
    private static Point RestAt(Point point)
    {
        MouseHelper.MoveTo(point.X - 60, point.Y);
        MouseHelper.MoveBy(60, 0, steps: 6);

        // Relative moves are scaled by the pointer speed setting, so report where the pointer
        // actually came to rest: that is where the dwell lands.
        var (x, y) = MouseHelper.GetMousePosition();
        Thread.Sleep(DwellMs + SlackMs);
        return new Point(x, y);
    }

    private static Point ToolbarButtonCenter(int index)
    {
        var toolbar = IntPtr.Zero;
        var found = WaitHelper.WaitForStable(
            () =>
            {
                toolbar = FindWindowW("DwellClickToolbarWindow", null);
                return toolbar != IntPtr.Zero && IsWindowVisible(toolbar);
            },
            visible => visible,
            timeoutMS: 10_000,
            requiredConsecutiveMatches: 2);
        Assert.IsTrue(found.Succeeded, "The Dwell Click toolbar did not appear.");

        GetWindowRect(toolbar, out var rect);
        var scale = GetDpiForWindow(toolbar) / 96.0;
        var step = ButtonSize + Gap;

        // The bar wraps into more columns on a short screen: count the buttons per column from its height.
        var perColumn = (int)Math.Round((((rect.Bottom - rect.Top) / scale) - (2 * Padding) + Gap) / step);
        perColumn = Math.Max(1, perColumn);
        var column = index / perColumn;
        var row = index % perColumn;
        return new Point(
            rect.Left + (int)((Padding + (column * step) + (ButtonSize / 2)) * scale),
            rect.Top + (int)((Padding + (row * step) + (ButtonSize / 2)) * scale));
    }

    /// <summary>
    /// Assert that Dwell Click injected exactly these keys, pressed in order and released in
    /// reverse, as one sequence.
    /// </summary>
    private void AssertKeysSent(params int[] keys)
    {
        var expected = keys.Select(vk => (vk, false)).Concat(keys.Reverse().Select(vk => (vk, true))).ToList();
        WaitFor(() => recorder!.InjectedKeys.Count >= expected.Count, $"{expected.Count} injected key events");
        var actual = recorder!.InjectedKeys;
        Assert.AreEqual(
            string.Join(", ", expected.Select(k => $"{(k.Item2 ? "up" : "down")} 0x{k.vk:X2}")),
            string.Join(", ", actual.Select(k => $"{(k.Up ? "up" : "down")} 0x{k.Vk:X2}")),
            "Dwell Click injected the wrong keys.");
    }

    /// <summary>Close whatever shell surface the keystroke opened, so the next test starts clean.</summary>
    private void CloseShellSurface()
    {
        Thread.Sleep(SlackMs * 2);
        var handle = recorder!.Handle;
        for (var attempt = 0; attempt < 3 && WindowControl.GetForegroundWindowHandle() != handle; attempt++)
        {
            KeyboardHelper.SendKey(Key.Esc);
            Thread.Sleep(SlackMs);
        }
    }

    /// <summary>
    /// Move off the toolbar and stay away for a few of its 15 ms ticks, so it sees the pointer
    /// leave and re-arms the button that last fired.
    /// </summary>
    private static void LeaveToolbar(Point away)
    {
        MouseHelper.MoveTo(away.X, away.Y);
        Thread.Sleep(200);
    }

    private static void AssertNear(Point expected, DwellClickActionRecorder.Message actual, string what)
    {
        Assert.IsTrue(
            Math.Abs(actual.X - expected.X) <= 6 && Math.Abs(actual.Y - expected.Y) <= 6,
            $"The {what} landed at ({actual.X},{actual.Y}), expected near ({expected.X},{expected.Y}).");
    }

    private static void WaitFor(Func<bool> condition, string what)
    {
        var result = WaitHelper.WaitForStable(() => condition(), ok => ok, timeoutMS: 5_000, requiredConsecutiveMatches: 1, pollIntervalMS: 50);
        Assert.IsTrue(result.Succeeded, $"Timed out waiting for {what}.");
    }

    private static string AllButtonsSettings() => $$"""
        {
          "name": "DwellClick",
          "version": "1.0",
          "properties": {
            "dwell_time_ms": { "value": {{DwellMs}} },
            "move_tolerance_pixels": { "value": 10 },
            "post_action_tolerance_pixels": { "value": 10 },
            "default_action": { "value": 0 },
            "revert_to_default_after_action": { "value": false },
            "show_toolbar": { "value": true },
            "toolbar_side": { "value": 0 },
            "toolbar_position": { "value": 50 },
            "toolbar_style": { "value": 0 },
            "show_countdown": { "value": true },
            "overlay_size": { "value": 1 },
            "play_sound": { "value": false },
            "toolbar_button_left_click": { "value": true },
            "toolbar_button_double_click": { "value": true },
            "toolbar_button_right_click": { "value": true },
            "toolbar_button_middle_click": { "value": true },
            "toolbar_button_drag": { "value": true },
            "toolbar_button_scroll_up": { "value": true },
            "toolbar_button_scroll_down": { "value": true },
            "toolbar_button_scroll_left": { "value": true },
            "toolbar_button_scroll_right": { "value": true },
            "toolbar_button_zoom_in": { "value": true },
            "toolbar_button_zoom_out": { "value": true },
            "toolbar_button_back": { "value": true },
            "toolbar_button_start": { "value": true },
            "toolbar_button_show_desktop": { "value": true },
            "toolbar_button_task_view": { "value": true },
            "toolbar_button_notifications": { "value": true },
            "toolbar_button_open_settings": { "value": true }
          }
        }
        """;

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string className, string? windowName);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int virtualKeyCode);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLengthW(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
