#include "MeshEditTool.h"
#include <algorithm>
#include <set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/Editor/EditorCamera.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "UI/EditorIcons.h"
#include "UI/Widgets.h"

namespace Kans
{
    using Widgets::OverlayGroup;
    using Widgets::ToolButton;

    namespace
    {
        constexpr ImU32 WireColor     = IM_COL32(110, 185, 255, 150);
        constexpr ImU32 VertexColor   = IM_COL32(110, 185, 255, 230);
        constexpr ImU32 SelectedColor = IM_COL32(255, 160, 40, 255);
        constexpr ImU32 FaceFill      = IM_COL32(255, 150, 40, 80);
        constexpr float ClickDrag     = 4.0f; // Pixels before a press becomes a rectangle selection.

        // The editor camera projects to OpenGL depth (-1..1); the picking helpers use Vulkan's 0..1.
        glm::mat4 VulkanDepth()
        {
            glm::mat4 m(1.0f);
            m[2][2] = 0.5f;
            m[3][2] = 0.5f;
            return m;
        }

        ElementType TypeOf(MeshEditTool::Mode mode)
        {
            return mode == MeshEditTool::Mode::Vertex ? ElementType::Vertex :
                   mode == MeshEditTool::Mode::Edge   ? ElementType::Edge :
                                                        ElementType::Face;
        }

        size_t Count(const MeshSelection& s)
        {
            return s.Type == ElementType::Vertex ? s.Vertices.size() :
                   s.Type == ElementType::Edge   ? s.Edges.size() :
                                                   s.Faces.size();
        }
    } // namespace

    bool MeshEditTool::IsEditing(Entity entity) const
    {
        return m_Mode != Mode::Object && entity && entity.HasComponent<EditableMeshComponent>();
    }

    void MeshEditTool::Reset()
    {
        m_Mode = Mode::Object;
        m_Selection = {};
        m_Press.reset();
        m_Dragging = m_Manipulating = false;
    }

    void MeshEditTool::SetMode(Mode mode, Entity entity)
    {
        if (mode == m_Mode)
            return;
        Validate(entity);
        if (mode != Mode::Object && entity && entity.HasComponent<EditableMeshComponent>() && Count(m_Selection))
            m_Selection = ConvertSelection(entity.GetComponent<EditableMeshComponent>().Mesh, m_Selection, TypeOf(mode));
        else if (mode != Mode::Object)
        {
            m_Selection      = {};
            m_Selection.Type = TypeOf(mode);
        }
        m_Mode = mode;
    }

    void MeshEditTool::Validate(Entity entity)
    {
        const UUID id = entity ? entity.GetUUID() : UUID(0);
        if (id != m_Entity)
        {
            m_Entity    = id;
            m_Selection = {};
            m_Press.reset();
            m_Dragging = m_Manipulating = false;
        }
        if (m_Mode != Mode::Object)
            m_Selection.Type = TypeOf(m_Mode);
        if (!entity || !entity.HasComponent<EditableMeshComponent>())
            return;
        // Undo and other edits can shrink the mesh under the selection.
        const auto& mesh      = entity.GetComponent<EditableMeshComponent>().Mesh;
        const auto  positions = uint32_t(mesh.Positions.size());
        const auto  faces     = uint32_t(mesh.Faces.size());
        auto&       s         = m_Selection;
        s.Vertices.erase(std::remove_if(s.Vertices.begin(), s.Vertices.end(), [&](uint32_t v) { return v >= positions; }),
                         s.Vertices.end());
        s.Faces.erase(std::remove_if(s.Faces.begin(), s.Faces.end(), [&](uint32_t f) { return f >= faces; }),
                      s.Faces.end());
        s.Edges.erase(std::remove_if(s.Edges.begin(),
                                     s.Edges.end(),
                                     [&](const MeshEdge& e) { return e[0] >= positions || e[1] >= positions; }),
                      s.Edges.end());
    }

    void MeshEditTool::SetSelection(MeshSelection selection)
    {
        selection.Type = TypeOf(m_Mode);
        m_Selection    = std::move(selection);
    }

    bool MeshEditTool::Selected(uint32_t index) const
    {
        const auto& list = m_Selection.Type == ElementType::Vertex ? m_Selection.Vertices : m_Selection.Faces;
        return std::find(list.begin(), list.end(), index) != list.end();
    }

    bool MeshEditTool::Selected(MeshEdge edge) const
    {
        edge = SortedEdge(edge[0], edge[1]);
        return std::find(m_Selection.Edges.begin(), m_Selection.Edges.end(), edge) != m_Selection.Edges.end();
    }

    void MeshEditTool::Toggle(const MeshSelection& picked, bool add)
    {
        if (!add)
        {
            SetSelection(picked);
            return;
        }
        // Shift: each picked element is added, or removed when already selected.
        const auto flip = [](auto& list, const auto& value) {
            const auto found = std::find(list.begin(), list.end(), value);
            if (found == list.end())
                list.push_back(value);
            else
                list.erase(found);
        };
        for (auto v : picked.Vertices)
            flip(m_Selection.Vertices, v);
        for (auto f : picked.Faces)
            flip(m_Selection.Faces, f);
        for (auto e : picked.Edges)
            flip(m_Selection.Edges, SortedEdge(e[0], e[1]));
    }

    glm::mat4 MeshEditTool::ModelViewProjection(Entity entity, const View& view) const
    {
        return VulkanDepth() * view.Camera->GetViewProjection() * view.Scene->GetWorldTransform(entity);
    }

    MeshSelection MeshEditTool::PickAt(Entity entity, const View& view, glm::vec2 pixel) const
    {
        const auto&     mesh = entity.GetComponent<EditableMeshComponent>().Mesh;
        const glm::vec2 size(view.Max.x - view.Min.x, view.Max.y - view.Min.y);
        MeshSelection   picked;
        picked.Type = TypeOf(m_Mode);
        if (m_Mode == Mode::Vertex)
        {
            if (auto hit = PickVertex(mesh, ModelViewProjection(entity, view), size, pixel, 10.0f))
                picked.Vertices.push_back(hit->Vertex);
        }
        else if (m_Mode == Mode::Edge)
        {
            if (auto hit = PickEdge(mesh, ModelViewProjection(entity, view), size, pixel, 8.0f))
                picked.Edges.push_back(hit->Edge);
        }
        else
        {
            const auto [origin, direction] = view.Camera->ScreenRay(pixel);
            const glm::mat4 toLocal        = glm::inverse(view.Scene->GetWorldTransform(entity));
            MeshRay         ray {glm::vec3(toLocal * glm::vec4(origin, 1.0f)), glm::vec3(toLocal * glm::vec4(direction, 0.0f))};
            if (auto hit = RaycastFaces(mesh, ray))
                picked.Faces.push_back(hit->Face);
        }
        return picked;
    }

    MeshEditTool::Release MeshEditTool::Update(Entity entity, const View& view)
    {
        Validate(entity);
        if (!m_Press || !IsEditing(entity))
        {
            m_Press.reset();
            return Release::None;
        }
        const ImGuiIO& io    = ImGui::GetIO();
        const ImVec2   start = *m_Press, mouse = io.MousePos;
        if (std::abs(mouse.x - start.x) > ClickDrag || std::abs(mouse.y - start.y) > ClickDrag)
            m_Dragging = true;
        const ImVec2 lo {std::min(start.x, mouse.x), std::min(start.y, mouse.y)};
        const ImVec2 hi {std::max(start.x, mouse.x), std::max(start.y, mouse.y)};
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (m_Dragging)
            {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(lo, hi, IM_COL32(110, 185, 255, 40));
                draw->AddRect(lo, hi, IM_COL32(110, 185, 255, 200));
            }
            return Release::None;
        }
        // Released: a rectangle selects everything in it, a click the element under the cursor.
        const bool dragged = m_Dragging;
        m_Press.reset();
        m_Dragging = false;
        const glm::vec2 origin(view.Min.x, view.Min.y);
        if (dragged)
        {
            const glm::vec2 size(view.Max.x - view.Min.x, view.Max.y - view.Min.y);
            Toggle(SelectInRect(entity.GetComponent<EditableMeshComponent>().Mesh,
                                ModelViewProjection(entity, view),
                                size,
                                {glm::vec2(lo.x, lo.y) - origin, glm::vec2(hi.x, hi.y) - origin},
                                TypeOf(m_Mode)),
                   io.KeyShift);
            return Release::Handled;
        }
        const auto picked = PickAt(entity, view, glm::vec2(mouse.x, mouse.y) - origin);
        if (!Count(picked))
        {
            if (!io.KeyShift)
                SetSelection({});
            return Release::Missed;
        }
        Toggle(picked, io.KeyShift);
        return Release::Handled;
    }

    void MeshEditTool::DrawOverlay(Entity entity, const View& view)
    {
        Validate(entity);
        if (!IsEditing(entity))
            return;
        const auto&     mesh = entity.GetComponent<EditableMeshComponent>().Mesh;
        const glm::mat4 mvp  = ModelViewProjection(entity, view);
        const float     width = view.Max.x - view.Min.x, height = view.Max.y - view.Min.y;

        // Project every position once; positions behind the camera are not drawn.
        std::vector<glm::vec2> screen(mesh.Positions.size());
        std::vector<char>      visible(mesh.Positions.size(), 0);
        std::vector<glm::vec2> ndc(mesh.Positions.size());
        std::vector<char>      front(mesh.Positions.size(), 0);
        for (size_t i = 0; i < mesh.Positions.size(); ++i)
        {
            const glm::vec4 clip = mvp * glm::vec4(mesh.Positions[i], 1.0f);
            if (clip.w <= 1e-5f)
                continue;
            ndc[i]     = glm::vec2(clip) / clip.w;
            screen[i]  = {view.Min.x + (ndc[i].x + 1.0f) * 0.5f * width, view.Min.y + (1.0f - ndc[i].y) * 0.5f * height};
            visible[i] = 1;
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(view.Min, view.Max, true);
        const auto at = [&](uint32_t v) { return ImVec2(screen[v].x, screen[v].y); };

        // Faces facing the camera (counter-clockwise on screen) show their wires and vertices,
        // so the far side of a closed mesh does not clutter the view.
        std::set<MeshEdge> wires;
        for (uint32_t f = 0; f < mesh.Faces.size(); ++f)
        {
            const auto& loop = mesh.Faces[f].Indices;
            bool        shown = true;
            float       area  = 0.0f;
            for (size_t i = 0; i < loop.size() && shown; ++i)
            {
                const uint32_t a = loop[i], b = loop[(i + 1) % loop.size()];
                shown            = visible[a] && visible[b];
                area += ndc[a].x * ndc[b].y - ndc[b].x * ndc[a].y;
            }
            if (!shown || area <= 0.0f)
                continue;
            for (size_t i = 0; i < loop.size(); ++i)
            {
                wires.insert(SortedEdge(loop[i], loop[(i + 1) % loop.size()]));
                front[loop[i]] = 1;
            }
            if (m_Mode == Mode::Face && Selected(f))
                for (const auto& t : TriangulateFace(mesh, mesh.Faces[f]))
                    draw->AddTriangleFilled(at(t[0]), at(t[1]), at(t[2]), FaceFill);
        }
        for (const auto& edge : wires)
        {
            const bool selected = m_Mode == Mode::Edge && Selected(edge);
            draw->AddLine(at(edge[0]), at(edge[1]), selected ? SelectedColor : WireColor, selected ? 3.0f : 1.0f);
        }
        if (m_Mode == Mode::Face)
            for (auto f : m_Selection.Faces)
            {
                const auto& loop = mesh.Faces[f].Indices;
                for (size_t i = 0; i < loop.size(); ++i)
                    if (visible[loop[i]] && visible[loop[(i + 1) % loop.size()]])
                        draw->AddLine(at(loop[i]), at(loop[(i + 1) % loop.size()]), SelectedColor, 2.0f);
            }
        if (m_Mode == Mode::Vertex)
            for (uint32_t v = 0; v < mesh.Positions.size(); ++v)
            {
                const bool selected = Selected(v);
                if (!visible[v] || (!front[v] && !selected))
                    continue;
                const float  r = selected ? 4.0f : 3.0f;
                const ImVec2 p = at(v);
                draw->AddRectFilled({p.x - r, p.y - r}, {p.x + r, p.y + r}, selected ? SelectedColor : VertexColor);
            }
        draw->PopClipRect();
    }

    Gizmo::Event MeshEditTool::Manipulate(Entity                 entity,
                                          const View&            view,
                                          Gizmo::Mode            mode,
                                          Gizmo::Orientation     orientation,
                                          const Gizmo::Settings& settings,
                                          const Gizmo::Input&    input,
                                          bool                   extrude)
    {
        Validate(entity);
        m_GizmoShown = false;
        if (!IsEditing(entity))
        {
            m_Gizmo.Reset();
            m_Manipulating = false;
            return Gizmo::Event::None;
        }
        const auto& component = entity.GetComponent<EditableMeshComponent>();
        if (SelectionVertices(component.Mesh, m_Selection).empty() && !m_Gizmo.IsDragging())
        {
            m_Gizmo.Reset();
            return Gizmo::Event::None;
        }
        const glm::mat4   world = view.Scene->GetWorldTransform(entity);
        const Gizmo::View gv    = Gizmo::View::From(*view.Camera, {view.Min.x, view.Min.y},
                                                    {view.Max.x - view.Min.x, view.Max.y - view.Min.y});
        // Pivot: the selection's centre. Local orientation stands on the elements' normal, with
        // the entity's X kept as level as it can be.
        Gizmo::Pivot pivot;
        pivot.Position = glm::vec3(world * glm::vec4(SelectionCentroid(component.Mesh, m_Selection), 1.0f));
        pivot.Axes     = Gizmo::ResolveAxes(orientation, world, gv);
        if (orientation == Gizmo::Orientation::Local)
        {
            glm::vec3 normal = glm::transpose(glm::inverse(glm::mat3(world))) * SelectionNormal(component.Mesh, m_Selection);
            if (glm::length(normal) > 1e-6f)
            {
                normal         = glm::normalize(normal);
                glm::vec3 side = pivot.Axes[0] - normal * glm::dot(pivot.Axes[0], normal);
                if (glm::length(side) < 1e-4f)
                    side = pivot.Axes[1] - normal * glm::dot(pivot.Axes[1], normal);
                side       = glm::normalize(side);
                pivot.Axes = glm::mat3(side, glm::cross(normal, side), normal);
            }
        }
        m_GizmoShown     = true;
        const auto event = m_Gizmo.Update(gv, pivot, mode, settings, input);
        switch (event)
        {
            case Gizmo::Event::None:
                return event;
            case Gizmo::Event::Cancelled:
                entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& c) { c = m_Original; });
                m_Selection    = m_OriginalSelection;
                m_Manipulating = false;
                return event;
            case Gizmo::Event::Began:
            {
                m_Manipulating      = true;
                m_Original          = component;
                m_OriginalSelection = m_Selection;
                m_StartMesh         = component.Mesh;
                // Shift while starting a move extrudes the selected faces (or boundary edges) and moves the copy.
                if (extrude && m_Gizmo.GetDelta().Op == Gizmo::Operation::Translate &&
                    (m_Mode == Mode::Face || m_Mode == Mode::Edge))
                {
                    constexpr float seed = 1e-4f; // Extrusion needs a nonzero distance; the drag sets the real one.
                    auto            result =
                        m_Mode == Mode::Face ?
                                       ExtrudeFaces(m_StartMesh, m_Selection.Faces, seed,
                                                    m_ExtrudeIndividual ? ExtrudeMode::Individual : ExtrudeMode::Group) :
                                       ExtrudeEdges(m_StartMesh, m_Selection.Edges, seed);
                    if (!result)
                    {
                        Fail("Extrude: " + result.Error);
                        m_StartMesh = component.Mesh;
                    }
                    else if (m_Mode == Mode::Face)
                        m_Selection.Faces = result.Faces;
                    else
                        m_Selection.Edges = result.Edges;
                }
                break;
            }
            case Gizmo::Event::Dragged:
            case Gizmo::Event::Committed:
                break;
        }
        if (!m_Manipulating)
            return event;
        if (event == Gizmo::Event::Committed)
            m_Manipulating = false;
        // The drag so far, as a world-space change, applied to the mesh it started from.
        const glm::mat4 delta = glm::inverse(world) * m_Gizmo.GetDelta().Matrix() * world;
        EditableMesh    mesh  = m_StartMesh;
        if (auto result = TransformVertices(mesh, SelectionVertices(mesh, m_Selection), delta); !result)
        {
            Fail("Transform: " + result.Error);
            return event;
        }
        entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& c) {
            c.Mesh = std::move(mesh);
            c.Shape.reset();
        });
        return event;
    }

    void MeshEditTool::AppendGizmoDraws(std::vector<GizmoDraw>& out) const
    {
        if (m_GizmoShown)
            m_Gizmo.AppendDraws(out);
    }

    bool MeshEditTool::Apply(Entity entity, const char* name, const Operation& operation, bool reselect)
    {
        if (!entity || !entity.HasComponent<EditableMeshComponent>())
            return false;
        EditableMesh mesh   = entity.GetComponent<EditableMeshComponent>().Mesh;
        auto         result = operation(mesh);
        if (!result)
        {
            Fail(std::string(name) + ": " + result.Error);
            return false;
        }
        entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& c) {
            c.Mesh = std::move(mesh);
            c.Shape.reset(); // Element edits replace the generated shape.
        });
        if (reselect)
        {
            MeshSelection next;
            next.Faces = result.Faces;
            next.Edges = result.Edges;
            SetSelection(std::move(next));
        }
        else if (!result.Remap.empty())
        {
            // Vertex indices moved: keep the selected vertices that survived.
            MeshSelection next;
            for (auto v : m_Selection.Vertices)
                if (v < result.Remap.size() && result.Remap[v] != UINT32_MAX)
                    next.Vertices.push_back(result.Remap[v]);
            std::sort(next.Vertices.begin(), next.Vertices.end());
            next.Vertices.erase(std::unique(next.Vertices.begin(), next.Vertices.end()), next.Vertices.end());
            SetSelection(m_Mode == Mode::Vertex ? std::move(next) : MeshSelection {});
        }
        m_Error.clear();
        return true;
    }

    void MeshEditTool::Fail(const std::string& message)
    {
        m_Error     = message;
        m_ErrorTime = std::chrono::steady_clock::now();
    }

    void MeshEditTool::DrawModeButtons(Entity entity)
    {
        const bool editable = entity && entity.HasComponent<EditableMeshComponent>();
        const std::pair<Mode, std::pair<const char*, const char*>> modes[] = {
            {Mode::Object, {ICON_FA_CUBE, "Object mode (1)"}},
            {Mode::Vertex, {ICON_FA_CIRCLE_DOT, "Vertex mode (2)"}},
            {Mode::Edge, {ICON_FA_MINUS, "Edge mode (3)"}},
            {Mode::Face, {ICON_FA_SQUARE, "Face mode (4)\nSelect editable mesh faces; Shift+move extrudes"}},
        };
        bool first = true;
        for (const auto& [mode, info] : modes)
        {
            if (!first)
                ImGui::SameLine();
            first = false;
            const bool active = editable ? m_Mode == mode : mode == Mode::Object;
            if (ToolButton(info.first, info.second, active, editable))
                SetMode(mode, entity);
        }
    }

    void MeshEditTool::HandleShortcuts(Entity entity)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!entity || !entity.HasComponent<EditableMeshComponent>() || io.WantTextInput)
            return;
        if (!io.KeyCtrl)
        {
            const std::pair<ImGuiKey, Mode> keys[] = {
                {ImGuiKey_1, Mode::Object}, {ImGuiKey_2, Mode::Vertex}, {ImGuiKey_3, Mode::Edge}, {ImGuiKey_4, Mode::Face}};
            for (const auto& [key, mode] : keys)
                if (ImGui::IsKeyPressed(key, false))
                    SetMode(mode, entity);
        }
        if (!IsEditing(entity))
            return;
        const auto& mesh = entity.GetComponent<EditableMeshComponent>().Mesh;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
        {
            MeshSelection all;
            if (m_Mode == Mode::Vertex)
                for (uint32_t v = 0; v < mesh.Positions.size(); ++v)
                    all.Vertices.push_back(v);
            else if (m_Mode == Mode::Face)
                for (uint32_t f = 0; f < mesh.Faces.size(); ++f)
                    all.Faces.push_back(f);
            else
                for (const auto& edge : mesh.Edges())
                    all.Edges.push_back(edge.Vertices);
            SetSelection(std::move(all));
        }
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_Mode == Mode::Face &&
            !m_Selection.Faces.empty())
        {
            const auto faces = m_Selection.Faces;
            if (Apply(entity, "Delete faces", [&](EditableMesh& m) { return DeleteFaces(m, faces); }))
                SetSelection({});
        }
    }

    void MeshEditTool::DrawPanel(Entity entity, ImVec2 position)
    {
        if (!IsEditing(entity))
            return;
        OverlayGroup(position, [&] {
            static const char* names[] = {"", "vertices", "edges", "faces"};
            ImGui::Text("%zu %s selected", Count(m_Selection), names[int(m_Mode)]);
            ImGui::TextDisabled("Shift: add/remove   Drag: rectangle   Ctrl+A: all");
            const bool any       = Count(m_Selection) > 0;
            const auto selection = m_Selection;
            const auto number    = [](const char* id, float& value, float speed, float min, float max) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f);
                ImGui::DragFloat(id, &value, speed, min, max, "%.3f");
            };
            ImGui::BeginDisabled(!any);
            if (m_Mode == Mode::Face)
            {
                if (ImGui::Button(ICON_FA_EXPAND "  Extrude"))
                    Apply(entity, "Extrude", [&](EditableMesh& m) {
                        return ExtrudeFaces(m, selection.Faces, m_ExtrudeDistance,
                                            m_ExtrudeIndividual ? ExtrudeMode::Individual : ExtrudeMode::Group);
                    }, true);
                number("##extrude", m_ExtrudeDistance, 0.01f, -100.0f, 100.0f);
                ImGui::SameLine();
                ImGui::Checkbox("Each face", &m_ExtrudeIndividual);
                if (ImGui::Button(ICON_FA_COMPRESS "  Inset"))
                    Apply(entity, "Inset", [&](EditableMesh& m) { return InsetFaces(m, selection.Faces, m_InsetAmount); },
                          true);
                number("##inset", m_InsetAmount, 0.005f, 0.001f, 100.0f);
                if (ImGui::Button(ICON_FA_RIGHT_LEFT "  Flip normals"))
                    Apply(entity, "Flip", [&](EditableMesh& m) { return FlipFaces(m, selection.Faces); });
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_TRASH "  Delete"))
                    if (Apply(entity, "Delete faces", [&](EditableMesh& m) { return DeleteFaces(m, selection.Faces); }))
                        SetSelection({});
                if (ImGui::Button("Smooth"))
                    Apply(entity, "Smooth", [&](EditableMesh& m) { return SetSmoothingGroup(m, selection.Faces, 1); });
                ImGui::SameLine();
                if (ImGui::Button("Flat"))
                    Apply(entity, "Flat", [&](EditableMesh& m) { return SetSmoothingGroup(m, selection.Faces, 0); });
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f);
                ImGui::InputInt("##slot", &m_MaterialSlot);
                m_MaterialSlot = std::clamp(m_MaterialSlot, 0, 63);
                ImGui::SameLine();
                if (ImGui::Button("Set material slot"))
                    Apply(entity, "Material", [&](EditableMesh& m) {
                        return SetFaceMaterial(m, selection.Faces, uint32_t(m_MaterialSlot));
                    });
                ImGui::SetNextItemWidth(130.0f);
                ImGui::DragFloat2("UV scale", glm::value_ptr(m_UV.Scale), 0.01f);
                ImGui::SetNextItemWidth(130.0f);
                ImGui::DragFloat2("UV offset", glm::value_ptr(m_UV.Offset), 0.01f);
                ImGui::SetNextItemWidth(130.0f);
                ImGui::DragFloat("UV rotation", &m_UV.Rotation, 0.5f, -360.0f, 360.0f, "%.1f deg");
                if (ImGui::Button("Apply UV"))
                    Apply(entity, "UV", [&](EditableMesh& m) { return SetFaceUV(m, selection.Faces, m_UV); });
            }
            else if (m_Mode == Mode::Edge)
            {
                if (ImGui::Button(ICON_FA_EXPAND "  Extrude edges"))
                    Apply(entity, "Extrude edges", [&](EditableMesh& m) {
                        return ExtrudeEdges(m, selection.Edges, m_ExtrudeDistance);
                    }, true);
                number("##extrudeEdges", m_ExtrudeDistance, 0.01f, 0.001f, 100.0f);
                ImGui::TextDisabled("Open (boundary) edges only.");
            }
            else
            {
                if (ImGui::Button(ICON_FA_OBJECT_GROUP "  Merge"))
                    Apply(entity, "Merge", [&](EditableMesh& m) { return MergeVertices(m, selection.Vertices); });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Collapse the selected vertices to their centre");
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_LINK "  Weld"))
                    Apply(entity, "Weld", [&](EditableMesh& m) {
                        return WeldVertices(m, selection.Vertices, m_WeldDistance);
                    });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Join selected vertices closer than the distance");
                number("##weld", m_WeldDistance, 0.001f, 0.0f, 10.0f);
            }
            ImGui::EndDisabled();
            if (!m_Error.empty() && std::chrono::steady_clock::now() - m_ErrorTime < std::chrono::seconds(5))
                ImGui::TextColored({0.95f, 0.45f, 0.40f, 1.0f}, "%s", m_Error.c_str());
        });
    }
} // namespace Kans
