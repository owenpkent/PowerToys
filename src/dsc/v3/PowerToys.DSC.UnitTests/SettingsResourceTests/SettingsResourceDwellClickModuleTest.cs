// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using ManagedCommon;
using Microsoft.PowerToys.Settings.UI.Library;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace PowerToys.DSC.UnitTests.SettingsResourceTests;

[TestClass]
public sealed class SettingsResourceDwellClickModuleTest : SettingsResourceModuleTest<DwellClickSettings>
{
    public SettingsResourceDwellClickModuleTest()
        : base(nameof(ModuleType.DwellClick))
    {
    }

    protected override Action<DwellClickSettings> GetSettingsModifier()
    {
        return s =>
        {
            s.Properties.DwellTimeMs.Value = 800;
            s.Properties.MoveTolerancePixels.Value = 12;
            s.Properties.PostActionTolerancePixels.Value = 20;
            s.Properties.DefaultAction.Value = 1;
            s.Properties.RevertToDefaultAfterAction.Value = !s.Properties.RevertToDefaultAfterAction.Value;
        };
    }
}
