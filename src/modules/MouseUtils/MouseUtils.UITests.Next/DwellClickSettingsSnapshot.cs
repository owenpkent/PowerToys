// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using Microsoft.PowerToys.UITest.Next;

namespace MouseUtils.UITests;

/// <summary>
/// One snapshot of the Dwell Click settings file shared by every Dwell Click test class. Two
/// classes each taking their own snapshot could capture the other's test settings (class cleanup
/// may run at the end of the assembly) and restore those instead of the user's.
/// </summary>
internal static class DwellClickSettingsSnapshot
{
    private static readonly object Gate = new();
    private static IDisposable? snapshot;

    internal static void Take()
    {
        lock (Gate)
        {
            snapshot ??= SettingsConfigHelper.PreserveModuleSettings("DwellClick");
        }
    }

    internal static void Restore()
    {
        lock (Gate)
        {
            snapshot?.Dispose();
            snapshot = null;
        }
    }
}
