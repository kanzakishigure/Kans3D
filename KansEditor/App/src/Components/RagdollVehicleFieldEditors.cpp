#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <functional>
#include <string>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include "Kans3D/Physics/RagdollComponent.h"
#include "Kans3D/Physics/VehicleComponent.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "KansEditorCore/Physics/RagdollEditing.h"
#include "KansEditorCore/Physics/VehicleFieldEditing.h"
#include "UI/CurveEditor.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    namespace
    {
        namespace Fields = VehicleFieldEditing;

        template<typename T, typename F>
        void EditComponent(Entity entity, F&& draw)
        {
            T    copy    = entity.GetComponent<T>();
            bool changed = draw(copy);
            if (changed)
                entity.PatchComponent<T>([&](T& c) { c = copy; });
        }

        const PhysicsSettings& Settings() { return KansFileSystem::GetProjectPhysicsSettings(); }

        bool Degrees(const char* label, float& value, float min, float max, const float* reset = nullptr)
        {
            return PropertyGrid::Float(label, value, 0.5f, min, max, "%.1f°", reset);
        }

        bool UIntRow(const char* label, uint32_t& value, int min, int max, const uint32_t* reset = nullptr)
        {
            int       v = int(value);
            const int r = reset ? int(*reset) : 0;
            if (!PropertyGrid::Int(label, v, min, max, reset ? &r : nullptr))
                return false;
            value = uint32_t(std::clamp(v, min, max));
            return true;
        }

        template<typename E>
        bool EnumCombo(const char* label, E& value, const char* const names[], int count, E reset)
        {
            int       index = int(value);
            const int def   = int(reset);
            if (!PropertyGrid::Combo(label, names, count, index, &def))
                return false;
            value = E(index);
            return true;
        }

        // The layers a mask lets through, as a combo of checkboxes.
        bool LayerMask(const char* label, uint32_t& mask, const char* tooltip)
        {
            if (!PropertyGrid::Row(label, tooltip, mask != UINT32_MAX))
                return false;
            ImGui::PushID(label);
            bool changed = false;
            if (ImGui::BeginCombo("##mask", PhysicsEditing::MaskSummary(mask, Settings()).c_str(), ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable("Everything", mask == UINT32_MAX))
                    mask = UINT32_MAX, changed = true;
                if (ImGui::Selectable("Nothing", mask == 0))
                    mask = 0, changed = true;
                ImGui::Separator();
                const auto labels = PhysicsEditing::LayerLabels(Settings());
                for (uint32_t i = 0; i < 32; ++i)
                {
                    bool on = mask & (1u << i);
                    if (ImGui::Checkbox(labels[i].c_str(), &on))
                        mask = on ? mask | (1u << i) : mask & ~(1u << i), changed = true;
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
            return changed;
        }

        // Which systems a field pushes: rigid bodies, cloth, spring bones, particles.
        bool SystemMask(uint32_t& mask)
        {
            if (!PropertyGrid::Row("Affects", "The systems this field pushes", mask != 15))
                return false;
            const std::pair<FieldSystem, const char*> systems[] = {{FieldSystem::RigidBodies, "Bodies"},
                                                                   {FieldSystem::Cloth, "Cloth"},
                                                                   {FieldSystem::SpringBones, "Springs"},
                                                                   {FieldSystem::Particles, "Particles"}};
            bool changed = false;
            ImGui::PushID("systems");
            for (size_t i = 0; i < 4; ++i)
            {
                if (i)
                    ImGui::SameLine();
                bool on = mask & uint32_t(systems[i].first);
                if (ImGui::Checkbox(systems[i].second, &on))
                    mask = on ? mask | uint32_t(systems[i].first) : mask & ~uint32_t(systems[i].first), changed = true;
            }
            ImGui::PopID();
            return changed;
        }

        // A direction row that refuses the zero vector.
        bool Direction(const char* label, glm::vec3& value, glm::vec3 reset)
        {
            glm::vec3 edited = value;
            if (!PropertyGrid::Vec3(label, edited, reset, 0.01f) || glm::length(edited) < 1e-4f)
                return false;
            value = edited;
            return true;
        }

        // The skeleton a ragdoll binds to on this entity: its Animator's.
        const SkeletonData* AnimatorSkeleton(Entity entity, AssetID* id = nullptr)
        {
            if (!entity.HasComponent<AnimatorComponent>())
                return nullptr;
            const auto& a = entity.GetComponent<AnimatorComponent>();
            if (id)
                *id = a.Skeleton;
            return a.SkeletonSource ? &a.SkeletonSource->GetData() : nullptr;
        }

        // A descendant of `owner` (children only, as the engine requires for wheel visuals).
        bool ChildField(const char* label, UUID& id, Scene* scene, Entity owner)
        {
            if (!PropertyGrid::Row(label, "A child of the vehicle that turns and steers with the wheel", uint64_t(id) != 0))
                return false;
            Entity target = scene && uint64_t(id) ? scene->GetEntityByUUID(id) : Entity {};
            std::string current =
                !uint64_t(id) ? "(none)" : target ? target.GetComponent<TagComponent>().Tag : "(missing)";
            if (target && scene && !scene->IsDescendantOf(target, owner))
                current += "  (not a child)";
            bool changed = false;
            ImGui::PushID(label);
            if (ImGui::BeginCombo("##child", current.c_str(), ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable("(none)", !uint64_t(id)))
                    id = 0, changed = true;
                if (scene)
                {
                    std::function<void(Entity, int)> visit = [&](Entity e, int depth) {
                        for (Entity child : scene->GetChildren(e))
                        {
                            const uint64_t uuid = uint64_t(child.GetUUID());
                            ImGui::PushID(&uuid);
                            const std::string name =
                                std::string(size_t(depth) * 2, ' ') + child.GetComponent<TagComponent>().Tag;
                            if (ImGui::Selectable(name.c_str(), child.GetUUID() == id))
                                id = child.GetUUID(), changed = true;
                            ImGui::PopID();
                            visit(child, depth + 1);
                        }
                    };
                    visit(owner, 0);
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const auto* payload = ImGui::AcceptDragDropPayload("entity_payload");
                    payload && payload->DataSize == sizeof(uint64_t))
                {
                    const UUID dropped(*static_cast<const uint64_t*>(payload->Data));
                    Entity     e = scene ? scene->GetEntityByUUID(dropped) : Entity {};
                    if (e && scene->IsDescendantOf(e, owner))
                        id = dropped, changed = true;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
            return changed;
        }

        // Small buttons on one row (free of the label column).
        bool RowButton(const char* label, const char* tooltip = nullptr, bool enabled = true)
        {
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::SmallButton(label);
            ImGui::EndDisabled();
            if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }

        bool FieldShapeRows(FieldShape& shape, float& radius, glm::vec3& halfExtents, FieldShape reset)
        {
            const char* shapes[] = {"Infinite", "Sphere", "Box"};
            bool        changed  = EnumCombo("Shape", shape, shapes, 3, reset);
            if (shape == FieldShape::Sphere)
                changed |= PropertyGrid::Float("Radius", radius, 0.01f, PhysicsEditing::MinimumSize, 1.0e5f, "%.2f m");
            else if (shape == FieldShape::Box)
            {
                glm::vec3 size = glm::abs(halfExtents) * 2.0f;
                if (PropertyGrid::Vec3("Size", size, glm::vec3(2.0f), 0.01f))
                    halfExtents = glm::max(size * 0.5f, glm::vec3(PhysicsEditing::MinimumSize)), changed = true;
            }
            else
                PropertyGrid::Text("", "Everywhere in the scene", true);
            return changed;
        }
    } // namespace

    // ==================================================================== ragdoll

    void DrawRagdoll(Entity entity, ComponentUI& ui)
    {
        static const RagdollComponent defaults;
        Scene*                        scene   = ui.GetScene();
        AssetID                       skeletonID;
        const SkeletonData*           skeleton = AnimatorSkeleton(entity, &skeletonID);
        EditComponent<RagdollComponent>(entity, [&](RagdollComponent& c) {
            bool changed = false;
            // The bound asset: loaded by the asset manager, or the code-bound source.
            Ref<const RagdollAsset> asset = c.Source;
            bool                    loading = false;
            if (c.Asset.IsValid() && ui.GetAssets())
            {
                const auto status = ui.GetAssets()->Request<RagdollAsset>(c.Asset);
                asset             = status.Asset ? status.Asset : asset;
                loading           = status.State == AssetLoadState::Loading;
            }
            if (PropertyGrid::BeginGroup("Asset"))
            {
                changed |= ui.AssetPicker("Ragdoll asset", AssetType::Ragdoll, c.Asset);
                if (PropertyGrid::Row("", "Bodies, constraints and collision pairs live in the .ragdoll asset"))
                {
                    if (RowButton(ICON_FA_PEN "  Edit", "Open the asset in the ragdoll editor", c.Asset.IsValid()))
                        ui.OpenRagdollEditor(c.Asset);
                    ImGui::SameLine();
                    if (RowButton(ICON_FA_PLUS "  Generate",
                                  "Generate a .ragdoll from the Animator's skeleton and assign it",
                                  skeleton != nullptr || skeletonID.IsValid()))
                        ui.CreateRagdoll(entity);
                }
                std::string status;
                if (!c.Asset.IsValid() && !c.Source)
                    status = "No ragdoll asset";
                else if (!asset)
                    status = loading ? "Loading..." : "The asset did not load";
                else if (!entity.HasComponent<AnimatorComponent>())
                    status = "Needs an Animator on this entity (it supplies the skeleton and pose)";
                else if (!skeleton)
                    status = "Waiting for the Animator's skeleton";
                else
                    status = RagdollEditing::Problem(asset->GetData(), skeleton, skeletonID);
                if (!status.empty())
                {
                    if (PropertyGrid::Row("Binding"))
                        ImGui::TextColored(ErrorColor, "%s", status.c_str());
                }
                else
                    PropertyGrid::Text("Binding", "OK  (" + std::to_string(asset->GetData().Bodies.size()) + " bodies)");
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Simulation"))
            {
                changed |= PropertyGrid::Checkbox("Enabled", c.Enabled, &defaults.Enabled);
                const char* modes[] = {"Kinematic (animated)", "Simulated", "Blended"};
                changed |= EnumCombo("Mode", c.Mode, modes, 3, defaults.Mode);
                changed |= PropertyGrid::Float(
                    "Drive strength", c.DriveStrength, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.DriveStrength);
                changed |= PropertyGrid::Float("Impact threshold", c.ImpactThreshold, 0.1f, 0.0f, 1.0e5f,
                                               c.ImpactThreshold > 0 ? "%.1f N·s" : "Off", &defaults.ImpactThreshold);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Root"))
            {
                const char* following[] = {"None", "Follow pelvis"};
                changed |= EnumCombo("Root following", c.RootFollowing, following, 2, defaults.RootFollowing);
                ImGui::BeginDisabled(c.RootFollowing == RagdollRootFollowing::None);
                changed |= PropertyGrid::Checkbox("Follow yaw", c.FollowYaw, &defaults.FollowYaw);
                ImGui::EndDisabled();
                changed |= PropertyGrid::Float("Teleport distance", c.TeleportDistance, 0.05f, 0.0f, 1.0e4f,
                                               c.TeleportDistance > 0 ? "%.2f m" : "Off", &defaults.TeleportDistance);
                changed |= PropertyGrid::Float("Teleport rotation", c.TeleportRotation, 0.5f, 0.0f, 180.0f,
                                               c.TeleportRotation > 0 ? "%.1f°" : "Off", &defaults.TeleportRotation);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Per-body overrides", nullptr, nullptr, !c.Bodies.empty()))
            {
                // Bones that have a body in the bound asset.
                std::vector<std::string> bones;
                if (asset)
                    for (const auto& body : asset->GetData().Bodies)
                        bones.push_back(body.Bone);
                for (size_t i = 0; i < c.Bodies.size(); ++i)
                {
                    auto& o = c.Bodies[i];
                    ImGui::PushID(int(i));
                    if (PropertyGrid::BeginGroup(o.Bone.empty() ? "(no bone)" : o.Bone.c_str()))
                    {
                        if (PropertyGrid::Row("Bone"))
                        {
                            if (ImGui::BeginCombo("##bone", o.Bone.empty() ? "(none)" : o.Bone.c_str(), ImGuiComboFlags_HeightLarge))
                            {
                                for (const auto& bone : bones)
                                    if (ImGui::Selectable(bone.c_str(), bone == o.Bone) && bone != o.Bone)
                                        o.Bone = bone, changed = true;
                                ImGui::EndCombo();
                            }
                        }
                        static const float one = 1.0f;
                        changed |= PropertyGrid::Float("Blend weight", o.BlendWeight, 0.01f, 0.0f, 1.0f, "%.2f", &one);
                        bool drive = o.DriveStrength >= 0.0f;
                        if (PropertyGrid::Checkbox("Own drive", drive))
                        {
                            o.DriveStrength = drive ? c.DriveStrength : -1.0f;
                            o.DriveDamping  = drive ? 0.0f : -1.0f;
                            changed         = true;
                        }
                        if (drive)
                        {
                            changed |= PropertyGrid::Float("Drive strength", o.DriveStrength, 0.01f, 0.0f, 100.0f, "%.2f");
                            changed |= PropertyGrid::Float("Drive damping", o.DriveDamping, 0.01f, 0.0f, 100.0f, "%.2f");
                        }
                        if (PropertyGrid::Row(""))
                            if (ImGui::SmallButton(ICON_FA_TRASH "  Remove override"))
                            {
                                c.Bodies.erase(c.Bodies.begin() + std::ptrdiff_t(i));
                                changed = true;
                            }
                        PropertyGrid::EndGroup();
                    }
                    ImGui::PopID();
                    if (changed && i >= c.Bodies.size())
                        break;
                }
                if (PropertyGrid::Row("", "A bone of the bound asset with its own blend weight or drive"))
                {
                    ImGui::BeginDisabled(bones.empty());
                    if (ImGui::SmallButton(ICON_FA_PLUS "  Add override"))
                        ImGui::OpenPopup("##addOverride");
                    ImGui::EndDisabled();
                    if (ImGui::BeginPopup("##addOverride"))
                    {
                        for (const auto& bone : bones)
                        {
                            const bool used = std::any_of(c.Bodies.begin(), c.Bodies.end(),
                                                          [&](const auto& o) { return o.Bone == bone; });
                            if (ImGui::MenuItem(bone.c_str(), nullptr, false, !used))
                            {
                                RagdollBodyOverride o;
                                o.Bone = bone;
                                c.Bodies.push_back(o);
                                changed = true;
                            }
                        }
                        ImGui::EndPopup();
                    }
                }
                PropertyGrid::EndGroup();
            }
            // While playing: what the simulation is doing (read-only).
            if (PhysicsWorld* world = scene ? scene->GetPhysicsWorld() : nullptr)
                if (PropertyGrid::BeginGroup("Runtime"))
                {
                    const char* modes[] = {"Kinematic", "Simulated", "Blended"};
                    PropertyGrid::Text("Current mode", modes[std::min<size_t>(size_t(c.Mode), 2)]);
                    size_t simulated = 0, total = asset ? asset->GetData().Bodies.size() : 0;
                    if (asset)
                        for (const auto& body : asset->GetData().Bodies)
                            simulated += world->GetRagdollBodyState(entity.GetUUID(), body.Bone).has_value();
                    PropertyGrid::Text("Bodies", std::to_string(simulated) + " of " + std::to_string(total) + " in the world",
                                       true);
                    PropertyGrid::EndGroup();
                }
            return changed;
        });
    }

    // ==================================================================== vehicle

    namespace
    {
        bool WheelRows(VehicleWheel& w, size_t index, Entity vehicle, Scene* scene, ComponentUI& ui)
        {
            static const VehicleWheel defaults;
            bool                      changed = false;
            if (PropertyGrid::BeginGroup("Placement"))
            {
                changed |= ChildField("Visual", w.Visual, scene, vehicle);
                changed |= PropertyGrid::Vec3("Attach position", w.AttachPosition, 0.0f, 0.01f);
                changed |= PropertyGrid::Float("Radius", w.Radius, 0.005f, 0.01f, 10.0f, "%.3f m", &defaults.Radius);
                changed |= PropertyGrid::Float("Width", w.Width, 0.005f, 0.01f, 10.0f, "%.3f m", &defaults.Width);
                changed |= PropertyGrid::Float("Mass", w.Mass, 0.1f, 0.01f, 1.0e4f, "%.1f kg", &defaults.Mass);
                changed |= PropertyGrid::Float("Moment of inertia", w.MomentOfInertia, 0.01f, 0.001f, 1.0e4f, "%.3f kg·m²",
                                               &defaults.MomentOfInertia);
                changed |= UIntRow("Axle", w.Axle, 0, 15, &defaults.Axle);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Suspension", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Float(
                    "Travel", w.SuspensionTravel, 0.005f, 0.001f, 10.0f, "%.3f m", &defaults.SuspensionTravel);
                changed |= PropertyGrid::Float(
                    "Stiffness", w.SpringStiffness, 100.0f, 1.0f, 1.0e8f, "%.0f N/m", &defaults.SpringStiffness);
                changed |= PropertyGrid::Float(
                    "Damping", w.SpringDamping, 10.0f, 0.0f, 1.0e7f, "%.0f N·s/m", &defaults.SpringDamping);
                changed |= Direction("Direction", w.SuspensionDirection, defaults.SuspensionDirection);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Tire", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Float("Longitudinal stiffness", w.LongitudinalStiffness, 100.0f, 0.0f, 1.0e8f,
                                               "%.0f", &defaults.LongitudinalStiffness);
                changed |= PropertyGrid::Float(
                    "Lateral stiffness", w.LateralStiffness, 100.0f, 0.0f, 1.0e8f, "%.0f", &defaults.LateralStiffness);
                // Friction against longitudinal slip: three (slip, friction) points.
                const char* points[] = {"At zero slip", "Peak", "Sliding"};
                for (int p = 0; p < 3; ++p)
                {
                    ImGui::PushID(p);
                    if (PropertyGrid::Row(points[p], "Slip and the friction multiplier there"))
                    {
                        float pair[2] = {w.FrictionVsSlip[size_t(p * 2)], w.FrictionVsSlip[size_t(p * 2 + 1)]};
                        if (ImGui::DragFloat2("##point", pair, 0.01f, 0.0f, 10.0f, "%.2f"))
                        {
                            w.FrictionVsSlip[size_t(p * 2)]     = p == 0 ? 0.0f : std::max(0.0f, pair[0]);
                            w.FrictionVsSlip[size_t(p * 2 + 1)] = std::max(0.0f, pair[1]);
                            changed                            = true;
                        }
                    }
                    ImGui::PopID();
                }
                for (size_t m = 0; m < w.MaterialFriction.size(); ++m)
                {
                    ImGui::PushID(int(m));
                    changed |= ui.AssetPicker("Surface material", AssetType::PhysicsMaterial, w.MaterialFriction[m].Material);
                    changed |= PropertyGrid::Float("Friction ×", w.MaterialFriction[m].Multiplier, 0.01f, 0.0f, 10.0f, "%.2f");
                    if (PropertyGrid::Row("") && ImGui::SmallButton(ICON_FA_TRASH "  Remove surface"))
                    {
                        w.MaterialFriction.erase(w.MaterialFriction.begin() + std::ptrdiff_t(m));
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (PropertyGrid::Row("", "A friction multiplier on one physics material"))
                    if (ImGui::SmallButton(ICON_FA_PLUS "  Add surface friction"))
                        w.MaterialFriction.push_back({}), changed = true;
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Brakes and steering", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Float(
                    "Brake torque", w.BrakeTorque, 10.0f, 0.0f, 1.0e6f, "%.0f N·m", &defaults.BrakeTorque);
                changed |= PropertyGrid::Float(
                    "Handbrake torque", w.HandbrakeTorque, 10.0f, 0.0f, 1.0e6f, "%.0f N·m", &defaults.HandbrakeTorque);
                changed |= Degrees("Max steer angle", w.MaxSteerAngle, -89.0f, 89.0f, &defaults.MaxSteerAngle);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Drive", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Checkbox("Driven", w.Driven, &defaults.Driven);
                ImGui::BeginDisabled(!w.Driven);
                changed |= PropertyGrid::Float("Torque split", w.TorqueSplit, 0.01f, 0.0f, 1.0f, "%.2f", &defaults.TorqueSplit);
                ImGui::EndDisabled();
                PropertyGrid::EndGroup();
            }
            (void)index;
            return changed;
        }

        std::string WheelName(const VehicleWheel& w, size_t index, Scene* scene)
        {
            std::string name = "Wheel " + std::to_string(index);
            if (Entity visual = scene && uint64_t(w.Visual) ? scene->GetEntityByUUID(w.Visual) : Entity {})
                name += "  " + visual.GetComponent<TagComponent>().Tag;
            return name + "  (axle " + std::to_string(w.Axle) + ")";
        }
    } // namespace

    void DrawVehicle(Entity entity, ComponentUI& ui)
    {
        static const VehicleComponent defaults;
        Scene*                        scene    = ui.GetScene();
        const int                     selected = ui.GetSelectedWheel(entity);
        EditComponent<VehicleComponent>(entity, [&](VehicleComponent& c) {
            bool changed = false;
            if (!entity.HasComponent<RigidBodyComponent>() ||
                entity.GetComponent<RigidBodyComponent>().Type != RigidBodyType::Dynamic)
                if (PropertyGrid::Row(""))
                    ImGui::TextColored(ErrorColor, "Needs a dynamic Rigid Body (the chassis)");
            if (PropertyGrid::BeginGroup("Drive"))
            {
                const char* models[] = {"Direct (torque per wheel)", "Engine and gearbox"};
                changed |= EnumCombo("Drive model", c.DriveModel, models, 2, defaults.DriveModel);
                if (c.DriveModel == VehicleDriveModel::Direct)
                    changed |= PropertyGrid::Float("Drive torque", c.DirectDriveTorque, 1.0f, 0.0f, 1.0e6f, "%.0f N·m",
                                                   &defaults.DirectDriveTorque);
                changed |= PropertyGrid::Float("Ackermann", c.Ackermann, 0.01f, 0.0f, 1.0f, "%.2f", &defaults.Ackermann);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Road"))
            {
                const char* queries[] = {"Raycast", "Sphere sweep"};
                changed |= EnumCombo("Road query", c.RoadQuery, queries, 2, defaults.RoadQuery);
                changed |= LayerMask("Road layers", c.RoadLayerMask, "The layers the wheels drive on");
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Wheels"))
            {
                for (size_t i = 0; i < c.Wheels.size(); ++i)
                {
                    ImGui::PushID(int(i));
                    PropertyGrid::BeginFree();
                    const std::string name = WheelName(c.Wheels[i], i, scene);
                    if (ImGui::Selectable((ICON_FA_CIRCLE_DOT "  " + name).c_str(), selected == int(i)))
                        ui.SelectWheel(entity, selected == int(i) ? -1 : int(i));
                    PropertyGrid::EndFree();
                    if (selected == int(i) && PropertyGrid::BeginGroup("Wheel"))
                    {
                        changed |= WheelRows(c.Wheels[i], i, entity, scene, ui);
                        if (PropertyGrid::Row(""))
                        {
                            if (RowButton(ICON_FA_ARROW_UP "##up", "Move up", i > 0))
                                std::swap(c.Wheels[i], c.Wheels[i - 1]), ui.SelectWheel(entity, int(i) - 1), changed = true;
                            ImGui::SameLine();
                            if (RowButton(ICON_FA_ARROW_DOWN "##down", "Move down", i + 1 < c.Wheels.size()))
                                std::swap(c.Wheels[i], c.Wheels[i + 1]), ui.SelectWheel(entity, int(i) + 1), changed = true;
                            ImGui::SameLine();
                            if (RowButton(ICON_FA_TRASH "  Remove wheel"))
                            {
                                c.Wheels.erase(c.Wheels.begin() + std::ptrdiff_t(i));
                                ui.SelectWheel(entity, -1);
                                changed = true;
                            }
                        }
                        PropertyGrid::EndGroup();
                    }
                    ImGui::PopID();
                    if (changed && i >= c.Wheels.size())
                        break;
                }
                if (PropertyGrid::Row("", "Wheels from children named Wheel, Tire or Tyre; the radius comes from their meshes"))
                {
                    if (ImGui::SmallButton(ICON_FA_PLUS "  Add wheel"))
                        c.Wheels.push_back({}), ui.SelectWheel(entity, int(c.Wheels.size()) - 1), changed = true;
                    ImGui::SameLine();
                    if (RowButton(ICON_FA_WAND_MAGIC_SPARKLES "  From children",
                                  "Add a wheel for each child named Wheel, Tire or Tyre", scene != nullptr) &&
                        Fields::AddWheelsFromChildren(*scene, entity, c) > 0)
                        changed = true;
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Anti-roll bars", nullptr, nullptr, !c.AntiRollBars.empty()))
            {
                std::vector<std::string> names;
                for (size_t i = 0; i < c.Wheels.size(); ++i)
                    names.push_back(WheelName(c.Wheels[i], i, scene));
                std::vector<const char*> items;
                for (const auto& n : names)
                    items.push_back(n.c_str());
                for (size_t b = 0; b < c.AntiRollBars.size(); ++b)
                {
                    auto& bar = c.AntiRollBars[b];
                    ImGui::PushID(int(b));
                    int a = int(bar.WheelA), w = int(bar.WheelB);
                    if (!items.empty())
                    {
                        if (PropertyGrid::Combo("Wheel A", items.data(), int(items.size()), a))
                            bar.WheelA = uint32_t(a), changed = true;
                        if (PropertyGrid::Combo("Wheel B", items.data(), int(items.size()), w))
                            bar.WheelB = uint32_t(w), changed = true;
                    }
                    changed |= PropertyGrid::Float("Stiffness", bar.Stiffness, 100.0f, 0.0f, 1.0e8f, "%.0f N·m/rad");
                    if (PropertyGrid::Row("") && ImGui::SmallButton(ICON_FA_TRASH "  Remove bar"))
                    {
                        c.AntiRollBars.erase(c.AntiRollBars.begin() + std::ptrdiff_t(b));
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (PropertyGrid::Row(""))
                {
                    ImGui::BeginDisabled(c.Wheels.size() < 2);
                    if (ImGui::SmallButton(ICON_FA_PLUS "  Add anti-roll bar"))
                        c.AntiRollBars.push_back({}), changed = true;
                    ImGui::EndDisabled();
                }
                PropertyGrid::EndGroup();
            }
            if (c.DriveModel == VehicleDriveModel::Engine)
            {
                if (PropertyGrid::BeginGroup("Engine"))
                {
                    // Torque over normalised RPM (0 = idle, 1 = max), as a fraction of max torque.
                    const bool curved = !c.TorqueCurve.Keys.empty();
                    if (PropertyGrid::Row("Torque curve", "Torque as a fraction of the maximum over normalised RPM"))
                    {
                        if (ImGui::SmallButton(curved ? ICON_FA_XMARK "  Flat" : ICON_FA_CHART_LINE "  Use a curve"))
                        {
                            if (curved)
                                c.TorqueCurve.Keys.clear();
                            else
                                Fields::ResetTorqueCurve(c.TorqueCurve);
                            changed = true;
                        }
                    }
                    if (!c.TorqueCurve.Keys.empty())
                    {
                        PropertyGrid::BeginFree();
                        if (CurveEditor::Draw("##torque", c.TorqueCurve, 1, glm::vec4(1.0f), 0.01f, "Idle", "Max RPM"))
                            Fields::ClampTorqueCurve(c.TorqueCurve), changed = true;
                        PropertyGrid::EndFree();
                    }
                    changed |= PropertyGrid::Float("Max torque", c.MaxTorque, 1.0f, 0.0f, 1.0e6f, "%.0f N·m", &defaults.MaxTorque);
                    changed |= PropertyGrid::Float("Idle RPM", c.IdleRpm, 10.0f, 0.0f, c.MaxRpm, "%.0f", &defaults.IdleRpm);
                    changed |= PropertyGrid::Float("Max RPM", c.MaxRpm, 10.0f, c.IdleRpm, 1.0e5f, "%.0f", &defaults.MaxRpm);
                    changed |= PropertyGrid::Float("Inertia", c.EngineMomentOfInertia, 0.01f, 0.001f, 1.0e3f, "%.3f kg·m²",
                                                   &defaults.EngineMomentOfInertia);
                    changed |= PropertyGrid::Float(
                        "Damping", c.EngineDamping, 0.01f, 0.0f, 100.0f, "%.3f", &defaults.EngineDamping);
                    PropertyGrid::EndGroup();
                }
                if (PropertyGrid::BeginGroup("Gearbox"))
                {
                    for (size_t g = 0; g < c.GearRatios.size(); ++g)
                    {
                        ImGui::PushID(int(g));
                        const std::string label = "Gear " + std::to_string(g + 1);
                        if (PropertyGrid::Row(label.c_str(), "Ratios run from first gear down"))
                        {
                            const float button = ImGui::GetFrameHeight();
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
                            float ratio = c.GearRatios[g];
                            if (ImGui::DragFloat("##ratio", &ratio, 0.01f, Fields::MinimumGearRatio, 1000.0f, "%.3f"))
                                Fields::SetGearRatio(c.GearRatios, g, ratio), changed = true;
                            ImGui::SameLine();
                            ImGui::BeginDisabled(c.GearRatios.size() <= 1);
                            if (ImGui::Button(ICON_FA_XMARK "##remove", {button, button}))
                                changed |= Fields::RemoveGear(c.GearRatios, g);
                            ImGui::EndDisabled();
                        }
                        ImGui::PopID();
                    }
                    if (PropertyGrid::Row("") && ImGui::SmallButton(ICON_FA_PLUS "  Add gear"))
                        Fields::AddGear(c.GearRatios), changed = true;
                    changed |= PropertyGrid::Float(
                        "Reverse", c.ReverseRatio, 0.01f, -1000.0f, -0.01f, "%.3f", &defaults.ReverseRatio);
                    changed |= PropertyGrid::Float(
                        "Final drive", c.FinalDrive, 0.01f, 0.01f, 100.0f, "%.3f", &defaults.FinalDrive);
                    changed |= PropertyGrid::Float(
                        "Switch time", c.SwitchTime, 0.01f, 0.0f, 10.0f, "%.2f s", &defaults.SwitchTime);
                    changed |= PropertyGrid::Checkbox("Automatic", c.AutomaticGearbox, &defaults.AutomaticGearbox);
                    ImGui::BeginDisabled(!c.AutomaticGearbox);
                    changed |= PropertyGrid::Float(
                        "Up-shift at", c.UpShiftRatio, 0.01f, c.DownShiftRatio, 1.0f, "%.2f × max RPM", &defaults.UpShiftRatio);
                    changed |= PropertyGrid::Float("Down-shift at", c.DownShiftRatio, 0.01f, 0.0f, c.UpShiftRatio,
                                                   "%.2f × max RPM", &defaults.DownShiftRatio);
                    ImGui::EndDisabled();
                    PropertyGrid::EndGroup();
                }
                if (PropertyGrid::BeginGroup("Clutch and differential"))
                {
                    changed |= PropertyGrid::Float(
                        "Clutch strength", c.ClutchStrength, 0.1f, 0.0f, 1.0e4f, "%.1f", &defaults.ClutchStrength);
                    const char* differentials[] = {"Open, four wheels", "Open, front", "Open, rear", "Limited slip",
                                                   "Multi-wheel"};
                    changed |= EnumCombo("Differential", c.Differential, differentials, 5, defaults.Differential);
                    if (c.Differential == VehicleDifferential::LimitedSlip)
                    {
                        changed |= PropertyGrid::Float(
                            "Torque bias", c.TorqueBias, 0.01f, 1.0f, 100.0f, "%.2f", &defaults.TorqueBias);
                        if (c.Wheels.size() != 4)
                            if (PropertyGrid::Row(""))
                                ImGui::TextColored(ErrorColor, "Limited slip needs four wheels; use Multi-wheel");
                    }
                    PropertyGrid::EndGroup();
                }
            }
            if (PropertyGrid::BeginGroup("Substeps", nullptr, nullptr, false))
            {
                changed |= UIntRow("Low speed", c.LowSpeedSubsteps, 1, 64, &defaults.LowSpeedSubsteps);
                changed |= UIntRow("High speed", c.HighSpeedSubsteps, 1, 64, &defaults.HighSpeedSubsteps);
                changed |= PropertyGrid::Float("Speed threshold", c.SubstepSpeedThreshold, 0.1f, 0.0f, 1000.0f, "%.1f m/s",
                                               &defaults.SubstepSpeedThreshold);
                PropertyGrid::EndGroup();
            }
            // While playing: the telemetry (read-only).
            PhysicsWorld* world = scene ? scene->GetPhysicsWorld() : nullptr;
            if (auto state = world ? world->GetVehicleState(entity.GetUUID()) : std::nullopt)
                if (PropertyGrid::BeginGroup("Telemetry"))
                {
                    char text[96];
                    std::snprintf(text, sizeof(text), "%.1f km/h", state->ForwardSpeed * 3.6f);
                    PropertyGrid::Text("Speed", text);
                    std::snprintf(text, sizeof(text), "%.0f", state->EngineRpm);
                    PropertyGrid::Text("RPM", text);
                    PropertyGrid::Text("Gear", state->CurrentGear < 0 ? "R" : state->CurrentGear == 0 ? "N" :
                                                                                                   std::to_string(state->CurrentGear));
                    for (size_t i = 0; i < state->Wheels.size(); ++i)
                    {
                        const auto& w = state->Wheels[i];
                        std::snprintf(text, sizeof(text), "%s  compression %.2f  slip %.2f / %.2f",
                                      w.Grounded ? "grounded" : "in the air", w.SuspensionCompression, w.LongitudinalSlip,
                                      w.LateralSlip);
                        PropertyGrid::Text(("Wheel " + std::to_string(i)).c_str(), text, !w.Grounded);
                    }
                    PropertyGrid::EndGroup();
                }
            return changed;
        });
    }

    // ==================================================================== wind zones and fields

    void DrawWindZone(Entity entity, ComponentUI&)
    {
        static const WindZoneComponent defaults;
        EditComponent<WindZoneComponent>(entity, [](WindZoneComponent& c) {
            bool changed = false;
            if (PropertyGrid::BeginGroup("Zone"))
            {
                changed |= FieldShapeRows(c.Shape, c.Radius, c.HalfExtents, defaults.Shape);
                if (c.Shape != FieldShape::Infinite)
                    changed |= PropertyGrid::Float("Falloff band", c.FalloffBand, 0.01f, 0.0f, 1.0f, "%.2f",
                                                   &defaults.FalloffBand);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Wind"))
            {
                changed |= Direction("Direction", c.Direction, defaults.Direction);
                changed |= PropertyGrid::Float("Strength", c.Strength, 0.05f, 0.0f, 1000.0f, "%.2f m/s", &defaults.Strength);
                PropertyGrid::Text("", "Bodies opt in with Rigid Body > Wind; cloth and springs scale it", true);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Turbulence"))
            {
                changed |= PropertyGrid::Float("Amplitude", c.TurbulenceAmplitude, 0.01f, 0.0f, 1000.0f, "%.2f m/s",
                                               &defaults.TurbulenceAmplitude);
                changed |= PropertyGrid::Float(
                    "Frequency", c.TurbulenceFrequency, 0.01f, 0.0f, 100.0f, "%.2f Hz", &defaults.TurbulenceFrequency);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Pulse", nullptr, nullptr, c.PulseMagnitude != 0.0f))
            {
                changed |= PropertyGrid::Float(
                    "Magnitude", c.PulseMagnitude, 0.01f, 0.0f, 1000.0f, "%.2f m/s", &defaults.PulseMagnitude);
                changed |= PropertyGrid::Float(
                    "Frequency", c.PulseFrequency, 0.01f, 0.0f, 100.0f, "%.2f Hz", &defaults.PulseFrequency);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Filter", nullptr, nullptr, false))
            {
                changed |= LayerMask("Layers", c.LayerMask, "The layers this wind reaches");
                changed |= SystemMask(c.AffectMask);
                PropertyGrid::EndGroup();
            }
            return changed;
        });
    }

    void DrawForceField(Entity entity, ComponentUI&)
    {
        static const ForceFieldComponent defaults;
        EditComponent<ForceFieldComponent>(entity, [](ForceFieldComponent& c) {
            bool changed = false;
            if (PropertyGrid::BeginGroup("Field"))
            {
                const char* types[] = {"Wind", "Radial", "Directional", "Vortex", "Noise", "Drag"};
                changed |= EnumCombo("Type", c.Type, types, 6, defaults.Type);
                changed |= PropertyGrid::Float("Strength", c.Strength, 0.05f, -1.0e5f, 1.0e5f, "%.2f", &defaults.Strength);
                const char* modes[] = {"Force (mass matters)", "Acceleration"};
                int         mode = c.Acceleration ? 1 : 0, def = defaults.Acceleration ? 1 : 0;
                if (PropertyGrid::Combo("Mode", modes, 2, mode, &def))
                    c.Acceleration = mode == 1, changed = true;
                switch (c.Type)
                {
                    case FieldType::Radial:
                        PropertyGrid::Text("", "Pushes away from the centre (negative strength pulls)", true);
                        break;
                    case FieldType::Directional:
                    case FieldType::Wind:
                        changed |= Direction("Direction", c.Direction, defaults.Direction);
                        break;
                    case FieldType::Vortex:
                        changed |= Direction("Axis", c.Direction, defaults.Direction);
                        changed |= PropertyGrid::Float(
                            "Inward pull", c.InwardPull, 0.01f, -100.0f, 100.0f, "%.2f", &defaults.InwardPull);
                        break;
                    case FieldType::Noise:
                        changed |= PropertyGrid::Float(
                            "Frequency", c.Frequency, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.Frequency);
                        changed |= PropertyGrid::Vec3("Scroll", c.Scroll, 0.0f, 0.01f);
                        {
                            int       seed = int(c.Seed);
                            const int zero = 0;
                            if (PropertyGrid::Int("Seed", seed, 0, 65535, &zero))
                                c.Seed = uint32_t(std::max(seed, 0)), changed = true;
                        }
                        changed |= PropertyGrid::Checkbox("Curl (divergence-free)", c.CurlNoise, &defaults.CurlNoise);
                        break;
                    case FieldType::Drag:
                        PropertyGrid::Text("", "Opposes velocity", true);
                        break;
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Shape"))
            {
                changed |= FieldShapeRows(c.Shape, c.Radius, c.HalfExtents, defaults.Shape);
                if (c.Shape != FieldShape::Infinite)
                {
                    const char* falloffs[] = {"None", "Linear", "Inverse square", "Curve"};
                    changed |= EnumCombo("Falloff", c.Falloff, falloffs, 4, defaults.Falloff);
                    if (c.Falloff == FieldFalloff::Curve)
                    {
                        TimelineCurve curve = Fields::FalloffCurveKeys(c.FalloffCurve);
                        PropertyGrid::BeginFree();
                        if (CurveEditor::Draw("##falloff", curve, 1, glm::vec4(1.0f), 0.01f, "Centre", "Edge"))
                            c.FalloffCurve = Fields::SampleFalloffCurve(curve), changed = true;
                        PropertyGrid::EndFree();
                    }
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Filter", nullptr, nullptr, false))
            {
                changed |= LayerMask("Layers", c.LayerMask, "The layers this field reaches");
                changed |= SystemMask(c.AffectMask);
                PropertyGrid::EndGroup();
            }
            return changed;
        });
    }
} // namespace Kans::ComponentEditors
