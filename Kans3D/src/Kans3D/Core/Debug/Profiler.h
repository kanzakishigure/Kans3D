#pragma once

#include <condition_variable>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>

#define KANS_PROFILE_SCOPE(name) ZoneScopedN(name)
#define KANS_PROFILE_FUNCTION() ZoneScoped
#define KANS_PROFILE_FRAME() FrameMark
#define KANS_PROFILE_FRAME_NAMED(name) FrameMarkNamed(name)
#define KANS_PROFILE_THREAD(name) ::tracy::SetThreadName(name)
#define KANS_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define KANS_PROFILE_MESSAGE(text) TracyMessageL(text)
#define KANS_PROFILE_LOCKABLE(type, variable, name) TracyLockableN(type, variable, name)
#define KANS_PROFILE_ALLOC(ptr, size, pool) TracyAllocNS(ptr, size, 0, pool)
#define KANS_PROFILE_FREE(ptr, pool) TracyFreeNS(ptr, 0, pool)
#define KANS_PROFILE_CONNECTED() TracyIsConnected
#else
#define KANS_PROFILE_SCOPE(name)
#define KANS_PROFILE_FUNCTION()
#define KANS_PROFILE_FRAME()
#define KANS_PROFILE_FRAME_NAMED(name)
#define KANS_PROFILE_THREAD(name)
#define KANS_PROFILE_PLOT(name, value)
#define KANS_PROFILE_MESSAGE(text)
#define KANS_PROFILE_LOCKABLE(type, variable, name) type variable
#define KANS_PROFILE_ALLOC(ptr, size, pool)
#define KANS_PROFILE_FREE(ptr, pool)
#define KANS_PROFILE_CONNECTED() false
#endif

namespace Kans
{
#ifdef TRACY_ENABLE
    using ProfilerConditionVariable = std::condition_variable_any;
#ifdef KANS_TRACY_MEMORY
    inline thread_local bool ProfilerMemoryPaused = false;

    // Real-time threads cannot enter Tracy's allocation event queue.
    class ProfilerMemoryPause
    {
    public:
        ProfilerMemoryPause() : m_Previous(ProfilerMemoryPaused) { ProfilerMemoryPaused = true; }
        ~ProfilerMemoryPause() { ProfilerMemoryPaused = m_Previous; }
        ProfilerMemoryPause(const ProfilerMemoryPause&)            = delete;
        ProfilerMemoryPause& operator=(const ProfilerMemoryPause&) = delete;

    private:
        bool m_Previous;
    };
#endif
#else
    using ProfilerConditionVariable = std::condition_variable;
#endif
} // namespace Kans
