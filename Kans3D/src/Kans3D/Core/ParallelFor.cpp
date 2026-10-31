#include "kspch.h"
#include "ParallelFor.h"
namespace Kans
{
    ParallelFor::ParallelFor(unsigned workers)
    {
        workers = std::min(workers, std::max(1u, std::thread::hardware_concurrency()));
        try
        {
            for (unsigned i = 0; i < workers; ++i)
                m_Threads.emplace_back([this] { Worker(); });
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Stop = true;
            }
            m_Start.notify_all();
            for (auto& t : m_Threads)
                t.join();
            throw;
        }
    }
    ParallelFor::~ParallelFor()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stop = true;
        }
        m_Start.notify_all();
        for (auto& t : m_Threads)
            t.join();
    }
    void ParallelFor::Run(size_t count, void* context, void (*function)(void*, size_t))
    {
        if (m_Threads.empty())
        {
            for (size_t i = 0; i < count; ++i)
                function(context, i);
            return;
        }
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Count     = count;
        m_Context   = context;
        m_Function  = function;
        m_Next      = 0;
        m_Remaining = m_Threads.size();
        ++m_Generation;
        m_Start.notify_all();
        m_Done.wait(lock, [this] { return m_Remaining == 0; });
    }
    void ParallelFor::Worker()
    {
        size_t                       generation = 0;
        std::unique_lock<std::mutex> lock(m_Mutex);
        for (;;)
        {
            m_Start.wait(lock, [&] { return m_Stop || generation != m_Generation; });
            if (m_Stop)
                return;
            generation = m_Generation;
            lock.unlock();
            for (;;)
            {
                auto i = m_Next.fetch_add(1, std::memory_order_relaxed);
                if (i >= m_Count)
                    break;
                m_Function(m_Context, i);
            }
            lock.lock();
            if (--m_Remaining == 0)
                m_Done.notify_one();
        }
    }
} // namespace Kans
