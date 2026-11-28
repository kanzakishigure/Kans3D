#pragma once
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/RHI/RHI.h"
#include "Kans3D/Renderer/Passes/AntiAliasing.h"

namespace Kans
{
    enum class GraphPassKind
    {
        Graphics,
        Compute,
        Transfer
    };
    using GraphDebugID = uint64_t;
    // Fixed FNV-1a, independent of std::hash, declaration indices and physical alias slots.
    GraphDebugID  GraphNameID(const std::string& name, bool resource = false, uint32_t occurrence = 0);
    inline double RenderClockMilliseconds()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    struct PassStatistics
    {
        GraphDebugID                                     ID = 0;
        std::string                                      Name;
        uint32_t                                         ExecutionIndex = uint32_t(-1);
        bool                                             Culled         = true;
        std::optional<double>                            CpuRecordMilliseconds, GpuMilliseconds;
        std::optional<RenderHardware::CommandStatistics> Draws;
        GraphPassKind                                    Kind = GraphPassKind::Graphics;
    };
    struct GpuPassTiming
    {
        GraphDebugID ID = 0;
        std::string  Name;
        uint32_t     ExecutionIndex = 0;
        // Relative to the first executed pass of SourceFrame, in milliseconds.
        double BeginMilliseconds = 0, EndMilliseconds = 0;
        GraphPassKind Kind = GraphPassKind::Graphics;
    };
    struct CompletedGpuFrame
    {
        uint64_t                   SourceFrame = 0;
        std::vector<GpuPassTiming> Passes;
        double                     PassMilliseconds = 0; // Sum of pass intervals, excludes debug capture work.
        std::optional<double>      FrameMilliseconds;    // From the first pass start to the last timestamp.
    };
    struct CullingStatistics
    {
        uint64_t SectionProxies = 0, InFrustum = 0, Drawn = 0;
#ifndef KS_DIST
        uint64_t BoundsTested = 0, BoundsVisible = 0; // One test per draw proxy, including merged batches.
#endif
    };
    enum class ShadowCacheMissReason : uint8_t
    {
        None,
        FirstFrame,
        StaticSetChanged,
        LightMoved,
        CascadeShifted,
        SettingsChanged,
        Invalidated,
        Count
    };
    struct ShadowPassStatistics
    {
        std::string Name;
        bool        Point   = false;
        uint32_t    Cascade = 0, Face = 0;
        uint64_t    CastersDrawn = 0, CastersCulled = 0;
        uint64_t    CachedCasters = 0, StaticCastersRendered = 0, MovableCastersRendered = 0;
        bool        CacheHit = false, CacheUpdated = false;
#ifndef KS_DIST
        ShadowCacheMissReason CacheMissReason = ShadowCacheMissReason::None;
        uint64_t              Light           = 0; // Scene light ID, when available.
#endif
    };
    struct StaticBatchStatistics
    {
        size_t   Batches = 0, Sections = 0, Rebuilt = 0;
        uint64_t BufferBytes         = 0;
        double   PrepareMilliseconds = 0;
    };
    struct FrameStatistics
    {
        uint64_t Frame                        = 0;
        glm::uvec2   RenderSize {}, DisplaySize {};
        AntiAliasing EffectiveAA                  = AntiAliasing::None;
        uint32_t     EffectiveSamples             = 1;
        double   WorldSyncExtractMilliseconds = 0, GraphBuildMilliseconds = 0, CompileMilliseconds = 0,
                 ExecuteSubmitMilliseconds = 0, TotalMilliseconds = 0;
        std::vector<PassStatistics>                      Passes; // Includes culled passes.
        std::optional<RenderHardware::CommandStatistics> Draws;
        std::optional<CompletedGpuFrame>  Gpu; // Latest complete frame for THIS ViewTarget; never partial.
        CullingStatistics                 Culling;
        StaticBatchStatistics             StaticBatches;
        uint32_t                          DirectionalLights = 0, PointLights = 0, ShadowedLights = 0;
        std::vector<ShadowPassStatistics> Shadows;
        uint64_t CachedShadowCasters = 0, StaticShadowCastersRendered = 0, MovableShadowCastersRendered = 0;
        uint64_t StaticShadowCacheBytes = 0;
        uint32_t ShadowCacheHits = 0, ShadowCacheMisses = 0;
#ifndef KS_DIST
        RenderHardware::DrawCategoryTotals                         DrawCategories {};
        RenderHardware::DeviceFrameCounters                        DeviceWork;
        RenderHardware::DeviceMemoryProfile                        Memory;
        std::array<uint32_t, size_t(ShadowCacheMissReason::Count)> ShadowMissReasons {};
        uint32_t ShadowFullRedraws = 0, SpotLights = 0, AreaLights = 0;
        uint64_t ApplicationFrame = 0, ShaderCompiles = 0;
        bool     DetailedCounters = false;
#endif
        double                            RecordedBeforeExecuteMilliseconds = 0, PassRecordMilliseconds = 0;
        uint32_t                          BloomMipCount  = 0;
        uint64_t                          ProxiesCreated = 0, ProxiesUpdated = 0, ProxiesRemoved = 0;
        size_t                            TransientTextureSlots = 0;
        std::optional<uint64_t>           DeviceTextureBytes, DeviceBufferBytes;
    };
#ifndef KS_DIST
    void PlotFrameStatistics(const FrameStatistics& statistics);
#endif
    // Per-view query history. Poll never waits; pools are recycled only after every query completes.
    class FrameProfiler
    {
    public:
        RenderHardware::Handle<RenderHardware::TimestampQueryPool>
        Begin(RenderHardware::Device& device, uint64_t frame, const std::vector<PassStatistics>& passes);
        void Poll(uint64_t beforeFrame);
        // The benchmark needs every completed sample, even when multiple frames finish between polls.
        void SetCompletionCallback(std::function<void(const CompletedGpuFrame&)> callback)
        {
            m_OnCompletion = std::move(callback);
        }
        const std::optional<CompletedGpuFrame>& Latest() const { return m_Latest; }
        void                                    AbortFrame(uint64_t frame);

    private:
        struct Pending
        {
            uint64_t                                                   Frame;
            std::vector<PassStatistics>                                Passes;
            RenderHardware::Handle<RenderHardware::TimestampQueryPool> Queries;
        };
        std::function<void(const CompletedGpuFrame&)>                                              m_OnCompletion;
        std::vector<Pending>                                                                       m_Pending;
        std::vector<std::pair<size_t, RenderHardware::Handle<RenderHardware::TimestampQueryPool>>> m_Free;
        std::optional<CompletedGpuFrame>                                                           m_Latest;
    };
} // namespace Kans
