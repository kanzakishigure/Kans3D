#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
namespace Kans
{
    // Persistent workers; caller publishes immutable inputs and disjoint output slots.
    // Callback must not throw. No task wrappers or per-dispatch allocations.
    class ParallelFor
    {
    public:
        explicit ParallelFor(unsigned workers);
        ~ParallelFor();
        ParallelFor(const ParallelFor&) = delete;
        void     Run(size_t count, void* context, void (*function)(void*, size_t));
        unsigned Workers() const { return unsigned(m_Threads.size()); }

    private:
        void                     Worker();
        std::vector<std::thread> m_Threads;
        std::mutex               m_Mutex;
        std::condition_variable  m_Start, m_Done;
        std::atomic<size_t>      m_Next {0};
        size_t                   m_Count = 0, m_Generation = 0, m_Remaining = 0;
        bool                     m_Stop    = false;
        void*                    m_Context = nullptr;
        void (*m_Function)(void*, size_t)  = nullptr;
    };
} // namespace Kans
