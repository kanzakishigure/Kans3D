#include "kspch.h"
#include "RenderStatistics.h"
#include <map>
#include <set>
namespace Kans
{
    GraphDebugID GraphNameID(const std::string& name, bool resource, uint32_t occurrence)
    {
        uint64_t hash = 14695981039346656037ull;
        auto     add  = [&](uint8_t byte) { hash = (hash ^ byte) * 1099511628211ull; };
        add(resource ? 1 : 0);
        for (auto byte : name)
            add(uint8_t(byte));
        add(0);
        for (unsigned shift = 0; shift < 32; shift += 8)
            add(uint8_t(occurrence >> shift));
        return hash;
    }
    RenderHardware::Handle<RenderHardware::TimestampQueryPool>
    FrameProfiler::Begin(RenderHardware::Device& device, uint64_t frame, const std::vector<PassStatistics>& passes)
    {
        // A pipeline can submit imported prefixes during graph construction. Group query
        // chunks by source frame so the UI never sees an incomplete prefix as a whole frame.
        std::map<uint64_t, std::vector<size_t>> groups;
        for (size_t i = 0; i < m_Pending.size(); ++i)
            if (m_Pending[i].Frame < frame)
                groups[m_Pending[i].Frame].push_back(i);
        for (const auto& group : groups)
        {
            std::vector<std::vector<uint64_t>> chunks;
            for (auto index : group.second)
            {
                auto ticks = m_Pending[index].Queries->TryGetResults();
                if (!ticks)
                    break;
                chunks.push_back(std::move(*ticks));
            }
            if (chunks.size() != group.second.size())
                continue;
            auto&             first  = m_Pending[group.second.front()];
            const auto        bits   = first.Queries->GetValidBits();
            const uint64_t    mask   = bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
            const double      scale  = first.Queries->GetTimestampPeriod() / 1e6;
            const auto        origin = chunks.front().front();
            CompletedGpuFrame completed;
            completed.SourceFrame = group.first;
            for (size_t chunk = 0; chunk < chunks.size(); ++chunk)
            {
                auto&                              pending = m_Pending[group.second[chunk]];
                std::vector<const PassStatistics*> ordered;
                for (auto& pass : pending.Passes)
                    if (!pass.Culled)
                        ordered.push_back(&pass);
                std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) {
                    return a->ExecutionIndex < b->ExecutionIndex;
                });
                const auto& ticks = chunks[chunk];
                for (size_t i = 0; i < ordered.size(); ++i)
                {
                    const auto&  pass  = *ordered[i];
                    const double start = double((ticks[i * 2] - origin) & mask) * scale;
                    const double end   = start + double((ticks[i * 2 + 1] - ticks[i * 2]) & mask) * scale;
                    completed.Passes.push_back({pass.ID, pass.Name, pass.ExecutionIndex, start, end, pass.Kind});
                    completed.PassMilliseconds += end - start;
                }
                m_Free.emplace_back(ticks.size(), std::move(pending.Queries));
            }
            std::sort(completed.Passes.begin(), completed.Passes.end(), [](const auto& a, const auto& b) {
                return a.ExecutionIndex < b.ExecutionIndex;
            });
            if (!m_Latest || completed.SourceFrame > m_Latest->SourceFrame)
                m_Latest = std::move(completed);
        }
        m_Pending.erase(
            std::remove_if(m_Pending.begin(), m_Pending.end(), [](const auto& pending) { return !pending.Queries; }),
            m_Pending.end());
        size_t count = 0;
        for (auto& pass : passes)
            if (!pass.Culled)
                count += 2;
        if (!count)
            return {};
        RenderHardware::Handle<RenderHardware::TimestampQueryPool> pool;
        for (auto it = m_Free.begin(); it != m_Free.end(); ++it)
            if (it->first == count)
            {
                pool = std::move(it->second);
                m_Free.erase(it);
                break;
            }
        // Bound histories when a device is not retiring frames, without introducing a wait.
        std::set<uint64_t> outstanding;
        for (auto& pending : m_Pending)
            outstanding.insert(pending.Frame);
        if (outstanding.size() >= device.GetDesc().FramesInFlight + 2 && !outstanding.count(frame))
            return {};
        if (!pool)
            pool = device.CreateTimestampQueryPool(uint32_t(count));
        if (pool)
            m_Pending.push_back({frame, passes, pool});
        // Discard obsolete sizes instead of retaining query pools for every viewport configuration.
        if (m_Free.size() > device.GetDesc().FramesInFlight + 2)
            m_Free.erase(m_Free.begin());
        return pool;
    }
} // namespace Kans
