#include "kspch.h"
#include "Kans3D/Core/EventWait.h"
#include <chrono>
#include <thread>
#include "Kans3D/Core/Window.h"

namespace Kans
{
    void EventWaitGate::Reset(uint64_t events)
    {
        m_Seen   = events;
        m_Settle = SettleFrames;
    }

    bool EventWaitGate::BeginEvents(uint64_t events)
    {
        if (events != m_Seen)
            Reset(events);
        if (m_Settle > 0)
        {
            --m_Settle;
            return false;
        }
        return true;
    }

    bool EventWaitGate::Wake(uint64_t events, bool changed)
    {
        if (events != m_Seen)
        {
            Reset(events);
            return true;
        }
        return changed;
    }

    // A window that cannot block on its events sleeps, then polls.
    void Window::WaitEvents(double timeoutSeconds)
    {
        std::this_thread::sleep_for(std::chrono::duration<double>(timeoutSeconds));
        ProcessEvents();
    }
} // namespace Kans
