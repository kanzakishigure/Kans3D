#include <imgui.h>
#include <cfloat>
#include <cstring>
#include <functional>
#include <string>
#include <glm/gtc/quaternion.hpp>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Physics/ClothAsset.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "KansEditorCore/Physics/SecondaryMotionEditing.h"
#include "UI/CurveEditor.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    namespace
    {
        namespace Edit = SecondaryMotionEditing;

        template<typename T, typename F>
        void EditComponent(Entity entity, F&& draw)
        {
            T    copy    = entity.GetComponent<T>();
            bool changed = draw(copy);
            if (changed)
                entity.PatchComponent<T>([&](T& c) { c = copy; });
        }

        // ---------------------------------------------------------------- presets

        struct PresetState
        {
            std::filesystem::path         BuiltIn, Project;
            std::vector<Edit::MotionPreset> List;
            std::vector<std::string>      Errors;
            // The name dialog: saving the captured values, or renaming `Target`.
            enum class Dialog
            {
                None,
                Save,
                Rename
            };
            Dialog             Kind = Dialog::None;
            bool               Open = false;
            char               Name[65] {};
            std::string        Problem;
            Edit::MotionPreset Pending, Target;
        };
        PresetState& Presets()
        {
            static PresetState state;
            return state;
        }
        void ReloadPresets()
        {
            auto& s = Presets();
            s.Errors.clear();
            s.List = Edit::ListPresets(s.BuiltIn, s.Project, &s.Errors);
        }

        void OpenNameDialog(PresetState::Dialog kind)
        {
            auto& s   = Presets();
            s.Kind    = kind;
            s.Problem.clear();
            s.Open = true;
        }

        // The name dialog of Save as preset and Rename; drawn where the menu is.
        void PresetNameDialog()
        {
            auto& s = Presets();
            if (s.Open)
            {
                ImGui::OpenPopup("Preset name");
                s.Open = false;
            }
            ImGui::SetNextWindowSize({EditorTheme::Px(320.0f), 0.0f});
            if (!ImGui::BeginPopupModal("Preset name", nullptr, ImGuiWindowFlags_NoResize))
                return;
            const bool saving = s.Kind == PresetState::Dialog::Save;
            ImGui::TextUnformatted(saving ? "Save the current values as a project preset" : "Rename the preset");
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool enter = ImGui::InputText("##name", s.Name, sizeof(s.Name), ImGuiInputTextFlags_EnterReturnsTrue);
            if (!s.Problem.empty())
                ImGui::TextColored(ErrorColor, "%s", s.Problem.c_str());
            if (ImGui::Button(saving ? "Save" : "Rename") || enter)
            {
                std::optional<AssetError> error;
                if (saving)
                {
                    auto saved = Edit::SaveProjectPreset(s.Project, s.Name, s.Pending, s.List);
                    if (auto* e = std::get_if<AssetError>(&saved))
                        error = *e;
                }
                else
                    error = Edit::RenameProjectPreset(s.Target, s.Name, s.List);
                if (error)
                    s.Problem = error->Message;
                else
                {
                    ReloadPresets();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // A "Presets" button opening MotionPresetMenuItems; true when a preset was applied.
        bool PresetMenu(const char*                                           id,
                        bool                                                  spring,
                        const std::function<Edit::MotionPreset()>&            capture,
                        const std::function<void(const Edit::MotionPreset&)>& apply)
        {
            bool applied = false;
            ImGui::PushID(id);
            if (ImGui::SmallButton(ICON_FA_WAND_MAGIC_SPARKLES "  Presets " ICON_FA_CARET_DOWN))
            {
                ReloadPresets();
                ImGui::OpenPopup("##presets");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(spring ? "Simulation values and curves only: bones and colliders stay" :
                                           "Simulation values only: the asset, section, maps and colliders stay");
            if (ImGui::BeginPopup("##presets"))
            {
                applied = MotionPresetMenuItems(spring, capture, apply);
                ImGui::EndPopup();
            }
            PresetNameDialog();
            ImGui::PopID();
            return applied;
        }


        // ---------------------------------------------------------------- shared rows

        // A base value that a curve along the chain can replace (0 = root, 1 = tip).
        bool CurvedFloat(const char*   label,
                         float&        value,
                         TimelineCurve& curve,
                         float         speed,
                         float         min,
                         float         max,
                         const char*   format,
                         const float*  reset)
        {
            const bool curved   = !curve.Keys.empty();
            const bool modified = curved || (reset && std::abs(value - *reset) > 1e-6f);
            if (!PropertyGrid::Row(label, "The curve button varies it along the chain (root to tip)", modified))
                return false;
            ImGui::PushID(label);
            bool        changed = false;
            const float button  = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
            ImGui::BeginDisabled(curved);
            changed |= ImGui::DragFloat("##value", &value, speed, min, max, curved ? "Curve" : format);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  curved ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) :
                                           ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::Button(ICON_FA_CHART_LINE "##curve", {button, button}))
            {
                if (curved)
                    curve.Keys.clear();
                else
                    Edit::ResetCurve(curve, glm::vec4(value));
                changed = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(curved ? "Use one value for the whole chain" : "Vary along the chain with a curve");
            if (!curve.Keys.empty())
            {
                PropertyGrid::BeginFree();
                changed |= CurveEditor::Draw("##curveEditor", curve, 1, glm::vec4(value), speed);
                PropertyGrid::EndFree();
            }
            ImGui::PopID();
            return changed;
        }

        // A list of entities (drop entities from the Hierarchy on the Add row).
        bool EntityList(const char* label, std::vector<UUID>& ids, Scene* scene, const char* addLabel)
        {
            bool changed = false;
            ImGui::PushID(label);
            for (size_t i = 0; i < ids.size(); ++i)
            {
                ImGui::PushID(int(i));
                uint64_t id = uint64_t(ids[i]);
                if (EntityField(i == 0 ? label : "", id, scene))
                    ids[i] = UUID(id), changed = true;
                if (PropertyGrid::Row(""))
                    if (ImGui::SmallButton(ICON_FA_XMARK "  Remove"))
                    {
                        ids.erase(ids.begin() + std::ptrdiff_t(i));
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                ImGui::PopID();
            }
            if (PropertyGrid::Row(ids.empty() ? label : "", "Drop an entity from the Hierarchy here"))
            {
                if (ImGui::Button((std::string(ICON_FA_PLUS "  ") + addLabel).c_str(), {-FLT_MIN, 0.0f}))
                    ids.push_back(UUID(0)), changed = true;
                if (ImGui::BeginDragDropTarget())
                {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("entity_payload");
                        payload && payload->DataSize == sizeof(uint64_t))
                    {
                        const UUID dropped(*static_cast<const uint64_t*>(payload->Data));
                        if (std::find(ids.begin(), ids.end(), dropped) == ids.end())
                            ids.push_back(dropped), changed = true;
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            ImGui::PopID();
            return changed;
        }

        bool UuidField(const char* label, UUID& id, Scene* scene)
        {
            uint64_t value = uint64_t(id);
            if (!EntityField(label, value, scene))
                return false;
            id = UUID(value);
            return true;
        }

        bool UIntField(const char* label, uint32_t& value, int min, int max, const uint32_t* reset)
        {
            int       v = int(value);
            const int r = reset ? int(*reset) : 0;
            if (!PropertyGrid::Int(label, v, min, max, reset ? &r : nullptr))
                return false;
            value = uint32_t(std::clamp(v, min, max));
            return true;
        }

        bool OffsetRows(Physics::ShapeOffset& offset)
        {
            bool      changed = PropertyGrid::Vec3("Center", offset.Position, 0.0f, 0.01f);
            glm::vec3 euler   = glm::degrees(glm::eulerAngles(offset.Rotation));
            if (PropertyGrid::Vec3("Rotation", euler, 0.0f, 0.5f))
                offset.Rotation = glm::quat(glm::radians(euler)), changed = true;
            if (PropertyGrid::Vec3("Scale", offset.Scale, 1.0f, 0.01f))
                offset.Scale = glm::max(offset.Scale, glm::vec3(PhysicsEditing::MinimumSize)), changed = true;
            return changed;
        }

        std::string BoneName(Entity e) { return e ? e.GetComponent<TagComponent>().Tag : std::string("(missing)"); }

        // ---------------------------------------------------------------- spring chains

        bool ChainRows(Entity owner, SpringBoneComponent& component, size_t index, ComponentUI& ui)
        {
            static const SpringBoneChain defaults;
            Scene*                       scene   = ui.GetScene();
            auto&                        chain   = component.Chains[index];
            bool                         changed = false;
            auto*                        picker  = ui.GetChainPicker();

            // Bones.
            if (picker && picker->IsPicking(owner.GetUUID(), index))
            {
                PropertyGrid::Text("Picking", picker->Prompt());
                if (PropertyGrid::Row(""))
                    if (ImGui::SmallButton(ICON_FA_XMARK "  Stop picking"))
                        picker->Cancel();
            }
            else if (picker && PropertyGrid::Row("", "Click the root bone, then its end bone, in the Hierarchy or the viewport"))
                if (ImGui::SmallButton(ICON_FA_CROSSHAIRS "  Pick from Hierarchy"))
                    picker->Begin(owner.GetUUID(), index);
            changed |= UuidField("Root bone", chain.RootBone, scene);
            changed |= UuidField("End bone", chain.EndBone, scene);
            changed |= EntityList("Excluded bones", chain.ExcludedBones, scene, "Exclude a bone");
            changed |= PropertyGrid::Float(
                "End extension", chain.EndExtension, 0.005f, 0.0f, 100.0f, "%.3f m", &defaults.EndExtension);
            if (scene)
            {
                const auto  resolved = Edit::ResolveChain(*scene, chain);
                std::string joints;
                for (size_t i = 0; i < resolved.Path.size(); ++i)
                {
                    if (i)
                        joints += "  >  ";
                    joints += BoneName(resolved.Path[i]);
                    if (i >= resolved.Simulated)
                        joints += " (tail)";
                }
                if (joints.empty())
                    joints = uint64_t(chain.RootBone) ? (uint64_t(chain.EndBone) ? "The end bone is not below the root" :
                                                                                   "No bones") :
                                                        "Set a root bone";
                const std::string label = "Joints (" + std::to_string(resolved.Simulated) + ")";
                PropertyGrid::Text(label.c_str(), joints, resolved.Path.empty());
            }

            if (PropertyGrid::BeginGroup("Motion"))
            {
                changed |= CurvedFloat("Stiffness", chain.Stiffness, chain.StiffnessCurve, 0.5f, 0.0f, 10000.0f, "%.1f",
                                       &defaults.Stiffness);
                changed |= CurvedFloat("Drag", chain.Drag, chain.DragCurve, 0.005f, 0.0f, 1.0f, "%.3f", &defaults.Drag);
                changed |= CurvedFloat("Gravity", chain.GravityPower, chain.GravityPowerCurve, 0.05f, 0.0f, 1000.0f,
                                       "%.2f m/s²", &defaults.GravityPower);
                // The direction: a vector, or a curve of vectors.
                const bool curved = !chain.GravityDirectionCurve.Keys.empty();
                if (PropertyGrid::Row("Gravity direction", nullptr, curved || chain.GravityDirection != defaults.GravityDirection))
                {
                    ImGui::PushID("gravityDirection");
                    const float button = ImGui::GetFrameHeight();
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
                    ImGui::BeginDisabled(curved);
                    changed |= ImGui::DragFloat3("##value", &chain.GravityDirection.x, 0.01f, -1.0f, 1.0f, "%.2f");
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_FA_CHART_LINE "##curve", {button, button}))
                    {
                        if (curved)
                            chain.GravityDirectionCurve.Keys.clear();
                        else
                            Edit::ResetCurve(chain.GravityDirectionCurve, glm::vec4(chain.GravityDirection, 0.0f));
                        changed = true;
                    }
                    if (!chain.GravityDirectionCurve.Keys.empty())
                    {
                        PropertyGrid::BeginFree();
                        changed |= CurveEditor::Draw(
                            "##directionCurve", chain.GravityDirectionCurve, 3, glm::vec4(chain.GravityDirection, 0.0f));
                        PropertyGrid::EndFree();
                    }
                    ImGui::PopID();
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Collision and limits"))
            {
                changed |= CurvedFloat("Hit radius", chain.HitRadius, chain.HitRadiusCurve, 0.001f, 0.0f, 10.0f, "%.3f m",
                                       &defaults.HitRadius);
                const char* axes[] = {"All (cone limit)", "X (hinge)", "Y (hinge)", "Z (hinge)"};
                int         axis = int(chain.RotationAxis), axisDefault = int(defaults.RotationAxis);
                if (PropertyGrid::Combo("Rotation axis", axes, 4, axis, &axisDefault))
                    chain.RotationAxis = SpringRotationAxis(axis), changed = true;
                changed |= CurvedFloat("Angle limit", chain.AngleLimit, chain.AngleLimitCurve, 0.5f, 0.0f, 180.0f,
                                       chain.AngleLimit >= 180.0f ? "Off" : "%.1f°", &defaults.AngleLimit);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Runtime multipliers", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Float("Stiffness", chain.StiffnessMultiplier, 0.01f, 0.0f, 100.0f, "%.2f×",
                                               &defaults.StiffnessMultiplier);
                changed |= PropertyGrid::Float(
                    "Drag", chain.DragMultiplier, 0.01f, 0.0f, 100.0f, "%.2f×", &defaults.DragMultiplier);
                changed |= PropertyGrid::Float("Gravity", chain.GravityMultiplier, 0.01f, 0.0f, 100.0f, "%.2f×",
                                               &defaults.GravityMultiplier);
                PropertyGrid::EndGroup();
            }
            return changed;
        }

        std::string ChainTitle(Scene* scene, const SpringBoneChain& chain, size_t index)
        {
            std::string title = "Chain " + std::to_string(index + 1);
            if (scene && uint64_t(chain.RootBone))
                title += "  " + BoneName(scene->GetEntityByUUID(chain.RootBone));
            return title;
        }
    } // namespace

    bool MotionPresetMenuItems(bool                                                                  spring,
                               const std::function<SecondaryMotionEditing::MotionPreset()>&           capture,
                               const std::function<void(const SecondaryMotionEditing::MotionPreset&)>& apply)
    {
        auto&      s       = Presets();
        bool       applied = false;
        const auto matches = [&](const Edit::MotionPreset& p) { return spring ? bool(p.Spring) : bool(p.Cloth); };
        if (s.List.empty() && s.Errors.empty())
            ReloadPresets();
        bool project = false, any = false;
        for (const auto& preset : s.List)
        {
            if (!matches(preset))
                continue;
            if (preset.Origin == Edit::PresetOrigin::Project && !project)
            {
                if (any)
                    ImGui::Separator();
                ImGui::TextDisabled("Project");
                project = true;
            }
            any = true;
            ImGui::PushID(preset.File.u8string().c_str());
            if (ImGui::MenuItem(preset.Label.c_str()))
                apply(preset), applied = true;
            ImGui::PopID();
        }
        if (!any)
            ImGui::TextDisabled("No presets found");
        for (const auto& error : s.Errors)
            ImGui::TextColored(ErrorColor, "%s", error.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save as preset...", nullptr, false, !s.Project.empty()))
        {
            s.Pending = capture();
            s.Name[0] = 0;
            OpenNameDialog(PresetState::Dialog::Save);
        }
        if (project && ImGui::BeginMenu(ICON_FA_PEN "  Project presets"))
        {
            for (const auto& preset : s.List)
                if (preset.Origin == Edit::PresetOrigin::Project && matches(preset))
                {
                    ImGui::PushID(preset.File.u8string().c_str());
                    if (ImGui::BeginMenu(preset.Label.c_str()))
                    {
                        if (ImGui::MenuItem("Rename..."))
                        {
                            s.Target = preset;
                            std::strncpy(s.Name, preset.Name.c_str(), sizeof(s.Name) - 1);
                            s.Name[sizeof(s.Name) - 1] = 0;
                            OpenNameDialog(PresetState::Dialog::Rename);
                        }
                        if (ImGui::MenuItem("Delete"))
                        {
                            if (auto error = Edit::DeleteProjectPreset(preset))
                                s.Errors.push_back(error->Message);
                            else
                                ReloadPresets();
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::PopID();
                    if (s.List.empty())
                        break;
                }
            ImGui::EndMenu();
        }
        return applied;
    }

    void SetMotionPresetDirectories(std::filesystem::path builtIn, std::filesystem::path project)
    {
        Presets().BuiltIn = std::move(builtIn);
        Presets().Project = std::move(project);
        Presets().List.clear();
    }

    void DrawSpringBone(Entity entity, ComponentUI& ui)
    {
        static const SpringBoneComponent defaults;
        Scene*                           scene = ui.GetScene();
        EditComponent<SpringBoneComponent>(entity, [&](SpringBoneComponent& c) {
            bool changed = false;
            if (PropertyGrid::Row("", "Applies to every chain and to the inertia, wind and substeps"))
                changed |= PresetMenu(
                    "componentPresets",
                    true,
                    [&] {
                        Edit::MotionPreset preset;
                        preset.Spring = Edit::CaptureSpringPreset(c, 0);
                        return preset;
                    },
                    [&](const Edit::MotionPreset& preset) { Edit::ApplySpringPreset(c, *preset.Spring); });
            if (PropertyGrid::BeginGroup("Simulation"))
            {
                changed |= PropertyGrid::Checkbox("Enabled", c.Enabled, &defaults.Enabled);
                changed |= UuidField("Center", c.Center, scene);
                changed |= PropertyGrid::Float(
                    "Inertia scale", c.InertiaScale, 0.01f, 0.0f, 1.0f, "%.2f", &defaults.InertiaScale);
                changed |= PropertyGrid::Float("Wind scale", c.WindScale, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.WindScale);
                changed |= PropertyGrid::Float(
                    "Substep rate", c.SubstepRate, 1.0f, 1.0f, 1000.0f, "%.0f Hz", &defaults.SubstepRate);
                changed |= UIntField("Max substeps", c.MaxSubsteps, 1, 1024, &defaults.MaxSubsteps);
                changed |= PropertyGrid::Float("Teleport distance", c.TeleportDistance, 0.05f, 0.0f, 1.0e4f,
                                               c.TeleportDistance > 0 ? "%.2f m" : "Off", &defaults.TeleportDistance);
                changed |= PropertyGrid::Float("Teleport rotation", c.TeleportRotation, 0.5f, 0.0f, 180.0f,
                                               c.TeleportRotation > 0 ? "%.1f°" : "Off", &defaults.TeleportRotation);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Collider groups"))
            {
                changed |= EntityList("Groups", c.ColliderGroups, scene, "Add a group");
                PropertyGrid::EndGroup();
            }
            for (size_t i = 0; i < c.Chains.size(); ++i)
            {
                ImGui::PushID(int(i));
                const std::string title = ChainTitle(scene, c.Chains[i], i);
                if (PropertyGrid::BeginGroup(title.c_str()))
                {
                    bool removed = false, moved = false;
                    if (PropertyGrid::Row(""))
                    {
                        changed |= PresetMenu(
                            "chainPresets",
                            true,
                            [&] {
                                Edit::MotionPreset preset;
                                preset.Spring = Edit::CaptureSpringPreset(c, i);
                                return preset;
                            },
                            [&](const Edit::MotionPreset& preset) { Edit::ApplySpringPreset(c.Chains[i], *preset.Spring); });
                        ImGui::SameLine();
                        ImGui::BeginDisabled(i == 0);
                        if (ImGui::SmallButton(ICON_FA_ARROW_UP))
                            std::swap(c.Chains[i], c.Chains[i - 1]), changed = moved = true;
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::BeginDisabled(i + 1 == c.Chains.size());
                        if (ImGui::SmallButton(ICON_FA_ARROW_DOWN))
                            std::swap(c.Chains[i], c.Chains[i + 1]), changed = moved = true;
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::SmallButton(ICON_FA_TRASH_CAN))
                            removed = true;
                    }
                    // A moved chain shows its new place from the next frame.
                    if (!removed && !moved)
                        changed |= ChainRows(entity, c, i, ui);
                    PropertyGrid::EndGroup();
                    if (removed)
                    {
                        if (auto* picker = ui.GetChainPicker(); picker && picker->Owner() == entity.GetUUID())
                            picker->Cancel();
                        c.Chains.erase(c.Chains.begin() + std::ptrdiff_t(i));
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }
            if (PropertyGrid::Row(""))
                if (ImGui::Button(ICON_FA_PLUS "  Add chain", {-FLT_MIN, 0.0f}))
                    c.Chains.emplace_back(), changed = true;
            return changed;
        });
    }

    void DrawMotionCollider(Entity entity, ComponentUI&)
    {
        EditComponent<MotionColliderComponent>(entity, [&](MotionColliderComponent& c) {
            bool changed = false;
            if (PropertyGrid::BeginGroup("Shape"))
            {
                const char* types[] = {"Sphere", "Capsule", "Plane", "Box"};
                int         type    = int(c.Shape.index()), sphere = 0;
                if (PropertyGrid::Combo("Type", types, 4, type, &sphere))
                {
                    // The placement carries over to the new shape.
                    const auto offset = std::visit([](const auto& s) { return s.Offset; }, c.Shape);
                    switch (type)
                    {
                        case 0:
                            c.Shape = Physics::Sphere {0.1f, offset};
                            break;
                        case 1:
                            c.Shape = Physics::Capsule {0.1f, 0.2f, Physics::Axis::Y, offset};
                            break;
                        case 2:
                            c.Shape = Physics::Plane {{0, 1, 0}, offset};
                            break;
                        default:
                            c.Shape = Physics::Box {glm::vec3(0.1f), offset};
                            break;
                    }
                    changed = true;
                }
                std::visit(
                    [&](auto& s) {
                        using T = std::decay_t<decltype(s)>;
                        if constexpr (std::is_same_v<T, Physics::Sphere>)
                        {
                            if (PropertyGrid::Float("Radius", s.Radius, 0.005f, PhysicsEditing::MinimumSize, 1.0e4f, "%.3f m"))
                                changed = true;
                        }
                        else if constexpr (std::is_same_v<T, Physics::Capsule>)
                        {
                            changed |= PropertyGrid::Float(
                                "Radius", s.Radius, 0.005f, PhysicsEditing::MinimumSize, 1.0e4f, "%.3f m");
                            float height = s.HalfHeight * 2.0f;
                            if (PropertyGrid::Float("Height", height, 0.01f, 0.0f, 1.0e4f, "%.3f m"))
                                s.HalfHeight = std::max(0.0f, height * 0.5f), changed = true;
                            const char* directions[] = {"X", "Y", "Z"};
                            int         direction = int(s.Direction), up = 1;
                            if (PropertyGrid::Combo("Direction", directions, 3, direction, &up))
                                s.Direction = Physics::Axis(direction), changed = true;
                        }
                        else if constexpr (std::is_same_v<T, Physics::Plane>)
                        {
                            glm::vec3 normal = s.Normal;
                            if (PropertyGrid::Vec3("Normal", normal, glm::vec3(0, 1, 0), 0.01f) &&
                                glm::length(normal) > 1e-4f)
                                s.Normal = normal, changed = true;
                        }
                        else
                        {
                            glm::vec3 size = glm::abs(s.HalfExtents) * 2.0f;
                            if (PropertyGrid::Vec3("Size", size, glm::vec3(0.2f), 0.01f))
                                s.HalfExtents = glm::max(size * 0.5f, glm::vec3(PhysicsEditing::MinimumSize)),
                                changed       = true;
                        }
                        changed |= OffsetRows(s.Offset);
                    },
                    c.Shape);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Collision"))
            {
                static const bool off = false;
                if (Edit::SupportsInside(c.Shape))
                    changed |= PropertyGrid::Checkbox("Inside", c.Inside, &off);
                else
                    PropertyGrid::Text("Inside", "Spheres and capsules only (planes and boxes push out)", true);
                PropertyGrid::EndGroup();
            }
            return changed;
        });
    }

    void DrawCloth(Entity entity, ComponentUI& ui)
    {
        static const ClothComponent defaults;
        Scene*                      scene = ui.GetScene();
        EditComponent<ClothComponent>(entity, [&](ClothComponent& c) {
            bool changed = false;
            if (PropertyGrid::Row("", "Simulation values only"))
                changed |= PresetMenu(
                    "clothPresets",
                    false,
                    [&] {
                        Edit::MotionPreset preset;
                        preset.Cloth = Edit::CaptureClothPreset(c);
                        return preset;
                    },
                    [&](const Edit::MotionPreset& preset) { Edit::ApplyClothPreset(c, *preset.Cloth); });

            if (PropertyGrid::BeginGroup("Asset"))
            {
                Ref<const MeshAsset> mesh = scene ? scene->ResolveMesh(entity) : nullptr;
                const AssetID        meshID =
                    entity.HasComponent<MeshComponent>() ? entity.GetComponent<MeshComponent>().Mesh :
                    mesh                                ? mesh->assetID :
                                                          AssetID {};
                // The section, by its material slot.
                if (mesh && !mesh->GetData().Sections.empty())
                {
                    const auto               labels = Edit::SectionLabels(mesh->GetData());
                    std::vector<const char*> items;
                    for (const auto& label : labels)
                        items.push_back(label.c_str());
                    int section = int(c.Section), first = 0;
                    if (section >= int(items.size()))
                        PropertyGrid::Text("Section", "Section " + std::to_string(c.Section) + " (missing)", true);
                    else if (PropertyGrid::Combo("Section", items.data(), int(items.size()), section, &first))
                        c.Section = uint32_t(section), changed = true;
                }
                else
                    changed |= UIntField("Section", c.Section, 0, 4096, &defaults.Section);
                changed |= ui.AssetPicker("Cloth asset", AssetType::Cloth, c.Asset);
                if (PropertyGrid::Row("", "Welds the section's vertices into particles and writes a .cloth file beside the mesh"))
                {
                    ImGui::BeginDisabled(!mesh);
                    if (ImGui::SmallButton(ICON_FA_PLUS "  Create cloth asset for this section"))
                        ui.CreateClothAsset(entity, c.Section, false);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN "##inBrowserFolder"))
                        ui.CreateClothAsset(entity, c.Section, true);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Create it in the Content Browser's open folder instead");
                    ImGui::EndDisabled();
                }
                // Binding: what the engine would say about this asset on this mesh section.
                std::string status;
                bool        problem = true;
                // The asset: loaded through the asset manager, or the one the scene has bound.
                Ref<const ClothAsset> asset;
                if (const auto* state = scene ? scene->GetClothState(entity) : nullptr;
                    state && state->Source && state->Source->assetID == c.Asset)
                    asset = state->Source;
                bool loading = false;
                if (!asset && c.Asset.IsValid() && ui.GetAssets())
                {
                    const auto loaded = ui.GetAssets()->Request<ClothAsset>(c.Asset);
                    asset             = loaded.Asset;
                    loading           = loaded.State == AssetLoadState::Loading;
                }
                if (!c.Asset.IsValid())
                    status = "No cloth asset";
                else if (!asset)
                    status = loading ? "Loading..." : "The asset did not load";
                else if (!mesh)
                    status = "No mesh on this entity";
                else
                {
                    status = Edit::BindingProblem(asset->GetData(), meshID, mesh->GetData(), c.Section);
                    if (status.empty())
                    {
                        problem = false;
                        status  = "OK  (" + std::to_string(asset->GetData().RestPositions.size()) + " particles)";
                    }
                }
                if (problem)
                {
                    if (PropertyGrid::Row("Binding"))
                        ImGui::TextColored(ErrorColor, "%s", status.c_str());
                }
                else
                    PropertyGrid::Text("Binding", status);
                if (PropertyGrid::Row("", "Paint the max distance, backstop and stiffness maps in the viewport"))
                {
                    const bool painting = ui.IsPaintingCloth(entity);
                    ImGui::BeginDisabled(painting || problem);
                    if (ImGui::SmallButton(painting ? ICON_FA_PAINTBRUSH "  Painting in the viewport" :
                                                      ICON_FA_PAINTBRUSH "  Paint maps"))
                        ui.PaintCloth(entity);
                    ImGui::EndDisabled();
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Solver"))
            {
                changed |= PropertyGrid::Checkbox("Enabled", c.Enabled, &defaults.Enabled);
                changed |= PropertyGrid::Float(
                    "Substep rate", c.SubstepRate, 1.0f, 1.0f, 1000.0f, "%.0f Hz", &defaults.SubstepRate);
                changed |= UIntField("Max substeps", c.MaxSubsteps, 1, 1024, &defaults.MaxSubsteps);
                changed |= UIntField("Iterations", c.Iterations, 1, 128, &defaults.Iterations);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Material"))
            {
                changed |= PropertyGrid::Float("Stretch compliance", c.StretchCompliance, 1e-8f, 0.0f, 1.0f, "%.2e",
                                               &defaults.StretchCompliance);
                changed |= PropertyGrid::Float(
                    "Bend compliance", c.BendCompliance, 1e-6f, 0.0f, 10.0f, "%.2e", &defaults.BendCompliance);
                changed |= PropertyGrid::Float(
                    "Tether scale", c.TetherScale, 0.005f, 1.0f, 10.0f, "%.3f", &defaults.TetherScale);
                changed |= PropertyGrid::Float("Damping", c.Damping, 0.001f, 0.0f, 1.0f, "%.3f", &defaults.Damping);
                changed |= PropertyGrid::Float(
                    "Velocity drag", c.VelocityDrag, 0.005f, 0.0f, 100.0f, "%.3f", &defaults.VelocityDrag);
                changed |= PropertyGrid::Float(
                    "Particle mass", c.ParticleMass, 0.0005f, 1e-6f, 100.0f, "%.4f kg", &defaults.ParticleMass);
                changed |= PropertyGrid::Float("Area density", c.AreaDensity, 0.005f, 0.0f, 100.0f,
                                               c.AreaDensity > 0 ? "%.3f kg/m²" : "Off (particle mass)",
                                               &defaults.AreaDensity);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Motion"))
            {
                changed |= PropertyGrid::Float(
                    "Gravity scale", c.GravityScale, 0.01f, -100.0f, 100.0f, "%.2f", &defaults.GravityScale);
                changed |= PropertyGrid::Float("Wind scale", c.WindScale, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.WindScale);
                changed |= PropertyGrid::Float("Lift", c.Lift, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.Lift);
                changed |= PropertyGrid::Float("Drag", c.Drag, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.Drag);
                changed |= UuidField("Center", c.Center, scene);
                changed |= PropertyGrid::Float(
                    "Inertia scale", c.InertiaScale, 0.01f, 0.0f, 1.0f, "%.2f", &defaults.InertiaScale);
                changed |= PropertyGrid::Float("Linear inertia limit", c.LinearInertiaLimit, 0.1f, 0.0f, 1.0e4f,
                                               "%.1f m/s", &defaults.LinearInertiaLimit);
                changed |= PropertyGrid::Float("Angular inertia limit", c.AngularInertiaLimit, 1.0f, 0.0f, 1.0e5f,
                                               "%.0f°/s", &defaults.AngularInertiaLimit);
                changed |= PropertyGrid::Float("Teleport distance", c.TeleportDistance, 0.05f, 0.0f, 1.0e4f,
                                               c.TeleportDistance > 0 ? "%.2f m" : "Off", &defaults.TeleportDistance);
                changed |= PropertyGrid::Float("Teleport rotation", c.TeleportRotation, 0.5f, 0.0f, 180.0f,
                                               c.TeleportRotation > 0 ? "%.1f°" : "Off", &defaults.TeleportRotation);
                changed |= PropertyGrid::Float("Max distance scale", c.MaxDistanceScale, 0.01f, 0.0f, 100.0f, "%.2f×",
                                               &defaults.MaxDistanceScale);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Collision"))
            {
                changed |= EntityList("Collider groups", c.ColliderGroups, scene, "Add a group");
                changed |= PropertyGrid::Float(
                    "Thickness", c.Thickness, 0.0005f, 0.0f, 1.0f, "%.4f m", &defaults.Thickness);
                changed |= PropertyGrid::Checkbox("Ground plane", c.GroundPlane, &defaults.GroundPlane);
                if (c.GroundPlane)
                    changed |= PropertyGrid::Float(
                        "Ground height", c.GroundHeight, 0.01f, -1.0e4f, 1.0e4f, "%.2f m", &defaults.GroundHeight);
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("LOD", nullptr, nullptr, false))
            {
                changed |= PropertyGrid::Float("Disable distance", c.DisableDistance, 0.5f, 0.0f, 1.0e5f,
                                               c.DisableDistance > 0 ? "%.1f m" : "Off", &defaults.DisableDistance);
                changed |= PropertyGrid::Checkbox(
                    "Simulate when not visible", c.SimulateWhenNotVisible, &defaults.SimulateWhenNotVisible);
                PropertyGrid::EndGroup();
            }
            return changed;
        });
    }
} // namespace Kans::ComponentEditors
