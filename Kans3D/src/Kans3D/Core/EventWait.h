#pragma once
#include <cstdint>
#include <functional>

namespace Kans
{
    // For an application that only needs a new frame when something changes (the project Hub
    // while a benchmark runs in another process): between frames the main loop sleeps on window
    // events instead of polling them, waking every TimeoutSeconds only to ask Changed.
    struct EventWait
    {
        double                TimeoutSeconds = 0.1;
        std::function<bool()> Changed; // True when the content needs a new frame.
    };

    // When the loop may sleep. Input is followed by SettleFrames more frames (ImGui settles
    // hover, popups and focus a frame after the event); then the loop sleeps until the next
    // event or change. A change draws one frame.
    class EventWaitGate
    {
    public:
        static constexpr uint32_t SettleFrames = 3;
        // `events` counts the events dispatched so far.
        void Reset(uint64_t events);
        // At the frame's event step: true to sleep instead of polling.
        bool BeginEvents(uint64_t events);
        // After a sleep: true to draw a frame.
        bool Wake(uint64_t events, bool changed);

    private:
        uint64_t m_Seen   = 0;
        uint32_t m_Settle = SettleFrames;
    };
} // namespace Kans
