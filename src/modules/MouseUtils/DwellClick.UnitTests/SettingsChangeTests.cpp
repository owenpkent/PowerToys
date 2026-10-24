// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "pch.h"
#include "DwellClickSettingsChange.h"

#include <atomic>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace dwellclick;

namespace
{
    class RecordingInjector : public IClickInjector
    {
    public:
        std::vector<ClickKind> calls;

        bool Inject(ClickKind kind, PointL /*pt*/) override
        {
            calls.push_back(kind);
            return true;
        }
    };

    // The module's published settings: atomics written by set_config on the runner thread and
    // read field by field into a snapshot on the hook thread, as dllmain.cpp does.
    struct PublishedSettings
    {
        std::atomic<int> dwellTimeMs{ 1000 };
        std::atomic<int> defaultAction{ static_cast<int>(DwellAction::LeftClick) };

        Settings Snapshot() const
        {
            Settings s;
            s.dwellTimeMs = dwellTimeMs.load();
            s.defaultAction = static_cast<DwellAction>(defaultAction.load());
            return s;
        }
    };

    // One pass of the hook thread's poll loop, in production order: snapshot, consume, apply,
    // then Poll. Returns the Poll result.
    PollResult PollLoopPass(Engine& engine, SettingsChangeSignal& signal, const PublishedSettings& published, DwellAction& appliedDefault, uint64_t tick, bool* consumed = nullptr)
    {
        const Settings snapshot = published.Snapshot();
        const bool changed = signal.Consume();
        if (consumed)
        {
            *consumed = changed;
        }
        if (changed)
        {
            ApplySettingsChange(engine, snapshot, appliedDefault);
        }
        return engine.Poll(tick, snapshot);
    }
}

namespace DwellClickSettingsChangeTests
{
    TEST_CLASS (SettingsChangeProtocol)
    {
    public:
        TEST_METHOD (NothingToConsumeWithoutAChange)
        {
            SettingsChangeSignal signal;
            Assert::IsFalse(signal.Consume());

            signal.PublishChange([] {});
            Assert::IsTrue(signal.Consume());
            Assert::IsFalse(signal.Consume());
        }

        TEST_METHOD (APassThatSeesANewValueAlwaysSeesTheFlag)
        {
            // The race the ordering exists for: the hook thread runs a pass in the middle of
            // set_config, right after the values land and before the closing raise.
            RecordingInjector injector;
            Engine engine(injector);
            SettingsChangeSignal signal;
            PublishedSettings published;
            DwellAction appliedDefault = DwellAction::LeftClick;

            engine.OnMove(PointL{ 100, 100 }, 0, published.Snapshot());
            Assert::IsFalse(PollLoopPass(engine, signal, published, appliedDefault, 900).fired); // 900 of 1000 ms banked

            bool consumed = false;
            PollResult midPublish{};
            signal.PublishChange([&] {
                published.dwellTimeMs.store(100);
                midPublish = PollLoopPass(engine, signal, published, appliedDefault, 901, &consumed);
            });

            // The pass saw the 100 ms dwell, so it must also have seen the flag, and locked
            // before polling: the banked 900 ms must not fire under the new, shorter threshold.
            Assert::IsTrue(consumed);
            Assert::IsFalse(midPublish.fired);
            Assert::IsFalse(PollLoopPass(engine, signal, published, appliedDefault, 5000).fired);
            Assert::AreEqual(static_cast<size_t>(0), injector.calls.size());
        }

        TEST_METHOD (APassBeforeTheValuesLandIsFollowedByOneWithTheCompleteSet)
        {
            // The hook thread consumes the opening raise before any value is written. The closing
            // raise must give a later pass the complete set, or the new default would be lost.
            RecordingInjector injector;
            Engine engine(injector);
            SettingsChangeSignal signal;
            PublishedSettings published;
            DwellAction appliedDefault = DwellAction::LeftClick;

            bool earlyConsumed = false;
            signal.PublishChange([&] {
                PollLoopPass(engine, signal, published, appliedDefault, 10, &earlyConsumed);
                published.defaultAction.store(static_cast<int>(DwellAction::RightClick));
            });
            Assert::IsTrue(earlyConsumed);
            Assert::IsTrue(engine.NextAction() == DwellAction::LeftClick); // nothing new seen yet

            bool lateConsumed = false;
            PollLoopPass(engine, signal, published, appliedDefault, 20, &lateConsumed);
            Assert::IsTrue(lateConsumed);
            Assert::IsTrue(engine.NextAction() == DwellAction::RightClick);
            Assert::IsTrue(appliedDefault == DwellAction::RightClick);
        }
    };

    TEST_CLASS (ApplySettingsChangeRules)
    {
    public:
        TEST_METHOD (AnyChangeLocksUntilThePointerMoves)
        {
            RecordingInjector injector;
            Engine engine(injector);
            Settings s;
            s.dwellTimeMs = 100;
            DwellAction appliedDefault = DwellAction::LeftClick;

            engine.OnMove(PointL{ 100, 100 }, 0, s);
            ApplySettingsChange(engine, s, appliedDefault);
            Assert::IsFalse(engine.Poll(5000, s).fired);

            engine.OnMove(PointL{ 300, 300 }, 5000, s);
            Assert::IsTrue(engine.Poll(5100, s).fired);
        }

        TEST_METHOD (AChangedDefaultReleasesAHeldDragAndBecomesTheNextAction)
        {
            RecordingInjector injector;
            Engine engine(injector);
            Settings s;
            s.dwellTimeMs = 100;
            DwellAction appliedDefault = DwellAction::LeftClick;

            engine.SetNextAction(DwellAction::Drag);
            engine.OnMove(PointL{ 100, 100 }, 0, s);
            Assert::IsTrue(engine.Poll(100, s).fired); // the drag's press
            Assert::IsTrue(engine.IsDragging());

            s.defaultAction = DwellAction::DoubleClick;
            ApplySettingsChange(engine, s, appliedDefault);

            Assert::IsFalse(engine.IsDragging());
            Assert::IsTrue(injector.calls.back() == ClickKind::LeftUp);
            Assert::IsTrue(engine.NextAction() == DwellAction::DoubleClick);
            Assert::IsTrue(appliedDefault == DwellAction::DoubleClick);
        }

        TEST_METHOD (AnUnrelatedChangeKeepsTheActionPickedOnTheToolbar)
        {
            RecordingInjector injector;
            Engine engine(injector);
            Settings s;
            DwellAction appliedDefault = DwellAction::LeftClick;

            engine.SetNextAction(DwellAction::ScrollDown); // chosen on the toolbar
            s.dwellTimeMs = 800; // the user only moved the dwell-time slider
            ApplySettingsChange(engine, s, appliedDefault);

            Assert::IsTrue(engine.NextAction() == DwellAction::ScrollDown);
            Assert::IsTrue(appliedDefault == DwellAction::LeftClick);
        }
    };
}
