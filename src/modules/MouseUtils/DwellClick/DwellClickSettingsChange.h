// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include <atomic>

#include "DwellClickCore.h"

// How a live settings change reaches the engine, kept Win32-free so the unit tests can drive the
// exact protocol dllmain.cpp uses. set_config runs on the runner thread, but the engine is not
// thread-safe and lives on the hook thread, so the runner only publishes new values (atomics in
// the module) and raises a flag; the hook thread's poll loop consumes the flag and applies the
// change to the engine.
namespace dwellclick
{
    // The cross-thread "settings changed" flag, and the one ordering that makes it safe.
    //
    // A live change must lock the engine before any Poll runs with a new value: shortening the
    // dwell time against an already-running countdown would otherwise fire the moment the new
    // value lands. So the flag is raised BEFORE the new values are published, and the hook thread
    // reads its settings snapshot BEFORE it consumes the flag. If a snapshot holds any new value,
    // that value was written after the first raise, so the consume that follows the snapshot
    // sees the flag and the lock lands before Poll. The flag is raised again AFTER publishing, so
    // a pass that consumed the first raise before the values landed is followed by one that sees
    // the complete set (the default action is applied from it). The extra lock is harmless: the
    // pointer is in the Settings window anyway.
    class SettingsChangeSignal
    {
    public:
        // Runner thread: publish() writes the new values.
        template<typename PublishValues>
        void PublishChange(PublishValues&& publish)
        {
            m_pending.store(true);
            publish();
            m_pending.store(true);
        }

        // Hook thread, once per pass, AFTER reading that pass's settings snapshot.
        bool Consume()
        {
            return m_pending.exchange(false);
        }

    private:
        std::atomic<bool> m_pending{ false };
    };

    // Hook thread: apply a consumed change to the engine, given the snapshot read before the
    // consume. Always locks until the pointer moves. A changed default action takes effect on the
    // next dwell, the way choosing it on the toolbar would, releasing a drag in progress first so
    // the switch never orphans the held button. appliedDefaultAction is the default last handed
    // to the engine: only a real change of the default replaces the next action, so an unrelated
    // change (the dwell time, say) never discards an action the user picked on the toolbar.
    inline void ApplySettingsChange(Engine& engine, const Settings& snapshot, DwellAction& appliedDefaultAction)
    {
        engine.LockUntilMove();
        if (snapshot.defaultAction == appliedDefaultAction)
        {
            return;
        }
        appliedDefaultAction = snapshot.defaultAction;
        if (engine.IsDragging())
        {
            engine.ReleaseDrag();
        }
        engine.SetNextAction(snapshot.defaultAction);
    }
}
