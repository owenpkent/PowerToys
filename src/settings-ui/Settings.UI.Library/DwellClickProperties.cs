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

        public DwellClickProperties()
        {
            // Defaults mirror the module's dllmain.cpp and the engine's Settings defaults; see
            // doc/devdocs/modules/mouseutils/dwellclick.md for their provenance.
            DwellTimeMs = new IntProperty(1200);
            MoveTolerancePixels = new IntProperty(10);
            PostActionTolerancePixels = new IntProperty(10);
            DefaultAction = new IntProperty(0); // 0 = left click; matches dwellclick::DwellAction
            RevertToDefaultAfterAction = new BoolProperty(true);
        }
    }
}
