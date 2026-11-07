#pragma once
#include <atomic>
#include <cstdint>

namespace Kans
{
    // A process-wide edit clock: every undo step the editor records (scene or asset document) takes
    // the next stamp, so steps of different documents can be ordered by when they were made.
    inline uint64_t NextEditStamp()
    {
        static std::atomic<uint64_t> clock {0};
        return ++clock;
    }
} // namespace Kans
