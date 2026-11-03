#include <imgui.h>
#include <cmath>
#include <functional>
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Scene/Components.h"
#include "Physics/PhysicsEditing.h"
#include "Physics/SecondaryMotionEditing.h"
#include "Physics/ShapeHandles.h"
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
    }

    void ViewportPanel::PhysicsHandles(Entity selected)
    {
        Scene&           scene   = *m_Scene;
        auto&            handles = m_Handles;
        const glm::mat4  world   = scene.GetWorldTransform(selected);
        const uint64_t   owner   = uint64_t(selected.GetUUID());
        // Stable IDs: the entity, the kind of handle and its index.
        const auto id = [&](uint64_t kind, uint64_t index = 0) {
            return Gizmo::MakeHandleId(Gizmo::MakeHandleId(owner, kind), index);
        };
        const auto tip = [](const Gizmo::HandleResult& r, const char* text) {
            if (r.Hovered && !r.Active && text)
                ImGui::SetTooltip("%s", text);
        };
        const auto knob = [](glm::vec4 color) {
            Gizmo::HandleStyle style;
            style.Color = color;
            style.Stem  = false;
            return style;
        };
        // A handle that drags `start` (kept from the drag's start) by a distance along `along`
        // from `position`; `apply` writes the dragged (or, cancelled, the start) value.
        const auto distanceHandle = [&](Gizmo::HandleId hid, glm::vec3 position, glm::vec3 along, glm::vec4 color,
                                        const char* text, const auto& current, const auto& apply) {
            using T = std::decay_t<decltype(current)>;
            Gizmo::LinearHandle h;
            h.Origin       = position;
            h.Direction    = along;
            h.Style        = knob(color);
            float distance = 0.0f;
            const auto r   = handles.Linear(hid, h, distance);
            if (r.Started)
                handles.Stash(current);
            if (r.Changed)
                if (const T* start = handles.Stashed<T>())
                    apply(*start, distance, r.Cancelled);
            tip(r, text);
        };

        // Primitive colliders: box faces (Alt: both sides), sphere radii, capsule caps and radii.
        const auto collider = [&](auto tag, uint64_t kind, const char* text) {
            using T = typename decltype(tag)::type;
            if (!selected.HasComponent<T>())
                return;
            auto       shape = selected.GetComponent<T>().Shape;
            const auto r     = ShapeHandles::Edit(handles, id(kind), world, shape, SelectedCollider);
            if (r.Changed)
                selected.PatchComponent<T>([&](T& c) { c.Shape = shape; });
            tip(r, text);
        };
        collider(std::common_type<BoxColliderComponent> {}, 1, "Drag to resize the collider (Alt: both sides)");
        collider(std::common_type<SphereColliderComponent> {}, 2, "Drag to resize the collider");
        collider(std::common_type<CapsuleColliderComponent> {}, 3, "Drag to resize the collider (Alt: both ends)");

        // Motion colliders: the same primitives, and the plane along its normal.
        if (selected.HasComponent<MotionColliderComponent>())
        {
            auto shape = selected.GetComponent<MotionColliderComponent>().Shape;
            std::visit(
                [&](auto& s) {
                    using S = std::decay_t<decltype(s)>;
                    if constexpr (std::is_same_v<S, Physics::Plane>)
                    {
                        const glm::mat4 matrix = world * s.Offset.Matrix();
                        const auto      h      = SecondaryMotionEditing::Handles(s).front();
                        distanceHandle(id(5), glm::vec3(matrix * glm::vec4(h.Position, 1.0f)),
                                       glm::vec3(matrix * glm::vec4(h.Direction, 0.0f)), SecondaryMotionEditing::ColliderColor,
                                       "Drag to move the plane along its normal", s,
                                       [&](const Physics::Plane& start, float distance, bool cancelled) {
                                           const Physics::Plane moved =
                                               cancelled ? start : SecondaryMotionEditing::DragHandle(start, 0, distance);
                                           selected.PatchComponent<MotionColliderComponent>([&](auto& c) { c.Shape = moved; });
                                       });
                    }
                    else if constexpr (std::is_same_v<S, Physics::Box> || std::is_same_v<S, Physics::Sphere> ||
                                       std::is_same_v<S, Physics::Capsule>)
                    {
                        const auto r = ShapeHandles::Edit(handles, id(5), world, s, SecondaryMotionEditing::ColliderColor);
                        if (r.Changed)
                            selected.PatchComponent<MotionColliderComponent>([&](auto& c) { c.Shape = s; });
                        tip(r, "Drag to resize the collider");
                    }
                },
                shape);
        }

        // Vehicle wheels: the radius on top and the attach position along the chassis axes, for the
        // card's selected wheel (every wheel while none is selected).
        if (selected.HasComponent<VehicleComponent>())
        {
            const auto      wheels = selected.GetComponent<VehicleComponent>().Wheels;
            const glm::mat4 frame  = VehicleFieldEditing::ChassisFrame(scene, selected);
            const int       chosen = m_SelectedWheel ? m_SelectedWheel(selected) : -1;
            const char*     tips[] = {"Drag to set the wheel radius", "Drag to move the wheel along X",
                                      "Drag to move the wheel along Y", "Drag to move the wheel along Z"};
            for (size_t w = 0; w < wheels.size(); ++w)
            {
                if (chosen >= 0 && int(w) != chosen)
                    continue;
                for (const auto& h : VehicleFieldEditing::WheelHandles(wheels[w]))
                    distanceHandle(id(6, w * 8 + size_t(h.ID)), glm::vec3(frame * glm::vec4(h.Position, 1.0f)),
                                   glm::vec3(frame * glm::vec4(h.Direction, 0.0f)),
                                   h.ID == 0 ? VehicleFieldEditing::WheelColor : glm::vec4(h.Direction * 0.8f + 0.2f, 1.0f),
                                   tips[h.ID], wheels[w], [&, w, hid = h.ID](const VehicleWheel& start, float distance, bool cancelled) {
                                       const auto moved = cancelled ? start : VehicleFieldEditing::DragWheelHandle(start, hid, distance);
                                       selected.PatchComponent<VehicleComponent>([&](VehicleComponent& c) {
                                           if (w < c.Wheels.size())
                                               c.Wheels[w] = moved;
                                       });
                                   });
            }
        }

        // Wind zones and force fields: a sphere's radii or a box's faces (both sides: the field
        // stays centred on its entity), and a wind zone's falloff band inside along +X.
        const auto fieldHandles = [&](auto tag, uint64_t kind) {
            using T = typename decltype(tag)::type;
            if (!selected.HasComponent<T>())
                return;
            const auto start  = VehicleFieldEditing::ShapeOf(selected.GetComponent<T>());
            auto       values = start;
            Gizmo::HandleResult r;
            if (values.Shape == FieldShape::Sphere)
            {
                Gizmo::SphereHandle h;
                h.Frame     = world;
                h.MinRadius = PhysicsEditing::MinimumSize;
                h.Style     = knob(VehicleFieldEditing::FieldColor);
                h.Style.Stem = true;
                float radius = std::abs(values.Radius);
                r            = handles.Sphere(id(kind), h, radius);
                values.Radius = radius;
            }
            else if (values.Shape == FieldShape::Box)
            {
                Gizmo::BoxHandle h;
                h.Frame      = world;
                h.MinHalf    = PhysicsEditing::MinimumSize;
                h.LockCenter = true;
                h.Style      = knob(VehicleFieldEditing::FieldColor);
                h.Style.Knob = Gizmo::KnobShape::Cube;
                h.Style.Stem = true;
                glm::vec3 centre(0.0f), half = glm::abs(values.HalfExtents);
                r                  = handles.Box(id(kind), h, centre, half);
                values.HalfExtents = half;
            }
            else
                return;
            tip(r, "Drag to resize the field");
            if (values.HasBand)
            {
                // The band's inner edge: full strength inside it, fading out to the shape.
                const float outer = values.Shape == FieldShape::Sphere ? std::abs(values.Radius) : std::abs(values.HalfExtents.x);
                Gizmo::LinearHandle band;
                band.Origin    = glm::vec3(world[3]);
                band.Direction = glm::vec3(world * glm::vec4(1, 0, 0, 0));
                band.Min       = 0.0f;
                band.Max       = outer;
                band.Style     = knob({1.0f, 0.9f, 0.3f, 1.0f});
                float inner    = outer * (1.0f - std::clamp(values.Band, 0.0f, 1.0f));
                const auto b   = handles.Linear(id(kind, 1), band, inner);
                if (b.Changed && outer > 0.0f)
                    values.Band = 1.0f - inner / outer;
                tip(b, "Drag to set the falloff band (full strength inside)");
                r |= b;
            }
            if (r.Started)
                handles.Stash(start);
            if (r.Cancelled)
                if (const auto* kept = handles.Stashed<VehicleFieldEditing::FieldShapeValues>())
                    values = *kept;
            if (r.Changed)
                selected.PatchComponent<T>([&](T& c) { VehicleFieldEditing::Apply(c, values); });
        };
        fieldHandles(std::common_type<WindZoneComponent> {}, 7);
        fieldHandles(std::common_type<ForceFieldComponent> {}, 8);

        // Hinge limits: the arc's two ends turn about the hinge axis (Ctrl inverts snapping).
        if (selected.HasComponent<JointComponent>() && selected.GetComponent<JointComponent>().Type == JointType::Hinge)
        {
            const auto      joint = selected.GetComponent<JointComponent>();
            const glm::mat4 frame = PhysicsEditing::JointFrame(world);
            for (int end = 0; end < 2; ++end)
            {
                Gizmo::AngleHandle h;
                h.Center    = glm::vec3(frame[3]);
                h.Axis      = glm::vec3(frame[0]);
                h.Reference = glm::vec3(frame[1]);
                h.Radius    = glm::length(PhysicsEditing::HingeArcPoint(frame, 0.0f) - h.Center);
                h.Min       = end ? joint.HingeLowerAngle : -180.0f;
                h.Max       = end ? 180.0f : joint.HingeUpperAngle;
                h.Snap      = m_SnapRotate;
                h.SnapOn    = m_GizmoSnap;
                h.Style     = knob({1.0f, 0.8f, 0.1f, 1.0f});
                h.Style.Stem = true;
                float      degrees = end ? joint.HingeUpperAngle : joint.HingeLowerAngle;
                const auto r       = handles.Angle(id(4, uint64_t(end)), h, degrees);
                if (r.Started)
                    handles.Stash(joint);
                if (r.Changed)
                {
                    const JointComponent* start = handles.Stashed<JointComponent>();
                    selected.PatchComponent<JointComponent>([&](JointComponent& c) {
                        if (r.Cancelled && start)
                            c = *start;
                        else if (end)
                            c.HingeUpperAngle = std::max(degrees, c.HingeLowerAngle);
                        else
                            c.HingeLowerAngle = std::min(degrees, c.HingeUpperAngle);
                    });
                }
                tip(r, end ? "Drag to set the upper limit (Ctrl inverts snapping)" :
                             "Drag to set the lower limit (Ctrl inverts snapping)");
            }
        }
    }
} // namespace Kans
