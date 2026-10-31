#include "ClothPaintTool.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include "Kans3D/Editor/EditorCamera.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Scene/Scene.h"
#include "UI/EditorIcons.h"
#include "UI/Widgets.h"

namespace Kans
{
    namespace Edit = SecondaryMotionEditing;

    namespace
    {
        ImU32 Color(glm::vec4 c, float alpha = 1.0f)
        {
            return ImGui::ColorConvertFloat4ToU32({c.r, c.g, c.b, c.a * alpha});
        }

        std::pair<float, float> Range(const std::vector<float>& values)
        {
            if (values.empty())
                return {0.0f, 1.0f};
            const auto [low, high] = std::minmax_element(values.begin(), values.end());
            return {*low, *high};
        }

        float Normalized(float value, float low, float high) { return high - low > 1e-6f ? (value - low) / (high - low) : 0.5f; }
    } // namespace

    void ClothPaintTool::Begin(Entity entity, AssetID asset, ClothData data, std::filesystem::path file)
    {
        m_Session.Begin(entity.GetUUID(), asset, std::move(data), std::move(file));
        m_Neighbours = Edit::Neighbours(m_Session.GetData());
        m_Hover.reset();
        m_LastDab.reset();
        m_GradientStart.reset();
        m_GradientEnd.reset();
        m_Stroking       = false;
        m_RefreshedFrame = UINT64_MAX;
        m_Status.clear();
    }

    Entity ClothPaintTool::Owner(Scene& scene) const
    {
        return m_Session.IsActive() ? scene.GetEntityByUUID(m_Session.GetEntity()) : Entity {};
    }

    void ClothPaintTool::Cancel(Scene* scene)
    {
        if (scene)
            if (Entity owner = Owner(*scene))
                scene->SetClothSourceOverride(owner, nullptr);
        m_Session.Cancel();
        m_Neighbours.clear();
        m_Normals.clear();
        m_Hover.reset();
        m_Stroking = false;
    }

    bool ClothPaintTool::Save()
    {
        if (!m_Session.IsActive())
            return false;
        if (auto error = m_Session.Save())
        {
            m_Status = error->Message;
            return false;
        }
        m_Status = "Saved " + m_Session.GetFile().filename().u8string();
        if (m_Saved)
            m_Saved(m_Session.GetFile());
        return true;
    }

    void ClothPaintTool::Changed(Scene& scene)
    {
        m_Neighbours = Edit::Neighbours(m_Session.GetData());
        if (Entity owner = Owner(scene))
            scene.SetClothSourceOverride(owner, m_Session.MakeAsset());
    }

    bool ClothPaintTool::Undo(Scene& scene)
    {
        if (m_Stroking || !m_Session.Undo())
            return false;
        Changed(scene);
        return true;
    }

    bool ClothPaintTool::Redo(Scene& scene)
    {
        if (m_Stroking || !m_Session.Redo())
            return false;
        Changed(scene);
        return true;
    }

    bool ClothPaintTool::ToScreen(const View& view, glm::vec3 point, ImVec2& out) const
    {
        const glm::vec4 clip = view.Camera->GetViewProjection() * glm::vec4(point, 1.0f);
        if (clip.w <= 1e-4f)
            return false;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = {view.Min.x + (ndc.x * 0.5f + 0.5f) * (view.Max.x - view.Min.x),
               view.Min.y + (0.5f - ndc.y * 0.5f) * (view.Max.y - view.Min.y)};
        return true;
    }

    void ClothPaintTool::Ring(const View& view, glm::vec3 centre, glm::vec3 normal, float radius, ImU32 color) const
    {
        if (!(radius > 0))
            return;
        const glm::vec3 helper = std::abs(normal.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 u      = glm::normalize(glm::cross(helper, normal)), v = glm::cross(normal, u);
        ImDrawList*     draw   = ImGui::GetWindowDrawList();
        ImVec2          first, last;
        bool            haveLast = false;
        for (int i = 0; i <= 32; ++i)
        {
            const float angle = float(i) / 32.0f * 6.2831853f;
            ImVec2      at;
            if (!ToScreen(view, centre + (u * std::cos(angle) + v * std::sin(angle)) * radius, at))
            {
                haveLast = false;
                continue;
            }
            if (haveLast)
                draw->AddLine(last, at, color, 1.5f);
            else if (i == 0)
                first = at;
            last     = at;
            haveLast = true;
        }
        (void)first;
    }

    void ClothPaintTool::Refresh(const View& view)
    {
        const uint64_t frame = uint64_t(ImGui::GetFrameCount());
        if (m_RefreshedFrame == frame || !view.Scene || !view.Camera)
            return;
        m_RefreshedFrame = frame;
        Scene&       scene = *view.Scene;
        const Entity owner = Owner(scene);
        if (!owner)
            return;
        const auto& positions = m_Session.UpdatePose(scene, owner, scene.ResolveMesh(owner));
        // Area-weighted normals on the posed particles (backstop spheres, the brush ring).
        const auto& data = m_Session.GetData();
        m_Normals.assign(positions.size(), glm::vec3(0.0f));
        for (size_t t = 0; t + 2 < data.Triangles.size(); t += 3)
        {
            const uint32_t a = data.Triangles[t], b = data.Triangles[t + 1], c = data.Triangles[t + 2];
            if (a >= positions.size() || b >= positions.size() || c >= positions.size())
                continue;
            const glm::vec3 n = glm::cross(positions[b] - positions[a], positions[c] - positions[a]);
            m_Normals[a] += n, m_Normals[b] += n, m_Normals[c] += n;
        }
        for (auto& n : m_Normals)
            n = glm::length(n) > 1e-12f ? glm::normalize(n) : glm::vec3(0, 1, 0);
        const ImVec2    mouse = ImGui::GetIO().MousePos;
        const glm::vec2 pixel(mouse.x - view.Min.x, mouse.y - view.Min.y);
        const auto [origin, direction] = view.Camera->ScreenRay(pixel);
        m_Hover                        = Edit::PickParticle(data, positions, origin, direction);
    }

    bool ClothPaintTool::Update(const View& view, bool hovered)
    {
        if (!m_Session.IsActive() || !view.Scene || !view.Camera)
            return false;
        Refresh(view);
        Scene&         scene = *view.Scene;
        const ImGuiIO& io    = ImGui::GetIO();
        const auto&    positions = m_Session.GetPositions();
        const ClothMap map       = ActiveTool == Tool::Pin ? ClothMap::MaxDistance : m_Session.ActiveMap;
        const auto     dab       = [&](glm::vec3 at) {
            Edit::Brush brush = m_Brush;
            if (ActiveTool == Tool::Pin)
            {
                // Pins set Max Distance to 0; Shift unpins to the unpin distance.
                brush.Mode     = Edit::BrushMode::Set;
                brush.Value    = io.KeyShift ? m_UnpinValue : 0.0f;
                brush.Strength = 1.0f;
                brush.Falloff  = 0.0f;
            }
            Edit::ApplyBrush(map, m_Session.StrokeValues(), positions, m_Neighbours, at, brush);
            m_LastDab = at;
        };
        // A press on the cloth starts a stroke (Alt + drag stays camera orbit).
        if (!m_Stroking && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_Hover &&
            !ImGui::IsAnyItemHovered())
        {
            const auto saved = m_Session.ActiveMap;
            m_Session.ActiveMap = map;
            switch (ActiveTool)
            {
                case Tool::Brush:
                case Tool::Pin:
                    m_Session.BeginStroke();
                    dab(m_Hover->Point);
                    m_Stroking = true;
                    break;
                case Tool::Fill:
                {
                    m_Session.BeginStroke();
                    auto& values = m_Session.StrokeValues();
                    if (m_FillAll)
                        std::fill(values.begin(), values.end(), Edit::ClampMapValue(map, m_FillValue));
                    else
                        for (uint32_t i : Edit::ConnectedRegion(values, m_Neighbours, m_Hover->Particle))
                            values[i] = Edit::ClampMapValue(map, m_FillValue);
                    if (m_Session.EndStroke())
                        Changed(scene);
                    break;
                }
                case Tool::Gradient:
                    m_GradientStart = m_GradientEnd = m_Hover->Point;
                    m_Stroking                      = true;
                    break;
            }
            if (ActiveTool == Tool::Pin)
                m_Session.ActiveMap = saved;
        }
        if (!m_Stroking)
            return false;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (ActiveTool == Tool::Gradient)
            {
                if (m_Hover)
                    m_GradientEnd = m_Hover->Point;
            }
            else if (m_Hover && (!m_LastDab || glm::distance(*m_LastDab, m_Hover->Point) > m_Brush.Radius * 0.25f))
                dab(m_Hover->Point);
            return true;
        }
        // Released: the stroke becomes one step.
        m_Stroking = false;
        if (ActiveTool == Tool::Gradient)
        {
            if (m_GradientStart && m_GradientEnd && glm::distance(*m_GradientStart, *m_GradientEnd) > 1e-5f)
            {
                m_Session.BeginStroke();
                auto& values = m_Session.StrokeValues();
                Edit::ApplyGradient(values, positions, *m_GradientStart, *m_GradientEnd,
                                    Edit::ClampMapValue(map, m_GradientFrom), Edit::ClampMapValue(map, m_GradientTo));
                if (m_Session.EndStroke())
                    Changed(scene);
            }
            m_GradientStart.reset();
            m_GradientEnd.reset();
        }
        else
        {
            const auto saved = m_Session.ActiveMap;
            m_Session.ActiveMap = map;
            if (m_Session.EndStroke())
                Changed(scene);
            m_Session.ActiveMap = saved;
        }
        m_LastDab.reset();
        return true;
    }

    void ClothPaintTool::DrawOverlay(const View& view)
    {
        if (!m_Session.IsActive() || !view.Camera)
            return;
        Refresh(view);
        const auto&    data      = m_Session.GetData();
        const auto&    positions = m_Session.GetPositions();
        const ClothMap map       = ActiveTool == Tool::Pin ? ClothMap::MaxDistance : m_Session.ActiveMap;
        // While a stroke runs, its values show.
        const std::vector<float> values = m_Session.InStroke() ? m_Session.StrokeValues() : Edit::MapValues(data, map);
        const auto [low, high]          = Range(values);
        if (positions.size() != values.size())
            return;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(view.Min, view.Max, true);
        std::vector<ImVec2> screen(positions.size());
        std::vector<bool>   visible(positions.size());
        for (size_t i = 0; i < positions.size(); ++i)
            visible[i] = ToScreen(view, positions[i], screen[i]);
        // The map as vertex colours over the triangles.
        const ImVec2 white = draw->_Data->TexUvWhitePixel;
        for (size_t t = 0; t + 2 < data.Triangles.size(); t += 3)
        {
            const uint32_t ids[3] = {data.Triangles[t], data.Triangles[t + 1], data.Triangles[t + 2]};
            if (!visible[ids[0]] || !visible[ids[1]] || !visible[ids[2]])
                continue;
            draw->PrimReserve(3, 3);
            const ImDrawIdx base = ImDrawIdx(draw->_VtxCurrentIdx);
            for (uint32_t id : ids)
                draw->PrimWriteVtx(screen[id], white, Color(Edit::HeatColor(Normalized(values[id], low, high)), 0.6f));
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(ImDrawIdx(base + 1));
            draw->PrimWriteIdx(ImDrawIdx(base + 2));
        }
        // Pinned particles (Max Distance 0) as white squares.
        const float s = EditorTheme::Px(2.0f);
        for (size_t i = 0; i < positions.size(); ++i)
            if (visible[i] && i < data.MaxDistance.size() && data.MaxDistance[i] == 0.0f)
                draw->AddRectFilled({screen[i].x - s, screen[i].y - s}, {screen[i].x + s, screen[i].y + s},
                                    IM_COL32(255, 255, 255, 230));
        // The brush (or the gradient line) and the hovered particle's spheres.
        if (m_GradientStart && m_GradientEnd)
        {
            ImVec2 a, b;
            if (ToScreen(view, *m_GradientStart, a) && ToScreen(view, *m_GradientEnd, b))
            {
                draw->AddLine(a, b, IM_COL32(255, 255, 255, 255), 2.0f);
                draw->AddCircleFilled(a, s * 2, Color(Edit::HeatColor(0.0f)));
                draw->AddCircleFilled(b, s * 2, Color(Edit::HeatColor(1.0f)));
            }
        }
        else if (m_Hover && m_Hover->Particle < positions.size())
        {
            const glm::vec3 normal = m_Hover->Particle < m_Normals.size() ? m_Normals[m_Hover->Particle] : glm::vec3(0, 1, 0);
            if (ActiveTool == Tool::Brush || ActiveTool == Tool::Pin)
            {
                Ring(view, m_Hover->Point, normal, m_Brush.Radius, IM_COL32(255, 255, 255, 230));
                if (ActiveTool == Tool::Brush && m_Brush.Falloff > 0)
                    Ring(view, m_Hover->Point, normal, m_Brush.Radius * (1.0f - m_Brush.Falloff), IM_COL32(255, 255, 255, 110));
            }
            const uint32_t  p      = m_Hover->Particle;
            const glm::vec3 anchor = positions[p];
            float           scale  = 1.0f;
            if (view.Scene)
                if (Entity owner = Owner(*view.Scene); owner && owner.HasComponent<ClothComponent>())
                    scale = owner.GetComponent<ClothComponent>().MaxDistanceScale;
            if (m_ShowMaxDistance && p < data.MaxDistance.size())
                for (glm::vec3 axis : {normal, glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)})
                    Ring(view, anchor, axis, data.MaxDistance[p] * scale, IM_COL32(120, 220, 255, 200));
            if (m_ShowBackstop && p < data.BackstopRadius.size() && data.BackstopRadius[p] > 0)
            {
                // The backstop sphere sits behind the surface: its centre is distance + radius along -normal.
                const glm::vec3 centre = anchor - normal * (data.BackstopDistance[p] + data.BackstopRadius[p]);
                for (glm::vec3 axis : {normal, glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)})
                    Ring(view, centre, axis, data.BackstopRadius[p], IM_COL32(255, 170, 60, 200));
            }
            if (visible[p])
            {
                char text[64];
                std::snprintf(text, sizeof(text), "#%u  %.4g", p, values[p]);
                draw->AddText({screen[p].x + EditorTheme::Px(10.0f), screen[p].y - EditorTheme::Px(18.0f)},
                              IM_COL32(255, 255, 255, 255), text);
            }
        }
        draw->PopClipRect();
    }

    void ClothPaintTool::DrawPanel(ImVec2 position, const View& view)
    {
        if (!m_Session.IsActive())
            return;
        using EditorTheme::Px;
        Widgets::OverlayGroup(position, [&] {
            ImGui::PushID("ClothPaint");
            ImGui::Text(ICON_FA_PAINTBRUSH "  Cloth painting  %s%s", m_Session.GetFile().filename().u8string().c_str(),
                        m_Session.IsDirty() ? " *" : "");
            // Map.
            const char* maps[Edit::MapCount];
            for (int i = 0; i < Edit::MapCount; ++i)
                maps[i] = Edit::MapName(ClothMap(i));
            int map = int(m_Session.ActiveMap);
            ImGui::SetNextItemWidth(Px(200.0f));
            ImGui::BeginDisabled(m_Stroking || ActiveTool == Tool::Pin);
            if (ImGui::Combo("Map", &map, maps, Edit::MapCount))
                m_Session.ActiveMap = ClothMap(map);
            ImGui::EndDisabled();
            // Tools.
            const std::pair<Tool, const char*> tools[] = {{Tool::Brush, ICON_FA_PAINTBRUSH "  Brush"},
                                                          {Tool::Fill, ICON_FA_FILL_DRIP "  Fill"},
                                                          {Tool::Gradient, ICON_FA_ARROW_TREND_UP "  Gradient"},
                                                          {Tool::Pin, ICON_FA_THUMBTACK "  Pin"}};
            for (size_t i = 0; i < 4; ++i)
            {
                if (i)
                    ImGui::SameLine();
                const bool active = ActiveTool == tools[i].first;
                if (active)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(tools[i].second) && !m_Stroking)
                    ActiveTool = tools[i].first;
                if (active)
                    ImGui::PopStyleColor();
            }
            const auto number = [&](const char* label, float& value, float speed, float min, float max, const char* format) {
                ImGui::SetNextItemWidth(Px(110.0f));
                ImGui::DragFloat(label, &value, speed, min, max, format);
            };
            switch (ActiveTool)
            {
                case Tool::Brush:
                {
                    const char* modes[] = {"Add", "Subtract", "Set", "Smooth"};
                    int         mode    = int(m_Brush.Mode);
                    ImGui::SetNextItemWidth(Px(110.0f));
                    if (ImGui::Combo("Mode", &mode, modes, 4))
                        m_Brush.Mode = Edit::BrushMode(mode);
                    number("Radius", m_Brush.Radius, 0.002f, 0.001f, 100.0f, "%.3f m");
                    ImGui::SameLine();
                    number("Strength", m_Brush.Strength, 0.01f, 0.0f, 1.0f, "%.2f");
                    number("Falloff", m_Brush.Falloff, 0.01f, 0.0f, 1.0f, "%.2f");
                    if (m_Brush.Mode != Edit::BrushMode::Smooth)
                    {
                        ImGui::SameLine();
                        number("Value", m_Brush.Value, 0.001f, -100.0f, 100.0f, "%.3f");
                    }
                    break;
                }
                case Tool::Fill:
                    number("Value", m_FillValue, 0.001f, -100.0f, 100.0f, "%.3f");
                    ImGui::SameLine();
                    ImGui::Checkbox("Whole cloth", &m_FillAll);
                    ImGui::TextDisabled("Click: the connected region of the clicked value");
                    break;
                case Tool::Gradient:
                    number("Start", m_GradientFrom, 0.001f, -100.0f, 100.0f, "%.3f");
                    ImGui::SameLine();
                    number("End", m_GradientTo, 0.001f, -100.0f, 100.0f, "%.3f");
                    ImGui::TextDisabled("Drag from the start to the end on the cloth");
                    break;
                case Tool::Pin:
                    number("Radius", m_Brush.Radius, 0.002f, 0.001f, 100.0f, "%.3f m");
                    ImGui::SameLine();
                    number("Unpin to", m_UnpinValue, 0.001f, 0.0f, 100.0f, "%.3f m");
                    ImGui::TextDisabled("Drag: pin (Max Distance 0)   Shift+drag: unpin");
                    break;
            }
            // Legend: the active map's range.
            const ClothMap shown = ActiveTool == Tool::Pin ? ClothMap::MaxDistance : m_Session.ActiveMap;
            const auto [low, high] = Range(Edit::MapValues(m_Session.GetData(), shown));
            {
                const ImVec2 at    = ImGui::GetCursorScreenPos();
                const float  width = Px(200.0f), height = Px(8.0f);
                ImDrawList*  draw  = ImGui::GetWindowDrawList();
                for (int i = 0; i < 16; ++i)
                {
                    const float x0 = at.x + width * float(i) / 16.0f, x1 = at.x + width * float(i + 1) / 16.0f;
                    draw->AddRectFilledMultiColor({x0, at.y}, {x1, at.y + height}, Color(Edit::HeatColor(float(i) / 16.0f)),
                                                  Color(Edit::HeatColor(float(i + 1) / 16.0f)),
                                                  Color(Edit::HeatColor(float(i + 1) / 16.0f)),
                                                  Color(Edit::HeatColor(float(i) / 16.0f)));
                }
                ImGui::Dummy({width, height});
                ImGui::Text("%.4g", low);
                ImGui::SameLine(width - ImGui::CalcTextSize("0000000").x);
                ImGui::Text("%.4g", high);
            }
            ImGui::Checkbox("Max distance", &m_ShowMaxDistance);
            ImGui::SameLine();
            ImGui::Checkbox("Backstop", &m_ShowBackstop);
            // Import a vertex colour channel into the active map (meshes imported without the cloth option).
            if (ImGui::TreeNodeEx("Import", ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                const char* channels[] = {"R", "G", "B", "A"};
                ImGui::SetNextItemWidth(Px(60.0f));
                ImGui::Combo("Channel", &m_ImportChannel, channels, 4);
                ImGui::SameLine();
                number("Scale", m_ImportScale, 0.01f, 0.0f, 1000.0f, "%.3f");
                if (ImGui::Button("From vertex colours") && view.Scene && !m_Stroking)
                    if (Entity owner = Owner(*view.Scene))
                        if (auto mesh = view.Scene->ResolveMesh(owner))
                        {
                            m_Session.BeginStroke();
                            m_Session.StrokeValues() =
                                Edit::MapFromVertexColors(m_Session.GetData(), mesh->GetData(), m_ImportChannel, m_ImportScale);
                            for (auto& v : m_Session.StrokeValues())
                                v = Edit::ClampMapValue(m_Session.ActiveMap, v);
                            if (m_Session.EndStroke())
                                Changed(*view.Scene);
                        }
                ImGui::TreePop();
            }
            // Session.
            ImGui::BeginDisabled(!m_Session.CanUndo() || m_Stroking);
            if (ImGui::Button(ICON_FA_ROTATE_LEFT "##undo") && view.Scene)
                Undo(*view.Scene);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Undo stroke (Ctrl+Z)");
            ImGui::SameLine();
            ImGui::BeginDisabled(!m_Session.CanRedo() || m_Stroking);
            if (ImGui::Button(ICON_FA_ROTATE_RIGHT "##redo") && view.Scene)
                Redo(*view.Scene);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Redo stroke (Ctrl+Y)");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
                Save();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Write the .cloth file (Ctrl+S)");
            ImGui::SameLine();
            if (ImGui::Button("Done"))
                if (!m_Session.IsDirty() || Save())
                    Cancel(view.Scene);
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                Cancel(view.Scene);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Discard the unsaved strokes and leave painting");
            if (!m_Status.empty())
                ImGui::TextDisabled("%s", m_Status.c_str());
            ImGui::PopID();
        });
    }
} // namespace Kans
