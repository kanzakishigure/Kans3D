#include "ProfilerPanel.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include "Kans3D/Core/Debug/Profiler.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    using namespace FrameProfiling;
    using EditorTheme::Px;

    namespace
    {
        constexpr size_t      AverageFrames  = 120;
        constexpr double      TargetLines[]  = {1000.0 / 60.0, 1000.0 / 144.0};
        constexpr const char* TargetLabels[] = {"60 fps", "144 fps"};

        ImRect LastItem() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

        ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
        {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        }

        ImVec4 Alpha(ImVec4 color, float alpha)
        {
            color.w = alpha;
            return color;
        }

        // A toolbar toggle, filled with `on` while active (as in the Audio Profiler).
        bool ToggleButton(const char* label, const char* tooltip, bool active, ImVec4 on)
        {
            const auto& p = EditorTheme::Colors();
            ImGui::PushStyleColor(ImGuiCol_Button, active ? on : p.Inset);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? Mix(on, p.Text, 0.15f) : p.Hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? Mix(on, p.Text, 0.25f) : p.Pressed);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.06f, 0.06f, 0.07f, 1.0f) : p.TextDim);
            const bool clicked = ImGui::Button(label);
            ImGui::PopStyleColor(4);
            if (tooltip && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }

        ImVec4 StageColor(CpuStage stage)
        {
            const auto& p = EditorTheme::Colors();
            switch (stage)
            {
                case CpuStage::WorldSync:
                    return p.TypeScene;
                case CpuStage::GraphBuild:
                    return p.TypeMaterial;
                case CpuStage::GraphCompile:
                    return p.TypeShader;
                case CpuStage::ExecuteSubmit:
                    return p.Accent;
                case CpuStage::RenderOther:
                    return p.TypeModel;
                case CpuStage::Waits:
                    return Alpha(p.TextOff, 0.9f);
                default:
                    return Alpha(p.TextDim, 0.55f);
            }
        }

        ImVec4 GpuColor() { return EditorTheme::Colors().TypeAudio; }

        std::string Ms(double value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.2f ms", value);
            return text;
        }

        std::string Count(uint64_t value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%" PRIu64, value);
            return text;
        }

        std::string Bytes(uint64_t bytes)
        {
            char text[32];
            if (bytes >= 1024ull * 1024 * 1024)
                std::snprintf(text, sizeof(text), "%.2f GiB", double(bytes) / (1024.0 * 1024 * 1024));
            else if (bytes >= 1024ull * 1024)
                std::snprintf(text, sizeof(text), "%.1f MiB", double(bytes) / (1024.0 * 1024));
            else if (bytes >= 1024)
                std::snprintf(text, sizeof(text), "%.1f KiB", double(bytes) / 1024.0);
            else
                std::snprintf(text, sizeof(text), "%" PRIu64 " B", bytes);
            return text;
        }

        void CellText(const char* text, bool dim = false)
        {
            if (dim)
                ImGui::TextDisabled("%s", text);
            else
                ImGui::TextUnformatted(text);
        }
    } // namespace

    // ------------------------------------------------------------------ state

    void ProfilerPanel::Record(uint64_t                       applicationFrame,
                               const FrameStatistics*         view,
                               const ApplicationFrameProfile* application)
    {
        if (m_History.IsPaused())
            return;
        m_History.Record(applicationFrame, view);
        if (application)
            m_History.AttachApplication(*application);
    }

    void ProfilerPanel::SetPaused(bool paused) { m_History.SetPaused(paused); }

    void ProfilerPanel::Clear()
    {
        m_History.Clear();
        m_Selected.reset();
    }

    void ProfilerPanel::Step(int delta)
    {
        const size_t size = m_History.Size();
        if (size == 0)
            return;
        const auto current = m_Selected ? m_History.Find(*m_Selected) : std::nullopt;
        const long base    = current ? long(*current) : long(size) - 1;
        const long next    = std::clamp(base + (current ? delta : 0), 0L, long(size) - 1);
        m_Selected         = m_History.At(size_t(next)).Frame;
    }

    std::optional<size_t> ProfilerPanel::DetailIndex() const
    {
        if (m_Selected)
            if (const auto index = m_History.Find(*m_Selected))
                return index;
        for (size_t i = m_History.Size(); i-- > 0;)
            if (m_History.At(i).HasGpu)
                return i;
        if (m_History.Size())
            return m_History.Size() - 1;
        return std::nullopt;
    }

    // ------------------------------------------------------------------ drawing

    void ProfilerPanel::Draw()
    {
        m_Boxes.PassRows = m_Boxes.CulledRows = m_Boxes.CategoryRows = m_Boxes.HeapRows = 0;
        m_Boxes.TopPasses                                                               = {-1, -1, -1};
        m_Boxes.CompileStutter                                                          = false;
        if (m_Selected && !m_History.Find(*m_Selected))
            m_Selected.reset(); // Scrolled out of the history.
        m_Boxes.Detail = DetailIndex();

        DrawToolbar();
        DrawHints();
        if (m_History.Size() == 0)
        {
            ImGui::TextDisabled("No frames recorded yet.");
            return;
        }
        DrawGraphs();
        if (!m_Boxes.Detail)
            return;

        const size_t index = *m_Boxes.Detail;
        const float  width = ImGui::GetContentRegionAvail().x;
        if (width >= Px(760.0f))
        {
            // Side by side: the pass table left, the counters right, each scrolling on its own.
            if (ImGui::BeginChild("##passesPane", {width * 0.55f, 0}, ImGuiChildFlags_None))
                DrawPasses(index);
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("##countersPane", {0, 0}, ImGuiChildFlags_None))
                DrawCounters(index);
            ImGui::EndChild();
        }
        else if (ImGui::BeginChild("##details", {0, 0}, ImGuiChildFlags_None))
        {
            DrawPasses(index);
            DrawCounters(index);
            ImGui::EndChild();
        }
        else
            ImGui::EndChild();
    }

    void ProfilerPanel::DrawToolbar()
    {
        const auto& p      = EditorTheme::Colors();
        const bool  paused = m_History.IsPaused();
        if (ToggleButton(paused ? ICON_FA_PLAY " Resume##pause" : ICON_FA_PAUSE " Pause##pause",
                         paused ? "Records new frames again (the history is kept)" :
                                  "Stops recording and keeps the history, to step through it",
                         paused,
                         p.AccentHover))
            SetPaused(!paused);
        m_Boxes.Pause = LastItem();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH_CAN " Clear##clear"))
            Clear();
        m_Boxes.Clear = LastItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Empties the history");

        ImGui::SameLine(0.0f, Px(16.0f));
        ImGui::BeginDisabled(!paused || m_History.Size() == 0);
        if (ImGui::Button(ICON_FA_BACKWARD_STEP "##previous"))
            Step(-1);
        m_Boxes.Previous = LastItem();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Previous frame (while paused)");
        ImGui::SameLine(0.0f, Px(2.0f));
        if (ImGui::Button(ICON_FA_FORWARD_STEP "##next"))
            Step(1);
        m_Boxes.Next = LastItem();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Next frame (while paused)");
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, Px(10.0f));
        ImGui::AlignTextToFramePadding();
        if (m_Boxes.Detail)
        {
            const auto& sample = m_History.At(*m_Boxes.Detail);
            ImGui::Text("Frame %" PRIu64, sample.Frame);
            ImGui::SameLine();
            ImGui::TextDisabled(m_Selected ? "(selected)" : "(newest with GPU timings)");
        }
        else
            ImGui::TextDisabled("No frame");
        ImGui::SameLine(0.0f, Px(10.0f));
        ImGui::TextDisabled(
            "%zu / %zu frames%s", m_History.Size(), FrameHistory::Capacity, paused ? ", paused" : ", recording");

        // Tracy: whether it is built in and a client is connected. Nothing is launched from here.
#ifdef TRACY_ENABLE
        const bool connected = KANS_PROFILE_CONNECTED();
        std::snprintf(m_Boxes.Tracy,
                      sizeof(m_Boxes.Tracy),
                      connected ? "Tracy connected (127.0.0.1:8086)" : "Tracy: no client (127.0.0.1:8086)");
#else
        const bool connected = false;
        std::snprintf(m_Boxes.Tracy, sizeof(m_Boxes.Tracy), "Tracy not compiled in");
#endif
        const float textWidth = ImGui::CalcTextSize(m_Boxes.Tracy).x;
        const float right     = ImGui::GetWindowContentRegionMax().x - textWidth;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Px(16.0f), right));
        if (connected)
            ImGui::TextColored(p.Success, "%s", m_Boxes.Tracy);
        else
            ImGui::TextDisabled("%s", m_Boxes.Tracy);
        if (ImGui::IsItemHovered())
#ifdef TRACY_ENABLE
            ImGui::SetTooltip("Connect the Tracy profiler (0.15) to this address for full zones, GPU passes and "
                              "plots; this panel works without it");
#else
            ImGui::SetTooltip("Debug and Release builds with the Tracy submodule checked out include it");
#endif
    }

    void ProfilerPanel::DrawHints()
    {
        const auto& p     = EditorTheme::Colors();
        const auto  color = [&](Bottleneck kind) {
            switch (kind)
            {
                case Bottleneck::CompileStutter:
                    return p.Danger;
                case Bottleneck::GpuBound:
                case Bottleneck::CpuBound:
                    return p.Warning;
                case Bottleneck::PresentLimited:
                    return p.Success;
                default:
                    return p.TextDim;
            }
        };
        Bottleneck single = Bottleneck::NoData;
        if (m_Boxes.Detail)
        {
            const size_t index  = *m_Boxes.Detail;
            const auto&  sample = m_History.At(index);
            single              = Classify(InputOf(sample));
            const auto  top     = TopGpuPasses(m_History.Passes(index), sample.PassCount);
            const char* name = top[0] >= 0 ? m_History.PassName(m_History.Passes(index)[top[0]].ID).c_str() : nullptr;
            FormatBottleneck(single,
                             name,
                             top[0] >= 0 ? m_History.Passes(index)[top[0]].GpuMilliseconds : 0.0,
                             m_Boxes.Hint,
                             sizeof(m_Boxes.Hint));
        }
        else
            std::snprintf(m_Boxes.Hint, sizeof(m_Boxes.Hint), "%s", BottleneckLabel(Bottleneck::NoData));

        WindowSummary summary;
        if (m_History.Size())
            summary = Summarize(m_History, m_History.Size() - 1, AverageFrames, m_Scratch);
        const Bottleneck average = Classify(summary.Average);
        FormatBottleneck(average,
                         summary.TopPass ? m_History.PassName(summary.TopPass).c_str() : nullptr,
                         summary.TopPassMilliseconds,
                         m_Boxes.Average,
                         sizeof(m_Boxes.Average));

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Selected frame");
        ImGui::SameLine();
        ImGui::TextColored(color(single), "%s", m_Boxes.Hint);
        ImGui::SameLine(0.0f, Px(24.0f));
        ImGui::TextDisabled("Last %zu frames", AverageFrames);
        ImGui::SameLine();
        ImGui::TextColored(color(average), "%s", m_Boxes.Average);
        if (summary.Frames)
        {
            ImGui::SameLine();
            if (summary.Average.GpuMilliseconds)
                ImGui::TextDisabled("(CPU %.2f ms, GPU %.2f ms, waits %.2f ms%s)",
                                    summary.Average.FrameMilliseconds,
                                    *summary.Average.GpuMilliseconds,
                                    summary.Average.WaitMilliseconds,
                                    summary.CompileFrames ? ", compile frames left out" : "");
            else
                ImGui::TextDisabled("(CPU %.2f ms, waits %.2f ms)",
                                    summary.Average.FrameMilliseconds,
                                    summary.Average.WaitMilliseconds);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shader compile stutter: a shader or pipeline compiled in the frame.\n"
                              "GPU-bound: the GPU pass sum is at least %.0f%% of the CPU frame and at least the CPU's "
                              "busy time (frame minus waits).\n"
                              "Present/VSync-limited: fence, acquire and present waits are at least %.0f%% of the "
                              "frame.\nCPU-bound: otherwise. The average leaves out frames that compiled.",
                              GpuBoundShare * 100.0,
                              WaitBoundShare * 100.0);
    }

    void ProfilerPanel::DrawGraphs()
    {
        // Legend: one swatch per CPU layer, then the GPU colour.
        auto*       draw   = ImGui::GetWindowDrawList();
        const float swatch = ImGui::GetTextLineHeight() * 0.7f;
        for (size_t s = 0; s < size_t(CpuStage::Count); ++s)
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy({swatch, ImGui::GetTextLineHeight()});
            const float y = at.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f;
            draw->AddRectFilled(
                {at.x, y}, {at.x + swatch, y + swatch}, ImGui::GetColorU32(StageColor(CpuStage(s))), Px(2.0f));
            ImGui::SameLine(0.0f, Px(4.0f));
            ImGui::TextDisabled("%s", StageLabel(CpuStage(s)));
            ImGui::SameLine(0.0f, Px(12.0f));
        }
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy({swatch, ImGui::GetTextLineHeight()});
            const float y = at.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f;
            draw->AddRectFilled({at.x, y}, {at.x + swatch, y + swatch}, ImGui::GetColorU32(GpuColor()), Px(2.0f));
            ImGui::SameLine(0.0f, Px(4.0f));
            ImGui::TextDisabled("GPU pass sum");
        }

        ImGui::TextDisabled("CPU");
        const auto cpuHover = DrawGraph("##cpuGraph", Px(110.0f), false, m_Boxes.CpuGraph);
        ImGui::TextDisabled("GPU");
        const auto gpuHover = DrawGraph("##gpuGraph", Px(64.0f), true, m_Boxes.GpuGraph);
        if (cpuHover)
            DrawTooltip(*cpuHover);
        else if (gpuHover)
            DrawTooltip(*gpuHover);
    }

    std::optional<size_t> ProfilerPanel::DrawGraph(const char* id, float height, bool gpu, ImRect& box)
    {
        const auto&  p      = EditorTheme::Colors();
        auto*        draw   = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  width  = std::max(Px(100.0f), ImGui::GetContentRegionAvail().x);
        ImGui::InvisibleButton(id, {width, height});
        box = {origin, {origin.x + width, origin.y + height}};
        draw->AddRectFilled(box.Min, box.Max, ImGui::GetColorU32(p.Inset), Px(3.0f));

        const size_t size = m_History.Size();
        double       peak = 0;
        for (size_t i = 0; i < size; ++i)
        {
            const auto& sample = m_History.At(i);
            peak = std::max(peak, gpu ? (sample.HasGpu ? sample.GpuMilliseconds : 0.0) : sample.CpuMilliseconds());
        }
        // At least 20 ms so the 60 fps line shows; spikes past 100 ms are clipped and marked.
        const double scale  = std::clamp(peak * 1.1, 20.0, 100.0);
        const float  barW   = width / float(FrameHistory::Capacity);
        const float  gap    = barW > Px(3.0f) ? Px(1.0f) : 0.0f;
        const float  left   = box.Max.x - float(size) * barW;
        const float  bottom = box.Max.y, top = box.Min.y;
        const auto   y = [&](double ms) { return bottom - float(std::min(ms, scale) / scale) * height; };

        const auto selected = m_Selected ? m_History.Find(*m_Selected) : std::nullopt;
        if (selected)
        {
            const float x = left + float(*selected) * barW;
            draw->AddRectFilled({x - Px(1.0f), top}, {x + barW + Px(1.0f), bottom}, ImGui::GetColorU32(p.Selection));
        }
        draw->PushClipRect(box.Min, box.Max, true);
        for (size_t i = 0; i < size; ++i)
        {
            const auto& sample = m_History.At(i);
            const float x0 = left + float(i) * barW, x1 = x0 + std::max(barW - gap, 1.0f);
            double      total = 0;
            if (gpu)
            {
                if (!sample.HasGpu)
                    continue;
                total = sample.GpuMilliseconds;
                draw->AddRectFilled({x0, y(total)}, {x1, bottom}, ImGui::GetColorU32(GpuColor()));
            }
            else
            {
                float base = bottom;
                for (size_t s = 0; s < size_t(CpuStage::Count) && base > top; ++s)
                {
                    const double value = sample.Stage(CpuStage(s));
                    if (value <= 0.0)
                        continue;
                    total += value;
                    const float next = y(total);
                    draw->AddRectFilled({x0, next}, {x1, base}, ImGui::GetColorU32(StageColor(CpuStage(s))));
                    base = next;
                }
                total = sample.CpuMilliseconds();
                if (sample.Compiles() > 0) // Compile stutter: a mark under the bar.
                    draw->AddRectFilled({x0, bottom - Px(3.0f)}, {x1, bottom}, ImGui::GetColorU32(p.Danger));
            }
            if (total > scale) // A spike past the scale: marked at the top.
                draw->AddRectFilled({x0, top}, {x1, top + Px(3.0f)}, ImGui::GetColorU32(p.Danger));
        }
        // Target lines with their labels.
        for (size_t t = 0; t < std::size(TargetLines); ++t)
        {
            if (TargetLines[t] >= scale)
                continue;
            const float ly = y(TargetLines[t]);
            draw->AddLine({box.Min.x, ly}, {box.Max.x, ly}, ImGui::GetColorU32(Alpha(p.Warning, 0.55f)), 1.0f);
            draw->AddText({box.Min.x + Px(4.0f), ly - ImGui::GetTextLineHeight()},
                          ImGui::GetColorU32(Alpha(p.Warning, 0.8f)),
                          TargetLabels[t]);
        }
        char label[32];
        std::snprintf(label, sizeof(label), "%.0f ms", scale);
        // The scale at the top right, clear of the target labels on the left.
        draw->AddText({box.Max.x - ImGui::CalcTextSize(label).x - Px(6.0f), top + Px(2.0f)},
                      ImGui::GetColorU32(p.TextDim),
                      label);
        draw->PopClipRect();

        std::optional<size_t> hovered;
        if (ImGui::IsItemHovered() && barW > 0.0f)
        {
            const float position = (ImGui::GetIO().MousePos.x - left) / barW;
            if (position >= 0.0f && position < float(size))
            {
                hovered       = size_t(position);
                const float x = left + float(*hovered) * barW;
                draw->AddRectFilled(
                    {x, top}, {x + std::max(barW, 1.0f), bottom}, ImGui::GetColorU32(Alpha(p.Text, 0.25f)));
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    m_Selected = m_History.At(*hovered).Frame;
            }
        }
        return hovered;
    }

    void ProfilerPanel::DrawTooltip(size_t index) const
    {
        const auto& p      = EditorTheme::Colors();
        const auto& sample = m_History.At(index);
        ImGui::BeginTooltip();
        ImGui::Text("Frame %" PRIu64, sample.Frame);
        if (sample.RenderFrame)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(view frame %" PRIu64 ")", sample.RenderFrame);
        }
        ImGui::Text("CPU %.2f ms", sample.CpuMilliseconds());
        if (!sample.HasApplication)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(render call only)");
        }
        for (size_t s = 0; s < size_t(CpuStage::Count); ++s)
        {
            const double value = sample.Stage(CpuStage(s));
            if (value <= 0.0)
                continue;
            ImGui::TextColored(StageColor(CpuStage(s)), "  %s", StageLabel(CpuStage(s)));
            ImGui::SameLine(Px(150.0f));
            ImGui::Text("%.2f ms", value);
        }
        if (sample.HasGpu)
            ImGui::Text("GPU %.2f ms", sample.GpuMilliseconds);
        else
            ImGui::TextDisabled(sample.RenderFrame ? "GPU: not completed yet" : "GPU: the view did not render");
        ImGui::TextDisabled("Waits: fence %.2f, acquire %.2f, present %.2f ms",
                            sample.FenceWaitMilliseconds(),
                            sample.AcquireWaitMilliseconds(),
                            sample.PresentWaitMilliseconds());
        ImGui::TextDisabled("%" PRIu64 " draws, %" PRIu64 " triangles", sample.DrawCalls, sample.Triangles);
        if (sample.Compiles())
            ImGui::TextColored(p.Danger,
                               ICON_FA_TRIANGLE_EXCLAMATION " %" PRIu64 " shader stages, %" PRIu64
                                                            " pipelines compiled",
                               sample.ShaderCompiles(),
                               sample.PipelineCompiles());
        ImGui::TextDisabled("%s", BottleneckLabel(Classify(InputOf(sample))));
        ImGui::EndTooltip();
    }

    void ProfilerPanel::DrawPasses(size_t index)
    {
        const auto&       p      = EditorTheme::Colors();
        const auto&       sample = m_History.At(index);
        const PassSample* passes = m_History.Passes(index);
        if (sample.PassCount == 0)
        {
            ImGui::TextDisabled(sample.RenderFrame ? "The view rendered no passes." : "The view did not render.");
            return;
        }
        const ImGuiTableFlags flags  = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                                       ImGuiTableFlags_SortTristate | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingStretchProp;
        const float           height = std::max(ImGui::GetContentRegionAvail().y, Px(160.0f));
        if (!ImGui::BeginTable("##passes", 6, flags, {0.0f, height}))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#",
                                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed,
                                Px(28.0f),
                                ImGuiID(PassSort::Execution));
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 3.0f, ImGuiID(PassSort::Name));
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_NoSort, 1.0f);
        ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_PreferSortDescending, 1.0f, ImGuiID(PassSort::Cpu));
        ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_PreferSortDescending, 1.2f, ImGuiID(PassSort::Gpu));
        ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_PreferSortDescending, 1.0f, ImGuiID(PassSort::Draws));
        ImGui::TableHeadersRow();
        if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
        {
            if (specs->SpecsCount > 0)
            {
                m_Sort      = PassSort(specs->Specs[0].ColumnUserID);
                m_Ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
            else
            {
                m_Sort      = PassSort::Execution;
                m_Ascending = true;
            }
        }
        OrderPasses(passes, sample.PassCount, m_Sort, m_Ascending, m_Order, &m_History);
        m_Boxes.TopPasses = TopGpuPasses(passes, sample.PassCount);
        char text[48];
        for (const uint32_t i : m_Order)
        {
            const PassSample& pass = passes[i];
            int               rank = -1;
            for (int r = 0; r < 3; ++r)
                if (m_Boxes.TopPasses[r] == int(i))
                    rank = r;
            ImGui::TableNextRow();
            if (rank >= 0)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                       ImGui::GetColorU32(Alpha(p.Warning, rank == 0 ? 0.24f : 0.13f)));
            if (pass.Culled)
                ++m_Boxes.CulledRows;
            else
                ++m_Boxes.PassRows;
            const bool dim = pass.Culled;
            ImGui::TableNextColumn();
            if (pass.Culled)
                CellText("-", true);
            else
            {
                std::snprintf(text, sizeof(text), "%u", pass.ExecutionIndex);
                CellText(text, true);
            }
            ImGui::TableNextColumn();
            if (dim)
                ImGui::TextColored(p.TextOff, "%s (culled)", m_History.PassName(pass.ID).c_str());
            else
                ImGui::TextUnformatted(m_History.PassName(pass.ID).c_str());
            ImGui::TableNextColumn();
            CellText(PassKindLabel(pass.Kind), true);
            ImGui::TableNextColumn();
            if (pass.HasCpu())
                std::snprintf(text, sizeof(text), "%.3f", double(pass.CpuMilliseconds));
            else
                std::snprintf(text, sizeof(text), "-");
            CellText(text, dim);
            ImGui::TableNextColumn();
            if (pass.HasGpu())
            {
                std::snprintf(text, sizeof(text), "%.3f", double(pass.GpuMilliseconds));
                if (rank >= 0)
                    ImGui::TextColored(p.Warning, "%s  #%d", text, rank + 1);
                else
                    CellText(text, dim);
            }
            else
                CellText(pass.Culled ? "-" : sample.HasGpu ? "not timed" : "pending", true);
            ImGui::TableNextColumn();
            std::snprintf(text, sizeof(text), "%" PRIu64, pass.DrawCalls);
            CellText(text, dim);
            if (ImGui::IsItemHovered() && pass.Triangles)
                ImGui::SetTooltip("%" PRIu64 " triangles", pass.Triangles);
        }
        ImGui::EndTable();
        if (sample.PassesTruncated)
            ImGui::TextColored(p.Warning, "Only the first %zu passes are kept.", FrameHistory::MaxPasses);
    }

    void ProfilerPanel::DrawCounters(size_t index)
    {
        const auto& p      = EditorTheme::Colors();
        const auto& sample = m_History.At(index);

#ifndef KS_DIST
        if (!sample.Detailed && sample.RenderFrame)
            ImGui::TextColored(p.Warning, "Frame profiling is off on the device: detailed counters are empty.");

        if (PropertyGrid::BeginCard("ProfilerDraws", ICON_FA_LAYER_GROUP, "Draws"))
        {
            PropertyGrid::Begin("ProfilerDrawsGrid");
            PropertyGrid::BeginFree();
            if (ImGui::BeginTable("##categories",
                                  4,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Draws");
                ImGui::TableSetupColumn("Instances");
                ImGui::TableSetupColumn("Triangles");
                ImGui::TableHeadersRow();
                const auto row =
                    [&](const char* name, uint64_t draws, uint64_t instances, uint64_t triangles, bool dim) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        CellText(name, dim);
                        ImGui::TableNextColumn();
                        CellText(Count(draws).c_str(), dim);
                        ImGui::TableNextColumn();
                        CellText(Count(instances).c_str(), dim);
                        ImGui::TableNextColumn();
                        CellText(Count(triangles).c_str(), dim);
                    };
                RenderHardware::CategoryDrawStatistics total;
                for (size_t c = 0; c < sample.Categories.size(); ++c)
                {
                    const auto& category = sample.Categories[c];
                    total += category;
                    row(CategoryLabel(RenderHardware::DrawCategory(c)),
                        category.DrawCalls,
                        category.Instances,
                        category.Triangles,
                        category.DrawCalls == 0);
                    ++m_Boxes.CategoryRows;
                }
                row("Total (view)", total.DrawCalls, total.Instances, total.Triangles, false);
                if (sample.HasApplication)
                    row("Editor UI (ImGui)",
                        sample.ApplicationDevice.ImGuiDrawCalls,
                        0,
                        sample.ApplicationDevice.ImGuiTriangles,
                        true);
                ImGui::EndTable();
            }
            PropertyGrid::EndFree();
            PropertyGrid::End();
        }
        PropertyGrid::EndCard();
#endif

        if (PropertyGrid::BeginCard("ProfilerFrame", ICON_FA_CHART_LINE, "Frame"))
        {
            if (PropertyGrid::BeginGroup("CPU"))
            {
                PropertyGrid::Text("Application frame",
                                   sample.HasApplication ? Ms(sample.ApplicationMilliseconds) : "not reported",
                                   !sample.HasApplication);
                PropertyGrid::Text("Render (view)", Ms(sample.RenderMilliseconds));
                for (size_t s = 0; s < 4; ++s)
                    PropertyGrid::Text(StageLabel(CpuStage(s)), Ms(sample.RenderStages[s]), true);
                PropertyGrid::Text("Fixed steps", Count(sample.FixedSteps));
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("GPU"))
            {
                PropertyGrid::Text("Pass sum",
                                   sample.HasGpu      ? Ms(sample.GpuMilliseconds) :
                                   sample.RenderFrame ? "not completed yet" :
                                                        "the view did not render",
                                   !sample.HasGpu);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Waits"))
            {
                PropertyGrid::Text("Fence", Ms(sample.FenceWaitMilliseconds()));
                PropertyGrid::Text("Acquire", Ms(sample.AcquireWaitMilliseconds()));
                PropertyGrid::Text("Present", Ms(sample.PresentWaitMilliseconds()));
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();

#ifdef KS_DIST
        ImGui::TextDisabled("Draw categories, device work, compiles, lights, shadow cache and memory counters are "
                            "compiled out of Dist builds.");
        (void)p;
#else

        if (PropertyGrid::BeginCard("ProfilerDevice", ICON_FA_MICROCHIP, "Device work"))
        {
            const auto& device = sample.Device();
            const auto  scope  = [&](uint64_t whole, uint64_t view) {
                return sample.HasApplication ? Count(whole) + "  (view " + Count(view) + ")" : Count(view);
            };
            if (PropertyGrid::BeginGroup("Binds"))
            {
                PropertyGrid::Text("Pipelines", scope(device.PipelineBinds, sample.RenderDevice.PipelineBinds));
                PropertyGrid::Text("Descriptor sets",
                                   scope(device.DescriptorSetBinds, sample.RenderDevice.DescriptorSetBinds));
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Uploads"))
            {
                PropertyGrid::Text("Buffers", Count(device.BufferUploads) + ", " + Bytes(device.BufferUploadBytes));
                PropertyGrid::Text("Textures", Count(device.TextureUploads) + ", " + Bytes(device.TextureUploadBytes));
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Compiles"))
            {
                m_Boxes.CompileStutter = sample.Compiles() > 0;
                PropertyGrid::Text("Shader stages", Count(sample.ShaderCompiles()));
                PropertyGrid::Text("Pipelines", Count(sample.PipelineCompiles()));
                if (m_Boxes.CompileStutter)
                {
                    PropertyGrid::BeginFree();
                    ImGui::TextColored(p.Danger,
                                       ICON_FA_TRIANGLE_EXCLAMATION " Compiling in this frame: a stutter source");
                    PropertyGrid::EndFree();
                }
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();

        if (PropertyGrid::BeginCard("ProfilerScene", ICON_FA_CUBES, "Scene"))
        {
            if (PropertyGrid::BeginGroup("Culling"))
            {
                PropertyGrid::Text("Mesh sections", Count(sample.SectionProxies));
                PropertyGrid::Text("In view / drawn", Count(sample.InFrustum) + " / " + Count(sample.Drawn));
                PropertyGrid::Text("Bounds tested / visible",
                                   Count(sample.BoundsTested) + " / " + Count(sample.BoundsVisible));
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Lights"))
            {
                PropertyGrid::Text("Directional", Count(sample.DirectionalLights));
                PropertyGrid::Text("Point", Count(sample.PointLights));
                PropertyGrid::Text("Spot", Count(sample.SpotLights));
                PropertyGrid::Text("Area", Count(sample.AreaLights));
                PropertyGrid::Text("Shadowed", Count(sample.ShadowedLights));
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Shadow cache"))
            {
                PropertyGrid::Text("Shadow passes", Count(sample.ShadowPasses));
                PropertyGrid::Text("Hits / misses",
                                   Count(sample.ShadowCacheHits) + " / " + Count(sample.ShadowCacheMisses));
                PropertyGrid::Text("Full redraws", Count(sample.ShadowFullRedraws));
                for (size_t r = 1; r < sample.ShadowMissReasons.size(); ++r)
                    if (sample.ShadowMissReasons[r])
                        PropertyGrid::Text(MissReasonLabel(ShadowCacheMissReason(r)),
                                           Count(sample.ShadowMissReasons[r]));
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();

        if (PropertyGrid::BeginCard("ProfilerMemory", ICON_FA_HARD_DRIVE, "Memory"))
        {
            if (PropertyGrid::BeginGroup("Heaps"))
            {
                for (uint32_t h = 0; h < sample.Memory.HeapCount && h < sample.Memory.Heaps.size(); ++h)
                {
                    const auto& heap = sample.Memory.Heaps[h];
                    char        label[48];
                    std::snprintf(label, sizeof(label), "Heap %u%s", h, heap.DeviceLocal ? " (VRAM)" : "");
                    if (!PropertyGrid::Row(label))
                        continue;
                    ++m_Boxes.HeapRows;
                    const float fraction =
                        heap.BudgetBytes ? float(double(heap.UsageBytes) / double(heap.BudgetBytes)) : 0.0f;
                    const std::string overlay = Bytes(heap.UsageBytes) + " / " + Bytes(heap.BudgetBytes);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                          fraction > 0.9f  ? p.Danger :
                                          fraction > 0.75f ? p.Warning :
                                                             p.Accent);
                    ImGui::ProgressBar(std::min(fraction, 1.0f), {-FLT_MIN, 0.0f}, overlay.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Usage %s of a %s budget%s; allocations %s",
                                          Bytes(heap.UsageBytes).c_str(),
                                          Bytes(heap.BudgetBytes).c_str(),
                                          sample.Memory.DriverBudget ? " (driver budget)" : " (heap size)",
                                          Bytes(heap.AllocationBytes).c_str());
                }
                if (sample.Memory.HeapCount == 0)
                    PropertyGrid::Text("Heaps", "not reported", true);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Totals"))
            {
                PropertyGrid::Text("VRAM", Bytes(sample.VramUsageBytes()) + " / " + Bytes(sample.VramBudgetBytes()));
                PropertyGrid::Text("Render targets", Bytes(sample.Memory.RenderTargetBytes));
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();
#endif
    }
} // namespace Kans
