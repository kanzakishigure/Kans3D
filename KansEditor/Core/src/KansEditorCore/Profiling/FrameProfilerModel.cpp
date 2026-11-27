#include "FrameProfilerModel.h"
#include <algorithm>
#include <cstdio>

namespace Kans::FrameProfiling
{
    const char* StageLabel(CpuStage stage)
    {
        switch (stage)
        {
            case CpuStage::WorldSync:
                return "Sync / extract";
            case CpuStage::GraphBuild:
                return "Graph build";
            case CpuStage::GraphCompile:
                return "Graph compile";
            case CpuStage::ExecuteSubmit:
                return "Execute / submit";
            case CpuStage::RenderOther:
                return "Render (other)";
            case CpuStage::Waits:
                return "Waits";
            case CpuStage::Rest:
                return "Rest of frame";
            case CpuStage::Count:
                break;
        }
        return "";
    }

    // ------------------------------------------------------------------ sample

    double FrameSample::WaitMilliseconds() const
    {
        return FenceWaitMilliseconds() + AcquireWaitMilliseconds() + PresentWaitMilliseconds();
    }

    double FrameSample::Stage(CpuStage stage) const
    {
        double stages = 0;
        for (double value : RenderStages)
            stages += std::max(value, 0.0);
        const double render = std::max(RenderMilliseconds, stages);
        switch (stage)
        {
            case CpuStage::WorldSync:
            case CpuStage::GraphBuild:
            case CpuStage::GraphCompile:
            case CpuStage::ExecuteSubmit:
                return std::max(RenderStages[size_t(stage)], 0.0);
            case CpuStage::RenderOther:
                return render - stages;
            case CpuStage::Waits:
                // Waits outside the render call; without the application frame they are not known.
                return HasApplication ? std::min(WaitMilliseconds(), std::max(ApplicationMilliseconds - render, 0.0)) :
                                        0.0;
            case CpuStage::Rest:
                return HasApplication ? std::max(ApplicationMilliseconds - render - Stage(CpuStage::Waits), 0.0) : 0.0;
            case CpuStage::Count:
                break;
        }
        return 0;
    }

    uint64_t FrameSample::VramUsageBytes() const
    {
        uint64_t total = 0;
        for (uint32_t i = 0; i < Memory.HeapCount && i < Memory.Heaps.size(); ++i)
            if (Memory.Heaps[i].DeviceLocal)
                total += Memory.Heaps[i].UsageBytes;
        return total;
    }

    uint64_t FrameSample::VramBudgetBytes() const
    {
        uint64_t total = 0;
        for (uint32_t i = 0; i < Memory.HeapCount && i < Memory.Heaps.size(); ++i)
            if (Memory.Heaps[i].DeviceLocal)
                total += Memory.Heaps[i].BudgetBytes;
        return total;
    }

    // ------------------------------------------------------------------ history

    FrameHistory::FrameHistory() : m_Samples(Capacity), m_Passes(Capacity * MaxPasses) {}

    std::optional<size_t> FrameHistory::Find(uint64_t frame) const
    {
        // Newest first: late results are for recent frames.
        for (size_t i = m_Count; i-- > 0;)
            if (At(i).Frame == frame)
                return i;
        return std::nullopt;
    }

    const std::string& FrameHistory::PassName(GraphDebugID id) const
    {
        static const std::string unknown = "?";
        const auto               found   = m_Names.find(id);
        return found != m_Names.end() ? found->second : unknown;
    }

    bool FrameHistory::Record(uint64_t frame, const FrameStatistics* statistics)
    {
        if (m_Paused)
            return false;
        size_t   slot;
        uint64_t replacedRender = 0;
        if (m_Count > 0 && At(m_Count - 1).Frame == frame)
        {
            slot           = Slot(m_Count - 1);
            replacedRender = m_Samples[slot].RenderFrame;
        }
        else if (m_Count < Capacity)
            slot = Slot(m_Count++);
        else
        {
            slot    = m_Start;
            m_Start = (m_Start + 1) % Capacity;
        }
        FrameSample& sample = m_Samples[slot];
        sample              = FrameSample {};
        sample.Frame        = frame;
        // The view keeps its last statistics when it does not render: a repeated frame number is no render.
        const bool rendered = statistics && statistics->Frame != 0 &&
                              (statistics->Frame != m_LastRenderFrame || statistics->Frame == replacedRender);
        if (!rendered)
            return true;
        const auto& s = *statistics;
        if (s.Frame < m_LastRenderFrame)
            ++m_Epoch; // A new view target counts from 1 again.
        m_LastRenderFrame         = s.Frame;
        sample.RenderFrame        = s.Frame;
        sample.RenderEpoch        = m_Epoch;
        sample.RenderMilliseconds = s.TotalMilliseconds;
        sample.RenderStages       = {s.WorldSyncExtractMilliseconds,
                                     s.GraphBuildMilliseconds,
                                     s.CompileMilliseconds,
                                     s.ExecuteSubmitMilliseconds};
        if (s.Draws)
        {
            sample.DrawCalls = s.Draws->DrawCalls;
            sample.Triangles = s.Draws->Triangles;
        }
        sample.SectionProxies    = s.Culling.SectionProxies;
        sample.InFrustum         = s.Culling.InFrustum;
        sample.Drawn             = s.Culling.Drawn;
        sample.DirectionalLights = s.DirectionalLights;
        sample.PointLights       = s.PointLights;
        sample.ShadowedLights    = s.ShadowedLights;
        sample.ShadowPasses      = uint32_t(s.Shadows.size());
        sample.ShadowCacheHits   = s.ShadowCacheHits;
        sample.ShadowCacheMisses = s.ShadowCacheMisses;
#ifndef KS_DIST
        sample.Detailed             = s.DetailedCounters;
        sample.RenderDevice         = s.DeviceWork;
        sample.RenderShaderCompiles = s.ShaderCompiles;
        sample.Categories           = s.DrawCategories;
        sample.BoundsTested         = s.Culling.BoundsTested;
        sample.BoundsVisible        = s.Culling.BoundsVisible;
        sample.SpotLights           = s.SpotLights;
        sample.AreaLights           = s.AreaLights;
        sample.ShadowFullRedraws    = s.ShadowFullRedraws;
        sample.ShadowMissReasons    = s.ShadowMissReasons;
        sample.Memory               = s.Memory;
#endif
        // Passes, without their GPU times: those belong to the completed frame, attached below.
        PassSample* passes     = &m_Passes[slot * MaxPasses];
        sample.PassCount       = uint32_t(std::min(s.Passes.size(), MaxPasses));
        sample.PassesTruncated = s.Passes.size() > MaxPasses;
        for (uint32_t i = 0; i < sample.PassCount; ++i)
        {
            const auto& pass    = s.Passes[i];
            PassSample& out     = passes[i];
            out                 = PassSample {};
            out.ID              = pass.ID;
            out.ExecutionIndex  = pass.ExecutionIndex;
            out.Culled          = pass.Culled;
            out.Kind            = pass.Kind;
            out.CpuMilliseconds = pass.CpuRecordMilliseconds ? float(*pass.CpuRecordMilliseconds) : -1.0f;
            if (pass.Draws)
            {
                out.DrawCalls = pass.Draws->DrawCalls;
                out.Triangles = pass.Draws->Triangles;
            }
            if (m_Names.find(pass.ID) == m_Names.end())
                m_Names.emplace(pass.ID, pass.Name);
        }
        if (s.Gpu)
            AttachGpu(*s.Gpu);
        return true;
    }

    bool FrameHistory::AttachApplication(const ApplicationFrameProfile& profile)
    {
        if (m_Paused)
            return false;
        const auto index = Find(profile.Frame);
        if (!index)
            return false;
        FrameSample& sample              = m_Samples[Slot(*index)];
        sample.HasApplication            = true;
        sample.ApplicationMilliseconds   = profile.CpuMilliseconds;
        sample.FixedSteps                = profile.FixedSteps;
        sample.ApplicationShaderCompiles = profile.ShaderCompiles;
        sample.ApplicationDevice         = profile.DeviceWork;
        return true;
    }

    bool FrameHistory::AttachGpu(const CompletedGpuFrame& gpu)
    {
        if (m_Paused || gpu.SourceFrame == 0)
            return false;
        for (size_t i = m_Count; i-- > 0;)
        {
            FrameSample& sample = m_Samples[Slot(i)];
            if (sample.RenderFrame == 0)
                continue;
            if (sample.RenderEpoch != m_Epoch)
                return false; // Older frames belong to a previous view target.
            if (sample.RenderFrame < gpu.SourceFrame)
                return false;
            if (sample.RenderFrame != gpu.SourceFrame)
                continue;
            sample.HasGpu          = true;
            sample.GpuMilliseconds = gpu.PassMilliseconds;
            PassSample* passes     = &m_Passes[Slot(i) * MaxPasses];
            for (const auto& timing : gpu.Passes)
                for (uint32_t p = 0; p < sample.PassCount; ++p)
                    if (passes[p].ID == timing.ID)
                    {
                        passes[p].GpuMilliseconds =
                            float(std::max(timing.EndMilliseconds - timing.BeginMilliseconds, 0.0));
                        break;
                    }
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ passes

    void OrderPasses(const PassSample*      passes,
                     size_t                 count,
                     PassSort               sort,
                     bool                   ascending,
                     std::vector<uint32_t>& out,
                     const FrameHistory*    names)
    {
        out.clear();
        for (uint32_t i = 0; i < count; ++i)
            out.push_back(i);
        const auto key = [&](const PassSample& pass) -> double {
            switch (sort)
            {
                case PassSort::Cpu:
                    return pass.CpuMilliseconds;
                case PassSort::Gpu:
                    return pass.GpuMilliseconds;
                case PassSort::Draws:
                    return double(pass.DrawCalls);
                default:
                    return 0.0;
            }
        };
        // std::sort with a total order: stable_sort would allocate a buffer on every call.
        std::sort(out.begin(), out.end(), [&](uint32_t a, uint32_t b) {
            const PassSample& x = passes[a];
            const PassSample& y = passes[b];
            if (x.Culled != y.Culled)
                return !x.Culled;
            if (x.Culled)
                return a < b; // Declaration order.
            if (sort == PassSort::Name && names)
            {
                const int order = names->PassName(x.ID).compare(names->PassName(y.ID));
                if (order != 0)
                    return ascending ? order < 0 : order > 0;
            }
            else if (sort != PassSort::Execution && sort != PassSort::Name)
            {
                const double kx = key(x), ky = key(y);
                if (kx != ky)
                    return ascending ? kx < ky : kx > ky;
            }
            if (x.ExecutionIndex != y.ExecutionIndex)
                return x.ExecutionIndex < y.ExecutionIndex;
            return a < b;
        });
    }

    std::array<int, 3> TopGpuPasses(const PassSample* passes, size_t count)
    {
        std::array<int, 3> top {-1, -1, -1};
        for (size_t i = 0; i < count; ++i)
        {
            if (passes[i].Culled || !passes[i].HasGpu())
                continue;
            int carry = int(i); // Displaced entries move down; equal times keep execution order.
            for (auto& slot : top)
            {
                if (slot < 0)
                {
                    slot = carry;
                    break;
                }
                if (passes[carry].GpuMilliseconds > passes[slot].GpuMilliseconds)
                    std::swap(slot, carry);
            }
        }
        return top;
    }

    // ------------------------------------------------------------------ bottleneck

    Bottleneck Classify(const BottleneckInput& input)
    {
        if (input.Compiles > 0)
            return Bottleneck::CompileStutter;
        if (!(input.FrameMilliseconds > 0.0))
            return Bottleneck::NoData;
        if (!input.GpuMilliseconds)
            return Bottleneck::GpuTimingUnavailable;
        const double frame = input.FrameMilliseconds;
        const double gpu   = *input.GpuMilliseconds;
        const double waits = std::clamp(input.WaitMilliseconds, 0.0, frame);
        const double busy  = frame - waits;
        if (gpu >= GpuBoundShare * frame && gpu >= busy)
            return Bottleneck::GpuBound;
        if (waits >= WaitBoundShare * frame)
            return Bottleneck::PresentLimited;
        return Bottleneck::CpuBound;
    }

    BottleneckInput InputOf(const FrameSample& sample)
    {
        BottleneckInput input;
        input.FrameMilliseconds = sample.CpuMilliseconds();
        if (sample.HasGpu)
            input.GpuMilliseconds = sample.GpuMilliseconds;
        input.WaitMilliseconds = sample.WaitMilliseconds();
        input.Compiles         = sample.Compiles();
        return input;
    }

    const char* BottleneckLabel(Bottleneck bottleneck)
    {
        switch (bottleneck)
        {
            case Bottleneck::NoData:
                return "No data";
            case Bottleneck::GpuTimingUnavailable:
                return "GPU timing unavailable";
            case Bottleneck::CpuBound:
                return "CPU-bound";
            case Bottleneck::GpuBound:
                return "GPU-bound";
            case Bottleneck::PresentLimited:
                return "Present/VSync-limited";
            case Bottleneck::CompileStutter:
                return "Shader compile stutter";
        }
        return "";
    }

    WindowSummary Summarize(const FrameHistory&                           history,
                            size_t                                        newest,
                            size_t                                        count,
                            std::vector<std::pair<GraphDebugID, double>>& scratch)
    {
        WindowSummary summary;
        scratch.clear();
        if (history.Size() == 0 || newest >= history.Size())
            return summary;
        const size_t first = newest + 1 >= count ? newest + 1 - count : 0;
        double       frame = 0, gpu = 0, waits = 0;
        size_t       timed = 0;
        for (size_t i = first; i <= newest; ++i)
        {
            const FrameSample& sample = history.At(i);
            if (!(sample.CpuMilliseconds() > 0.0))
                continue;
            ++summary.Frames;
            if (sample.Compiles() > 0)
            {
                ++summary.CompileFrames;
                continue;
            }
            ++timed;
            frame += sample.CpuMilliseconds();
            waits += sample.WaitMilliseconds();
            if (!sample.HasGpu)
                continue;
            ++summary.GpuFrames;
            gpu += sample.GpuMilliseconds;
            const PassSample* passes = history.Passes(i);
            for (uint32_t p = 0; p < sample.PassCount; ++p)
            {
                if (!passes[p].HasGpu())
                    continue;
                auto found = std::lower_bound(scratch.begin(),
                                              scratch.end(),
                                              passes[p].ID,
                                              [](const auto& entry, GraphDebugID id) { return entry.first < id; });
                if (found == scratch.end() || found->first != passes[p].ID)
                    found = scratch.insert(found, {passes[p].ID, 0.0});
                found->second += passes[p].GpuMilliseconds;
            }
        }
        if (timed)
        {
            summary.Average.FrameMilliseconds = frame / double(timed);
            summary.Average.WaitMilliseconds  = waits / double(timed);
        }
        if (summary.GpuFrames)
        {
            summary.Average.GpuMilliseconds = gpu / double(summary.GpuFrames);
            for (const auto& [id, total] : scratch)
                if (total / double(summary.GpuFrames) > summary.TopPassMilliseconds)
                {
                    summary.TopPass             = id;
                    summary.TopPassMilliseconds = total / double(summary.GpuFrames);
                }
        }
        return summary;
    }

    void
    FormatBottleneck(Bottleneck bottleneck, const char* topPass, double topPassMilliseconds, char* out, size_t size)
    {
        if (bottleneck == Bottleneck::GpuBound && topPass)
            std::snprintf(out, size, "GPU-bound (pass %s, %.2f ms)", topPass, topPassMilliseconds);
        else
            std::snprintf(out, size, "%s", BottleneckLabel(bottleneck));
    }

    const char* MissReasonLabel(ShadowCacheMissReason reason)
    {
        switch (reason)
        {
            case ShadowCacheMissReason::None:
                return "None";
            case ShadowCacheMissReason::FirstFrame:
                return "First frame";
            case ShadowCacheMissReason::StaticSetChanged:
                return "Static set changed";
            case ShadowCacheMissReason::LightMoved:
                return "Light moved";
            case ShadowCacheMissReason::CascadeShifted:
                return "Cascade shifted";
            case ShadowCacheMissReason::SettingsChanged:
                return "Settings changed";
            case ShadowCacheMissReason::Invalidated:
                return "Invalidated";
            case ShadowCacheMissReason::Count:
                break;
        }
        return "";
    }

    const char* CategoryLabel(RenderHardware::DrawCategory category)
    {
        using RenderHardware::DrawCategory;
        switch (category)
        {
            case DrawCategory::Auxiliary:
                return "Auxiliary";
            case DrawCategory::Opaque:
                return "Opaque";
            case DrawCategory::Transparent:
                return "Transparent";
            case DrawCategory::Shadow:
                return "Shadow";
            case DrawCategory::Post:
                return "Post";
            case DrawCategory::UI:
                return "UI";
            case DrawCategory::Count:
                break;
        }
        return "";
    }

    const char* PassKindLabel(GraphPassKind kind)
    {
        switch (kind)
        {
            case GraphPassKind::Graphics:
                return "Graphics";
            case GraphPassKind::Compute:
                return "Compute";
            case GraphPassKind::Transfer:
                return "Transfer";
        }
        return "";
    }
} // namespace Kans::FrameProfiling
