#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Kans3D/Core/Debug/ApplicationFrameProfile.h"
#include "Kans3D/Renderer/RenderStatistics.h"

// The Profiler panel's data, kept apart from the drawing so a benchmark can record the same samples:
// a fixed ring of the last frames (application CPU, the view's render stages and passes, the GPU
// time of the frame it measured, device waits and work, draw categories, shadows and memory), the
// pass ordering of the selected frame and the bottleneck rule. Once constructed, recording a steady
// frame allocates nothing (pass names are stored once per new pass).
namespace Kans::FrameProfiling
{
    // Layers of the stacked CPU graph, bottom first.
    enum class CpuStage : uint8_t
    {
        WorldSync,     // Render world sync and extract.
        GraphBuild,    // FrameGraph build.
        GraphCompile,  // FrameGraph compile.
        ExecuteSubmit, // Pass recording, submit.
        RenderOther,   // The rest of the view's render call.
        Waits,         // Fence, acquire and present waits of the application frame.
        Rest,          // Everything else in the application frame (scene update, editor UI, present).
        Count
    };
    const char* StageLabel(CpuStage stage);

    struct PassSample
    {
        GraphDebugID  ID              = 0;
        uint32_t      ExecutionIndex  = uint32_t(-1);
        float         CpuMilliseconds = -1.0f, GpuMilliseconds = -1.0f; // Negative: not timed.
        uint64_t      DrawCalls = 0, Triangles = 0;
        GraphPassKind Kind   = GraphPassKind::Graphics;
        bool          Culled = true;
        bool          HasCpu() const { return CpuMilliseconds >= 0.0f; }
        bool          HasGpu() const { return GpuMilliseconds >= 0.0f; }
    };

    struct FrameSample
    {
        uint64_t Frame          = 0; // Application frame.
        uint64_t RenderFrame    = 0; // The view's own frame number; 0 when the view did not render.
        uint32_t RenderEpoch    = 0; // Increases when the view's frame numbers restart (a new view target).
        bool     HasApplication = false, HasGpu = false, Detailed = false;

        double                ApplicationMilliseconds = 0; // Whole application frame on the CPU.
        uint32_t              FixedSteps              = 0;
        double                RenderMilliseconds      = 0; // The view's render call.
        std::array<double, 4> RenderStages {};             // WorldSync .. ExecuteSubmit.
        double                RecordedBeforeExecuteMilliseconds = 0, PassRecordMilliseconds = 0;
        std::optional<double> GpuFrameMilliseconds;
        double                GpuMilliseconds = 0;         // Pass sum of this frame, once it completed.

        // Device work of the view's render, and of the whole application frame (UI, uploads, present).
        RenderHardware::DeviceFrameCounters RenderDevice, ApplicationDevice;
        uint64_t                            RenderShaderCompiles = 0, ApplicationShaderCompiles = 0;

        RenderHardware::DrawCategoryTotals Categories {};
        uint64_t                           DrawCalls = 0, Triangles = 0;
        uint64_t SectionProxies = 0, InFrustum = 0, Drawn = 0, BoundsTested = 0, BoundsVisible = 0;
        uint32_t DirectionalLights = 0, PointLights = 0, SpotLights = 0, AreaLights = 0, ShadowedLights = 0;
        uint32_t ShadowPasses = 0, ShadowCacheHits = 0, ShadowCacheMisses = 0, ShadowFullRedraws = 0;
        std::array<uint32_t, size_t(ShadowCacheMissReason::Count)> ShadowMissReasons {};
        RenderHardware::DeviceMemoryProfile                        Memory;

        uint32_t PassCount       = 0;
        bool     PassesTruncated = false; // More passes than FrameHistory::MaxPasses.

        // The application frame's device counters when it arrived, else the render's.
        const RenderHardware::DeviceFrameCounters& Device() const
        {
            return HasApplication ? ApplicationDevice : RenderDevice;
        }
        double   FenceWaitMilliseconds() const { return double(Device().FenceWaitNanoseconds) / 1e6; }
        double   AcquireWaitMilliseconds() const { return double(Device().AcquireWaitNanoseconds) / 1e6; }
        double   PresentWaitMilliseconds() const { return double(Device().PresentWaitNanoseconds) / 1e6; }
        double   WaitMilliseconds() const;
        uint64_t ShaderCompiles() const { return HasApplication ? ApplicationShaderCompiles : RenderShaderCompiles; }
        uint64_t PipelineCompiles() const { return Device().PipelineCompiles; }
        uint64_t Compiles() const { return ShaderCompiles() + PipelineCompiles(); }
        // The application frame when it arrived, else the render call.
        double CpuMilliseconds() const { return HasApplication ? ApplicationMilliseconds : RenderMilliseconds; }
        // One layer of the stacked CPU graph; the layers add up to CpuMilliseconds() (never negative).
        double   Stage(CpuStage stage) const;
        uint64_t VramUsageBytes() const;  // Device-local heaps.
        uint64_t VramBudgetBytes() const; // Device-local heaps.
    };

    // The last `Capacity` application frames, oldest first.
    class FrameHistory
    {
    public:
        static constexpr size_t Capacity  = 600;
        static constexpr size_t MaxPasses = 128;

        explicit FrameHistory(size_t capacity = Capacity, size_t maxPasses = MaxPasses, size_t nameBytes = 0);

        // Records application frame `frame` with the view's latest statistics (null when there is no
        // view). Statistics whose frame number was already recorded count as "did not render". The
        // completed GPU frame they carry belongs to an earlier frame and is attached to it. Recording
        // the same application frame again replaces it. Ignored while paused; returns whether recorded.
        bool Record(uint64_t frame, const FrameStatistics* statistics);
        // The application publishes its totals after the frame ended: they go to the frame they measured.
        bool AttachApplication(const ApplicationFrameProfile& profile);
        // GPU timings of the view's frame `gpu.SourceFrame` (current render epoch only).
        bool AttachGpu(const CompletedGpuFrame& gpu);

        void SetPaused(bool paused) { m_Paused = paused; }
        bool IsPaused() const { return m_Paused; }
        // Forgets the samples; a GPU result for a frame recorded before the clear finds nothing.
        void Clear() { m_Start = m_Count = 0; }

        size_t             Size() const { return m_Count; }
        const FrameSample& At(size_t i) const { return m_Samples[Slot(i)]; } // 0 is the oldest.
        const PassSample*  Passes(size_t i) const { return &m_Passes[Slot(i) * m_MaxPasses]; }
        // Index of application frame `frame`, if it is still in the history.
        std::optional<size_t> Find(uint64_t frame) const;
        const std::pmr::string& PassName(GraphDebugID id) const;

    private:
        size_t Slot(size_t i) const { return (m_Start + i) % m_Samples.size(); }

        const size_t                                            m_MaxPasses;
        std::vector<FrameSample>                      m_Samples;
        std::vector<PassSample>                       m_Passes;
        size_t                                        m_Start = 0, m_Count = 0;
        bool                                          m_Paused          = false;
        uint64_t                                      m_LastRenderFrame = 0;
        uint32_t                                      m_Epoch           = 0;
        std::vector<std::byte>                                  m_NameBuffer;
        std::pmr::monotonic_buffer_resource                     m_NameArena;
        std::pmr::unordered_map<GraphDebugID, std::pmr::string> m_Names;
    };

    // ---- Pass table ----
    enum class PassSort : uint8_t
    {
        Execution, // Executed passes in execution order.
        Name,
        Cpu,
        Gpu,
        Draws
    };
    // Indices into `passes`: executed passes sorted (times descend unless `ascending`), culled passes
    // after them in declaration order; ties keep execution order.
    void OrderPasses(const PassSample*      passes,
                     size_t                 count,
                     PassSort               sort,
                     bool                   ascending,
                     std::vector<uint32_t>& out,
                     const FrameHistory*    names = nullptr);
    // The three passes with the most GPU time, most first (-1 where fewer were timed).
    std::array<int, 3> TopGpuPasses(const PassSample* passes, size_t count);

    // ---- Bottleneck ----
    enum class Bottleneck : uint8_t
    {
        NoData,               // No CPU frame time.
        GpuTimingUnavailable, // CPU frame known, GPU result missing.
        CpuBound,
        GpuBound,
        PresentLimited, // CPU blocked in fence, acquire or present waits while the GPU is not saturated.
        CompileStutter  // A shader or pipeline compiled in the frame.
    };
    struct BottleneckInput
    {
        double                FrameMilliseconds = 0; // Application CPU frame (includes the waits).
        std::optional<double> GpuMilliseconds;       // Pass sum of the view's frame.
        double                WaitMilliseconds = 0;  // Fence + acquire + present.
        uint64_t              Compiles         = 0;  // Shader stages + pipelines compiled.
    };
    // GPU-bound: the GPU took at least this share of the frame and at least the CPU's busy time.
    inline constexpr double GpuBoundShare = 0.8;
    // Present/VSync-limited: the CPU waited at least this share of the frame.
    inline constexpr double WaitBoundShare = 0.25;
    // In order: compiles > 0 -> CompileStutter; frame <= 0 -> NoData; no GPU -> GpuTimingUnavailable;
    // gpu >= GpuBoundShare * frame and gpu >= frame - waits -> GpuBound; waits >= WaitBoundShare * frame
    // -> PresentLimited; otherwise CpuBound.
    Bottleneck      Classify(const BottleneckInput& input);
    BottleneckInput InputOf(const FrameSample& sample);
    const char*     BottleneckLabel(Bottleneck bottleneck);

    struct WindowSummary
    {
        BottleneckInput Average; // Over frames without compiles; GPU over frames with a GPU result.
        size_t          Frames              = 0;
        size_t          CompileFrames       = 0; // Left out of the averages.
        size_t          GpuFrames           = 0;
        GraphDebugID    TopPass             = 0; // The pass with the most average GPU time.
        double          TopPassMilliseconds = 0;
    };
    // The `count` frames ending at `newest` (inclusive). `scratch` is reused between calls.
    WindowSummary Summarize(const FrameHistory&                           history,
                            size_t                                        newest,
                            size_t                                        count,
                            std::vector<std::pair<GraphDebugID, double>>& scratch);
    // "GPU-bound (pass Lighting, 3.20 ms)"; `topPass` may be null.
    void
    FormatBottleneck(Bottleneck bottleneck, const char* topPass, double topPassMilliseconds, char* out, size_t size);

    const char* MissReasonLabel(ShadowCacheMissReason reason);
    const char* CategoryLabel(RenderHardware::DrawCategory category);
    const char* PassKindLabel(GraphPassKind kind);
} // namespace Kans::FrameProfiling
