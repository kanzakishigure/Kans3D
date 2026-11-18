#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>

namespace Kans::RenderHardware
{
    // Serials describe completed GPU work, never elapsed CPU frames. Entries can
    // arrive out of order when retiring commands releases their captured resources.
    class DeferredDestruction
    {
    public:
        void Enqueue(uint64_t serial, std::function<void()> destroy) { m_Entries.emplace(serial, std::move(destroy)); }
        size_t Retire(uint64_t completed)
        {
            size_t count = 0;
            while (!m_Entries.empty() && m_Entries.begin()->first <= completed)
            {
                auto entry = m_Entries.extract(m_Entries.begin());
                entry.mapped()();
                ++count;
            }
            return count;
        }

    private:
        std::multimap<uint64_t, std::function<void()>> m_Entries;
    };
} // namespace Kans::RenderHardware
