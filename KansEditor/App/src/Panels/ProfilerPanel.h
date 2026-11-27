#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include "KansEditorCore/Profiling/FrameProfilerModel.h"

namespace Kans
{
    // The Profiler (Window > Debug): the editor's frames without Tracy.
    //  - Toolbar: Pause (stops recording, keeps the history) / Resume, Clear, and while paused the
    //    previous / next frame buttons; on the right whether Tracy is compiled in and connected.
    //  - Bottleneck line for the selected frame and the average of the last 120 frames.
    //  - Graphs over the last 600 frames: the CPU frame stacked by render stage, waits and the rest of
    //    the frame, and the view's GPU pass sum on the frame it measured; 60 and 144 fps lines, spikes
    //    past the scale marked at the top. Hover shows a frame's values, a click selects it.
    //  - The selected frame (else the newest with GPU timings): the FrameGraph passes in execution order
    //    with CPU record and GPU time (sortable, culled passes dimmed, the three slowest GPU passes
    //    marked), then draws per category, device work, compiles (stutter sources), culling, lights,
    //    shadow cache and memory per heap.
    class ProfilerPanel
    {
    public:
        // Once per editor frame while the panel is open, before Draw (also when it is behind a tab).
        // `view` is the editor view's latest statistics; `application` the last finished application
        // frame (the application publishes it after the frame), null when there is none.
        void Record(uint64_t applicationFrame, const FrameStatistics* view, const ApplicationFrameProfile* application);
        void Draw();

        void                    SetPaused(bool paused);
        bool                    IsPaused() const { return m_History.IsPaused(); }
        void                    Clear();
        void                    Select(std::optional<uint64_t> frame) { m_Selected = frame; }
        std::optional<uint64_t> GetSelected() const { return m_Selected; }
        // Moves the selection by `delta` frames (from the newest frame when nothing is selected).
        void Step(int delta);

        const FrameProfiling::FrameHistory& GetHistory() const { return m_History; }
        // The frame the details show: the selection, else the newest frame with GPU timings, else the newest.
        std::optional<size_t> DetailIndex() const;

        // Where the last frame drew its controls and what it showed, for UI tests.
        struct Boxes
        {
            ImRect                Pause, Clear, Previous, Next, CpuGraph, GpuGraph;
            std::optional<size_t> Detail; // The frame shown in the details.
            size_t                PassRows = 0, CulledRows = 0, CategoryRows = 0, HeapRows = 0;
            std::array<int, 3>    TopPasses {-1, -1, -1}; // Pass indices marked as the slowest on the GPU.
            bool                  CompileStutter = false; // The details marked a compile.
            char                  Hint[160] {}, Average[160] {}, Tracy[96] {};
        };
        const Boxes& GetBoxes() const { return m_Boxes; }

    private:
        void DrawToolbar();
        void DrawHints();
        void DrawGraphs();
        // A bar graph over the history; returns the hovered sample index.
        std::optional<size_t> DrawGraph(const char* id, float height, bool gpu, ImRect& box);
        void                  DrawTooltip(size_t index) const;
        void                  DrawPasses(size_t index);
        void                  DrawCounters(size_t index);

        FrameProfiling::FrameHistory                 m_History;
        std::optional<uint64_t>                      m_Selected;
        FrameProfiling::PassSort                     m_Sort      = FrameProfiling::PassSort::Execution;
        bool                                         m_Ascending = true;
        std::vector<uint32_t>                        m_Order;
        std::vector<std::pair<GraphDebugID, double>> m_Scratch;
        Boxes                                        m_Boxes;
    };
} // namespace Kans
