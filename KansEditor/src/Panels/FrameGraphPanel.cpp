#include "FrameGraphPanel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include "UI/EditorIcons.h"
#include "ViewportClient.h"

namespace Kans
{
    namespace
    {
        using RenderHardware::Format;

        constexpr float ColumnWidth = 230.0f, PassWidth = 190.0f, PassHeight = 66.0f;
        constexpr float VersionWidth = 170.0f, ThumbnailWidth = VersionWidth - 12.0f, VersionRowTop = 50.0f;

        GraphDebugID VersionID(GraphDebugID resource, uint32_t version)
        {
            return resource ^ ((version + 1) * 0x9E3779B97F4A7C15ull);
        }

        const char* FormatName(Format format)
        {
            switch (format)
            {
                case Format::RGBA8Unorm: return "RGBA8";
                case Format::RGBA8Srgb: return "RGBA8 sRGB";
                case Format::RGBA16Float: return "RGBA16F";
                case Format::RGBA32Float: return "RGBA32F";
                case Format::R32Float:
                    return "R32F";
                case Format::RG16Float:
                    return "RG16F";
                case Format::RG8Unorm: return "RG8";
                case Format::R32Uint: return "R32UI";
                case Format::R8Unorm: return "R8";
                case Format::Depth32Float: return "D32F";
                case Format::Depth24Stencil8: return "D24S8";
            }
            return "?";
        }

        const char* AccessName(GraphAccess access)
        {
            switch (access)
            {
                case GraphAccess::SampledRead: return "sampled";
                case GraphAccess::ColorWrite: return "color write";
                case GraphAccess::ColorReadWrite: return "color read/write";
                case GraphAccess::DepthWrite: return "depth write";
                case GraphAccess::DepthReadWrite: return "depth read/write";
                case GraphAccess::TransferRead: return "copy source";
                case GraphAccess::TransferWrite: return "copy destination";
                case GraphAccess::StorageRead:
                    return "storage read";
                case GraphAccess::StorageWrite:
                    return "storage write";
                case GraphAccess::StorageReadWrite:
                    return "storage read/write";
                case GraphAccess::IndirectRead:
                    return "indirect";
                case GraphAccess::VertexRead:
                    return "vertex";
                case GraphAccess::IndexRead:
                    return "index";
                case GraphAccess::UniformRead:
                    return "uniform";
            }
            return "?";
        }

        bool Writes(GraphAccess access)
        {
            return access == GraphAccess::ColorWrite || access == GraphAccess::ColorReadWrite ||
                   access == GraphAccess::DepthWrite || access == GraphAccess::DepthReadWrite ||
                   access == GraphAccess::TransferWrite || access == GraphAccess::StorageWrite ||
                   access == GraphAccess::StorageReadWrite;
        }

        bool IsDepth(Format format) { return format == Format::Depth32Float || format == Format::Depth24Stencil8; }

        std::string Milliseconds(const std::optional<double>& value)
        {
            if (!value)
                return "-";
            char text[32];
            std::snprintf(text, sizeof(text), "%.3f ms", *value);
            return text;
        }

        std::string SettingsKey(const CaptureDisplaySettings& s)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "%u|%d|%.3f|%.4f|%.4f|%d|%d", unsigned(s.ChannelMask), s.Solo, s.Exposure,
                          s.RangeMin, s.RangeMax, int(s.SRGB), int(s.LinearDepth));
            return text;
        }

        float ThumbnailHeight(const CompiledGraphResource& resource)
        {
            const auto& d = resource.Description;
            return std::clamp(ThumbnailWidth * float(d.Height) / float(std::max(1u, d.Width)), 24.0f, 120.0f);
        }
        float VersionHeight(const CompiledGraphResource& resource) { return 40.0f + ThumbnailHeight(resource) + 6.0f; }
    } // namespace

    GraphCaptureSettings FrameGraphPanel::CaptureSettings() const
    {
        GraphCaptureSettings settings;
        settings.CaptureGraph = m_Capture && !m_Freeze;
        if (m_SelectedPass && m_SelectedResource)
            settings.Inspector = CaptureSelection {m_SelectedPass, m_SelectedResource, {m_Mip, m_Face}};
        return settings;
    }

    const CompiledGraphPass* FrameGraphPanel::FindPass(GraphDebugID id) const
    {
        for (const auto& pass : m_Passes)
            if (pass.ID == id)
                return &pass;
        return nullptr;
    }

    const CompiledGraphResource* FrameGraphPanel::FindGraphResource(GraphDebugID id) const
    {
        for (const auto& resource : m_Resources)
            if (resource.ID == id)
                return &resource;
        return nullptr;
    }

    const PassStatistics* FrameGraphPanel::FindStatistics(GraphDebugID id) const
    {
        for (const auto& pass : m_Stats.Passes)
            if (pass.ID == id)
                return &pass;
        return nullptr;
    }

    std::vector<const CompiledGraphResource*> FrameGraphPanel::Outputs(const CompiledGraphPass& pass) const
    {
        std::vector<const CompiledGraphResource*> outputs;
        for (const auto& use : pass.Uses)
            if (Writes(use.Access) && use.Texture < m_Resources.size() &&
                std::find(outputs.begin(), outputs.end(), &m_Resources[use.Texture]) == outputs.end())
                outputs.push_back(&m_Resources[use.Texture]);
        return outputs;
    }

    void FrameGraphPanel::Snapshot()
    {
        if (!m_Client || m_Freeze)
            return;
        const auto& result = m_Client->GetLastResult();
        m_Passes    = result.Graph.Passes();
        m_Resources = result.Graph.Resources();
        m_Stats     = result.FrameStats;
        if (result.Captures)
            m_Captures = result.Captures;
        Layout();
    }

    void FrameGraphPanel::Layout()
    {
        m_Nodes.clear();
        m_Edges.clear();
        m_EdgeWrites.clear();
        // Passes left to right in execution order; culled passes after them.
        std::vector<const CompiledGraphPass*> order;
        for (const auto& pass : m_Passes)
            order.push_back(&pass);
        std::stable_sort(order.begin(), order.end(), [](const auto* a, const auto* b) {
            return a->ExecutionIndex < b->ExecutionIndex;
        });
        std::map<GraphDebugID, float> column;
        for (size_t i = 0; i < order.size(); ++i)
        {
            m_Nodes.push_back({order[i]->ID, true, {float(i) * ColumnWidth, 0.0f}, {PassWidth, PassHeight}});
            column[order[i]->ID] = float(i);
        }
        // Walk each texture's accesses in execution order: a write starts a new version,
        // a read uses the current one, and a read-write consumes one and produces the next.
        std::map<int, int> stacked;
        const auto         place = [&](Node node, float at) {
            const int slot = stacked[int(std::lround(at * 2))]++;
            node.Position  = {at * ColumnWidth + (PassWidth - VersionWidth) * 0.5f,
                             PassHeight + VersionRowTop + slot * (VersionHeight(m_Resources[node.Resource]) + 16)};
            node.Size      = {VersionWidth, VersionHeight(m_Resources[node.Resource])};
            m_Nodes.push_back(node);
            return m_Nodes.back().ID;
        };
        for (uint32_t r = 0; r < m_Resources.size(); ++r)
        {
            const auto& resource = m_Resources[r];
            std::vector<const CompiledResourceAccess*> accesses;
            for (const auto& access : resource.DeclaredAccesses)
                if (access.ExecutionIndex != uint32_t(-1) && access.Pass < m_Passes.size())
                    accesses.push_back(&access);
            std::stable_sort(accesses.begin(), accesses.end(),
                             [](const auto* a, const auto* b) { return a->ExecutionIndex < b->ExecutionIndex; });
            GraphDebugID current = 0;
            uint32_t     version = 0;
            for (const auto* access : accesses)
            {
                const auto& pass   = m_Passes[access->Pass];
                const bool  writes = Writes(access->Access);
                const bool  reads  = !writes || access->Access == GraphAccess::ColorReadWrite ||
                                     access->Access == GraphAccess::DepthReadWrite ||
                                     access->Access == GraphAccess::StorageReadWrite;
                if (reads)
                {
                    if (!current) // Read before any write: the content the frame started with.
                        current = place({VersionID(resource.ID, 0), false, {}, {}, r, 0, 0, false},
                                        column[pass.ID] - 0.5f);
                    for (auto& node : m_Nodes)
                        if (node.ID == current)
                            node.Read = true;
                    m_Edges.push_back({current, pass.ID});
                    m_EdgeWrites.push_back(false);
                }
                if (writes)
                {
                    ++version;
                    current = place({VersionID(resource.ID, version), false, {}, {}, r, version, pass.ID, false},
                                    column[pass.ID] + 0.5f);
                    m_Edges.push_back({pass.ID, current});
                    m_EdgeWrites.push_back(true);
                }
            }
            // An imported texture's last version leaves the frame (it is displayed or kept).
            if (resource.Imported && current)
                for (auto& node : m_Nodes)
                    if (node.ID == current)
                        node.Read = true;
        }
    }

    CaptureDisplaySettings FrameGraphPanel::ThumbnailSettings(const CompiledGraphResource& resource) const
    {
        CaptureDisplaySettings settings;
        if (IsDepth(resource.Description.PixelFormat))
        {
            // Shadow maps use orthographic or per-light projections, not the view's: show
            // them raw. The view's depth is linearised with its own projection.
            settings.Solo        = 0;
            settings.LinearDepth = resource.Name.find("hadow") == std::string::npos;
            settings.RangeMax    = settings.LinearDepth ? m_DepthFar : 1.0f;
        }
        else if (resource.Description.PixelFormat == Format::RGBA16Float ||
                 resource.Description.PixelFormat == Format::RGBA32Float)
            settings.SRGB = true; // Scene-linear HDR; shown encoded so it is not too dark.
        return settings;
    }

    uintptr_t FrameGraphPanel::Display(const CapturedImage& image, const CaptureDisplaySettings& settings)
    {
        if (!image.Texture || !image.Unavailable.empty() || !m_Client)
            return 0;
        auto&      cached = m_DisplayCache[{image.Selection.Pass, image.Selection.Resource, image.FullResolution,
                                            SettingsKey(settings)}];
        auto       capture = m_Client->GetGraphCapture();
        if (!cached.Texture || cached.Frame != image.Frame)
        {
            cached.Texture = capture->Display(image, settings);
            cached.Frame   = image.Frame;
        }
        return cached.Texture ? m_Client->GetRenderer()->GetDevice()->GetImGuiTextureID(cached.Texture) : 0;
    }

    void FrameGraphPanel::Draw()
    {
        Snapshot();
        // Toolbar.
        ImGui::Checkbox("Capture", &m_Capture);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Snapshot every pass's output of the editor view each frame (costs GPU time)");
        ImGui::SameLine();
        ImGui::Checkbox("Freeze", &m_Freeze);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Keep showing this frame");
        ImGui::SameLine();
        if (ImGui::Button("Fit"))
        {
            m_Pan  = {40, 40};
            m_Zoom = 1.0f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Frame %llu  |  %zu passes  |  %zu resources  |  GPU %s", (unsigned long long)m_Stats.Frame,
                            m_Passes.size(), m_Resources.size(),
                            m_Stats.Gpu ? Milliseconds(m_Stats.Gpu->PassMilliseconds).c_str() : "-");
        if (m_Passes.empty())
        {
            ImGui::TextDisabled("No frame rendered yet.");
            return;
        }
        // Prune display textures of frames no longer shown.
        if (m_Captures)
            for (auto it = m_DisplayCache.begin(); it != m_DisplayCache.end();)
                it = it->second.Frame != m_Captures->Frame ? m_DisplayCache.erase(it) : std::next(it);

        const float inspector = 380.0f;
        if (ImGui::BeginChild("##Canvas",
                              {ImGui::GetContentRegionAvail().x - inspector, 0},
                              ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            DrawCanvas();
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##Inspector", {0, 0}, ImGuiChildFlags_Borders))
            DrawInspector();
        ImGui::EndChild();
    }

    void FrameGraphPanel::DrawCanvas()
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##canvas", {std::max(size.x, 1.0f), std::max(size.y, 1.0f)},
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                                   ImGuiButtonFlags_MouseButtonRight);
        const bool     hovered = ImGui::IsItemHovered();
        const ImGuiIO& io      = ImGui::GetIO();
        // Pan with the middle or right button, zoom around the cursor with the wheel.
        if (ImGui::IsItemActive() &&
            (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
            m_Pan = {m_Pan.x + io.MouseDelta.x, m_Pan.y + io.MouseDelta.y};
        if (hovered && io.MouseWheel != 0.0f)
        {
            const float  zoom  = std::clamp(m_Zoom * std::pow(1.15f, io.MouseWheel), 0.2f, 3.0f);
            const ImVec2 mouse = {io.MousePos.x - origin.x, io.MousePos.y - origin.y};
            m_Pan              = {mouse.x - (mouse.x - m_Pan.x) * zoom / m_Zoom, mouse.y - (mouse.y - m_Pan.y) * zoom / m_Zoom};
            m_Zoom             = zoom;
        }
        const auto toScreen = [&](ImVec2 p) {
            return ImVec2(origin.x + m_Pan.x + p.x * m_Zoom, origin.y + m_Pan.y + p.y * m_Zoom);
        };
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
        draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(24, 25, 28, 255));
        // Background grid.
        const float step = 32.0f * m_Zoom;
        for (float x = std::fmod(m_Pan.x, step); x < size.x; x += step)
            draw->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + size.y}, IM_COL32(40, 42, 46, 255));
        for (float y = std::fmod(m_Pan.y, step); y < size.y; y += step)
            draw->AddLine({origin.x, origin.y + y}, {origin.x + size.x, origin.y + y}, IM_COL32(40, 42, 46, 255));

        std::map<GraphDebugID, const Node*> nodes;
        for (const auto& node : m_Nodes)
            nodes[node.ID] = &node;
        // Edges: writes orange (pass to texture), reads blue (texture to pass).
        for (int layer = 0; layer < 2; ++layer)
        for (size_t i = 0; i < m_Edges.size(); ++i)
        {
            const bool inSelection = m_Edges[i].first == m_SelectedPass || m_Edges[i].second == m_SelectedPass ||
                                     m_Edges[i].first == m_SelectedVersion || m_Edges[i].second == m_SelectedVersion;
            if (inSelection != (layer == 1))
                continue;
            const Node* from = nodes.count(m_Edges[i].first) ? nodes[m_Edges[i].first] : nullptr;
            const Node* to   = nodes.count(m_Edges[i].second) ? nodes[m_Edges[i].second] : nullptr;
            if (!from || !to)
                continue;
            const ImVec2 a = toScreen({from->Position.x + from->Size.x, from->Position.y + from->Size.y * 0.5f});
            const ImVec2 b = toScreen({to->Position.x, to->Position.y + std::min(to->Size.y * 0.5f, 30.0f)});
            const float  bend = std::max(40.0f, std::abs(b.x - a.x) * 0.5f);
            const bool   selected     = inSelection;
            // Only the selection's edges stand out; the rest stay faint context.
            const bool   anySelection = m_SelectedPass || m_SelectedVersion;
            const int    alpha        = selected ? 255 : anySelection ? 25 : 70;
            const ImU32  color        = m_EdgeWrites[i] ? IM_COL32(235, 150, 60, alpha) : IM_COL32(90, 160, 240, alpha);
            draw->AddBezierCubic(a, {a.x + bend, a.y}, {b.x - bend, b.y}, b, color, selected ? 2.5f : 1.0f);
        }

        ImFont*     font     = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * m_Zoom;
        const auto  text     = [&](ImVec2 at, ImU32 color, const std::string& value) {
            if (fontSize >= 6.0f)
                draw->AddText(font, fontSize, at, color, value.c_str());
        };
        GraphDebugID clicked = 0;
        bool         clickedPass = false;
        for (const auto& node : m_Nodes)
        {
            const ImVec2 min = toScreen(node.Position);
            const ImVec2 max = toScreen({node.Position.x + node.Size.x, node.Position.y + node.Size.y});
            if (max.x < origin.x || min.x > origin.x + size.x || max.y < origin.y || min.y > origin.y + size.y)
                continue;
            const bool over = hovered && ImGui::IsMouseHoveringRect(min, max);
            if (over && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                clicked     = node.ID;
                clickedPass = node.Pass;
            }
            const float pad = 6.0f * m_Zoom;
            if (node.Pass)
            {
                const auto* pass     = FindPass(node.ID);
                const auto* stats    = FindStatistics(node.ID);
                const bool  selected = node.ID == m_SelectedPass;
                const bool  culled   = pass && pass->Culled;
                draw->AddRectFilled(min, max, culled ? IM_COL32(45, 45, 48, 230) : IM_COL32(44, 52, 66, 240), 6.0f * m_Zoom);
                draw->AddRectFilled(min, {max.x, min.y + 22 * m_Zoom},
                                    culled ? IM_COL32(70, 70, 74, 255) : IM_COL32(60, 92, 140, 255), 6.0f * m_Zoom,
                                    ImDrawFlags_RoundCornersTop);
                draw->AddRect(min, max, selected ? IM_COL32(255, 200, 80, 255) : over ? IM_COL32(150, 170, 200, 255) :
                                                                                        IM_COL32(80, 90, 110, 255),
                              6.0f * m_Zoom, 0, selected ? 2.5f : 1.0f);
                if (!pass)
                    continue;
                text({min.x + pad, min.y + 3 * m_Zoom}, IM_COL32(240, 240, 240, 255),
                     (culled ? "(culled) " : "#" + std::to_string(pass->ExecutionIndex) + "  ") + pass->Name);
                std::string timing = "CPU " + Milliseconds(stats ? stats->CpuRecordMilliseconds : std::nullopt) +
                                     "  GPU " + Milliseconds(stats ? stats->GpuMilliseconds : std::nullopt);
                text({min.x + pad, min.y + 26 * m_Zoom}, IM_COL32(190, 200, 210, 255), timing);
                if (stats && stats->Draws)
                    text({min.x + pad, min.y + 44 * m_Zoom}, IM_COL32(160, 170, 180, 255),
                         std::to_string(stats->Draws->DrawCalls) + " draws, " + std::to_string(stats->Draws->Triangles) +
                             " tris");
            }
            else
            {
                const auto& resource = m_Resources.at(node.Resource);
                const bool  selected = node.ID == m_SelectedVersion;
                // Versions nobody reads (and that do not leave the frame) are wasted work.
                const ImU32 fill = node.Read ? IM_COL32(52, 46, 40, 240) : IM_COL32(42, 40, 40, 200);
                draw->AddRectFilled(min, max, fill, 8.0f * m_Zoom);
                draw->AddRect(min, max, selected ? IM_COL32(255, 200, 80, 255) : over ? IM_COL32(200, 170, 140, 255) :
                                                                                        IM_COL32(110, 95, 80, 255),
                              8.0f * m_Zoom, 0, selected ? 2.5f : 1.0f);
                text({min.x + pad, min.y + 4 * m_Zoom}, node.Read ? IM_COL32(240, 230, 220, 255) : IM_COL32(150, 145, 140, 255),
                     resource.Name + "  v" + std::to_string(node.Version) + (node.Read ? "" : "  (unused)"));
                const auto& d = resource.Description;
                char        info[96];
                if (resource.IsBuffer)
                    std::snprintf(info,
                                  sizeof(info),
                                  "%zu bytes%s",
                                  resource.BufferDescription.Bytes,
                                  resource.Imported ? "  imported buffer" : "  transient buffer");
                else
                    std::snprintf(info,
                                  sizeof(info),
                                  "%ux%u %s%s",
                                  d.Width,
                                  d.Height,
                                  FormatName(d.PixelFormat),
                                  resource.Imported ? "  imported" : "  transient");
                text({min.x + pad, min.y + 20 * m_Zoom}, IM_COL32(170, 160, 150, 255), info);
                // The content right after the write that produced this version.
                const ImVec2 imageMin = toScreen({node.Position.x + 6, node.Position.y + 40});
                const ImVec2 imageMax =
                    toScreen({node.Position.x + 6 + ThumbnailWidth, node.Position.y + 40 + ThumbnailHeight(resource)});
                uintptr_t id = 0;
                if (m_Captures && node.Writer)
                    if (const auto* image = m_Captures->Find({node.Writer, resource.ID, {0, 0}}))
                        id = Display(*image, ThumbnailSettings(resource));
                if (id)
                    draw->AddImage(static_cast<ImTextureID>(id), imageMin, imageMax, {0, 0}, {1, 1});
                else
                    draw->AddRectFilled(imageMin, imageMax, IM_COL32(35, 35, 38, 255));
                draw->AddRect(imageMin, imageMax, IM_COL32(70, 70, 80, 255));
            }
        }
        draw->PopClipRect();
        if (clicked)
        {
            if (clickedPass)
            {
                m_SelectedPass    = clicked;
                m_SelectedVersion = 0;
                // Keep the resource selected when the new pass writes it; otherwise its first output.
                const auto* pass    = FindPass(clicked);
                const auto  outputs = pass ? Outputs(*pass) : std::vector<const CompiledGraphResource*> {};
                if (std::none_of(outputs.begin(), outputs.end(), [&](const auto* r) { return r->ID == m_SelectedResource; }))
                    m_SelectedResource = outputs.empty() ? 0 : outputs.front()->ID;
            }
            else
                for (const auto& node : m_Nodes)
                    if (node.ID == clicked)
                    {
                        // A version shows the image its writer produced.
                        m_SelectedVersion  = node.ID;
                        m_SelectedResource = m_Resources.at(node.Resource).ID;
                        m_SelectedPass     = node.Writer;
                    }
            m_Probe.reset();
            m_ProbeResult.reset();
        }
        else if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            m_SelectedPass = m_SelectedResource = m_SelectedVersion = 0;
    }

    void FrameGraphPanel::DrawInspector()
    {
        const auto* pass     = FindPass(m_SelectedPass);
        const auto* resource = FindGraphResource(m_SelectedResource);
        if (!pass && !resource)
        {
            ImGui::TextDisabled("Click a pass or a texture.");
            ImGui::Spacing();
            ImGui::TextDisabled("Middle/right drag: pan   Wheel: zoom");
            ImGui::TextDisabled("Orange: writes   Blue: reads");
            return;
        }
        if (pass)
        {
            ImGui::TextUnformatted(pass->Name.c_str());
            ImGui::TextDisabled("%s",
                                pass->Kind == GraphPassKind::Compute  ? "Compute" :
                                pass->Kind == GraphPassKind::Transfer ? "Transfer" :
                                                                        "Graphics");
            const auto* stats = FindStatistics(pass->ID);
            if (pass->Culled)
                ImGui::TextDisabled("Culled: its outputs are not used.");
            else
                ImGui::Text("Execution #%u", pass->ExecutionIndex);
            ImGui::Text("CPU %s   GPU %s", Milliseconds(stats ? stats->CpuRecordMilliseconds : std::nullopt).c_str(),
                        Milliseconds(stats ? stats->GpuMilliseconds : std::nullopt).c_str());
            if (stats && stats->Draws)
                ImGui::Text("%llu draws, %llu triangles", (unsigned long long)stats->Draws->DrawCalls,
                            (unsigned long long)stats->Draws->Triangles);
            if (ImGui::TreeNodeEx("Resources", ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const auto& use : pass->Uses)
                    if (use.Texture < m_Resources.size())
                    {
                        const auto& r = m_Resources[use.Texture];
                        ImGui::PushID(int(use.Texture));
                        if (ImGui::Selectable((r.Name + "  (" + AccessName(use.Access) + ")").c_str(),
                                              r.ID == m_SelectedResource))
                            m_SelectedResource = r.ID;
                        ImGui::PopID();
                    }
                ImGui::TreePop();
            }
            ImGui::Separator();
        }
        if (resource)
        {
            const auto& d = resource->Description;
            ImGui::TextUnformatted(resource->Name.c_str());
            if (resource->IsBuffer)
                ImGui::Text("Buffer: %zu bytes", resource->BufferDescription.Bytes);
            else
                ImGui::Text("%u x %u  %s  %u mip%s  %u layer%s",
                            d.Width,
                            d.Height,
                            FormatName(d.PixelFormat),
                            d.MipLevels,
                            d.MipLevels > 1 ? "s" : "",
                            d.ArrayLayers,
                            d.ArrayLayers > 1 ? "s" : "");
            ImGui::TextDisabled("%s%s", resource->Imported ? "Imported" : "Transient",
                                resource->PhysicalSlot != uint32_t(-1) ?
                                    (", aliased slot " + std::to_string(resource->PhysicalSlot)).c_str() :
                                    "");
            if (!pass && ImGui::TreeNodeEx("Used by", ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const auto& access : resource->DeclaredAccesses)
                    if (access.Pass < m_Passes.size())
                    {
                        const auto& user = m_Passes[access.Pass];
                        ImGui::PushID(int(access.Pass));
                        if (ImGui::Selectable((user.Name + "  (" + AccessName(access.Access) + ")").c_str()))
                            m_SelectedPass = user.ID;
                        ImGui::PopID();
                    }
                ImGui::TreePop();
            }
        }
        if (pass && resource)
        {
            const auto outputs = Outputs(*pass);
            if (std::find(outputs.begin(), outputs.end(), resource) != outputs.end())
                DrawImageInspector(*pass, *resource);
            else
                ImGui::TextDisabled("Select an output of this pass to see its image.");
        }
    }

    void FrameGraphPanel::DrawImageInspector(const CompiledGraphPass& pass, const CompiledGraphResource& resource)
    {
        if (resource.IsBuffer)
        {
            ImGui::TextDisabled("Buffer resource; no image preview.");
            return;
        }
        const auto& d     = resource.Description;
        const bool  depth = IsDepth(d.PixelFormat);
        ImGui::Separator();
        // Channels and range.
        const char* channels[] = {"R", "G", "B", "A"};
        for (int c = 0; c < 4; ++c)
        {
            if (c)
                ImGui::SameLine();
            bool on = (m_Display.ChannelMask >> c) & 1;
            if (ImGui::Checkbox(channels[c], &on))
                m_Display.ChannelMask = uint8_t(on ? m_Display.ChannelMask | (1 << c) : m_Display.ChannelMask & ~(1 << c));
        }
        ImGui::SameLine();
        const char* solo[] = {"Mask", "R only", "G only", "B only", "A only"};
        int         index  = m_Display.Solo + 1;
        ImGui::SetNextItemWidth(80);
        if (ImGui::Combo("##solo", &index, solo, 5))
            m_Display.Solo = index - 1;
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Exposure", &m_Display.Exposure, -10.0f, 10.0f, "%.1f stops");
        ImGui::SetNextItemWidth(160);
        ImGui::DragFloatRange2("Range", &m_Display.RangeMin, &m_Display.RangeMax, 0.01f, -100.0f, 1000.0f);
        ImGui::Checkbox("sRGB", &m_Display.SRGB);
        if (depth)
        {
            ImGui::SameLine();
            ImGui::Checkbox("Linear depth", &m_Display.LinearDepth);
            if (m_Display.LinearDepth)
            {
                ImGui::SetNextItemWidth(120);
                ImGui::DragFloat("Far (m)", &m_DepthFar, 0.5f, 0.1f, 10000.0f);
                m_Display.RangeMax = m_DepthFar;
            }
        }
        if (d.MipLevels > 1 || d.ArrayLayers > 1)
        {
            int mip = int(m_Mip), face = int(m_Face);
            ImGui::SetNextItemWidth(100);
            if (ImGui::SliderInt("Mip", &mip, 0, int(d.MipLevels) - 1))
                m_Mip = uint32_t(mip);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            if (ImGui::SliderInt("Layer", &face, 0, int(d.ArrayLayers) - 1))
                m_Face = uint32_t(face);
        }
        if (ImGui::SmallButton("Reset view"))
        {
            m_Display = {};
            if (depth)
            {
                m_Display.LinearDepth = true;
                m_Display.Solo        = 0;
                m_Display.RangeMax    = m_DepthFar;
            }
        }

        // The full-resolution capture of this output (requested for the next frame).
        const CapturedImage* image =
            m_Captures ? m_Captures->Find({pass.ID, resource.ID, {m_Mip, m_Face}}, true) : nullptr;
        if (!image)
        {
            ImGui::TextDisabled(m_Capture ? "Capturing..." : "Turn on Capture to see this image.");
            return;
        }
        if (!image->Unavailable.empty())
        {
            ImGui::TextDisabled("%s", image->Unavailable.c_str());
            return;
        }
        const uintptr_t id = Display(*image, m_Display);
        if (!id)
            return;
        const float  width  = ImGui::GetContentRegionAvail().x;
        const float  scale  = width / float(std::max(1u, image->Texture->GetDesc().Width));
        const ImVec2 extent = {width, float(image->Texture->GetDesc().Height) * scale};
        const ImVec2 at     = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(id), extent);
        // Pixel probe: the stored value under the cursor, read back without stalling.
        if (ImGui::IsItemHovered())
        {
            const ImVec2     mouse = ImGui::GetIO().MousePos;
            const glm::uvec2 pixel(uint32_t(std::clamp((mouse.x - at.x) / scale, 0.0f, float(image->Texture->GetDesc().Width - 1))),
                                   uint32_t(std::clamp((mouse.y - at.y) / scale, 0.0f, float(image->Texture->GetDesc().Height - 1))));
            if (!m_Probe && pixel != m_ProbePixel)
            {
                m_Probe      = m_Client->GetGraphCapture()->RequestPixel(*image, pixel);
                m_ProbePixel = pixel;
            }
        }
        if (m_Probe)
            if (auto result = m_Client->GetGraphCapture()->PollPixel(*m_Probe))
            {
                m_ProbeResult = std::move(result);
                m_Probe.reset();
            }
        if (m_ProbeResult)
        {
            const auto& v = m_ProbeResult->Value;
            ImGui::Text("Pixel (%u, %u)", m_ProbeResult->Pixel.x, m_ProbeResult->Pixel.y);
            if (depth)
                ImGui::Text("Depth %.6f", v[0]);
            else
                ImGui::Text("R %.4f  G %.4f  B %.4f  A %.4f", v[0], v[1], v[2], v[3]);
        }
        else
            ImGui::TextDisabled("Hover the image for the stored value.");
    }
} // namespace Kans
