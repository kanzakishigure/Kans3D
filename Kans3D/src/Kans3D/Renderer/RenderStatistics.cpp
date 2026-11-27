#include "kspch.h"
#include "RenderStatistics.h"
#include <map>
#include <set>
namespace Kans
{
#ifndef KS_DIST
    void PlotFrameStatistics([[maybe_unused]] const FrameStatistics& stats)
    {
#ifdef TRACY_ENABLE
        if (!KANS_PROFILE_CONNECTED())
            return;
        using RenderHardware::DrawCategory;
        const auto& draws = stats.DrawCategories;
#define KANS_PLOT_CATEGORY(label, category) \
    KANS_PROFILE_PLOT(label " draws", int64_t(draws[size_t(DrawCategory::category)].DrawCalls)); \
    KANS_PROFILE_PLOT(label " instances", int64_t(draws[size_t(DrawCategory::category)].Instances))
        KANS_PLOT_CATEGORY("Opaque", Opaque);
        KANS_PLOT_CATEGORY("Transparent", Transparent);
        KANS_PLOT_CATEGORY("Shadow", Shadow);
        KANS_PLOT_CATEGORY("Post", Post);
        KANS_PLOT_CATEGORY("UI", UI);
#undef KANS_PLOT_CATEGORY
        if (stats.Draws)
            KANS_PROFILE_PLOT("View triangles", int64_t(stats.Draws->Triangles));
        const auto& work = stats.DeviceWork;
        KANS_PROFILE_PLOT("Pipeline binds", int64_t(work.PipelineBinds));
        KANS_PROFILE_PLOT("Descriptor set binds", int64_t(work.DescriptorSetBinds));
        KANS_PROFILE_PLOT("Buffer uploads", int64_t(work.BufferUploads));
        KANS_PROFILE_PLOT("Buffer upload bytes", int64_t(work.BufferUploadBytes));
        KANS_PROFILE_PLOT("Texture uploads", int64_t(work.TextureUploads));
        KANS_PROFILE_PLOT("Texture upload bytes", int64_t(work.TextureUploadBytes));
        KANS_PROFILE_PLOT("Pipeline compiles", int64_t(work.PipelineCompiles));
        KANS_PROFILE_PLOT("Shader stages compiled", int64_t(stats.ShaderCompiles));
        KANS_PROFILE_PLOT("Shadow cache hits", int64_t(stats.ShadowCacheHits));
        KANS_PROFILE_PLOT("Shadow cache misses", int64_t(stats.ShadowCacheMisses));
        KANS_PROFILE_PLOT("Shadow cache full redraws", int64_t(stats.ShadowFullRedraws));
        const auto& reasons = stats.ShadowMissReasons;
        KANS_PROFILE_PLOT("Shadow cache first frame", int64_t(reasons[size_t(ShadowCacheMissReason::FirstFrame)]));
        KANS_PROFILE_PLOT("Shadow cache light moved", int64_t(reasons[size_t(ShadowCacheMissReason::LightMoved)]));
        KANS_PROFILE_PLOT("Shadow cache cascade shifted",
                          int64_t(reasons[size_t(ShadowCacheMissReason::CascadeShifted)]));
        KANS_PROFILE_PLOT("Shadow cache static set changed",
                          int64_t(reasons[size_t(ShadowCacheMissReason::StaticSetChanged)]));
        KANS_PROFILE_PLOT("Shadow cache settings changed",
                          int64_t(reasons[size_t(ShadowCacheMissReason::SettingsChanged)]));
        KANS_PROFILE_PLOT("Shadow cache invalidated", int64_t(reasons[size_t(ShadowCacheMissReason::Invalidated)]));
        KANS_PROFILE_PLOT("Static casters drawn", int64_t(stats.StaticShadowCastersRendered));
        KANS_PROFILE_PLOT("Dynamic casters drawn", int64_t(stats.MovableShadowCastersRendered));
        KANS_PROFILE_PLOT("Camera section proxies tested", int64_t(stats.Culling.SectionProxies));
        KANS_PROFILE_PLOT("Camera section proxies visible", int64_t(stats.Culling.InFrustum));
        KANS_PROFILE_PLOT("Camera bounds tested", int64_t(stats.Culling.BoundsTested));
        KANS_PROFILE_PLOT("Camera bounds visible", int64_t(stats.Culling.BoundsVisible));
        KANS_PROFILE_PLOT("Directional lights", int64_t(stats.DirectionalLights));
        KANS_PROFILE_PLOT("Point lights", int64_t(stats.PointLights));
        KANS_PROFILE_PLOT("Spot lights", int64_t(stats.SpotLights));
        KANS_PROFILE_PLOT("Area lights", int64_t(stats.AreaLights));
        KANS_PROFILE_PLOT("Shadowed lights", int64_t(stats.ShadowedLights));
        if (stats.DeviceTextureBytes)
            KANS_PROFILE_PLOT("VMA texture bytes", int64_t(*stats.DeviceTextureBytes));
        if (stats.DeviceBufferBytes)
            KANS_PROFILE_PLOT("VMA buffer bytes", int64_t(*stats.DeviceBufferBytes));
        KANS_PROFILE_PLOT("VMA render target bytes", int64_t(stats.Memory.RenderTargetBytes));
        // Tracy uses the label address as series identity.
#define KANS_HEAP_LABELS(label) \
    "VMA heap 0 " label, "VMA heap 1 " label, "VMA heap 2 " label, "VMA heap 3 " label, "VMA heap 4 " label, \
        "VMA heap 5 " label, "VMA heap 6 " label, "VMA heap 7 " label, "VMA heap 8 " label, "VMA heap 9 " label, \
        "VMA heap 10 " label, "VMA heap 11 " label, "VMA heap 12 " label, "VMA heap 13 " label, "VMA heap 14 " label, \
        "VMA heap 15 " label
        static constexpr const char* usageLabels[]  = {KANS_HEAP_LABELS("usage bytes")};
        static constexpr const char* budgetLabels[] = {KANS_HEAP_LABELS("budget bytes")};
#undef KANS_HEAP_LABELS
        for (uint32_t i = 0; i < stats.Memory.HeapCount; ++i)
        {
            KANS_PROFILE_PLOT(usageLabels[i], int64_t(stats.Memory.Heaps[i].UsageBytes));
            KANS_PROFILE_PLOT(budgetLabels[i], int64_t(stats.Memory.Heaps[i].BudgetBytes));
        }
        KANS_PROFILE_PLOT("View CPU frame (ms)", stats.TotalMilliseconds);
        if (stats.Gpu)
            KANS_PROFILE_PLOT("View GPU pass sum (ms)", stats.Gpu->PassMilliseconds);
#endif
    }
#endif
    void FrameProfiler::AbortFrame(uint64_t frame)
    {
        // Unwritten queries will never become ready. Submitted commands retain their
        // own pool lease until their fence retires; do not recycle a partial pool.
        m_Pending.erase(std::remove_if(m_Pending.begin(),
                                       m_Pending.end(),
                                       [frame](const auto& pending) { return pending.Frame == frame; }),
                        m_Pending.end());
    }
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
