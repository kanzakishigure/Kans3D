#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Animation/DeformationComponents.h"
#include "Kans3D/Renderer/Shading/Skinning.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    namespace
    {
        // The skeleton the model's pose is built on: the Animator's, the IK component's own, or the
        // one the scene resolved for a model without playback.
        const SkeletonData* ModelSkeleton(Entity entity)
        {
            if (entity.HasComponent<AnimatorComponent>() && entity.GetComponent<AnimatorComponent>().SkeletonSource)
                return &entity.GetComponent<AnimatorComponent>().SkeletonSource->GetData();
            if (entity.HasComponent<IKComponent>() && entity.GetComponent<IKComponent>().SkeletonSource)
                return &entity.GetComponent<IKComponent>().SkeletonSource->GetData();
            if (entity.HasComponent<ModelPoseComponent>() && entity.GetComponent<ModelPoseComponent>().SkeletonSource)
                return &entity.GetComponent<ModelPoseComponent>().SkeletonSource->GetData();
            return nullptr;
        }

        bool DrawConstraint(IKConstraint& constraint, const SkeletonData* skeleton, Scene* scene)
        {
            bool changed = false;
            if (PropertyGrid::Row("Name"))
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::InputText("##name", &constraint.Name);
            }
            changed |= PropertyGrid::Float("Weight", constraint.Weight, .01f, 0.f, 1.f);
            std::visit(
                [&](auto& ik) {
                    ik.Name = constraint.Name;
                    if constexpr (std::is_same_v<std::decay_t<decltype(ik)>, TwoBoneIK>)
                    {
                        changed |= JointCombo("Root", ik.Root, skeleton);
                        changed |= JointCombo("Middle", ik.Middle, skeleton);
                        changed |= JointCombo("End", ik.End, skeleton);
                    }
                    else
                        for (size_t b = 0; b < ik.Bones.size(); ++b)
                        {
                            ImGui::PushID(int(b));
                            changed |= JointCombo(b ? "" : "Bones", ik.Bones[b].Joint, skeleton);
                            ImGui::PopID();
                        }
                    changed |= EntityField("Target", ik.Target.Entity, scene);
                    if (!ik.Target.Entity)
                        changed |= PropertyGrid::Vec3("Target point", ik.Target.Position, 0.f, .01f);
                    if constexpr (std::is_same_v<std::decay_t<decltype(ik)>, TwoBoneIK>)
                    {
                        changed |= EntityField("Pole", ik.PoleEntity, scene);
                        if (!ik.PoleEntity)
                            changed |= PropertyGrid::Vec3(
                                "Pole direction", ik.PoleDirection, TwoBoneIK {}.PoleDirection, .01f);
                        changed |= PropertyGrid::Float("Rotation weight", ik.RotationWeight, .01f, 0.f, 1.f);
                        changed |= PropertyGrid::Checkbox("Stretch", ik.Stretch);
                    }
                    else
                    {
                        float degrees = glm::degrees(ik.AngleLimit);
                        if (PropertyGrid::Float("Angle limit", degrees, .5f, 0.f, 180.f, "%.0f deg"))
                            ik.AngleLimit = glm::radians(degrees), changed = true;
                        PropertyGrid::BeginFree();
                        if (ImGui::SmallButton(ICON_FA_PLUS " Bone"))
                            ik.Bones.push_back({}), changed = true;
                        if (ik.Bones.size() > 1)
                        {
                            ImGui::SameLine();
                            if (ImGui::SmallButton(ICON_FA_MINUS " Bone"))
                                ik.Bones.pop_back(), changed = true;
                        }
                        PropertyGrid::EndFree();
                    }
                },
                constraint.Data);
            return changed;
        }

        // Shape keys are grouped by the part of their name before the first '_' or '.'.
        std::string KeyGroup(const std::string& name)
        {
            const auto cut = name.find_first_of("_.");
            return cut == std::string::npos || cut == 0 || cut + 1 == name.size() ? std::string() : name.substr(0, cut);
        }

        bool Matches(const std::string& name, const std::string& filter)
        {
            return filter.empty() ||
                   std::search(name.begin(), name.end(), filter.begin(), filter.end(), [](char a, char b) {
                       return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
                   }) != name.end();
        }
    } // namespace

    void DrawIK(Entity entity, ComponentUI& ui)
    {
        Scene*                                scene    = ui.GetScene();
        const SkeletonData*                   skeleton = ModelSkeleton(entity);
        IKComponent                           edited   = entity.GetComponent<IKComponent>();
        bool                                  changed  = false;
        std::optional<std::pair<size_t, int>> move; // Constraint and direction (0 removes it).
        if (edited.Constraints.empty())
            ImGui::TextDisabled("Constraints run after the animation, in this order.");
        for (size_t i = 0; i < edited.Constraints.size(); ++i)
        {
            auto&       constraint = edited.Constraints[i];
            const bool  twoBone    = std::holds_alternative<TwoBoneIK>(constraint.Data);
            std::string title      = std::string(twoBone ? ICON_FA_LINK : ICON_FA_EYE) + "  " + constraint.Name;
            ImGui::PushID(int(i));
            bool toggled = false;
            if (PropertyGrid::BeginGroup(title.c_str(), &constraint.Enabled, &toggled))
            {
                changed |= DrawConstraint(constraint, skeleton, scene);
                PropertyGrid::BeginFree();
                if (ImGui::SmallButton(ICON_FA_ARROW_UP) && i > 0)
                    move = {i, -1};
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_ARROW_DOWN) && i + 1 < edited.Constraints.size())
                    move = {i, 1};
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH " Remove"))
                    move = {i, 0};
                PropertyGrid::EndFree();
                PropertyGrid::EndGroup();
            }
            changed |= toggled;
            ImGui::PopID();
        }
        if (move)
        {
            auto [i, direction] = *move;
            if (direction == 0)
                edited.Constraints.erase(edited.Constraints.begin() + long(i));
            else
                std::swap(edited.Constraints[i], edited.Constraints[i + direction]);
            changed = true;
        }
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(ICON_FA_PLUS "  Two-bone IK", {half, 0}))
            edited.Constraints.push_back({"Limb" + std::to_string(edited.Constraints.size()), true, 1, TwoBoneIK {}}),
                changed = true;
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS "  Look-at", {-FLT_MIN, 0}))
            edited.Constraints.push_back(
                {"LookAt" + std::to_string(edited.Constraints.size()), true, 1, LookAtIK {"", {{}}}}),
                changed = true;
        if (changed)
        {
            entity.SetComponent<IKComponent>(edited);
            if (scene && entity.HasComponent<AnimatorComponent>())
                scene->ResetAnimator(entity);
        }
    }

    void DrawShapeKeys(Entity entity, ComponentUI& ui)
    {
        Scene*      scene = ui.GetScene();
        const auto  pose  = scene ? scene->GetSkinningPose(entity) : nullptr;
        const auto& c     = entity.GetComponent<ShapeKeysComponent>();
        // Applies new saved weights; a model with an Animator shows them at once.
        const auto save = [&](const std::function<void(ShapeKeysComponent&)>& edit) {
            entity.PatchComponent<ShapeKeysComponent>(edit);
            if (!entity.HasComponent<AnimatorComponent>())
                return;
            auto& a = entity.GetComponent<AnimatorComponent>();
            if (scene && a.Player)
            {
                a.Player->BaseShapeKeyWeights = entity.GetComponent<ShapeKeysComponent>().Weights;
                a.Player->RebuildPose();
                scene->ApplyAnimatorPose(entity, a.Player->GetPose());
            }
        };

        if (PropertyGrid::BeginGroup("Expressions", nullptr, nullptr, c.Expression.IsValid()))
        {
            AssetID expression = c.Expression;
            if (ui.AssetPicker("Asset", AssetType::Expression, expression))
                save([&](ShapeKeysComponent& keys) { keys.Expression = expression, keys.ExpressionSource.reset(); });
            if (c.ExpressionSource && !c.ExpressionSource->GetData().Presets.empty())
            {
                PropertyGrid::BeginFree();
                // Presets as buttons that flow onto as many lines as they need.
                const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                bool        first = true;
                for (const auto& preset : c.ExpressionSource->GetData().Presets)
                {
                    const float width =
                        ImGui::CalcTextSize(preset.Name.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
                    if (!first && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < right)
                        ImGui::SameLine();
                    first = false;
                    if (ImGui::Button(preset.Name.c_str()))
                        save([&](ShapeKeysComponent& keys) { keys.ApplyPreset(preset.Name); });
                }
                PropertyGrid::EndFree();
            }
            else if (c.Expression.IsValid())
                PropertyGrid::Text("Presets", c.ExpressionSource ? "None in this asset" : "Loading...", true);
            PropertyGrid::EndGroup();
        }

        if (!pose || pose->ShapeKeys.empty())
        {
            ImGui::TextDisabled(pose ? "This model has no shape keys." :
                                       "The model's shape keys appear once it loads.");
            return;
        }
        // Keys the running animation drives this frame show their live value next to the saved one.
        const AnimationPose* live = nullptr;
        if (entity.HasComponent<AnimatorComponent>() && entity.GetComponent<AnimatorComponent>().Player)
            live = &entity.GetComponent<AnimatorComponent>().Player->GetPose();
        static std::map<UUID, std::string> filters;
        auto&                              filter = filters[entity.GetUUID()];
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Filter shape keys", &filter);

        std::map<std::string, std::vector<size_t>> groups;
        for (size_t k = 0; k < pose->ShapeKeys.size(); ++k)
            if (Matches(pose->ShapeKeys[k].Name, filter))
                groups[KeyGroup(pose->ShapeKeys[k].Name)].push_back(k);
        const auto drawKey = [&](size_t k) {
            const auto& key   = pose->ShapeKeys[k];
            const auto  found = c.Weights.find(key.Name);
            float       saved = found != c.Weights.end() ? found->second : key.DefaultWeight;
            const bool  driven =
                live && k < live->MorphInfluence.size() && live->MorphInfluence[k] > 0 && k < live->MorphWeights.size();
            const bool modified = found != c.Weights.end() && found->second != key.DefaultWeight;
            if (!PropertyGrid::Row(key.Name.c_str(), driven ? "Driven by the animation now" : nullptr, modified))
                return;
            ImGui::PushID(int(k));
            char format[48];
            if (driven)
                std::snprintf(format, sizeof(format), "%%.2f  " ICON_FA_PERSON_RUNNING " %.2f", live->MorphWeights[k]);
            else
                std::snprintf(format, sizeof(format), "%%.2f");
            if (ImGui::SliderFloat("##weight", &saved, 0.0f, 1.0f, format))
                save([&](ShapeKeysComponent& keys) { keys.Weights[key.Name] = saved; });
            if (PropertyGrid::RowReset(modified))
                save([&](ShapeKeysComponent& keys) { keys.Weights.erase(key.Name); });
            ImGui::PopID();
        };
        if (groups.size() == 1)
        {
            PropertyGrid::Begin("##keys");
            for (size_t k : groups.begin()->second)
                drawKey(k);
            PropertyGrid::End();
        }
        else
            for (const auto& [name, keys] : groups)
            {
                const std::string title =
                    (name.empty() ? std::string("Other") : name) + "  (" + std::to_string(keys.size()) + ")";
                if (PropertyGrid::BeginGroup(title.c_str(), nullptr, nullptr, !filter.empty() || groups.size() <= 3))
                {
                    for (size_t k : keys)
                        drawKey(k);
                    PropertyGrid::EndGroup();
                }
            }
        if (groups.empty())
            ImGui::TextDisabled("No shape key matches the filter.");
    }
} // namespace Kans::ComponentEditors
