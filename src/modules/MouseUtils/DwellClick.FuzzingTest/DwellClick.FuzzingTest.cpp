// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// libFuzzer target for the Dwell Click engine state machine.
//
// Dwell Click is a user-input module, so PowerToys policy requires a fuzz target
// (AGENTS.md: "New modules handling file I/O or user input must implement fuzzing tests").
// The decision logic lives in the Win32-free dwellclick::Engine (DwellClickCore.h), so we can
// drive it deterministically under ASan with no OS state involved: the fuzzer's bytes are
// decoded into a sequence of engine events (pointer moves, polls, action changes, pause and
// lifecycle calls) with adversarial ticks, coordinates, and settings. A counting injector
// stands in for the real SendInput-based IClickInjector.
//
// The goal is to prove the state machine stays memory-safe and crash-free for arbitrary input.
// This target pins the guards the engine documents: the squared-distance comparison done in
// double so extreme coordinates cannot overflow, the dwell-time clamp that keeps progress
// finite, and the elapsed-time guard that keeps a tick older than the anchor from wrapping the
// unsigned subtraction and firing instantly.

#include <cstddef>
#include <cstdint>

#include "DwellClickCore.h"

using namespace dwellclick;

namespace
{
    // Records click-injection calls and can be told to fail, exercising the injection failure
    // branches in the engine (a fired action that locks without consuming, a drag press that
    // must not enter drag state).
    class CountingInjector : public IClickInjector
    {
    public:
        int count = 0;
        bool succeed = true;

        bool Inject(ClickKind, PointL) override
        {
            ++count;
            return succeed;
        }
    };

    // Bounds-checked reader over the fuzzer buffer: reads past the end yield 0 so the harness
    // never reads out of range (ASan would flag it otherwise).
    struct Reader
    {
        const uint8_t* data;
        size_t size;
        size_t pos = 0;

        explicit Reader(const uint8_t* d, size_t n) :
            data(d), size(n) {}

        bool done() const { return pos >= size; }

        uint8_t u8()
        {
            return pos < size ? data[pos++] : static_cast<uint8_t>(0);
        }

        uint32_t u32()
        {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
            {
                v = (v << 8) | u8();
            }
            return v;
        }
    };
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Reader r(data, size);

    CountingInjector injector;
    Engine engine(injector);

    Settings settings; // shipping defaults; mutated below from fuzzer bytes
    uint64_t tick = 0;

    // Seed whether injection "succeeds", to reach both fire branches from the first action.
    injector.succeed = (r.u8() & 1u) != 0u;

    // Bound the number of ops so any single input terminates regardless of size.
    for (int step = 0; step < 4096 && !r.done(); ++step)
    {
        const uint8_t op = r.u8();

        // Advance the monotonic-ish clock by a fuzzer-chosen delta. Covers 0, small, and large
        // jumps; ResetTransient below can also move the anchor past the current tick to
        // exercise the elapsed-time wrap guard.
        tick += r.u32();

        // Periodically mutate the settings snapshot, including out-of-range / negative values
        // that a hand-edited settings.json could carry (the engine must stay defined for them).
        // The raw byte cast for the action deliberately produces values outside the enum's
        // named range; the engine's switch defaults must absorb them.
        if (op & 0x40u)
        {
            const uint8_t flags = r.u8();
            settings.revertToDefaultAfterAction = (flags & 0x01u) != 0u;
            settings.defaultAction = static_cast<DwellAction>(flags >> 1);
            settings.dwellTimeMs = static_cast<int>(r.u32());
            settings.moveTolerancePixels = static_cast<int>(r.u32());
            settings.postActionTolerancePixels = static_cast<int>(r.u32());
        }

        // Flip injection success mid-run so drags can fail at either half of the gesture.
        if (op & 0x20u)
        {
            injector.succeed = !injector.succeed;
        }

        const long x = static_cast<long>(static_cast<int32_t>(r.u32()));
        const long y = static_cast<long>(static_cast<int32_t>(r.u32()));
        const PointL pt{ x, y };

        switch (op & 0x07u)
        {
        case 0:
            engine.OnMove(pt, tick, settings);
            break;
        case 1:
            (void)engine.Poll(tick, settings);
            break;
        case 2:
            // Bits 3-4 pick the action; the settings mutation above covers out-of-range values.
            engine.SetNextAction(static_cast<DwellAction>((op >> 3) & 0x07u));
            break;
        case 3:
            engine.SetPaused((op & 0x08u) != 0u);
            break;
        case 4:
            engine.OnPhysicalClick();
            break;
        case 5:
            engine.LockUntilMove();
            break;
        case 6:
            (void)engine.ReleaseDrag();
            break;
        default:
            engine.ResetTransient(tick);
            break;
        }
    }

    // Always drain any in-flight drag so the release path runs at least once per input.
    (void)engine.ReleaseDrag();
    return 0;
}

#ifndef DISABLE_FOR_FUZZING

// Plain entry point for the non-fuzzing (Debug) configuration: runs one canned input so the
// project is runnable/debuggable without the libFuzzer driver.
int main()
{
    const uint8_t seed[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    };
    return LLVMFuzzerTestOneInput(seed, sizeof(seed));
}

#endif
