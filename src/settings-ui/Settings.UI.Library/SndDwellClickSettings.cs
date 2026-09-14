// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Text.Json;
using System.Text.Json.Serialization;

namespace Microsoft.PowerToys.Settings.UI.Library
{
    public class SndDwellClickSettings
    {
        [JsonPropertyName("DwellClick")]
        public DwellClickSettings DwellClick { get; set; }

        public SndDwellClickSettings()
        {
        }

        public SndDwellClickSettings(DwellClickSettings settings)
        {
            DwellClick = settings;
        }

        public string ToJsonString()
        {
            return JsonSerializer.Serialize(this);
        }
    }
}
