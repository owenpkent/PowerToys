// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.PowerToys.UITest.Next;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace MouseUtils.UITests;

[TestClass]
public class DwellClickSettingsTests : UITestBase
{
    private const string ModuleName = "DwellClick";
    private const string GroupId = "MouseUtils_DwellClickTestId";
    private const string ModuleToggleId = "MouseUtils_DwellClickToggleId";
    private const string OptionsExpanderId = "MouseUtils_DwellClickOptionsId";
    private const string DwellTimeId = "MouseUtils_DwellClickDwellTimeId";
    private const string DefaultActionId = "MouseUtils_DwellClickDefaultActionId";
    private const string RevertToDefaultId = "MouseUtils_DwellClickRevertToDefaultId";
    private const string MoveToleranceId = "MouseUtils_DwellClickMoveToleranceId";
    private const string PostActionToleranceId = "MouseUtils_DwellClickPostActionToleranceId";
    private const string ShowToolbarId = "MouseUtils_DwellClickShowToolbarId";
    private const string ToolbarSideId = "MouseUtils_DwellClickToolbarSideId";
    private const string ShowCountdownId = "MouseUtils_DwellClickShowCountdownId";
    private const string ToolbarButtonMiddleClickId = "MouseUtils_DwellClickToolbarButtonMiddleClickId";
    private const string ToolbarButtonScrollDownId = "MouseUtils_DwellClickToolbarButtonScrollDownId";

    // The behavioral tests observe the Drag action: its first dwell presses and HOLDS the left
    // button (readable in GetAsyncKeyState, like the Mouse Button Lock tests), and its second
    // dwell releases it. A plain click's down+up pair is too transient to observe reliably.
    //
    // A short dwell time keeps those tests fast; the UI slider minimum is 200 ms and the module
    // clamps hand-edited values at 100 ms (see dllmain.cpp).
    private const int FastDwellTimeMs = 300;

    // Slack on top of the dwell time so scheduler jitter never makes a completed dwell look
    // unfired. The poll loop advances every 15 ms, so this is generous.
    private const int DwellSlackMs = 500;

    // Settings applies module enable/disable through the runner asynchronously, so the hook and
    // poll thread can still be absent (or still present) for a moment after the toggle reports
    // its new state.
    private const int ModuleSettleTimeoutMs = 10_000;

    // Stray-dwell safety: these tests can leave the module live with a short dwell time while
    // automation drives the Settings window. Two engine guarantees keep that safe (see
    // DwellClickCore.h): every settings change and every physical click LOCK the machine until
    // the pointer moves beyond the movement tolerance, and pattern-based UIA interactions do
    // not move the pointer. The behavioral gestures below are the only deliberate pointer
    // moves, and they run with Settings minimized so a fired dwell lands on the desktop.
    private static readonly IDisposable ModuleSettings = SettingsConfigHelper.PreserveModuleSettings(ModuleName);

    public DwellClickSettingsTests()
        : base(PowerToysModule.PowerToysSettings, enableModules: new[] { ModuleName })
    {
    }

    [ClassCleanup]
    public static void RestoreModuleSettings() => ModuleSettings.Dispose();

    protected override void PrepareTestState()
    {
        // The behavioral tests hide the toolbar so their desktop gestures have exactly one
        // moving part; the persistence tests keep the shipping defaults.
        var settings = TestContext.TestName switch
        {
            nameof(SectionNavigationAndModuleLifecycleAreAvailable) =>
                CreateSettings(dwellTimeMs: FastDwellTimeMs, moveTolerancePixels: 10, postActionTolerancePixels: 10, defaultAction: 4, revertToDefault: false, showToolbar: false),
            nameof(ADwellCompletesADragAndASecondDwellReleasesIt) =>
                CreateSettings(dwellTimeMs: FastDwellTimeMs, moveTolerancePixels: 10, postActionTolerancePixels: 10, defaultAction: 4, revertToDefault: false, showToolbar: false),
            _ =>
                CreateSettings(dwellTimeMs: 1200, moveTolerancePixels: 10, postActionTolerancePixels: 10, defaultAction: 0, revertToDefault: true, showToolbar: true),
        };
        MouseUtilsTestHelper.ReplaceModuleSettings(ModuleName, settings);
    }

    [TestCleanup]
    public async Task ReleaseDragsAndDisableModule()
    {
        await CaptureFailureArtifactsBeforeCleanupAsync();

        try
        {
            // A failed assertion can leave a drag's left button held. Release it directly; the
            // module ignores button-up events, so a plain injected up cannot confuse the engine.
            if (IsLeftButtonDown())
            {
                MouseHelper.LeftUp();
            }
        }
        catch
        {
            // Best effort; the desktop may already be in a torn-down state if the test failed early.
        }

        try
        {
            if (Session.Has(By.AccessibilityId(ModuleToggleId), 500))
            {
                var toggle = Session.Find<ToggleSwitch>(By.AccessibilityId(ModuleToggleId), 500);
                if (toggle.IsOn)
                {
                    toggle.Toggle(false);
                    toggle.WaitForProperty("ToggleState", "Off", 5_000);
                }
            }
        }
        catch
        {
            // The base cleanup will stop the test-owned Runner if Settings is no longer reachable.
        }
    }

    [TestMethod]
    [TestCategory("MouseUtils")]
    [TestCategory("DwellClick")]
    public void SectionNavigationAndModuleLifecycleAreAvailable()
    {
        MouseUtilsTestHelper.NavigateToMouseUtilities(this);

        var group = Session.Find<Element>(By.AccessibilityId(GroupId), 10_000);
        Assert.IsTrue(group.Displayed, "Dwell Click settings group was not visible.");

        MouseUtilsTestHelper.SetModuleEnabled(this, ModuleToggleId, false);
        var expander = Session.Find<Element>(By.AccessibilityId(OptionsExpanderId), 5_000);
        Assert.IsFalse(expander.IsEnabled, "Timing and actions options should be disabled with the module.");

        // The module runs inside the runner with no worker process, window, or named event to
        // probe, so its disabled/enabled effect is asserted the way it is externally
        // observable: whether resting the pointer actually starts the seeded Drag action.
        AssertDwellPressOnceSettled(expectPress: false);

        MouseUtilsTestHelper.SetModuleEnabled(this, ModuleToggleId, true);
        expander = Session.Find<Element>(By.AccessibilityId(OptionsExpanderId), 5_000);
        Assert.IsTrue(expander.IsEnabled, "Timing and actions options should be enabled with the module.");

        AssertDwellPressOnceSettled(expectPress: true);
    }

    [TestMethod]
    [TestCategory("MouseUtils")]
    [TestCategory("DwellClick")]
    public void ActionAndRevertPersistAcrossRestart()
    {
        OpenOptions();

        AssertCheckBoxState(RevertToDefaultId, expectedChecked: true);

        Session.Find<ComboBox>(By.AccessibilityId(DefaultActionId), 5_000).Select("Right click");
        AssertPersistedInt("default_action", 1);

        SetCheckBox(RevertToDefaultId, check: false);
        AssertPersistedBool("revert_to_default_after_action", false);

        // Overlay options. Everything toolbar-related is set before hiding the toolbar,
        // because hiding it disables those controls (mirroring the IsEnabled bindings).
        Session.Find<ComboBox>(By.AccessibilityId(ToolbarSideId), 5_000).Select("Right edge");
        AssertPersistedInt("toolbar_side", 1);
        SetCheckBox(ToolbarButtonMiddleClickId, check: true);
        AssertPersistedBool("toolbar_button_middle_click", true);
        SetCheckBox(ToolbarButtonScrollDownId, check: false);
        AssertPersistedBool("toolbar_button_scroll_down", false);
        SetCheckBox(ShowCountdownId, check: false);
        AssertPersistedBool("show_countdown", false);
        SetCheckBox(ShowToolbarId, check: false);
        AssertPersistedBool("show_toolbar", false);

        RestartScope();
        OpenOptions();
        AssertCheckBoxState(RevertToDefaultId, expectedChecked: false);
        AssertCheckBoxState(ShowToolbarId, expectedChecked: false);
        AssertCheckBoxState(ShowCountdownId, expectedChecked: false);
        AssertPersistedInt("default_action", 1);
        AssertPersistedBool("revert_to_default_after_action", false);
        AssertPersistedInt("toolbar_side", 1);
        AssertPersistedBool("toolbar_button_middle_click", true);
        AssertPersistedBool("toolbar_button_scroll_down", false);
        AssertPersistedBool("show_toolbar", false);
        AssertPersistedBool("show_countdown", false);
    }

    [TestMethod]
    [TestCategory("MouseUtils")]
    [TestCategory("DwellClick")]
    public void DwellTimeAndTolerancesPersistAtBoundaries()
    {
        OpenOptions();

        Session.Find<Slider>(By.AccessibilityId(DwellTimeId), 5_000).SetValue(200);
        AssertPersistedInt("dwell_time_ms", 200);
        Session.Find<Slider>(By.AccessibilityId(DwellTimeId), 5_000).SetValue(5000);
        AssertPersistedInt("dwell_time_ms", 5000);

        Session.Find<NumberBox>(By.AccessibilityId(MoveToleranceId), 5_000).SetValue(0);
        AssertPersistedInt("move_tolerance_pixels", 0);
        Session.Find<NumberBox>(By.AccessibilityId(MoveToleranceId), 5_000).SetValue(100);
        AssertPersistedInt("move_tolerance_pixels", 100);

        Session.Find<NumberBox>(By.AccessibilityId(PostActionToleranceId), 5_000).SetValue(0);
        AssertPersistedInt("post_action_tolerance_pixels", 0);
        Session.Find<NumberBox>(By.AccessibilityId(PostActionToleranceId), 5_000).SetValue(100);
        AssertPersistedInt("post_action_tolerance_pixels", 100);

        RestartScope();
        OpenOptions();
        Assert.AreEqual(
            5000d,
            Session.Find<Slider>(By.AccessibilityId(DwellTimeId), 5_000).Value,
            0.01,
            "Dwell time slider did not reflect its persisted maximum value after restart.");
        AssertPersistedInt("dwell_time_ms", 5000);
        AssertPersistedInt("move_tolerance_pixels", 100);
        AssertPersistedInt("post_action_tolerance_pixels", 100);
    }

    [TestMethod]
    [TestCategory("MouseUtils")]
    [TestCategory("DwellClick")]
    public void ADwellCompletesADragAndASecondDwellReleasesIt()
    {
        MouseUtilsTestHelper.NavigateToMouseUtilities(this);
        MouseUtilsTestHelper.SetModuleEnabled(this, ModuleToggleId, true);

        // First dwell presses and holds the left button. Waiting for this first press also
        // confirms the hook and poll thread are live.
        AssertDwellPressOnceSettled(expectPress: true);

        // The button is now held mid-drag. Moving past the post-action tolerance and resting
        // again completes the gesture: the second dwell releases the button.
        WindowHelper.MinimizeWindow(new IntPtr(Session.WindowHandle));
        try
        {
            MouseHelper.MoveBy(60, 0, steps: 6);
            Thread.Sleep(FastDwellTimeMs + DwellSlackMs);
            Assert.IsTrue(
                WaitForLeftButtonReleased(2_000),
                "The second dwell did not release the drag's held left button.");
        }
        finally
        {
            Session.EnsureForeground();
        }
    }

    private void OpenOptions()
    {
        MouseUtilsTestHelper.NavigateToMouseUtilities(this);
        MouseUtilsTestHelper.SetModuleEnabled(this, ModuleToggleId, true);

        if (!Session.Has(By.AccessibilityId(RevertToDefaultId), 500))
        {
            Session.EnsureForeground();
            Session.Find<Element>(By.AccessibilityId(OptionsExpanderId), 5_000).MouseClick(msPostAction: 500);
        }

        Assert.IsTrue(Session.Has(By.AccessibilityId(DwellTimeId), 5_000), "Dwell time slider was not available.");
        Assert.IsTrue(Session.Has(By.AccessibilityId(DefaultActionId), 5_000), "Default action combo box was not available.");
        Assert.IsTrue(Session.Has(By.AccessibilityId(RevertToDefaultId), 5_000), "Revert checkbox was not available.");
        Assert.IsTrue(Session.Has(By.AccessibilityId(MoveToleranceId), 5_000), "Movement tolerance control was not available.");
        Assert.IsTrue(Session.Has(By.AccessibilityId(PostActionToleranceId), 5_000), "Post-action tolerance control was not available.");
        Assert.IsTrue(Session.Has(By.AccessibilityId(ShowToolbarId), 5_000), "Show toolbar checkbox was not available.");
        Assert.IsTrue(Session.Has(By.AccessibilityId(ShowCountdownId), 5_000), "Show countdown checkbox was not available.");
    }

    private void SetCheckBox(string accessibilityId, bool check)
    {
        var checkBox = Session.Find<CheckBox>(By.AccessibilityId(accessibilityId), 5_000);
        checkBox.SetCheck(check);
        Assert.AreEqual(check, checkBox.IsChecked, $"{accessibilityId} did not reach the expected checked state.");
    }

    private void AssertCheckBoxState(string accessibilityId, bool expectedChecked)
    {
        var checkBox = Session.Find<CheckBox>(By.AccessibilityId(accessibilityId), 5_000);
        Assert.AreEqual(expectedChecked, checkBox.IsChecked, $"{accessibilityId} did not have the expected persisted state.");
    }

    /// <summary>
    /// Repeats a rest-at-screen-center gesture until whether it pressed the left button matches
    /// <paramref name="expectPress"/>, for up to <see cref="ModuleSettleTimeoutMs"/>. This absorbs
    /// the delay between the Settings toggle and the runner starting or stopping the module.
    /// A press (the seeded Drag action's first half) is released before returning, so no attempt
    /// leaves the button held.
    /// </summary>
    private void AssertDwellPressOnceSettled(bool expectPress)
    {
        var stopwatch = Stopwatch.StartNew();
        var pressed = RestAndObservePress();
        while (pressed != expectPress && stopwatch.ElapsedMilliseconds < ModuleSettleTimeoutMs)
        {
            Thread.Sleep(250);
            pressed = RestAndObservePress();
        }

        Assert.AreEqual(
            expectPress,
            pressed,
            $"Resting the pointer was expected to {(expectPress ? "start" : "not start")} the seeded Drag action.");
    }

    /// <summary>
    /// Moves the pointer to the screen center (arming the engine: deliberate movement is the
    /// only thing that unlocks it), rests past the dwell time, and returns whether the seeded
    /// Drag action pressed the left button. A pressed button is released by completing the
    /// drag's second dwell a short move away, and that release is asserted. Settings is
    /// minimized during the gesture so the drag happens over the desktop.
    /// </summary>
    private bool RestAndObservePress()
    {
        WindowHelper.MinimizeWindow(new IntPtr(Session.WindowHandle));
        try
        {
            var (centerX, centerY) = WindowHelper.GetScreenCenter();

            // Approach in steps so the arming movement cannot be mistaken for jitter, then rest.
            MouseHelper.MoveTo(centerX - 60, centerY);
            MouseHelper.MoveBy(60, 0, steps: 6);
            Thread.Sleep(FastDwellTimeMs + DwellSlackMs);

            var pressed = IsLeftButtonDown();
            if (pressed)
            {
                // Complete the gesture: move past the post-action tolerance and rest again so
                // the second dwell releases the button.
                MouseHelper.MoveBy(60, 0, steps: 6);
                Thread.Sleep(FastDwellTimeMs + DwellSlackMs);
                Assert.IsTrue(
                    WaitForLeftButtonReleased(2_000),
                    "The drag's second dwell did not release the left button.");
            }

            return pressed;
        }
        finally
        {
            Session.EnsureForeground();
        }
    }

    private static bool WaitForLeftButtonReleased(int timeoutMs) =>
        WaitHelper.WaitForStable(
            () => IsLeftButtonDown(),
            down => !down,
            timeoutMS: timeoutMs,
            requiredConsecutiveMatches: 2,
            pollIntervalMS: 20).Succeeded;

    private static bool IsLeftButtonDown() => (GetAsyncKeyState(0x01) & 0x8000) != 0;

    private static void AssertPersistedBool(string propertyName, bool expected)
    {
        var result = WaitHelper.WaitForStable(
            () => ReadPersistedBool(propertyName),
            actual => actual == expected,
            timeoutMS: 10_000,
            requiredConsecutiveMatches: 2,
            pollIntervalMS: 200);
        Assert.IsTrue(
            result.Succeeded,
            $"{propertyName} did not persist as {expected}. Last observed value: {result.LastObservation}.");
    }

    private static void AssertPersistedInt(string propertyName, int expected)
    {
        var result = WaitHelper.WaitForStable(
            () => ReadPersistedInt(propertyName),
            actual => actual == expected,
            timeoutMS: 10_000,
            requiredConsecutiveMatches: 2,
            pollIntervalMS: 200);
        Assert.IsTrue(
            result.Succeeded,
            $"{propertyName} did not persist as {expected}. Last observed value: {result.LastObservation}.");
    }

    private static bool? ReadPersistedBool(string propertyName)
    {
        if (!File.Exists(SettingsPath))
        {
            return null;
        }

        using var document = JsonDocument.Parse(File.ReadAllText(SettingsPath));
        return document.RootElement.GetProperty("properties").GetProperty(propertyName).GetProperty("value").GetBoolean();
    }

    private static int? ReadPersistedInt(string propertyName)
    {
        if (!File.Exists(SettingsPath))
        {
            return null;
        }

        using var document = JsonDocument.Parse(File.ReadAllText(SettingsPath));
        return document.RootElement.GetProperty("properties").GetProperty(propertyName).GetProperty("value").GetInt32();
    }

    private static string SettingsPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "Microsoft",
        "PowerToys",
        ModuleName,
        "settings.json");

    private static string CreateSettings(int dwellTimeMs, int moveTolerancePixels, int postActionTolerancePixels, int defaultAction, bool revertToDefault, bool showToolbar) => $$"""
        {
          "name": "DwellClick",
          "version": "1.0",
          "properties": {
            "dwell_time_ms": { "value": {{dwellTimeMs}} },
            "move_tolerance_pixels": { "value": {{moveTolerancePixels}} },
            "post_action_tolerance_pixels": { "value": {{postActionTolerancePixels}} },
            "default_action": { "value": {{defaultAction}} },
            "revert_to_default_after_action": { "value": {{revertToDefault.ToString().ToLowerInvariant()}} },
            "show_toolbar": { "value": {{showToolbar.ToString().ToLowerInvariant()}} },
            "toolbar_side": { "value": 0 },
            "show_countdown": { "value": true }
          }
        }
        """;

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int virtualKeyCode);
}
