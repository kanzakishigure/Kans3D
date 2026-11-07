#include "AnimatorGraphPanel.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "Components/ComponentEditors.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    namespace
    {
        constexpr float NodeWidth = 150.0f, NodeHeight = 44.0f;
        const ImVec2    AnyStatePosition {-200.0f, 0.0f};

        // A name field; full width unless the caller set the next item's width.
        bool InputName(const char* id, std::string& value)
        {
            char buffer[128] = {};
            value.copy(buffer, sizeof(buffer) - 1);
            if (!(ImGui::GetCurrentContext()->NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth))
                ImGui::SetNextItemWidth(-FLT_MIN);
            if (!ImGui::InputText(id, buffer, sizeof(buffer)))
                return false;
            value = buffer;
            return true;
        }

        ImVec2 Add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
        ImVec2 Center(ImVec2 position) { return {position.x + NodeWidth * 0.5f, position.y + NodeHeight * 0.5f}; }

        // Where the segment from the centre towards `toward` leaves the node rectangle.
        ImVec2 Edge(ImVec2 center, ImVec2 toward)
        {
            const float dx = toward.x - center.x, dy = toward.y - center.y;
            const float sx = std::abs(dx) > 1e-3f ? (NodeWidth * 0.5f) / std::abs(dx) : 1e9f;
            const float sy = std::abs(dy) > 1e-3f ? (NodeHeight * 0.5f) / std::abs(dy) : 1e9f;
            const float s  = std::min({sx, sy, 1.0f});
            return {center.x + dx * s, center.y + dy * s};
        }
    } // namespace

    ImVec2 AnimatorGraphPanel::NodePosition(const AnimationLayer& layer, size_t state)
    {
        // States never moved in the editor sit on a grid; the scene stores a position once moved.
        if (const auto& saved = layer.States[state].EditorPosition)
            return {saved->x, saved->y};
        return {float(state % 4) * 210.0f, float(state / 4) * 110.0f};
    }

    void AnimatorGraphPanel::MoveNode(AnimationLayer& layer, size_t state, ImVec2 delta)
    {
        const ImVec2 p                     = Add(NodePosition(layer, state), delta);
        layer.States[state].EditorPosition = glm::vec2(p.x, p.y);
    }

    bool AnimatorGraphPanel::ClipCombo(const char* id, AssetID& clip)
    {
        std::string current = "(none)";
        for (const auto& choice : m_Choices)
            if (choice.ID == clip)
                current = choice.Name;
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("(none)", !clip.IsValid()))
                clip = {}, changed = true;
            for (const auto& choice : m_Choices)
                if (choice.Type == AssetType::AnimationClip && ImGui::Selectable(choice.Name.c_str(), choice.ID == clip))
                    clip = choice.ID, changed = true;
            ImGui::EndCombo();
        }
        return changed;
    }

    void AnimatorGraphPanel::Draw()
    {
        Scene* scene  = m_Hierarchy.GetScene();
        Entity picked = m_Hierarchy.GetSelected();
        Entity root   = scene && picked ? scene->GetPrefabInstanceRoot(picked) : Entity {};
        root          = root ? root : picked;
        if (!scene || !root || !root.HasComponent<AnimatorComponent>())
        {
            ImGui::TextDisabled("Select an animated model to see its animator.");
            return;
        }
        auto& animator = root.GetComponent<AnimatorComponent>();
        if (root.GetUUID() != m_Entity)
        {
            m_Entity = root.GetUUID();
            m_Layer = 0, m_SelectedState = m_SelectedTransition = -1, m_LinkFrom = -2;
            m_Pan   = {240, 60};
        }
        if (!animator.InlineController)
        {
            ImGui::TextDisabled("This animator has no controller.");
            if (ImGui::Button(ICON_FA_PLUS "  Create controller"))
            {
                AnimationControllerData controller;
                controller.Layers.push_back({"Base"});
                animator.InlineController = std::move(controller);
                scene->ResetAnimator(root);
            }
            return;
        }
        auto& controller = *animator.InlineController;
        bool  changed    = false;
        if (ImGui::BeginTabBar("##layers"))
        {
            for (size_t i = 0; i < controller.Layers.size(); ++i)
                if (ImGui::BeginTabItem(((controller.Layers[i].Slot.empty() ? "" : ICON_FA_BOLT " ") +
                                         controller.Layers[i].Name + "##" + std::to_string(i))
                                            .c_str()))
                {
                    if (m_Layer != i)
                        m_SelectedState = m_SelectedTransition = -1, m_LinkFrom = -2;
                    m_Layer = i;
                    ImGui::EndTabItem();
                }
            if (ImGui::TabItemButton(ICON_FA_PLUS "##addlayer", ImGuiTabItemFlags_Trailing))
            {
                controller.Layers.push_back({"Layer" + std::to_string(controller.Layers.size())});
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Add a layer");
            if (ImGui::BeginTabItem(ICON_FA_FACE_SMILE " Expressions"))
            {
                m_Layer = controller.Layers.size();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (m_Layer >= controller.Layers.size())
            changed |= DrawExpressions(controller);
        else
        {
            const float side = 320.0f;
            if (ImGui::BeginChild("##graph",
                                  {ImGui::GetContentRegionAvail().x - side, 0},
                                  ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                changed |= DrawGraph(controller, m_Layer);
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("##selection", {0, 0}, ImGuiChildFlags_Borders))
                changed |= DrawSelection(controller, m_Layer);
            ImGui::EndChild();
        }
        if (changed)
            scene->ResetAnimator(root);
    }

    bool AnimatorGraphPanel::DrawGraph(AnimationControllerData& controller, size_t layerIndex)
    {
        auto&          layer  = controller.Layers[layerIndex];
        const ImVec2   origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
        const ImGuiIO& io     = ImGui::GetIO();
        ImGui::InvisibleButton("##canvas", {std::max(size.x, 1.0f), std::max(size.y, 1.0f)},
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            m_Pan = Add(m_Pan, io.MouseDelta);
        const auto toScreen = [&](ImVec2 p) { return ImVec2(origin.x + m_Pan.x + p.x, origin.y + m_Pan.y + p.y); };
        ImDrawList* draw    = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, Add(origin, size), true);
        draw->AddRectFilled(origin, Add(origin, size), IM_COL32(24, 25, 28, 255));
        for (float x = std::fmod(m_Pan.x, 32.0f); x < size.x; x += 32.0f)
            draw->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + size.y}, IM_COL32(38, 40, 44, 255));
        for (float y = std::fmod(m_Pan.y, 32.0f); y < size.y; y += 32.0f)
            draw->AddLine({origin.x, origin.y + y}, {origin.x + size.x, origin.y + y}, IM_COL32(38, 40, 44, 255));

        bool changed = false;
        // The live state, when the animator is running or previewing.
        Scene*  scene    = m_Hierarchy.GetScene();
        Entity  root     = scene->GetEntityByUUID(m_Entity);
        int32_t playing  = -1;
        if (root && root.GetComponent<AnimatorComponent>().Player)
        {
            const auto& status = root.GetComponent<AnimatorComponent>().Player->GetLayers();
            if (layerIndex < status.size())
                playing = int32_t(status[layerIndex].State);
        }
        const auto position = [&](int32_t state) {
            return state < 0 ? AnyStatePosition : NodePosition(layer, size_t(state));
        };

        // Transitions: arrows from source to target (offset sideways when both directions exist).
        for (size_t i = 0; i < layer.Transitions.size(); ++i)
        {
            const auto& t = layer.Transitions[i];
            if (t.Target >= layer.States.size())
                continue;
            ImVec2      a = toScreen(Center(position(t.Source))), b = toScreen(Center(position(int32_t(t.Target))));
            const float dx = b.x - a.x, dy = b.y - a.y, length = std::max(std::sqrt(dx * dx + dy * dy), 1.0f);
            const ImVec2 normal {-dy / length * 6.0f, dx / length * 6.0f};
            a                   = Add(a, normal);
            b                   = Add(b, normal);
            const ImVec2 from   = Edge(a, b), to = Edge(b, a);
            const bool   chosen = int(i) == m_SelectedTransition;
            const ImU32  color  = chosen ? IM_COL32(255, 200, 80, 255) : IM_COL32(170, 180, 200, 200);
            draw->AddLine(from, to, color, chosen ? 3.0f : 1.8f);
            const ImVec2 mid {(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f};
            const ImVec2 dir {dx / length, dy / length};
            draw->AddTriangleFilled(Add(mid, {dir.x * 7, dir.y * 7}), Add(mid, {-dir.x * 5 + dir.y * 5, -dir.y * 5 - dir.x * 5}),
                                    Add(mid, {-dir.x * 5 - dir.y * 5, -dir.y * 5 + dir.x * 5}), color);
            // Clicking near the arrow head selects the transition.
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                const float hx = io.MousePos.x - mid.x, hy = io.MousePos.y - mid.y;
                if (hx * hx + hy * hy < 100.0f)
                    m_SelectedTransition = int(i), m_SelectedState = -1;
            }
        }

        // Nodes: Any State, then the layer's states.
        ImFont* font = ImGui::GetFont();
        for (int32_t s = -1; s < int32_t(layer.States.size()); ++s)
        {
            const ImVec2 min      = toScreen(position(s));
            const ImVec2 max      = Add(min, {NodeWidth, NodeHeight});
            const bool   over     = hovered && ImGui::IsMouseHoveringRect(min, max);
            const bool   entry    = s >= 0 && uint32_t(s) == layer.EntryState;
            const bool   selected = s == m_SelectedState && m_SelectedTransition < 0;
            const ImU32  fill     = s < 0             ? IM_COL32(60, 90, 80, 240) :
                                    s == playing      ? IM_COL32(150, 95, 30, 245) :
                                    entry             ? IM_COL32(50, 80, 60, 240) :
                                                        IM_COL32(48, 56, 72, 240);
            draw->AddRectFilled(min, max, fill, 6.0f);
            draw->AddRect(min, max, selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(110, 120, 140, 255), 6.0f, 0,
                          selected ? 2.5f : 1.0f);
            const std::string name = s < 0 ? "Any State" : layer.States[s].Name;
            draw->AddText(font, ImGui::GetFontSize(), {min.x + 8, min.y + 6}, IM_COL32(240, 240, 240, 255), name.c_str());
            if (s >= 0)
                draw->AddText(font, ImGui::GetFontSize() * 0.85f, {min.x + 8, min.y + 24}, IM_COL32(170, 175, 185, 255),
                              entry ? "entry" : s == playing ? "playing" : "");
            if (!over)
                continue;
            // Left drag moves a state; Shift-drag (or right-click "Make transition") links.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                if (io.KeyShift)
                    m_LinkFrom = s;
                else
                    m_SelectedState = s, m_SelectedTransition = -1;
            }
            if (s >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !io.KeyShift && m_LinkFrom == -2 &&
                m_SelectedState == s)
                MoveNode(layer, size_t(s), io.MouseDelta);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && m_LinkFrom != -2 && s >= 0 && s != m_LinkFrom)
            {
                layer.Transitions.push_back({m_LinkFrom, uint32_t(s)});
                m_SelectedTransition = int(layer.Transitions.size() - 1);
                m_SelectedState      = -1;
                changed              = true;
            }
        }
        // A link being dragged.
        if (m_LinkFrom != -2)
        {
            draw->AddLine(toScreen(Center(position(m_LinkFrom))), io.MousePos, IM_COL32(255, 200, 80, 200), 2.0f);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                m_LinkFrom = -2;
        }
        // Dragging a selected node keeps moving it even when the cursor outruns it.
        if (m_SelectedState >= 0 && m_SelectedTransition < 0 && ImGui::IsItemActive() && m_LinkFrom == -2 &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
            !ImGui::IsMouseHoveringRect(toScreen(position(m_SelectedState)),
                                        Add(toScreen(position(m_SelectedState)), {NodeWidth, NodeHeight})))
            MoveNode(layer, size_t(m_SelectedState), io.MouseDelta);
        draw->PopClipRect();

        // Right-click: add a state where clicked.
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##graphmenu");
        if (ImGui::BeginPopup("##graphmenu"))
        {
            if (ImGui::MenuItem(ICON_FA_PLUS "  Add state"))
            {
                controller.Motions.push_back({});
                layer.States.push_back({"State" + std::to_string(layer.States.size()), uint32_t(controller.Motions.size() - 1)});
                layer.States.back().EditorPosition =
                    glm::vec2(ImGui::GetMousePosOnOpeningCurrentPopup().x - origin.x - m_Pan.x,
                              ImGui::GetMousePosOnOpeningCurrentPopup().y - origin.y - m_Pan.y);
                m_SelectedState = int(layer.States.size() - 1), m_SelectedTransition = -1;
                changed         = true;
            }
            if (ImGui::MenuItem(ICON_FA_PLUS "  Add blend tree state"))
            {
                AnimationMotion tree;
                tree.Type = AnimationMotionType::Blend1D;
                controller.Motions.push_back(tree);
                layer.States.push_back({"Blend" + std::to_string(layer.States.size()), uint32_t(controller.Motions.size() - 1)});
                m_SelectedState = int(layer.States.size() - 1), m_SelectedTransition = -1;
                changed         = true;
            }
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos({origin.x + 8, origin.y + size.y - 24});
        ImGui::TextDisabled("Right-click: add state   Shift-drag between states: transition   Middle-drag: pan");
        return changed;
    }

    bool AnimatorGraphPanel::DrawSelection(AnimationControllerData& controller, size_t layerIndex)
    {
        auto& layer   = controller.Layers[layerIndex];
        bool  changed = false;
        if (m_SelectedTransition >= 0 && size_t(m_SelectedTransition) < layer.Transitions.size())
        {
            auto& t = layer.Transitions[m_SelectedTransition];
            ImGui::Text("%s  " ICON_FA_ARROW_RIGHT "  %s", t.Source < 0 ? "Any State" : layer.States[t.Source].Name.c_str(),
                        layer.States[t.Target].Name.c_str());
            if (PropertyGrid::Begin("##transition"))
            {
                float duration = float(t.Duration);
                if (PropertyGrid::Float("Fade", duration, 0.01f, 0.0f, 10.0f, "%.2f s"))
                    t.Duration = duration, changed = true;
                bool exit = t.ExitTime.has_value();
                if (PropertyGrid::Checkbox("Exit time", exit))
                    t.ExitTime = exit ? std::optional<double>(1.0) : std::nullopt, changed = true;
                if (t.ExitTime)
                {
                    float at = float(*t.ExitTime);
                    if (PropertyGrid::Float("At", at, 0.01f, 0.0f, 10.0f, "%.2f"))
                        t.ExitTime = at, changed = true;
                }
                const char* interruptions[] = {"None", "Source", "Destination", "Both"};
                int         interruption    = int(t.Interruption);
                if (PropertyGrid::Combo("Interruption", interruptions, 4, interruption))
                    t.Interruption = AnimationInterruption(interruption), changed = true;
                PropertyGrid::End();
            }
            ImGui::TextDisabled("Conditions");
            const char* ops[] = {">", "<", "==", "!=", "If", "If not"};
            for (size_t k = 0; k < t.Conditions.size(); ++k)
            {
                auto& c = t.Conditions[k];
                ImGui::PushID(int(k));
                ImGui::SetNextItemWidth(110);
                if (ImGui::BeginCombo("##param", c.Parameter.c_str()))
                {
                    for (const auto& p : controller.Parameters)
                        if (ImGui::Selectable(p.Name.c_str(), p.Name == c.Parameter))
                            c.Parameter = p.Name, changed = true;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                int op = int(c.Op);
                ImGui::SetNextItemWidth(60);
                if (ImGui::Combo("##op", &op, ops, 6))
                    c.Op = AnimationConditionOp(op), changed = true;
                if (c.Op != AnimationConditionOp::If && c.Op != AnimationConditionOp::IfNot)
                {
                    ImGui::SameLine();
                    float value = float(c.Value);
                    ImGui::SetNextItemWidth(60);
                    if (ImGui::DragFloat("##value", &value, 0.01f))
                        c.Value = value, changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH))
                    t.Conditions.erase(t.Conditions.begin() + long(k)), --k, changed = true;
                ImGui::PopID();
            }
            if (ImGui::SmallButton(ICON_FA_PLUS " Condition"))
                t.Conditions.push_back({controller.Parameters.empty() ? "" : controller.Parameters.front().Name}),
                    changed = true;
            ImGui::Separator();
            if (ImGui::Button(ICON_FA_TRASH "  Delete transition"))
            {
                layer.Transitions.erase(layer.Transitions.begin() + m_SelectedTransition);
                m_SelectedTransition = -1;
                changed              = true;
            }
            return changed;
        }
        if (m_SelectedState < 0 || size_t(m_SelectedState) >= layer.States.size())
            return DrawLayerSettings(controller, layerIndex);
        auto& state = layer.States[m_SelectedState];
        if (PropertyGrid::Begin("##state"))
        {
            PropertyGrid::Row("Name");
            changed |= InputName("##name", state.Name);
            float speed = float(state.Speed);
            if (PropertyGrid::Float("Speed", speed, 0.01f, -10.0f, 10.0f))
                state.Speed = speed, changed = true;
            changed |= PropertyGrid::Checkbox("Loop", state.Loop);
            PropertyGrid::End();
        }
        if (layer.EntryState != uint32_t(m_SelectedState) && ImGui::Button("Make entry state"))
            layer.EntryState = uint32_t(m_SelectedState), changed = true;
        if (state.Motion >= controller.Motions.size())
            return changed;
        auto& motion = controller.Motions[state.Motion];
        ImGui::Separator();
        const char* types[] = {"Clip", "1D blend tree", "2D blend tree"};
        int         type    = int(motion.Type);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##motiontype", &type, types, 3))
            motion.Type = AnimationMotionType(type), changed = true;
        if (motion.Type == AnimationMotionType::Clip)
            changed |= ClipCombo("##clip", motion.Clip);
        else
        {
            // Blend tree: parameters and children (each child is its own clip motion).
            const auto parameterCombo = [&](const char* label, std::string& name) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                bool picked = false;
                if (ImGui::BeginCombo(label, name.empty() ? "(parameter)" : name.c_str()))
                {
                    for (const auto& p : controller.Parameters)
                        if (p.Type == AnimationParameterType::Float && ImGui::Selectable(p.Name.c_str(), p.Name == name))
                            name = p.Name, picked = true;
                    ImGui::EndCombo();
                }
                return picked;
            };
            changed |= parameterCombo("##px", motion.ParameterX);
            if (motion.Type == AnimationMotionType::Blend2D)
                changed |= parameterCombo("##py", motion.ParameterY);
            for (size_t c = 0; c < motion.Children.size(); ++c)
            {
                auto& child = motion.Children[c];
                ImGui::PushID(int(c));
                if (child.Motion < controller.Motions.size())
                    changed |= ClipCombo("##childclip", controller.Motions[child.Motion].Clip);
                if (motion.Type == AnimationMotionType::Blend1D)
                {
                    ImGui::SetNextItemWidth(120);
                    changed |= ImGui::DragFloat("Threshold", &child.Threshold, 0.01f);
                }
                else
                {
                    ImGui::SetNextItemWidth(160);
                    changed |= ImGui::DragFloat2("Position", &child.Position.x, 0.01f);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH))
                    motion.Children.erase(motion.Children.begin() + long(c)), --c, changed = true;
                ImGui::Separator();
                ImGui::PopID();
            }
            if (ImGui::SmallButton(ICON_FA_PLUS " Motion"))
            {
                controller.Motions.push_back({});
                // `motion` may have moved with the vector; reach it again by index.
                auto& tree = controller.Motions[state.Motion];
                tree.Children.push_back({uint32_t(controller.Motions.size() - 1),
                                         tree.Children.empty() ? 0.0f : tree.Children.back().Threshold + 1.0f});
                changed = true;
            }
        }
        ImGui::Separator();
        if (ImGui::Button(ICON_FA_TRASH "  Delete state"))
        {
            const auto removed = int32_t(m_SelectedState);
            layer.States.erase(layer.States.begin() + removed);
            for (auto it = layer.Transitions.begin(); it != layer.Transitions.end();)
                it = (it->Source == removed || int32_t(it->Target) == removed) ? layer.Transitions.erase(it) : std::next(it);
            for (auto& t : layer.Transitions)
            {
                if (t.Source > removed)
                    --t.Source;
                if (int32_t(t.Target) > removed)
                    --t.Target;
            }
            if (layer.EntryState >= layer.States.size())
                layer.EntryState = layer.States.empty() ? 0 : uint32_t(layer.States.size() - 1);
            m_SelectedState = -1;
            changed         = true;
        }
        return changed;
    }

    bool AnimatorGraphPanel::DrawLayerSettings(AnimationControllerData& controller, size_t layerIndex)
    {
        auto&               layer    = controller.Layers[layerIndex];
        bool                changed  = false;
        Scene*              scene    = m_Hierarchy.GetScene();
        Entity              root     = scene->GetEntityByUUID(m_Entity);
        const auto&         a        = root.GetComponent<AnimatorComponent>();
        const SkeletonData* skeleton = a.SkeletonSource ? &a.SkeletonSource->GetData() : nullptr;
        ImGui::TextDisabled("Select a state or a transition to edit it.");
        if (PropertyGrid::BeginGroup("Layer"))
        {
            if (PropertyGrid::Row("Name"))
                changed |= InputName("##layername", layer.Name);
            // A slot plays one-shot actions over this layer's state machine (unique per controller).
            if (PropertyGrid::Row("Slot", "Actions name this slot to play over the layer; empty: none"))
                changed |= InputName("##slot", layer.Slot);
            changed |= PropertyGrid::Float("Weight", layer.Weight, 0.01f, 0.0f, 1.0f);
            const char* modes[] = {"Override", "Additive"};
            int         mode    = int(layer.Mode);
            if (PropertyGrid::Combo("Mode", modes, 2, mode))
                layer.Mode = AnimationLayerMode(mode), changed = true;
            if (layerIndex > 0)
            {
                PropertyGrid::BeginFree();
                if (ImGui::Button(ICON_FA_TRASH "  Remove layer", {-FLT_MIN, 0}))
                {
                    controller.Layers.erase(controller.Layers.begin() + long(layerIndex));
                    m_Layer = layerIndex - 1;
                    PropertyGrid::EndFree();
                    PropertyGrid::EndGroup();
                    return true;
                }
                PropertyGrid::EndFree();
            }
            PropertyGrid::EndGroup();
        }
        // Joints the layer affects; empty: every joint and shape key.
        if (PropertyGrid::BeginGroup(
                ("Mask (" + std::to_string(layer.Mask.size()) + ")").c_str(), nullptr, nullptr, !layer.Mask.empty()))
        {
            for (size_t i = 0; i < layer.Mask.size(); ++i)
            {
                auto& m = layer.Mask[i];
                ImGui::PushID(int(i));
                changed |= ComponentEditors::JointCombo("Joint", m.Joint, skeleton);
                changed |= PropertyGrid::Float("Weight", m.Weight, 0.01f, 0.0f, 1.0f);
                changed |= PropertyGrid::Checkbox("With children", m.IncludeChildren);
                PropertyGrid::BeginFree();
                if (ImGui::SmallButton(ICON_FA_TRASH " Remove joint"))
                    layer.Mask.erase(layer.Mask.begin() + long(i)), --i, changed = true;
                PropertyGrid::EndFree();
                ImGui::PopID();
            }
            PropertyGrid::BeginFree();
            if (layer.Mask.empty())
                ImGui::TextDisabled("Empty: the layer affects every joint and shape key.");
            if (ImGui::Button(ICON_FA_PLUS "  Joint", {-FLT_MIN, 0}))
                layer.Mask.push_back({}), changed = true;
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        // The controller's parameters (shared by every layer) and their defaults.
        if (PropertyGrid::BeginGroup(("Parameters (" + std::to_string(controller.Parameters.size()) + ")").c_str()))
        {
            const char* types[] = {"Float", "Int", "Bool", "Trigger"};
            PropertyGrid::BeginFree();
            for (size_t i = 0; i < controller.Parameters.size(); ++i)
            {
                auto& p = controller.Parameters[i];
                ImGui::PushID(int(i));
                const float spacing = ImGui::GetStyle().ItemSpacing.x, remove = ImGui::GetFrameHeight();
                const float avail = ImGui::GetContentRegionAvail().x - remove - 3 * spacing;
                ImGui::SetNextItemWidth(avail * 0.45f);
                changed |= InputName("##name", p.Name);
                ImGui::SameLine();
                int type = int(p.Type);
                ImGui::SetNextItemWidth(avail * 0.3f);
                if (ImGui::Combo("##type", &type, types, 4))
                    p.Type = AnimationParameterType(type), changed = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(avail * 0.25f);
                float value = float(p.Default);
                if (p.Type != AnimationParameterType::Trigger && ImGui::DragFloat("##default", &value, 0.01f))
                    p.Default = value, changed = true;
                if (p.Type == AnimationParameterType::Trigger)
                    ImGui::Dummy({avail * 0.25f, ImGui::GetFrameHeight()});
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Default value");
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_TRASH, {remove, remove}))
                    controller.Parameters.erase(controller.Parameters.begin() + long(i)), --i, changed = true;
                ImGui::PopID();
            }
            if (ImGui::Button(ICON_FA_PLUS "  Parameter", {-FLT_MIN, 0}))
                controller.Parameters.push_back(
                    {"Param" + std::to_string(controller.Parameters.size()), AnimationParameterType::Float, 0}),
                    changed = true;
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        return changed;
    }

    bool AnimatorGraphPanel::DrawExpressions(AnimationControllerData& controller)
    {
        Scene*  scene   = m_Hierarchy.GetScene();
        Entity  root    = scene->GetEntityByUUID(m_Entity);
        auto&   a       = root.GetComponent<AnimatorComponent>();
        bool    changed = false;
        if (!a.Player)
            scene->PrepareAnimator(root);
        // Edit mode does not advance time: show an expression at once.
        const auto preview = [&](const std::string& name) {
            if (!a.Player || !a.Player->SetExpression(name, 0.0))
                return;
            a.Player->RebuildPose();
            scene->ApplyAnimatorPose(root, a.Player->GetPose());
        };
        const std::vector<ShapeKey>* keys =
            a.Player ? &a.Player->GetShapeKeys() : nullptr;
        if (!keys || keys->empty())
            ImGui::TextDisabled("This model has no shape keys (or the animator is not prepared yet).");
        for (size_t e = 0; e < controller.Expressions.size(); ++e)
        {
            auto& preset = controller.Expressions[e];
            ImGui::PushID(int(e));
            const bool open = ImGui::TreeNodeEx("##preset", ImGuiTreeNodeFlags_DefaultOpen, "%s", preset.Name.c_str());
            ImGui::SameLine();
            if (a.Player && ImGui::SmallButton(ICON_FA_PLAY " Preview"))
                preview(preset.Name);
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_TRASH))
            {
                controller.Expressions.erase(controller.Expressions.begin() + long(e));
                changed = true;
                if (open)
                    ImGui::TreePop();
                ImGui::PopID();
                break;
            }
            if (open)
            {
                ImGui::SetNextItemWidth(200);
                changed |= InputName("##presetname", preset.Name);
                for (size_t w = 0; w < preset.Weights.size(); ++w)
                {
                    auto& weight = preset.Weights[w];
                    ImGui::PushID(int(w));
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::BeginCombo("##key", weight.Name.c_str()))
                    {
                        if (keys)
                            for (const auto& key : *keys)
                                if (ImGui::Selectable(key.Name.c_str(), key.Name == weight.Name))
                                    weight.Name = key.Name, changed = true;
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120);
                    changed |= ImGui::SliderFloat("##w", &weight.Weight, 0.0f, 1.0f);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ICON_FA_TRASH))
                        preset.Weights.erase(preset.Weights.begin() + long(w)), --w, changed = true;
                    ImGui::PopID();
                }
                if (ImGui::SmallButton(ICON_FA_PLUS " Shape key"))
                    preset.Weights.push_back({keys && !keys->empty() ? keys->front().Name : "", 1.0f}), changed = true;
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::Button(ICON_FA_PLUS "  Expression"))
            controller.Expressions.push_back({"Expression" + std::to_string(controller.Expressions.size())}), changed = true;
        ImGui::SameLine();
        if (a.Player && ImGui::Button("Neutral"))
            preview("");
        return changed;
    }
} // namespace Kans
