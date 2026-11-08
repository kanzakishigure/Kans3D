#include <imgui.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    namespace
    {
        // Editor-only preview of an animator: which state is shown and where in it.
        struct Preview
        {
            int     Layer = 0, State = 0;
            double  Time    = 0;
            bool    Playing = false;
            AssetID Action; // Tried over the previewed state; the card advances it while it plays.
            bool    TryingAction = false;
        };
        std::map<UUID, Preview>& Previews()
        {
            static std::map<UUID, Preview> previews;
            return previews;
        }

    } // namespace

    bool JointCombo(const char* label, std::string& joint, const SkeletonData* skeleton)
    {
        if (!PropertyGrid::Row(label))
            return false;
        ImGui::PushID(label);
        bool changed = false;
        if (ImGui::BeginCombo("##joint", joint.empty() ? "(none)" : joint.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("(none)", joint.empty()))
                joint.clear(), changed = true;
            if (skeleton)
                for (const auto& j : skeleton->Joints)
                    if (ImGui::Selectable(j.Name.c_str(), j.Name == joint))
                        joint = j.Name, changed = true;
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    namespace
    {

        double MotionDuration(const AnimationControllerData& c, uint32_t motion, ComponentUI& ui)
        {
            if (motion >= c.Motions.size() || !ui.GetAssets())
                return 0;
            const auto& m = c.Motions[motion];
            if (m.InlineClip)
                return m.InlineClip->Duration;
            if (m.Type == AnimationMotionType::Clip)
            {
                auto status = ui.GetAssets()->Request<AnimationClipAsset>(m.Clip);
                return status.IsReady() ? status.Asset->GetData().Duration : 0;
            }
            double longest = 0;
            for (const auto& child : m.Children)
                longest = std::max(longest, MotionDuration(c, child.Motion, ui));
            return longest;
        }

        // Sampling rate of a motion, for frame-accurate preview; blend trees use their fastest clip.
        double MotionFrameRate(const AnimationControllerData& c, uint32_t motion, ComponentUI& ui)
        {
            if (motion >= c.Motions.size() || !ui.GetAssets())
                return 30;
            const auto& m = c.Motions[motion];
            if (m.InlineClip)
                return m.InlineClip->FrameRate;
            if (m.Type == AnimationMotionType::Clip)
            {
                auto status = ui.GetAssets()->Request<AnimationClipAsset>(m.Clip);
                return status.IsReady() ? status.Asset->GetData().FrameRate : 30;
            }
            double fastest = 0;
            for (const auto& child : m.Children)
                fastest = std::max(fastest, MotionFrameRate(c, child.Motion, ui));
            return fastest > 0 ? fastest : 30;
        }

    } // namespace

    void DrawAnimator(Entity entity, ComponentUI& ui)
    {
        Scene*              scene    = ui.GetScene();
        auto&               a        = entity.GetComponent<AnimatorComponent>();
        const SkeletonData* skeleton = a.SkeletonSource ? &a.SkeletonSource->GetData() : nullptr;
        if (scene && !skeleton)
            scene->PrepareAnimator(entity); // Loads the skeleton and controller shown below.
        const AnimationControllerData* controller = a.InlineController ? &*a.InlineController :
                                                    a.ControllerSource ? &a.ControllerSource->GetData() :
                                                                         nullptr;

        bool changed = false;
        if (PropertyGrid::BeginGroup("Animator"))
        {
            if (a.InlineController)
                PropertyGrid::Text("Controller", "Inline (saved with the scene)");
            else
            {
                AssetID asset = a.Controller;
                if (ui.AssetPicker("Controller", AssetType::AnimationController, asset))
                    a.Controller = asset, changed = true;
            }
            PropertyGrid::Text("Skeleton",
                               skeleton             ? std::to_string(skeleton->Joints.size()) + " joints" :
                               a.Skeleton.IsValid() ? std::string("Loading...") :
                                                      std::string("(none)"),
                               !skeleton);
            changed |= PropertyGrid::Checkbox("Enabled", a.Enabled);
            float speed = float(a.Speed);
            if (PropertyGrid::Float("Speed", speed, 0.01f, 0.0f, 10.0f))
                a.Speed = speed, changed = true;
            PropertyGrid::BeginFree();
            if (!controller)
            {
                ImGui::TextDisabled("No controller: the model stays in its rest pose.");
                if (ImGui::Button(ICON_FA_PLUS "  Create controller", {-FLT_MIN, 0}))
                {
                    AnimationControllerData created;
                    created.Layers.push_back({"Base"});
                    a.InlineController = std::move(created);
                    changed            = true;
                }
            }
            // States, transitions, parameters and layers are edited in the graph window.
            if (ImGui::Button(ICON_FA_DIAGRAM_PROJECT "  Open graph", {-FLT_MIN, 0}))
                ui.OpenAnimatorGraph();
            // A ragdoll generated on the skeleton (the ragdoll editor's dialog), assigned once imported.
            ImGui::BeginDisabled(!a.Skeleton.IsValid() && !a.SkeletonSource);
            if (ImGui::Button(ICON_FA_PERSON_FALLING "  Create ragdoll", {-FLT_MIN, 0}))
                ui.CreateRagdoll(entity);
            ImGui::EndDisabled();
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        if (a.LastError)
            ImGui::TextColored(ErrorColor, "%s", a.LastError->Message.c_str());

        // Folded unless used; the toggle on the heading applies root motion.
        bool toggled = false;
        if (PropertyGrid::BeginGroup("Root motion", &a.ApplyRootMotion, &toggled, a.ApplyRootMotion))
        {
            if (a.InlineController)
            {
                auto& c = *a.InlineController;
                changed |= JointCombo("Root joint", c.RootMotionJoint, skeleton);
                // Hips-rooted rigs (Mixamo): only ground travel and turning move the entity.
                changed |= PropertyGrid::Checkbox("Planar (XZ + yaw)", c.RootMotionPlanar);
                changed |= PropertyGrid::Checkbox("Keep initial offset", c.RootMotionKeepInitialOffset);
            }
            else
                PropertyGrid::Text("Settings", "From the controller asset", true);
            PropertyGrid::EndGroup();
        }
        changed |= toggled;
        // Edits change the controller the animator was prepared with.
        if (changed && scene)
            scene->ResetAnimator(entity);
        controller = a.InlineController ? &*a.InlineController :
                     a.ControllerSource ? &a.ControllerSource->GetData() :
                                          nullptr;
        if (!controller || controller->Layers.empty())
            return;

        // What the animator is doing now (play mode or preview), with live parameter values.
        if (a.Player && a.Player->IsPrepared() && PropertyGrid::BeginGroup("Status"))
        {
            const auto& layers = a.Player->GetLayers();
            for (size_t i = 0; i < layers.size() && i < controller->Layers.size(); ++i)
            {
                const auto& layer  = controller->Layers[i];
                const auto& status = layers[i];
                if (status.State >= layer.States.size() || !PropertyGrid::Row(layer.Name.c_str()))
                    continue;
                ImGui::PushID(int(i));
                const auto&  state  = layer.States[status.State];
                const double length = std::max(MotionDuration(*controller, state.Motion, ui), 1e-3);
                const float  played = float(std::fmod(std::max(status.Time, 0.0), length) / length);
                char         overlay[160];
                if (status.Transitioning && status.Target < layer.States.size())
                    std::snprintf(overlay,
                                  sizeof(overlay),
                                  "%s " ICON_FA_ARROW_RIGHT " %s  %.0f%%",
                                  state.Name.c_str(),
                                  layer.States[status.Target].Name.c_str(),
                                  100.0f * status.CrossfadeWeight());
                else
                    std::snprintf(overlay, sizeof(overlay), "%s  %.2f s", state.Name.c_str(), status.Time);
                ImGui::ProgressBar(played, {-FLT_MIN, ImGui::GetFrameHeight()}, overlay);
                ImGui::PopID();
            }
            // One-shot actions playing in the slots, over the layers above.
            for (const auto& action : a.Player->GetActions())
            {
                if (!action.Playing || !PropertyGrid::Row(("Slot " + action.Slot).c_str()))
                    continue;
                ImGui::PushID(action.Slot.c_str());
                char overlay[160];
                std::snprintf(overlay,
                              sizeof(overlay),
                              "%s%s  %.2f s%s",
                              ICON_FA_BOLT " ",
                              action.Section.empty() ? "Action" : action.Section.c_str(),
                              action.Time,
                              action.BlendingOut ? "  (blending out)" : "");
                ImGui::ProgressBar(action.Weight, {-FLT_MIN, ImGui::GetFrameHeight()}, overlay);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Bar: the action's weight over the layer");
                ImGui::PopID();
            }
            // Live values of the running animator; their defaults are edited in the graph window.
            for (const auto& p : controller->Parameters)
            {
                ImGui::PushID(p.Name.c_str());
                const float current = float(a.Player->GetParameter(p.Name).value_or(p.Default));
                switch (p.Type)
                {
                    case AnimationParameterType::Float: {
                        float value = current;
                        if (PropertyGrid::Float(p.Name.c_str(), value, 0.01f))
                            a.Player->SetFloat(p.Name, value);
                        break;
                    }
                    case AnimationParameterType::Int: {
                        int value = int(std::lround(current));
                        if (PropertyGrid::Int(p.Name.c_str(), value, INT_MIN, INT_MAX))
                            a.Player->SetInt(p.Name, value);
                        break;
                    }
                    case AnimationParameterType::Bool: {
                        bool value = current != 0;
                        if (PropertyGrid::Checkbox(p.Name.c_str(), value))
                            a.Player->SetBool(p.Name, value);
                        break;
                    }
                    case AnimationParameterType::Trigger:
                        if (PropertyGrid::Row(p.Name.c_str()) && ImGui::Button(current != 0 ? "Set" : "Fire"))
                            a.Player->SetTrigger(p.Name);
                        break;
                }
                ImGui::PopID();
            }
            PropertyGrid::EndGroup();
        }

        // Preview: shows a state at a time without entering play mode.
        if (scene && PropertyGrid::BeginGroup("Preview"))
        {
            PropertyGrid::BeginFree();
            auto& p           = Previews()[entity.GetUUID()];
            const auto& c           = *controller;
            p.Layer           = std::clamp(p.Layer, 0, int(c.Layers.size()) - 1);
            const auto& layer = c.Layers[p.Layer];
            if (layer.States.empty())
                ImGui::TextDisabled("Add a state with a clip to preview it.");
            else
            {
                p.State = std::clamp(p.State, 0, int(layer.States.size()) - 1);
                ImGui::SetNextItemWidth(120);
                if (ImGui::BeginCombo("##previewstate", layer.States[p.State].Name.c_str()))
                {
                    for (int s = 0; s < int(layer.States.size()); ++s)
                        if (ImGui::Selectable(layer.States[s].Name.c_str(), s == p.State))
                            p.State = s, p.Time = 0;
                    ImGui::EndCombo();
                }
                const uint32_t motion   = layer.States[p.State].Motion;
                const double   duration = std::max(MotionDuration(c, motion, ui), 1e-3);
                const double   fps      = std::max(MotionFrameRate(c, motion, ui), 1.0);
                // Frames are counted from 0; the last frame sits exactly at the clip's end.
                const int  lastFrame = std::max(int(std::lround(duration * fps)), 1);
                int        frame     = std::clamp(int(std::floor(p.Time * fps + 1e-6)), 0, lastFrame);
                const auto seek      = [&](int to) {
                    frame     = std::clamp(to, 0, lastFrame);
                    p.Time    = frame / fps;
                    p.Playing = false;
                };
                const bool shift = ImGui::GetIO().KeyShift; // Shift steps ten frames.
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_BACKWARD_FAST))
                    seek(0);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("First frame");
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_BACKWARD_STEP))
                    seek(frame - (shift ? 10 : 1));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Previous frame (Shift: 10 frames)");
                ImGui::SameLine();
                if (ImGui::Button(p.Playing ? ICON_FA_PAUSE "##PlayPause" : ICON_FA_PLAY "##PlayPause"))
                    p.Playing = !p.Playing;
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FORWARD_STEP))
                    seek(frame + (shift ? 10 : 1));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Next frame (Shift: 10 frames)");
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FORWARD_FAST))
                    seek(lastFrame);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Last frame");
                ImGui::SameLine();
                int typed = frame;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##frame", &typed, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
                    seek(typed);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Frame (of %d at %.0f fps)", lastFrame, fps);

                if (p.Playing)
                    p.Time =
                        std::fmod(p.Time + ImGui::GetIO().DeltaTime * layer.States[p.State].Speed * a.Speed, duration);
                // Scrubbing snaps to whole frames; the label also shows seconds for long clips.
                int  scrub = std::clamp(int(std::floor(p.Time * fps + 1e-6)), 0, lastFrame);
                char label[48];
                std::snprintf(label, sizeof(label), "%d / %d  (%.2f s)", scrub, lastFrame, p.Time);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderInt("##time", &scrub, 0, lastFrame, label))
                    seek(scrub);
                // An action from the project, tried over the previewed state through its slot.
                const bool slots =
                    std::any_of(c.Layers.begin(), c.Layers.end(), [](const auto& l) { return !l.Slot.empty(); });
                if (slots)
                {
                    PropertyGrid::EndFree();
                    ui.AssetPicker("Try action", AssetType::Action, p.Action);
                    PropertyGrid::BeginFree();
                    if (p.TryingAction)
                    {
                        if (ImGui::Button(ICON_FA_STOP "  Stop action", {-FLT_MIN, 0}))
                            p.TryingAction = false, scene->ResetAnimator(entity);
                    }
                    else
                    {
                        ImGui::BeginDisabled(!p.Action.IsValid() || !a.Player);
                        if (ImGui::Button(ICON_FA_BOLT "  Play action", {-FLT_MIN, 0}) &&
                            a.Player->Play(size_t(p.Layer), uint32_t(p.State), p.Time))
                        {
                            p.TryingAction = a.Player->PlayAction(p.Action);
                            p.Playing      = false;
                        }
                        ImGui::EndDisabled();
                    }
                    if (a.Player && a.Player->GetActionError())
                        ImGui::TextColored(ErrorColor, "%s", a.Player->GetActionError()->Message.c_str());
                }
                if (p.TryingAction && a.Player)
                {
                    // Runs the animator itself while the action plays; root motion is not applied.
                    a.Player->Advance(ImGui::GetIO().DeltaTime);
                    scene->ApplyAnimatorPose(entity, a.Player->GetPose());
                    const auto& actions = a.Player->GetActions();
                    if (std::none_of(actions.begin(), actions.end(), [](const auto& s) { return s.Playing; }))
                        p.TryingAction = false, scene->ResetAnimator(entity);
                }
                else
                    scene->PreviewAnimation(entity, size_t(p.Layer), uint32_t(p.State), p.Time);
            }
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
    }
} // namespace Kans::ComponentEditors
