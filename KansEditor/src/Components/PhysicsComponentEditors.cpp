#include <imgui.h>
#include <cfloat>
#include <functional>
#include <string>
#include <glm/gtc/quaternion.hpp>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/Physics/PhysicsMaterial.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "Physics/PhysicsEditing.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    bool PhysicsMaterialRows(PhysicsMaterialValues& values)
    {
        static const PhysicsMaterialValues defaults;
        const char*                        modes[] = {"Average", "Minimum", "Multiply", "Maximum"};
        bool                               changed = false;
        changed |= PropertyGrid::Float(
            "Static friction", values.StaticFriction, 0.01f, 0.0f, 100.0f, "%.3f", &defaults.StaticFriction);
        changed |= PropertyGrid::Float(
            "Dynamic friction", values.DynamicFriction, 0.01f, 0.0f, 100.0f, "%.3f", &defaults.DynamicFriction);
        changed |=
            PropertyGrid::Float("Bounciness", values.Restitution, 0.01f, 0.0f, 1.0f, "%.3f", &defaults.Restitution);
        int       friction = int(values.FrictionCombine), restitution = int(values.RestitutionCombine);
        const int frictionDefault    = int(defaults.FrictionCombine),
                  restitutionDefault = int(defaults.RestitutionCombine);
        if (PropertyGrid::Combo("Friction combine", modes, 4, friction, &frictionDefault))
            values.FrictionCombine = PhysicsCombineMode(friction), changed = true;
        if (PropertyGrid::Combo("Bounce combine", modes, 4, restitution, &restitutionDefault))
            values.RestitutionCombine = PhysicsCombineMode(restitution), changed = true;
        return changed;
    }

    namespace
    {
        namespace Edit = PhysicsEditing;

        const PhysicsSettings& Settings() { return KansFileSystem::GetProjectPhysicsSettings(); }

        // A whole-component edit: the drawers edit a copy and write it back once, so the
        // scene sees one change (and the history one step) per interaction.
        template<typename T, typename F>
        void EditComponent(Entity entity, F&& draw)
        {
            T    copy    = entity.GetComponent<T>();
            bool changed = draw(copy);
            if (changed)
                entity.PatchComponent<T>([&](T& c) { c = copy; });
        }

        bool Degrees(const char* label, float& value, float min, float max, const float* reset = nullptr)
        {
            return PropertyGrid::Float(label, value, 0.5f, min, max, "%.1f°", reset);
        }

        // Radians per second shown as degrees per second.
        bool DegreesPerSecond(const char* label, float& radians, const float* reset = nullptr)
        {
            float degrees      = glm::degrees(radians);
            float resetDegrees = reset ? glm::degrees(*reset) : 0.0f;
            if (!PropertyGrid::Float(
                    label, degrees, 1.0f, -100000.0f, 100000.0f, "%.1f°/s", reset ? &resetDegrees : nullptr))
                return false;
            radians = glm::radians(degrees);
            return true;
        }

        // A value that is unlimited at FLT_MAX: "∞" with a toggle; turning it off starts at `finite`.
        bool InfiniteFloat(const char* label, float& value, float finite, const char* format = "%.1f")
        {
            const bool infinite = value >= FLT_MAX;
            if (!PropertyGrid::Row(label, "Unlimited when the infinity toggle is on", !infinite))
                return false;
            ImGui::PushID(label);
            bool        changed = false;
            const float toggle  = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - toggle - ImGui::GetStyle().ItemSpacing.x);
            if (infinite)
            {
                ImGui::BeginDisabled();
                float shown = 0.0f;
                ImGui::DragFloat("##value", &shown, 0.0f, 0.0f, 0.0f, "∞");
                ImGui::EndDisabled();
            }
            else
                changed |= ImGui::DragFloat("##value", &value, 1.0f, 0.0f, FLT_MAX * 0.5f, format);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  infinite ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) :
                                             ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::Button(ICON_FA_INFINITY "##infinite", {toggle, toggle}))
            {
                value   = infinite ? finite : FLT_MAX;
                changed = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(infinite ? "Infinite (click to set a limit)" : "Make infinite");
            ImGui::PopID();
            return changed;
        }

        bool AxisToggles(const char* label, std::array<bool, 3>& axes, const char* tooltip)
        {
            const bool modified = axes[0] || axes[1] || axes[2];
            if (!PropertyGrid::Row(label, tooltip, modified))
                return false;
            ImGui::PushID(label);
            bool changed = false;
            for (int i = 0; i < 3; ++i)
            {
                if (i)
                    ImGui::SameLine();
                changed |= ImGui::Checkbox(i == 0 ? "X" : i == 1 ? "Y" : "Z", &axes[size_t(i)]);
            }
            ImGui::PopID();
            return changed;
        }

        bool LayerCombo(const char* label, uint32_t& layer)
        {
            const auto labels = Edit::LayerLabels(Settings());
            if (!PropertyGrid::Row(label, "Project Settings > Physics names the layers", layer != 0))
                return false;
            ImGui::PushID(label);
            bool changed = false;
            if (ImGui::BeginCombo("##layer", layer < 32 ? labels[layer].c_str() : "?", ImGuiComboFlags_HeightLarge))
            {
                for (uint32_t i = 0; i < 32; ++i)
                    if (ImGui::Selectable(labels[i].c_str(), i == layer) && i != layer)
                        layer = i, changed = true;
                ImGui::EndCombo();
            }
            ImGui::PopID();
            return changed;
        }

        // A collider's layer: its body's (empty) or its own.
        bool ColliderLayerCombo(std::optional<uint32_t>& layer)
        {
            const auto labels  = Edit::LayerLabels(Settings());
            const int  current = Edit::ColliderLayerIndex(layer);
            if (!PropertyGrid::Row("Layer", "The body's layer, or this collider's own", current != 0))
                return false;
            bool        changed = false;
            const char* preview = current == 0 ? "From the body" : labels[size_t(current - 1)].c_str();
            if (ImGui::BeginCombo("##colliderLayer", preview, ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable("From the body", current == 0) && current != 0)
                    layer = Edit::ColliderLayerFromIndex(0), changed = true;
                ImGui::Separator();
                for (int i = 1; i <= 32; ++i)
                    if (ImGui::Selectable(labels[size_t(i - 1)].c_str(), i == current) && i != current)
                        layer = Edit::ColliderLayerFromIndex(i), changed = true;
                ImGui::EndCombo();
            }
            return changed;
        }

        bool MaskCombo(const char* label, uint32_t& mask)
        {
            const auto& settings = Settings();
            if (!PropertyGrid::Row(label, "The layers this collides with", mask != UINT32_MAX))
                return false;
            ImGui::PushID(label);
            bool changed = false;
            if (ImGui::BeginCombo("##mask", Edit::MaskSummary(mask, settings).c_str(), ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable("Everything", mask == UINT32_MAX))
                    mask = UINT32_MAX, changed = true;
                if (ImGui::Selectable("Nothing", mask == 0))
                    mask = 0, changed = true;
                ImGui::Separator();
                const auto labels = Edit::LayerLabels(settings);
                for (uint32_t i = 0; i < 32; ++i)
                {
                    bool on = mask & (1u << i);
                    if (ImGui::Checkbox(labels[i].c_str(), &on))
                    {
                        mask    = on ? mask | (1u << i) : mask & ~(1u << i);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
            return changed;
        }

        // The entities with a rigid body, with what 0 means for this field.
        bool BodyField(const char* label, UUID& id, Scene* scene, const std::string& zero, const char* tooltip)
        {
            if (!PropertyGrid::Row(label, tooltip, uint64_t(id) != 0))
                return false;
            Entity      target  = scene && uint64_t(id) ? scene->GetEntityByUUID(id) : Entity {};
            std::string current = !uint64_t(id) ? zero : target ? target.GetComponent<TagComponent>().Tag : "(missing)";
            if (target && !target.HasComponent<RigidBodyComponent>())
                current += "  (no rigid body)";
            bool changed = false;
            ImGui::PushID(label);
            if (ImGui::BeginCombo("##body", current.c_str(), ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable(zero.c_str(), !uint64_t(id)))
                    id = 0, changed = true;
                if (scene)
                {
                    ImGui::Separator();
                    // Every entity with a rigid body, in hierarchy order.
                    std::function<void(Entity)> visit = [&](Entity e) {
                        if (e.HasComponent<RigidBodyComponent>())
                        {
                            const uint64_t uuid = uint64_t(e.GetUUID());
                            ImGui::PushID(&uuid);
                            if (ImGui::Selectable(e.GetComponent<TagComponent>().Tag.c_str(), e.GetUUID() == id))
                                id = e.GetUUID(), changed = true;
                            ImGui::PopID();
                        }
                        for (Entity child : scene->GetChildren(e))
                            visit(child);
                    };
                    for (Entity root : scene->GetRootEntities())
                        visit(root);
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const auto* payload = ImGui::AcceptDragDropPayload("entity_payload");
                    payload && payload->DataSize == sizeof(uint64_t))
                    id = UUID(*static_cast<const uint64_t*>(payload->Data)), changed = true;
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
            return changed;
        }

        // The shape's placement on the entity; rotation in degrees.
        bool OffsetRows(Physics::ShapeOffset& offset)
        {
            bool      changed = PropertyGrid::Vec3("Center", offset.Position, 0.0f, 0.01f);
            glm::vec3 euler   = glm::degrees(glm::eulerAngles(offset.Rotation));
            if (PropertyGrid::Vec3("Rotation", euler, 0.0f, 0.5f))
            {
                offset.Rotation = glm::quat(glm::radians(euler));
                changed         = true;
            }
            changed |= PropertyGrid::Vec3("Scale", offset.Scale, 1.0f, 0.01f);
            return changed;
        }

        // Asset > inline > project default; the group heading says which one the collider uses.
        bool MaterialGroup(ColliderMaterial& material, ComponentUI& ui)
        {
            bool assetLoaded = false, loading = false;
            if (material.Asset.IsValid() && ui.GetAssets())
            {
                const auto status = ui.GetAssets()->Request<PhysicsMaterialAsset>(material.Asset);
                assetLoaded       = status.IsReady();
                loading           = status.State == AssetLoadState::Loading;
            }
            // While loading, the asset is about to win.
            const auto source =
                loading ? Edit::MaterialSource::Asset : Edit::EffectiveMaterialSource(material, assetLoaded);
            bool changed = false;
            if (!PropertyGrid::BeginGroup("Material"))
                return false;
            std::string uses = Edit::MaterialSourceName(source);
            if (material.Asset.IsValid() && !assetLoaded && !loading)
                uses += "  (the asset did not load)";
            PropertyGrid::Text("Uses", uses, source == Edit::MaterialSource::ProjectDefault);
            changed |= ui.AssetPicker("Asset", AssetType::PhysicsMaterial, material.Asset);
            if (material.Asset.IsValid() && PropertyGrid::Row(""))
                if (ImGui::SmallButton(ICON_FA_XMARK "  Clear asset"))
                    material.Asset = {}, changed = true;
            // Inline values apply when there is no asset; off uses the project default.
            bool       inlineValues = !material.UseDefault, toggled = false;
            const bool open = PropertyGrid::BeginGroup("Inline values", &inlineValues, &toggled, false);
            if (toggled)
                material.UseDefault = !inlineValues, changed = true;
            if (open)
            {
                if (material.Asset.IsValid())
                    PropertyGrid::Text("", "The asset overrides these", true);
                changed |= PhysicsMaterialRows(material.Values);
                PropertyGrid::EndGroup();
            }
            PropertyGrid::EndGroup();
            return changed;
        }

        bool CollisionGroup(ColliderProperties& c)
        {
            static const ColliderProperties defaults;
            bool                            changed = false;
            if (!PropertyGrid::BeginGroup("Collision"))
                return false;
            changed |= PropertyGrid::Checkbox("Is trigger", c.IsTrigger, &defaults.IsTrigger);
            changed |= PropertyGrid::Float("Density", c.Density, 1.0f, 0.001f, 1.0e6f, "%.1f kg/m³", &defaults.Density);
            changed |= ColliderLayerCombo(c.Layer);
            PropertyGrid::EndGroup();
            return changed;
        }

        template<typename T, typename Shape>
        void DrawCollider(Entity entity, ComponentUI& ui, Shape&& shapeRows)
        {
            EditComponent<T>(entity, [&](T& c) {
                bool changed = false;
                if (PropertyGrid::BeginGroup("Shape"))
                {
                    changed |= shapeRows(c);
                    changed |= OffsetRows(c.Shape.Offset);
                    PropertyGrid::EndGroup();
                }
                changed |= CollisionGroup(c);
                changed |= MaterialGroup(c.Material, ui);
                return changed;
            });
        }
    } // namespace

    void DrawRigidBody(Entity entity, ComponentUI&)
    {
        static const RigidBodyComponent defaults;
        EditComponent<RigidBodyComponent>(entity, [](RigidBodyComponent& c) {
            bool        changed = false;
            const bool  dynamic = c.Type == RigidBodyType::Dynamic;
            const char* types[] = {"Static", "Dynamic", "Kinematic"};
            if (PropertyGrid::BeginGroup("Body"))
            {
                int       type        = int(c.Type);
                const int typeDefault = int(defaults.Type);
                if (PropertyGrid::Combo("Type", types, 3, type, &typeDefault))
                    c.Type = RigidBodyType(type), changed = true;
                changed |= LayerCombo("Layer", c.Layer);
                if (dynamic)
                {
                    changed |= PropertyGrid::Checkbox("Mass from colliders", c.AutoMass, &defaults.AutoMass);
                    if (c.AutoMass)
                        PropertyGrid::Text("Mass", "Collider volumes × densities", true);
                    else
                        changed |=
                            PropertyGrid::Float("Mass", c.Mass, 0.05f, 0.0001f, 1.0e7f, "%.3f kg", &defaults.Mass);
                    changed |= PropertyGrid::Vec3("Center of mass", c.CenterOfMassOffset, 0.0f, 0.01f);
                }
                PropertyGrid::EndGroup();
            }
            if (c.Type != RigidBodyType::Static && PropertyGrid::BeginGroup("Motion"))
            {
                if (dynamic)
                {
                    changed |= PropertyGrid::Float(
                        "Linear damping", c.LinearDamping, 0.01f, 0.0f, 1000.0f, "%.3f", &defaults.LinearDamping);
                    changed |= PropertyGrid::Float(
                        "Angular damping", c.AngularDamping, 0.01f, 0.0f, 1000.0f, "%.3f", &defaults.AngularDamping);
                    changed |= PropertyGrid::Float(
                        "Gravity scale", c.GravityScale, 0.01f, -100.0f, 100.0f, "%.2f", &defaults.GravityScale);
                    changed |= PropertyGrid::Checkbox("Start awake", c.StartAwake, &defaults.StartAwake);
                }
                const char* detections[] = {"Discrete", "Continuous (CCD)", "Speculative"};
                int         detection    = int(c.ContinuousDetection);
                const int   ccdDefault   = int(defaults.ContinuousDetection);
                if (PropertyGrid::Combo("Collision detection", detections, 3, detection, &ccdDefault))
                    c.ContinuousDetection = ContinuousDetectionMode(detection), changed = true;
                const char* interpolations[] = {"None", "Interpolate"};
                int         interpolation    = int(c.Interpolation);
                const int   interpDefault    = int(defaults.Interpolation);
                if (PropertyGrid::Combo("Interpolation", interpolations, 2, interpolation, &interpDefault))
                    c.Interpolation = PhysicsInterpolation(interpolation), changed = true;
                PropertyGrid::EndGroup();
            }
            if (dynamic && PropertyGrid::BeginGroup("Constraints", nullptr, nullptr, false))
            {
                changed |= AxisToggles("Freeze position", c.LockLinear, "Locks movement along world axes");
                changed |= AxisToggles("Freeze rotation", c.LockAngular, "Locks rotation about world axes");
                PropertyGrid::EndGroup();
            }
            if (dynamic && PropertyGrid::BeginGroup("Initial velocity", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Vec3("Linear", c.InitialLinearVelocity, 0.0f, 0.05f);
                glm::vec3 angular = glm::degrees(c.InitialAngularVelocity);
                if (PropertyGrid::Vec3("Angular (°/s)", angular, 0.0f, 1.0f))
                    c.InitialAngularVelocity = glm::radians(angular), changed = true;
                PropertyGrid::EndGroup();
            }
            if (dynamic)
            {
                bool       wind = c.AffectedByWind, toggled = false;
                const bool open = PropertyGrid::BeginGroup("Wind", &wind, &toggled, false);
                if (toggled)
                    c.AffectedByWind = wind, changed = true;
                if (open)
                {
                    changed |=
                        PropertyGrid::Float("Area", c.WindArea, 0.01f, 0.0f, 1.0e4f, "%.2f m²", &defaults.WindArea);
                    changed |= PropertyGrid::Float(
                        "Drag coefficient", c.DragCoefficient, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.DragCoefficient);
                    changed |= PropertyGrid::Float(
                        "Air density", c.AirDensity, 0.01f, 0.0f, 100.0f, "%.3f kg/m³", &defaults.AirDensity);
                    PropertyGrid::EndGroup();
                }
            }
            return changed;
        });
    }

    void DrawBoxCollider(Entity entity, ComponentUI& ui)
    {
        DrawCollider<BoxColliderComponent>(entity, ui, [](BoxColliderComponent& c) {
            glm::vec3 size = glm::abs(c.Shape.HalfExtents) * 2.0f;
            if (!PropertyGrid::Vec3("Size", size, 1.0f, 0.01f))
                return false;
            c.Shape.HalfExtents = glm::max(size * 0.5f, glm::vec3(PhysicsEditing::MinimumSize));
            return true;
        });
    }

    void DrawSphereCollider(Entity entity, ComponentUI& ui)
    {
        static const Physics::Sphere defaults;
        DrawCollider<SphereColliderComponent>(entity, ui, [](SphereColliderComponent& c) {
            return PropertyGrid::Float(
                "Radius", c.Shape.Radius, 0.01f, PhysicsEditing::MinimumSize, 1.0e5f, "%.3f m", &defaults.Radius);
        });
    }

    void DrawCapsuleCollider(Entity entity, ComponentUI& ui)
    {
        static const Physics::Capsule defaults;
        DrawCollider<CapsuleColliderComponent>(entity, ui, [](CapsuleColliderComponent& c) {
            bool changed = PropertyGrid::Float(
                "Radius", c.Shape.Radius, 0.01f, PhysicsEditing::MinimumSize, 1.0e5f, "%.3f m", &defaults.Radius);
            // The full height, caps included, as Unity and Godot show it.
            float       height  = 2.0f * (std::abs(c.Shape.HalfHeight) + std::abs(c.Shape.Radius));
            const float minimum = 2.0f * std::abs(c.Shape.Radius);
            if (PropertyGrid::Float("Height", height, 0.01f, minimum, 1.0e5f, "%.3f m"))
            {
                c.Shape.HalfHeight = std::max(0.0f, height * 0.5f - std::abs(c.Shape.Radius));
                changed            = true;
            }
            const char* axes[]      = {"X", "Y", "Z"};
            int         axis        = int(c.Shape.Direction);
            const int   axisDefault = int(defaults.Direction);
            if (PropertyGrid::Combo("Direction", axes, 3, axis, &axisDefault))
                c.Shape.Direction = Physics::Axis(axis), changed = true;
            return changed;
        });
    }

    void DrawMeshCollider(Entity entity, ComponentUI& ui)
    {
        DrawCollider<MeshColliderComponent>(entity, ui, [&](MeshColliderComponent& c) {
            static const bool convexDefault = true;
            bool              changed       = PropertyGrid::Checkbox("Convex", c.Convex, &convexDefault);
            if (!c.Convex)
                PropertyGrid::Text("", "Triangle meshes only collide on static or kinematic bodies", true);
            changed |= ui.AssetPicker("Mesh", AssetType::Mesh, c.Shape.Mesh);
            if (!c.Shape.Mesh.IsValid())
                PropertyGrid::Text("", "Empty: the entity's own mesh", true);
            bool       whole    = c.Shape.Section == UINT32_MAX;
            const bool wasWhole = whole;
            if (PropertyGrid::Checkbox("Whole mesh", whole) && whole != wasWhole)
                c.Shape.Section = whole ? UINT32_MAX : 0, changed = true;
            if (!whole)
            {
                int section = int(c.Shape.Section);
                if (PropertyGrid::Int("Section", section, 0, 4096))
                    c.Shape.Section = uint32_t(std::max(section, 0)), changed = true;
            }
            if (!c.Shape.Points.empty())
            {
                PropertyGrid::Text(
                    "Points", std::to_string(c.Shape.Points.size()) + " (from a script; replace the mesh)", true);
                if (PropertyGrid::Row(""))
                    if (ImGui::SmallButton(ICON_FA_TRASH "  Clear points"))
                        c.Shape.Points.clear(), changed = true;
            }
            return changed;
        });
    }

    void DrawCharacterController(Entity entity, ComponentUI&)
    {
        static const CharacterControllerComponent defaults;
        EditComponent<CharacterControllerComponent>(entity, [](CharacterControllerComponent& c) {
            bool changed = false;
            if (PropertyGrid::BeginGroup("Shape"))
            {
                const char* shapes[]     = {"Capsule", "Box"};
                int         shape        = int(c.Shape);
                const int   shapeDefault = int(defaults.Shape);
                if (PropertyGrid::Combo("Shape", shapes, 2, shape, &shapeDefault))
                    c.Shape = ControllerShape(shape), changed = true;
                if (c.Shape == ControllerShape::Capsule)
                {
                    changed |=
                        PropertyGrid::Float("Radius", c.Radius, 0.01f, 0.001f, 1000.0f, "%.3f m", &defaults.Radius);
                    float       height        = c.Height + 2.0f * c.Radius;
                    const float heightDefault = defaults.Height + 2.0f * defaults.Radius;
                    if (PropertyGrid::Float(
                            "Height", height, 0.01f, 2.0f * c.Radius, 1000.0f, "%.3f m", &heightDefault))
                        c.Height = std::max(0.0f, height - 2.0f * c.Radius), changed = true;
                }
                else
                {
                    glm::vec3 size = c.HalfExtents * 2.0f;
                    if (PropertyGrid::Vec3("Size", size, defaults.HalfExtents * 2.0f, 0.01f))
                        c.HalfExtents = glm::max(size * 0.5f, glm::vec3(0.001f)), changed = true;
                }
                changed |= PropertyGrid::Vec3("Center", c.Center, 0.0f, 0.01f);
                PropertyGrid::Text("", "World units; the entity's rotation and scale do not apply", true);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Movement"))
            {
                changed |= Degrees("Slope limit", c.SlopeLimit, 0.0f, 90.0f, &defaults.SlopeLimit);
                const char* modes[]     = {"Prevent climbing", "Force sliding"};
                int         mode        = int(c.NonWalkable);
                const int   modeDefault = int(defaults.NonWalkable);
                if (PropertyGrid::Combo("Steeper slopes", modes, 2, mode, &modeDefault))
                    c.NonWalkable = NonWalkableMode(mode), changed = true;
                changed |= PropertyGrid::Float(
                    "Step offset", c.StepOffset, 0.01f, 0.0f, 100.0f, "%.3f m", &defaults.StepOffset);
                changed |= PropertyGrid::Float(
                    "Skin width", c.SkinWidth, 0.001f, 0.0001f, 10.0f, "%.4f m", &defaults.SkinWidth);
                changed |= PropertyGrid::Float(
                    "Min move distance", c.MinMoveDistance, 0.0005f, 0.0f, 10.0f, "%.4f m", &defaults.MinMoveDistance);
                changed |= PropertyGrid::Float(
                    "Ground snap", c.SnapDistance, 0.01f, 0.0f, 10.0f, "%.3f m", &defaults.SnapDistance);
                changed |= PropertyGrid::Vec3("Up direction", c.UpDirection, defaults.UpDirection, 0.01f);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Collision"))
            {
                changed |= LayerCombo("Layer", c.Layer);
                changed |= MaskCombo("Collides with", c.CollisionMask);
                PropertyGrid::EndGroup();
            }
            {
                bool       push = c.PushRigidBodies, toggled = false;
                const bool open = PropertyGrid::BeginGroup("Push rigid bodies", &push, &toggled, false);
                if (toggled)
                    c.PushRigidBodies = push, changed = true;
                if (open)
                {
                    changed |= PropertyGrid::Float("Mass", c.Mass, 0.5f, 0.001f, 1.0e6f, "%.1f kg", &defaults.Mass);
                    changed |= PropertyGrid::Float(
                        "Force scale", c.PushForceScale, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.PushForceScale);
                    PropertyGrid::EndGroup();
                }
            }
            if (PropertyGrid::BeginGroup("Platforms"))
            {
                changed |=
                    PropertyGrid::Checkbox("Follow moving platforms", c.FollowPlatforms, &defaults.FollowPlatforms);
                PropertyGrid::EndGroup();
            }
            return changed;
        });
    }

    void DrawJoint(Entity entity, ComponentUI& ui)
    {
        static const JointComponent defaults;
        Scene*                      scene = ui.GetScene();
        // What BodyA = 0 resolves to, shown in its picker.
        std::string autoBody = "Auto (no rigid body up from here)";
        if (Entity resolved = scene ? PhysicsEditing::AutoBodyA(*scene, entity) : Entity {})
            autoBody = "Auto: " + resolved.GetComponent<TagComponent>().Tag;
        EditComponent<JointComponent>(entity, [&](JointComponent& c) {
            bool        changed = false;
            const char* types[] = {"Fixed", "Hinge", "Slider", "Spherical", "Distance", "D6 (configurable)"};
            if (PropertyGrid::BeginGroup("Joint"))
            {
                int       type        = int(c.Type);
                const int typeDefault = int(defaults.Type);
                if (PropertyGrid::Combo("Type", types, 6, type, &typeDefault))
                    c.Type = JointType(type), changed = true;
                changed |= BodyField("Body A",
                                     c.BodyA,
                                     scene,
                                     autoBody,
                                     "Empty: the nearest rigid body from this entity up its parents");
                changed |= BodyField("Body B", c.BodyB, scene, "World", "Empty: the joint holds Body A to the world");
                changed |= PropertyGrid::Checkbox("Bodies collide", c.EnableCollision, &defaults.EnableCollision);
                PropertyGrid::Text(
                    "", "The entity's transform is the joint frame; +X is the hinge, slider and twist axis", true);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Breaking", nullptr, nullptr, false))
            {
                changed |= InfiniteFloat("Break force", c.BreakForce, 1000.0f, "%.1f N");
                changed |= InfiniteFloat("Break torque", c.BreakTorque, 1000.0f, "%.1f N·m");
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Mass scales", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Float(
                    "Inverse mass A", c.InvMassScaleA, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.InvMassScaleA);
                changed |= PropertyGrid::Float(
                    "Inverse mass B", c.InvMassScaleB, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.InvMassScaleB);
                changed |= PropertyGrid::Float(
                    "Inverse inertia A", c.InvInertiaScaleA, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.InvInertiaScaleA);
                changed |= PropertyGrid::Float(
                    "Inverse inertia B", c.InvInertiaScaleB, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.InvInertiaScaleB);
                PropertyGrid::EndGroup();
            }
            const uint32_t groups      = PhysicsEditing::JointGroups(c.Type);
            const auto     toggleGroup = [&](const char* title, bool& enabled, auto&& rows) {
                bool       on = enabled, toggled = false;
                const bool open = PropertyGrid::BeginGroup(title, &on, &toggled);
                if (toggled)
                    enabled = on, changed = true;
                if (open)
                {
                    rows();
                    PropertyGrid::EndGroup();
                }
            };
            if (groups & PhysicsEditing::HingeLimit)
                toggleGroup("Limit", c.HingeLimitEnabled, [&] {
                    changed |= Degrees(
                        "Lower angle", c.HingeLowerAngle, -360.0f, c.HingeUpperAngle, &defaults.HingeLowerAngle);
                    changed |=
                        Degrees("Upper angle", c.HingeUpperAngle, c.HingeLowerAngle, 360.0f, &defaults.HingeUpperAngle);
                    changed |= PropertyGrid::Float(
                        "Stiffness", c.HingeLimitStiffness, 1.0f, 0.0f, 1.0e9f, "%.1f", &defaults.HingeLimitStiffness);
                    changed |= PropertyGrid::Float(
                        "Damping", c.HingeLimitDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.HingeLimitDamping);
                    PropertyGrid::Text("", "Stiffness 0 is a hard limit; drag the arc's ends in the viewport", true);
                });
            if (groups & PhysicsEditing::HingeMotor)
                toggleGroup("Motor", c.HingeMotorEnabled, [&] {
                    changed |= DegreesPerSecond("Target velocity", c.HingeMotorVelocity, &defaults.HingeMotorVelocity);
                    changed |= InfiniteFloat("Max torque", c.HingeMotorMaxForce, 100.0f, "%.1f N·m");
                    changed |= PropertyGrid::Checkbox("Free spin", c.HingeFreeSpin, &defaults.HingeFreeSpin);
                });
            if (groups & PhysicsEditing::HingeSpring)
                toggleGroup("Spring", c.HingeSpringEnabled, [&] {
                    changed |= Degrees("Target angle", c.HingeTargetAngle, -360.0f, 360.0f, &defaults.HingeTargetAngle);
                    changed |= PropertyGrid::Float("Stiffness",
                                                   c.HingeSpringStiffness,
                                                   1.0f,
                                                   0.0f,
                                                   1.0e9f,
                                                   "%.1f",
                                                   &defaults.HingeSpringStiffness);
                    changed |= PropertyGrid::Float(
                        "Damping", c.HingeSpringDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.HingeSpringDamping);
                });
            if (groups & PhysicsEditing::SliderLimit)
                toggleGroup("Limit", c.SliderLimitEnabled, [&] {
                    changed |= PropertyGrid::Float("Lower",
                                                   c.SliderLowerLimit,
                                                   0.01f,
                                                   -1.0e5f,
                                                   c.SliderUpperLimit,
                                                   "%.3f m",
                                                   &defaults.SliderLowerLimit);
                    changed |= PropertyGrid::Float("Upper",
                                                   c.SliderUpperLimit,
                                                   0.01f,
                                                   c.SliderLowerLimit,
                                                   1.0e5f,
                                                   "%.3f m",
                                                   &defaults.SliderUpperLimit);
                    changed |= PropertyGrid::Float("Stiffness",
                                                   c.SliderLimitStiffness,
                                                   1.0f,
                                                   0.0f,
                                                   1.0e9f,
                                                   "%.1f",
                                                   &defaults.SliderLimitStiffness);
                    changed |= PropertyGrid::Float(
                        "Damping", c.SliderLimitDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.SliderLimitDamping);
                });
            if (groups & PhysicsEditing::SliderDrive)
                toggleGroup("Drive", c.SliderDriveEnabled, [&] {
                    changed |= PropertyGrid::Float("Target position",
                                                   c.SliderTargetPosition,
                                                   0.01f,
                                                   -1.0e5f,
                                                   1.0e5f,
                                                   "%.3f m",
                                                   &defaults.SliderTargetPosition);
                    changed |= PropertyGrid::Float("Target velocity",
                                                   c.SliderTargetVelocity,
                                                   0.01f,
                                                   -1.0e5f,
                                                   1.0e5f,
                                                   "%.3f m/s",
                                                   &defaults.SliderTargetVelocity);
                    changed |= PropertyGrid::Float("Stiffness",
                                                   c.SliderDriveStiffness,
                                                   1.0f,
                                                   0.0f,
                                                   1.0e9f,
                                                   "%.1f",
                                                   &defaults.SliderDriveStiffness);
                    changed |= PropertyGrid::Float(
                        "Damping", c.SliderDriveDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.SliderDriveDamping);
                    changed |= InfiniteFloat("Max force", c.SliderDriveMaxForce, 1000.0f, "%.1f N");
                });
            if (groups & PhysicsEditing::SphericalCone)
                toggleGroup("Cone limit", c.SphericalLimitEnabled, [&] {
                    changed |= Degrees("Swing Y", c.SphericalYAngle, 0.0f, 179.0f, &defaults.SphericalYAngle);
                    changed |= Degrees("Swing Z", c.SphericalZAngle, 0.0f, 179.0f, &defaults.SphericalZAngle);
                    changed |= PropertyGrid::Float("Stiffness",
                                                   c.SphericalLimitStiffness,
                                                   1.0f,
                                                   0.0f,
                                                   1.0e9f,
                                                   "%.1f",
                                                   &defaults.SphericalLimitStiffness);
                    changed |= PropertyGrid::Float("Damping",
                                                   c.SphericalLimitDamping,
                                                   0.1f,
                                                   0.0f,
                                                   1.0e9f,
                                                   "%.2f",
                                                   &defaults.SphericalLimitDamping);
                });
            if ((groups & PhysicsEditing::DistanceRange) && PropertyGrid::BeginGroup("Distance"))
            {
                changed |= PropertyGrid::Checkbox("Minimum", c.DistanceMinEnabled, &defaults.DistanceMinEnabled);
                if (c.DistanceMinEnabled)
                    changed |= PropertyGrid::Float(
                        "Min distance", c.DistanceMin, 0.01f, 0.0f, c.DistanceMax, "%.3f m", &defaults.DistanceMin);
                changed |= PropertyGrid::Checkbox("Maximum", c.DistanceMaxEnabled, &defaults.DistanceMaxEnabled);
                if (c.DistanceMaxEnabled)
                    changed |= PropertyGrid::Float(
                        "Max distance", c.DistanceMax, 0.01f, c.DistanceMin, 1.0e5f, "%.3f m", &defaults.DistanceMax);
                changed |= PropertyGrid::Float(
                    "Stiffness", c.DistanceStiffness, 1.0f, 0.0f, 1.0e9f, "%.1f", &defaults.DistanceStiffness);
                changed |= PropertyGrid::Float(
                    "Damping", c.DistanceDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.DistanceDamping);
                changed |= PropertyGrid::Float(
                    "Tolerance", c.DistanceTolerance, 0.001f, 0.0f, 10.0f, "%.3f m", &defaults.DistanceTolerance);
                PropertyGrid::EndGroup();
            }
            if ((groups & PhysicsEditing::D6Motion) && PropertyGrid::BeginGroup("Motion"))
            {
                const char* motions[] = {"Locked", "Limited", "Free"};
                const char* axes[]    = {"X", "Y", "Z", "Twist (X)", "Swing Y", "Swing Z"};
                const int   locked    = int(JointMotion::Locked);
                for (size_t a = 0; a < 6; ++a)
                {
                    int motion = int(c.D6Motion[a]);
                    if (PropertyGrid::Combo(axes[a], motions, 3, motion, &locked))
                        c.D6Motion[a] = JointMotion(motion), changed = true;
                }
                PropertyGrid::EndGroup();
            }
            if ((groups & PhysicsEditing::D6Limits) && PropertyGrid::BeginGroup("Limits"))
            {
                changed |= PropertyGrid::Vec3("Linear lower", c.D6LinearLower, defaults.D6LinearLower, 0.01f);
                changed |= PropertyGrid::Vec3("Linear upper", c.D6LinearUpper, defaults.D6LinearUpper, 0.01f);
                changed |= PropertyGrid::Float(
                    "Linear stiffness", c.D6LinearStiffness, 1.0f, 0.0f, 1.0e9f, "%.1f", &defaults.D6LinearStiffness);
                changed |= PropertyGrid::Float(
                    "Linear damping", c.D6LinearDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.D6LinearDamping);
                changed |= Degrees("Twist lower", c.D6TwistLower, -360.0f, c.D6TwistUpper, &defaults.D6TwistLower);
                changed |= Degrees("Twist upper", c.D6TwistUpper, c.D6TwistLower, 360.0f, &defaults.D6TwistUpper);
                changed |= Degrees("Swing Y", c.D6SwingY, 0.0f, 179.0f, &defaults.D6SwingY);
                changed |= Degrees("Swing Z", c.D6SwingZ, 0.0f, 179.0f, &defaults.D6SwingZ);
                changed |= PropertyGrid::Float("Angular stiffness",
                                               c.D6AngularStiffness,
                                               1.0f,
                                               0.0f,
                                               1.0e9f,
                                               "%.1f",
                                               &defaults.D6AngularStiffness);
                changed |= PropertyGrid::Float(
                    "Angular damping", c.D6AngularDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.D6AngularDamping);
                PropertyGrid::EndGroup();
            }
            if ((groups & PhysicsEditing::D6Drives) && PropertyGrid::BeginGroup("Drives", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Vec3("Target position", c.DrivePosition, 0.0f, 0.01f);
                glm::vec3 rotation = glm::degrees(glm::eulerAngles(c.DriveRotation));
                if (PropertyGrid::Vec3("Target rotation", rotation, 0.0f, 0.5f))
                    c.DriveRotation = glm::quat(glm::radians(rotation)), changed = true;
                changed |= PropertyGrid::Vec3("Linear velocity", c.DriveLinearVelocity, 0.0f, 0.01f);
                glm::vec3 angular = glm::degrees(c.DriveAngularVelocity);
                if (PropertyGrid::Vec3("Angular velocity (°/s)", angular, 0.0f, 1.0f))
                    c.DriveAngularVelocity = glm::radians(angular), changed = true;
                changed |= PropertyGrid::Checkbox("Slerp rotation", c.SlerpDrive, &defaults.SlerpDrive);
                // Slots: X, Y, Z, swing, twist, slerp; slerp replaces swing and twist.
                const char* slots[] = {"X", "Y", "Z", "Swing", "Twist", "Slerp"};
                for (size_t s = 0; s < 6; ++s)
                {
                    if ((s == 3 || s == 4) && c.SlerpDrive)
                        continue;
                    if (s == 5 && !c.SlerpDrive)
                        continue;
                    ImGui::PushID(int(s));
                    if (PropertyGrid::BeginGroup((std::string(slots[s]) + " drive").c_str(),
                                                 nullptr,
                                                 nullptr,
                                                 c.DriveStiffness[s] != 0.0f || c.DriveDamping[s] != 0.0f))
                    {
                        changed |= PropertyGrid::Float(
                            "Stiffness", c.DriveStiffness[s], 1.0f, 0.0f, 1.0e9f, "%.1f", &defaults.DriveStiffness[s]);
                        changed |= PropertyGrid::Float(
                            "Damping", c.DriveDamping[s], 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.DriveDamping[s]);
                        changed |= InfiniteFloat("Max force", c.DriveMaxForce[s], 1000.0f);
                        PropertyGrid::EndGroup();
                    }
                    ImGui::PopID();
                }
                PropertyGrid::EndGroup();
            }
            return changed;
        });
    }
} // namespace Kans::ComponentEditors
