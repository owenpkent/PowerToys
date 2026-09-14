// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Text.Json.Serialization;

namespace Microsoft.PowerToys.Settings.UI.Library
{
    public class DwellClickProperties
    {
        [JsonPropertyName("dwell_time_ms")]
        public IntProperty DwellTimeMs { get; set; }

        [JsonPropertyName("move_tolerance_pixels")]
        public IntProperty MoveTolerancePixels { get; set; }

        [JsonPropertyName("post_action_tolerance_pixels")]
        public IntProperty PostActionTolerancePixels { get; set; }

        [JsonPropertyName("default_action")]
        public IntProperty DefaultAction { get; set; }

        [JsonPropertyName("revert_to_default_after_action")]
        public BoolProperty RevertToDefaultAfterAction { get; set; }

        [JsonPropertyName("show_toolbar")]
        public BoolProperty ShowToolbar { get; set; }

        [JsonPropertyName("toolbar_side")]
        public IntProperty ToolbarSide { get; set; }

        [JsonPropertyName("show_countdown")]
        public BoolProperty ShowCountdown { get; set; }

        [JsonPropertyName("overlay_size")]
        public IntProperty OverlaySize { get; set; }

        [JsonPropertyName("toolbar_button_left_click")]
        public BoolProperty ToolbarButtonLeftClick { get; set; }

        [JsonPropertyName("toolbar_button_double_click")]
        public BoolProperty ToolbarButtonDoubleClick { get; set; }

        [JsonPropertyName("toolbar_button_right_click")]
        public BoolProperty ToolbarButtonRightClick { get; set; }

        [JsonPropertyName("toolbar_button_middle_click")]
        public BoolProperty ToolbarButtonMiddleClick { get; set; }

        [JsonPropertyName("toolbar_button_drag")]
        public BoolProperty ToolbarButtonDrag { get; set; }

        [JsonPropertyName("toolbar_button_scroll_up")]
        public BoolProperty ToolbarButtonScrollUp { get; set; }

        [JsonPropertyName("toolbar_button_scroll_down")]
        public BoolProperty ToolbarButtonScrollDown { get; set; }

        [JsonPropertyName("toolbar_button_open_settings")]
        public BoolProperty ToolbarButtonOpenSettings { get; set; }

        public DwellClickProperties()
        {
            // Defaults mirror the module's dllmain.cpp and the engine's Settings defaults; see
            // doc/devdocs/modules/mouseutils/dwellclick.md for their provenance.
            DwellTimeMs = new IntProperty(1200);
            MoveTolerancePixels = new IntProperty(10);
            PostActionTolerancePixels = new IntProperty(10);
            DefaultAction = new IntProperty(0); // 0 = left click; matches dwellclick::DwellAction
            RevertToDefaultAfterAction = new BoolProperty(true);
            ShowToolbar = new BoolProperty(true);
            ToolbarSide = new IntProperty(0); // 0 = left edge, 1 = right edge
            ShowCountdown = new BoolProperty(true);
            OverlaySize = new IntProperty(1); // 0 = small, 1 = medium, 2 = large

            // The toolbar's collapse handle and pause button are always present; these choose
            // the rest. Middle click is off by default as the least-used action.
            ToolbarButtonLeftClick = new BoolProperty(true);
            ToolbarButtonDoubleClick = new BoolProperty(true);
            ToolbarButtonRightClick = new BoolProperty(true);
            ToolbarButtonMiddleClick = new BoolProperty(false);
            ToolbarButtonDrag = new BoolProperty(true);
            ToolbarButtonScrollUp = new BoolProperty(true);
            ToolbarButtonScrollDown = new BoolProperty(true);
            ToolbarButtonOpenSettings = new BoolProperty(true);
        }
    }
}
