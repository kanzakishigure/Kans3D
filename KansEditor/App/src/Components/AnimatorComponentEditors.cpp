#include <imgui.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Animation/ContactTracks.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/ImGui/EditorTheme.h"
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
            int         ContactWindow = -1, ContactEdge = 0;
            std::string ContactError;
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

    namespace
    {
        // Same half-open sections, frame snapping, edge handles and context actions as Timeline rows.
        bool ContactRows(ClipRetargetSettings& settings, const AnimationClipData& clip, double playhead, Preview& p)
        {
            bool         changed = false;
            const auto&  colors  = EditorTheme::Colors();
            const double fps = std::max(clip.FrameRate, 1.0), duration = std::max(clip.Duration, 1e-3);
            const float  canvasWidth = std::max(ImGui::GetContentRegionAvail().x, float(duration * 40));
            ImGui::BeginChild(
                "Contact tracks",
                {0, std::min(220.f, ImGui::GetFrameHeightWithSpacing() * (settings.Tracks.size() * 2 + 1))},
                ImGuiChildFlags_Borders,
                ImGuiWindowFlags_HorizontalScrollbar);
            auto*      draw  = ImGui::GetWindowDrawList();
            const auto ruler = ImGui::GetCursorScreenPos();
            for (double second = 0; second <= duration; second += 1)
            {
                const float x = ruler.x + float(second / duration) * canvasWidth;
                char        label[32];
                std::snprintf(label, sizeof(label), "%.0fs", second);
                draw->AddText({x + 3, ruler.y}, ImGui::ColorConvertFloat4ToU32(colors.Accent), label);
                draw->AddLine({x, ruler.y},
                              {x, ruler.y + ImGui::GetTextLineHeight()},
                              ImGui::ColorConvertFloat4ToU32(colors.Accent));
            }
            ImGui::Dummy({canvasWidth, ImGui::GetTextLineHeight()});
            for (size_t index = 0; index < settings.Tracks.size(); ++index)
            {
                auto& track = settings.Tracks[index];
                ImGui::PushID(int(index));
                ImGui::TextUnformatted(track.Foot.c_str());
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const float  width = canvasWidth, height = ImGui::GetFrameHeight();
                auto         x    = [&](double time) { return origin.x + float(time / duration) * width; };
                auto         time = [&] {
                    return std::clamp(std::round((ImGui::GetIO().MousePos.x - origin.x) / width * duration * fps) / fps,
                                      0.0,
                                      duration);
                };
                ImGui::InvisibleButton("##contacts", {width, height});
                draw->AddRectFilled(
                    origin, {origin.x + width, origin.y + height}, ImGui::ColorConvertFloat4ToU32(colors.Raised));
                for (size_t w = 0; w < track.Windows.size(); ++w)
                {
                    const auto& window = track.Windows[w];
                    draw->AddRectFilled({x(window.Start), origin.y + 3},
                                        {x(window.End), origin.y + height - 3},
                                        ImGui::ColorConvertFloat4ToU32(colors.TypeModel),
                                        2);
                    for (double edge : {window.Start, window.End})
                        draw->AddLine({x(edge), origin.y + 3},
                                      {x(edge), origin.y + height - 3},
                                      ImGui::ColorConvertFloat4ToU32(colors.Accent),
                                      2);
                }
                draw->AddLine({x(playhead), origin.y},
                              {x(playhead), origin.y + height},
                              ImGui::ColorConvertFloat4ToU32(colors.Accent));
                if (ImGui::IsItemActivated())
                {
                    p.ContactWindow = -1;
                    float closest   = 8;
                    for (size_t w = 0; w < track.Windows.size(); ++w)
                        for (int edge = 0; edge < 2; ++edge)
                        {
                            const float distance = std::abs(ImGui::GetIO().MousePos.x -
                                                            x(edge ? track.Windows[w].End : track.Windows[w].Start));
                            if (distance < closest)
                                closest = distance, p.ContactWindow = int(w), p.ContactEdge = edge;
                        }
                }
                if (ImGui::IsItemActive() && p.ContactWindow >= 0 && ImGui::IsMouseDragging(0))
                {
                    auto&        w = track.Windows[size_t(p.ContactWindow)];
                    const double t = time();
                    if (p.ContactEdge)
                        w.End = std::clamp(t,
                                           w.Start + 1 / fps,
                                           size_t(p.ContactWindow + 1) < track.Windows.size() ?
                                               track.Windows[size_t(p.ContactWindow + 1)].Start :
                                               duration);
                    else
                        w.Start = std::clamp(t,
                                             p.ContactWindow > 0 ? track.Windows[size_t(p.ContactWindow - 1)].End : 0.0,
                                             w.End - 1 / fps);
                    changed = true;
                    ImGui::SetTooltip("%.3f - %.3f s", w.Start, w.End);
                }
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::BeginPopupContextItem("Contact actions"))
                {
                    const double t = std::clamp(
                        std::round((ImGui::GetMousePosOnOpeningCurrentPopup().x - origin.x) / width * duration * fps) /
                            fps,
                        0.0,
                        duration);
                    auto found = std::find_if(track.Windows.begin(), track.Windows.end(), [&](const auto& w) {
                        return t >= w.Start && t < w.End;
                    });
                    if (found != track.Windows.end())
                    {
                        if (ImGui::MenuItem("Delete contact"))
                            track.Windows.erase(found), changed = true;
                        else if (ImGui::MenuItem("Toe pivot", nullptr, found->ToePivot))
                            found->ToePivot = !found->ToePivot, changed = true;
                    }
                    else if (ImGui::MenuItem("Add contact"))
                    {
                        auto next = std::lower_bound(track.Windows.begin(),
                                                     track.Windows.end(),
                                                     t,
                                                     [](const auto& w, double value) { return w.Start < value; });
                        const double end = std::min(t + .2, next == track.Windows.end() ? duration : next->Start);
                        if (end - t >= 1 / fps)
                            track.Windows.insert(next, {t, end, false}), changed = true;
                    }
                    ImGui::EndPopup();
                }
                if (hovered && !ImGui::IsPopupOpen("Contact actions"))
                    ImGui::SetTooltip("Drag an edge. Right-click to add/delete a contact. Times are clip seconds.");
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (changed)
                settings.Edited = true;
            return changed;
        }

        bool RetargetEditor(Entity                    entity,
                            const SkeletonData&       skeleton,
                            const BoundAnimationClip& bound,
                            double                    time,
                            Preview&                  preview)
        {
            auto& animator = entity.GetComponent<AnimatorComponent>();
            if (!bound.Clip || !PropertyGrid::BeginGroup("Retarget contacts", nullptr, nullptr, false))
                return false;
            bool changed = false;
            auto found   = std::find_if(animator.Retargets.begin(), animator.Retargets.end(), [&](const auto& c) {
                return c.Clip == bound.Clip->Name;
            });
            PropertyGrid::BeginFree();
            if (found == animator.Retargets.end() && ImGui::Button("Set up contacts"))
            {
                ClipRetargetSettings settings;
                settings.Clip = bound.Clip->Name;
                if (entity.HasComponent<IKComponent>())
                    for (const auto& constraint : entity.GetComponent<IKComponent>().Constraints)
                        if (auto feet = std::get_if<FootPlacementIK>(&constraint.Data))
                        {
                            settings.RootJoint = feet->Pelvis;
                            for (const auto& leg : feet->Legs)
                                if (!leg.Ankle.empty() &&
                                    std::none_of(settings.Tracks.begin(),
                                                 settings.Tracks.end(),
                                                 [&](const auto& track) { return track.Foot == leg.Ankle; }))
                                    settings.Tracks.push_back({leg.Ankle, "", leg.Toe, {}});
                            break;
                        }
                animator.Retargets.push_back(settings);
                found   = std::prev(animator.Retargets.end());
                changed = true;
            }
            PropertyGrid::EndFree();
            if (found != animator.Retargets.end())
            {
                auto& settings = *found;
                changed |= JointCombo("Pelvis/root joint", settings.RootJoint, &skeleton);
                changed |= PropertyGrid::Vec3("Root translation scale", settings.RootTranslationScale, {1, 1, 1}, .01f);
                for (int axis = 0; axis < 3; ++axis)
                    settings.RootTranslationScale[axis] = std::clamp(settings.RootTranslationScale[axis], 0.f, 4.f);
                changed |= PropertyGrid::Float("Height band", settings.Analysis.HeightBand, .001f, 0.f, 1.f);
                changed |= PropertyGrid::Float("Vertical speed", settings.Analysis.VerticalSpeed, .01f, 0.f, 5.f);
                for (auto entry : {std::make_pair("Minimum duration", &settings.Analysis.MinimumDuration),
                                   std::make_pair("Merge gap", &settings.Analysis.MergeGap)})
                {
                    float v = float(*entry.second);
                    if (PropertyGrid::Float(entry.first, v, .01f, 0.f, 5.f))
                        *entry.second = v, changed = true;
                }
                for (size_t index = 0; index < settings.Tracks.size(); ++index)
                {
                    ImGui::PushID(int(index));
                    auto& track = settings.Tracks[index];
                    changed |= JointCombo("Foot", track.Foot, &skeleton);
                    changed |= JointCombo("Heel (optional)", track.Heel, &skeleton);
                    changed |= JointCombo("Toe (optional)", track.Toe, &skeleton);
                    ImGui::PopID();
                }
                PropertyGrid::BeginFree();
                if (settings.Tracks.size() < 16 && ImGui::Button("Add foot track"))
                    for (const auto& joint : skeleton.Joints)
                        if (std::none_of(settings.Tracks.begin(), settings.Tracks.end(), [&](const auto& track) {
                                return track.Foot == joint.Name;
                            }))
                        {
                            settings.Tracks.push_back({joint.Name, "", "", {}});
                            changed = true;
                            break;
                        }
                auto analyze = [&] {
                    try
                    {
                        AnalyzeFootContacts(skeleton, bound, settings);
                        preview.ContactError.clear();
                        changed = true;
                    }
                    catch (const std::exception& e)
                    {
                        preview.ContactError = e.what();
                    }
                };
                if (ImGui::Button("Analyze"))
                {
                    if (settings.Edited)
                        ImGui::OpenPopup("Overwrite contact edits?");
                    else
                        analyze();
                }
                if (ImGui::BeginPopupModal("Overwrite contact edits?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextUnformatted("Analyze replaces all edited contact windows for this clip.");
                    if (ImGui::Button("Overwrite"))
                        analyze(), ImGui::CloseCurrentPopup();
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Estimate from contacts"))
                {
                    try
                    {
                        settings.RootTranslationScale = EstimateRootTranslationScale(skeleton, bound, settings);
                        preview.ContactError.clear();
                        changed = true;
                    }
                    catch (const std::exception& e)
                    {
                        preview.ContactError = e.what();
                    }
                }
                changed |= ContactRows(settings, *bound.Clip, time, preview);
                if (!preview.ContactError.empty())
                    ImGui::TextUnformatted(preview.ContactError.c_str());
                PropertyGrid::EndFree();
            }
            PropertyGrid::EndGroup();
            return changed;
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
            if (a.Player && skeleton && p.State < int(layer.States.size()))
            {
                const auto  motion   = layer.States[p.State].Motion;
                const auto& bindings = a.Player->GetBindings();
                if (motion < bindings.size() && RetargetEditor(entity, *skeleton, bindings[motion], p.Time, p))
                {
                    scene->ResetAnimator(entity);
                    // Recreate the preview immediately so status rows and contact handles keep their screen positions.
                    scene->PreviewAnimation(entity, size_t(p.Layer), uint32_t(p.State), p.Time);
                }
            }
        }
    }
} // namespace Kans::ComponentEditors
