#include <imgui.h>
#include <cmath>
#include <functional>
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Scene/Components.h"
#include "ImGuizmo.h"
#include "Physics/PhysicsEditing.h"
#include "Physics/SecondaryMotionEditing.h"
#include "Physics/VehicleFieldEditing.h"
#include "UI/EditorIcons.h"
#include "ViewportPanel.h"

namespace Kans
{
    namespace
    {
        ImU32 Color(glm::vec4 c) { return ImGui::ColorConvertFloat4ToU32({c.r, c.g, c.b, c.a}); }

        constexpr glm::vec4 SelectedCollider {0.35f, 1.0f, 0.45f, 1.0f};
        constexpr glm::vec4 OtherCollider {0.35f, 1.0f, 0.45f, 0.45f};
        constexpr glm::vec4 Controller {0.4f, 0.85f, 1.0f, 1.0f};

        // Every entity of the scene, parents before children.
        void ForEachEntity(Scene& scene, const std::function<void(Entity)>& visit)
        {
            std::function<void(Entity)> walk = [&](Entity e) {
                visit(e);
                for (Entity child : scene.GetChildren(e))
                    walk(child);
            };
            for (Entity root : scene.GetRootEntities())
                walk(root);
        }
    } // namespace

    void ViewportPanel::OverlaySegment(ImDrawList* draw, glm::vec3 a, glm::vec3 b, ImU32 color, float thickness) const
    {
        const glm::mat4 viewProjection = ViewProjection();
        glm::vec4       ca = viewProjection * glm::vec4(a, 1.0f), cb = viewProjection * glm::vec4(b, 1.0f);
        constexpr float nearW = 1e-3f;
        if (ca.w < nearW && cb.w < nearW)
            return;
        // Cut the part behind the camera off at w = nearW.
        if (ca.w < nearW)
            ca = glm::mix(ca, cb, (nearW - ca.w) / (cb.w - ca.w));
        else if (cb.w < nearW)
            cb = glm::mix(cb, ca, (nearW - cb.w) / (ca.w - cb.w));
        const auto screen = [&](const glm::vec4& clip) {
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            return ImVec2 {m_Min.x + (ndc.x * 0.5f + 0.5f) * (m_Max.x - m_Min.x),
                           m_Min.y + (0.5f - ndc.y * 0.5f) * (m_Max.y - m_Min.y)};
        };
        draw->AddLine(screen(ca), screen(cb), color, thickness);
    }

    void ViewportPanel::DrawPhysicsMenu()
    {
        ImGui::TextDisabled("Physics");
        const std::pair<PhysicsDebugFlags, const char*> flags[] = {
            {PhysicsDebugFlags::Colliders, ICON_FA_BOX "  Colliders"},
            {PhysicsDebugFlags::Contacts, ICON_FA_CROSSHAIRS "  Contacts (playing)"},
            {PhysicsDebugFlags::Sleeping, ICON_FA_CIRCLE "  Sleeping bodies (playing)"},
            {PhysicsDebugFlags::Joints, ICON_FA_LINK "  Joints"},
            {PhysicsDebugFlags::CentersOfMass, ICON_FA_WEIGHT_HANGING "  Centers of mass (playing)"},
            {PhysicsDebugFlags::Ragdolls, ICON_FA_PERSON_FALLING "  Ragdolls (playing)"},
            {PhysicsDebugFlags::Vehicles, ICON_FA_CAR "  Vehicles"},
            {PhysicsDebugFlags::Fields, ICON_FA_TORNADO "  Fields"},
            {PhysicsDebugFlags::FieldArrows, ICON_FA_WIND "  Field arrows"},
        };
        for (const auto& [flag, name] : flags)
        {
            bool on = uint32_t(m_PhysicsView) & uint32_t(flag);
            if (ImGui::Checkbox(name, &on))
                m_PhysicsView = PhysicsDebugFlags(on ? uint32_t(m_PhysicsView) | uint32_t(flag) :
                                                       uint32_t(m_PhysicsView) & ~uint32_t(flag));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This viewport only; Project Settings > Physics has the project's own flags");
    }

    void ViewportPanel::DrawPhysicsOverlay()
    {
        m_PhysicsHandleHot = false;
        if (!m_Scene)
            return;
        // While the scene runs, the physics world draws what this viewport asks for as well.
        m_Scene->SetPhysicsDebugView(m_PhysicsView);
        if (UsesSceneCamera())
            return;
        Scene&     scene    = *m_Scene;
        Entity     selected = m_Hierarchy.GetSelected();
        const bool running  = scene.GetPhysicsWorld() != nullptr;
        DebugDraw  lines;
        if (!running)
        {
            // Outside play the toggles draw the authored shapes (the world draws its own while running).
            const bool colliders = uint32_t(m_PhysicsView) & uint32_t(PhysicsDebugFlags::Colliders);
            const bool joints    = uint32_t(m_PhysicsView) & uint32_t(PhysicsDebugFlags::Joints);
            const bool vehicles  = uint32_t(m_PhysicsView) & uint32_t(PhysicsDebugFlags::Vehicles);
            const bool fields    = uint32_t(m_PhysicsView) & uint32_t(PhysicsDebugFlags::Fields);
            if (colliders || joints || vehicles || fields)
                ForEachEntity(scene, [&](Entity e) {
                    if (e == selected)
                        return;
                    if (vehicles)
                        VehicleFieldEditing::DrawVehicle(lines, scene, e, -1);
                    if (fields)
                        VehicleFieldEditing::DrawField(lines, scene, e);
                });
            if (colliders || joints)
                ForEachEntity(scene, [&](Entity e) {
                    if (e == selected)
                        return;
                    if (colliders)
                    {
                        PhysicsEditing::DrawColliders(lines, scene, e, OtherCollider);
                        SecondaryMotionEditing::DrawMotionCollider(
                            lines, scene, e, SecondaryMotionEditing::ColliderColor * glm::vec4(1, 1, 1, 0.45f));
                        PhysicsEditing::DrawController(lines, scene, e, Controller * glm::vec4(1, 1, 1, 0.45f));
                    }
                    if (joints)
                        PhysicsEditing::DrawJoint(lines, scene, e);
                });
        }
        if (selected)
        {
            PhysicsEditing::DrawColliders(lines, scene, selected, SelectedCollider);
            PhysicsEditing::DrawController(lines, scene, selected, Controller);
            PhysicsEditing::DrawJoint(lines, scene, selected);
            // Spring chains and every collider their groups use; a motion collider itself.
            SecondaryMotionEditing::DrawSecondaryMotion(lines, scene, selected);
            // Wheels (the card's selected row highlighted) and field shapes, outside play too.
            if (!running)
                VehicleFieldEditing::DrawVehicle(lines, scene, selected, m_SelectedWheel ? m_SelectedWheel(selected) : -1);
            VehicleFieldEditing::DrawField(lines, scene, selected);
        }
        // Field arrows: while playing the world samples around each field; in edit mode a grid
        // around the selection samples the fields the scene would run with.
        if (!running && (uint32_t(m_PhysicsView) & uint32_t(PhysicsDebugFlags::FieldArrows)))
        {
            const ForceFieldSet fields = VehicleFieldEditing::BuildFieldSet(scene);
            const glm::vec3     centre = selected ? glm::vec3(scene.GetWorldTransform(selected)[3]) : m_Camera.GetFocus();
            VehicleFieldEditing::DrawFieldArrows(lines, fields, centre, 7, 0.5f, ImGui::GetTime());
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(m_Min, m_Max, true);
        for (const auto& line : lines.GetLines())
            OverlaySegment(draw, line.A, line.B, Color(line.Color), 1.5f);
        draw->PopClipRect();
        // Handles edit authored data, so not while the scene runs.
        if (selected && !m_Playing && !m_MeshTool.IsEditing(selected) && !IsPaintingCloth())
            PhysicsHandles(selected);
        else
            m_PhysicsDrag.reset();
    }

    void ViewportPanel::PhysicsHandles(Entity selected)
    {
        Scene&         scene     = *m_Scene;
        ImDrawList*    draw      = ImGui::GetWindowDrawList();
        const ImGuiIO& io        = ImGui::GetIO();
        const float    radius    = 5.0f;
        bool           anyActive = false;
        // While the transform gizmo drags, the handles only show.
        const bool gizmoBusy = ImGuizmo::IsUsing();
        // One handle: a dot that drags along `direction` (world) from `position` (world).
        const auto handle = [&](ImGuiID                                               id,
                                glm::vec3                                             position,
                                glm::vec3                                             along,
                                glm::vec4                                             tint,
                                const std::function<void(const PhysicsDrag&, float)>& drag,
                                const std::function<PhysicsDrag::ShapeVariant()>&     capture,
                                const char*                                           tooltip) {
            ImVec2 at, ahead;
            if (!ToScreen(position, at) || !ToScreen(position + along, ahead))
                return;
            if (gizmoBusy)
            {
                draw->AddCircleFilled(at, radius, Color(tint));
                return;
            }
            ImGui::SetCursorScreenPos({at.x - radius - 2, at.y - radius - 2});
            ImGui::PushID(int(id));
            ImGui::InvisibleButton("##physicsHandle", {2 * radius + 4, 2 * radius + 4});
            const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
            if (ImGui::IsItemActivated())
                m_PhysicsDrag = PhysicsDrag {id, io.MousePos, {ahead.x - at.x, ahead.y - at.y}, capture()};
            if (active && m_PhysicsDrag && m_PhysicsDrag->Handle == id)
            {
                // The pointer's travel along the handle's screen direction, in shape units.
                const glm::vec2 travel {io.MousePos.x - m_PhysicsDrag->Start.x, io.MousePos.y - m_PhysicsDrag->Start.y};
                const glm::vec2 unit    = m_PhysicsDrag->PixelsPerUnit;
                const float     length2 = glm::dot(unit, unit);
                drag(*m_PhysicsDrag, length2 > 1e-6f ? glm::dot(travel, unit) / length2 : 0.0f);
                anyActive = true;
            }
            if (hovered && !active && tooltip)
                ImGui::SetTooltip("%s", tooltip);
            ImGui::PopID();
            m_PhysicsHandleHot |= hovered || active;
            const ImU32 fill = hovered || active ? IM_COL32(255, 255, 255, 255) : Color(tint);
            draw->AddCircleFilled(at, radius, fill);
            draw->AddCircle(at, radius, IM_COL32(20, 20, 20, 220), 0, 1.5f);
        };

        // Primitive colliders: the handle pulls in offset space, mapped through the world.
        const auto shapeHandles = [&](auto tag, int kind) {
            using T = typename decltype(tag)::type;
            if (!selected.HasComponent<T>())
                return;
            const auto&     shape  = selected.GetComponent<T>().Shape;
            const glm::mat4 matrix = scene.GetWorldTransform(selected) * shape.Offset.Matrix();
            for (const auto& h : PhysicsEditing::Handles(shape))
            {
                const glm::vec3 position = glm::vec3(matrix * glm::vec4(h.Position, 1.0f));
                const glm::vec3 along    = glm::vec3(matrix * glm::vec4(h.Direction, 0.0f));
                handle(
                    ImGuiID(kind * 16 + h.ID),
                    position,
                    along,
                    SelectedCollider,
                    [&, id = h.ID](const PhysicsDrag& d, float distance) {
                        using Shape   = std::decay_t<decltype(shape)>;
                        const Shape s = PhysicsEditing::DragHandle(std::get<Shape>(d.Shape), id, distance);
                        selected.PatchComponent<T>([&](T& c) { c.Shape = s; });
                    },
                    [&] { return PhysicsDrag::ShapeVariant(shape); },
                    "Drag to resize the collider");
            }
        };
        shapeHandles(std::common_type<BoxColliderComponent> {}, 1);
        shapeHandles(std::common_type<SphereColliderComponent> {}, 2);
        shapeHandles(std::common_type<CapsuleColliderComponent> {}, 3);

        // Motion colliders (sphere, capsule, box faces and the plane along its normal).
        if (selected.HasComponent<MotionColliderComponent>())
        {
            const auto&     shape  = selected.GetComponent<MotionColliderComponent>().Shape;
            const auto      offset = std::visit([](const auto& s) { return s.Offset; }, shape);
            const glm::mat4 matrix = scene.GetWorldTransform(selected) * offset.Matrix();
            for (const auto& h : SecondaryMotionEditing::Handles(shape))
                handle(
                    ImGuiID(5 * 16 + h.ID),
                    glm::vec3(matrix * glm::vec4(h.Position, 1.0f)),
                    glm::vec3(matrix * glm::vec4(h.Direction, 0.0f)),
                    SecondaryMotionEditing::ColliderColor,
                    [&, id = h.ID](const PhysicsDrag& d, float distance) {
                        const auto start = std::visit(
                            [](const auto& s) -> std::optional<Physics::MotionColliderShape> {
                                using S = std::decay_t<decltype(s)>;
                                if constexpr (std::is_same_v<S, Physics::Box> || std::is_same_v<S, Physics::Sphere> ||
                                              std::is_same_v<S, Physics::Capsule> || std::is_same_v<S, Physics::Plane>)
                                    return Physics::MotionColliderShape(s);
                                else
                                    return std::nullopt;
                            },
                            d.Shape);
                        if (!start)
                            return;
                        const auto resized = SecondaryMotionEditing::DragHandle(*start, id, distance);
                        selected.PatchComponent<MotionColliderComponent>([&](auto& c) { c.Shape = resized; });
                    },
                    [&] {
                        return std::visit([](const auto& s) { return PhysicsDrag::ShapeVariant(s); }, shape);
                    },
                    std::holds_alternative<Physics::Plane>(shape) ? "Drag to move the plane along its normal" :
                                                                    "Drag to resize the collider");
        }

        // Vehicle wheels: the radius on top and the attach position along the chassis axes, for the
        // card's selected wheel (every wheel while none is selected).
        if (selected.HasComponent<VehicleComponent>())
        {
            const auto&     wheels = selected.GetComponent<VehicleComponent>().Wheels;
            const glm::mat4 frame  = VehicleFieldEditing::ChassisFrame(scene, selected);
            const int       chosen = m_SelectedWheel ? m_SelectedWheel(selected) : -1;
            const char*     tips[] = {"Drag to set the wheel radius", "Drag to move the wheel along X",
                                      "Drag to move the wheel along Y", "Drag to move the wheel along Z"};
            for (size_t w = 0; w < wheels.size(); ++w)
            {
                if (chosen >= 0 && int(w) != chosen)
                    continue;
                for (const auto& h : VehicleFieldEditing::WheelHandles(wheels[w]))
                    handle(
                        ImGuiID(1000 + w * 8 + size_t(h.ID)),
                        glm::vec3(frame * glm::vec4(h.Position, 1.0f)),
                        glm::vec3(frame * glm::vec4(h.Direction, 0.0f)),
                        h.ID == 0 ? VehicleFieldEditing::WheelColor : glm::vec4(h.Direction * 0.8f + 0.2f, 1.0f),
                        [&, w, id = h.ID](const PhysicsDrag& d, float distance) {
                            const auto* start = std::get_if<VehicleWheel>(&d.Shape);
                            if (!start)
                                return;
                            const auto moved = VehicleFieldEditing::DragWheelHandle(*start, id, distance);
                            selected.PatchComponent<VehicleComponent>([&](VehicleComponent& c) {
                                if (w < c.Wheels.size())
                                    c.Wheels[w] = moved;
                            });
                        },
                        [&, w] { return PhysicsDrag::ShapeVariant(wheels[w]); },
                        tips[h.ID]);
            }
        }

        // Wind zones and force fields: radius or box faces, and a wind zone's falloff band.
        const auto fieldHandles = [&](auto tag, int kind) {
            using T = typename decltype(tag)::type;
            if (!selected.HasComponent<T>())
                return;
            const auto      values = VehicleFieldEditing::ShapeOf(selected.GetComponent<T>());
            const glm::mat4 world  = scene.GetWorldTransform(selected);
            for (const auto& h : VehicleFieldEditing::FieldHandles(values))
                handle(
                    ImGuiID(kind * 16 + h.ID),
                    glm::vec3(world * glm::vec4(h.Position, 1.0f)),
                    glm::vec3(world * glm::vec4(h.Direction, 0.0f)),
                    h.ID == VehicleFieldEditing::BandHandle ? glm::vec4(1.0f, 0.9f, 0.3f, 1.0f) : VehicleFieldEditing::FieldColor,
                    [&, id = h.ID](const PhysicsDrag& d, float distance) {
                        const auto* start = std::get_if<VehicleFieldEditing::FieldShapeValues>(&d.Shape);
                        if (!start)
                            return;
                        const auto resized = VehicleFieldEditing::DragFieldHandle(*start, id, distance);
                        selected.PatchComponent<T>([&](T& c) { VehicleFieldEditing::Apply(c, resized); });
                    },
                    [&] { return PhysicsDrag::ShapeVariant(values); },
                    h.ID == VehicleFieldEditing::BandHandle ? "Drag to set the falloff band (full strength inside)" :
                                                              "Drag to resize the field");
        };
        fieldHandles(std::common_type<WindZoneComponent> {}, 7);
        fieldHandles(std::common_type<ForceFieldComponent> {}, 8);

        // Hinge limits: the arc's two ends follow the pointer around the hinge axis.
        if (selected.HasComponent<JointComponent>() && selected.GetComponent<JointComponent>().Type == JointType::Hinge)
        {
            const auto&     joint = selected.GetComponent<JointComponent>();
            const glm::mat4 frame = PhysicsEditing::JointFrame(scene.GetWorldTransform(selected));
            for (int end = 0; end < 2; ++end)
            {
                const float     degrees  = end ? joint.HingeUpperAngle : joint.HingeLowerAngle;
                const glm::vec3 position = PhysicsEditing::HingeArcPoint(frame, degrees);
                handle(
                    ImGuiID(4 * 16 + end),
                    position,
                    glm::vec3(0.0f),
                    {1.0f, 0.8f, 0.1f, 1.0f},
                    [&, end](const PhysicsDrag&, float) {
                        const glm::vec2 pixel(io.MousePos.x - m_Min.x, io.MousePos.y - m_Min.y);
                        const auto [origin, direction] = m_Camera.ScreenRay(pixel);
                        auto angle                     = PhysicsEditing::HingeAngleAt(frame, origin, direction);
                        if (!angle)
                            return;
                        float value = *angle;
                        if (m_GizmoSnap || io.KeyCtrl)
                            value = std::round(value / m_SnapRotate) * m_SnapRotate;
                        selected.PatchComponent<JointComponent>([&](JointComponent& c) {
                            if (end)
                                c.HingeUpperAngle = std::max(value, c.HingeLowerAngle);
                            else
                                c.HingeLowerAngle = std::min(value, c.HingeUpperAngle);
                        });
                    },
                    [] { return PhysicsDrag::ShapeVariant(); },
                    end ? "Drag to set the upper limit (Ctrl snaps)" : "Drag to set the lower limit (Ctrl snaps)");
            }
        }
        if (!anyActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            m_PhysicsDrag.reset();
    }
} // namespace Kans
