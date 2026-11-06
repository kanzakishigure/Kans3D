#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>
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
    };
    struct CullingStatistics
    {
        uint64_t SectionProxies = 0, InFrustum = 0, Drawn = 0;
    };
    struct ShadowPassStatistics
    {
        std::string Name;
        bool        Point   = false;
        uint32_t    Cascade = 0, Face = 0;
        uint64_t    CastersDrawn = 0, CastersCulled = 0;
    };
    struct FrameStatistics
    {
        uint64_t Frame                        = 0;
        AntiAliasing EffectiveAA                  = AntiAliasing::None;
        uint32_t     EffectiveSamples             = 1;
        double   WorldSyncExtractMilliseconds = 0, GraphBuildMilliseconds = 0, CompileMilliseconds = 0,
                 ExecuteSubmitMilliseconds = 0, TotalMilliseconds = 0;
        std::vector<PassStatistics>                      Passes; // Includes culled passes.
        std::optional<RenderHardware::CommandStatistics> Draws;
        std::optional<CompletedGpuFrame>  Gpu; // Latest complete frame for THIS ViewTarget; never partial.
        CullingStatistics                 Culling;
        uint32_t                          DirectionalLights = 0, PointLights = 0, ShadowedLights = 0;
        std::vector<ShadowPassStatistics> Shadows;
        uint32_t                          BloomMipCount  = 0;
        uint64_t                          ProxiesCreated = 0, ProxiesUpdated = 0, ProxiesRemoved = 0;
        size_t                            TransientTextureSlots = 0;
        std::optional<uint64_t>           DeviceTextureBytes, DeviceBufferBytes;
    };
    // Per-view query history. Poll never waits; pools are recycled only after every query completes.
    class FrameProfiler
    {
    public:
        RenderHardware::Handle<RenderHardware::TimestampQueryPool>
        Begin(RenderHardware::Device& device, uint64_t frame, const std::vector<PassStatistics>& passes);
        const std::optional<CompletedGpuFrame>& Latest() const { return m_Latest; }

    private:
        struct Pending
        {
            uint64_t                                                   Frame;
            std::vector<PassStatistics>                                Passes;
            RenderHardware::Handle<RenderHardware::TimestampQueryPool> Queries;
        };
        std::vector<Pending>                                                                       m_Pending;
        std::vector<std::pair<size_t, RenderHardware::Handle<RenderHardware::TimestampQueryPool>>> m_Free;
        std::optional<CompletedGpuFrame>                                                           m_Latest;
    };
} // namespace Kans
