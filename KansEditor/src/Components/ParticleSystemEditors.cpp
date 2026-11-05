#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "ParticleSystemEditing.h"
#include "Panels/VfxGizmos.h"
#include "Physics/PhysicsEditing.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    namespace
    {
        namespace Edit = ParticleSystemEditing;

        // A typed editor for one user parameter value; true when it changed.
        bool ValueField(const char* id, Vfx::Value& v, const Vfx::Parameter& spec, float width)
        {
            ImGui::SetNextItemWidth(width);
            const float lo = spec.Range ? spec.Range->x : 0.0f, hi = spec.Range ? spec.Range->y : 0.0f;
            switch (v.Kind)
            {
                case Vfx::Type::Float:
                    return ImGui::DragFloat(id, &v.Data.x, 0.01f, lo, hi);
                case Vfx::Type::Vec2:
                    return ImGui::DragFloat2(id, &v.Data.x, 0.01f, lo, hi);
                case Vfx::Type::Vec3:
                case Vfx::Type::Position:
                    return ImGui::DragFloat3(id, &v.Data.x, 0.01f, lo, hi);
                case Vfx::Type::Vec4:
                case Vfx::Type::Quat:
                    return ImGui::DragFloat4(id, &v.Data.x, 0.01f, lo, hi);
                case Vfx::Type::Color: {
                    // A swatch with its picker, then the values dimmed.
                    const bool changed = ImGui::ColorEdit4(id, &v.Data.x,
                                                           ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                                                               ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.2f  %.2f  %.2f  %.2f", v.Data.x, v.Data.y, v.Data.z, v.Data.w);
                    return changed;
                }
                case Vfx::Type::Bool: {
                    bool b = v.Integer != 0;
                    if (!ImGui::Checkbox(id, &b))
                        return false;
                    v.Integer = b;
                    return true;
                }
                default:
                    if (!spec.EnumNames.empty())
                    {
                        int index = std::clamp(v.Integer, 0, int(spec.EnumNames.size()) - 1);
                        if (ImGui::BeginCombo(id, spec.EnumNames[size_t(index)].c_str()))
                        {
                            bool changed = false;
                            for (int i = 0; i < int(spec.EnumNames.size()); ++i)
                                if (ImGui::Selectable(spec.EnumNames[size_t(i)].c_str(), i == index))
                                    v.Integer = i, changed = true;
                            ImGui::EndCombo();
                            return changed;
                        }
                        return false;
                    }
                    return ImGui::DragInt(id, &v.Integer, 0.2f, int(lo), int(hi));
            }
        }

        bool CollisionMask(uint32_t& mask)
        {
            const auto& settings = KansFileSystem::GetProjectPhysicsSettings();
            if (!PropertyGrid::Row("Collides with", "The physics layers particle collision tests", mask != UINT32_MAX))
                return false;
            bool changed = false;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##vfxmask", PhysicsEditing::MaskSummary(mask, settings).c_str(), ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable("Everything", mask == UINT32_MAX))
                    mask = UINT32_MAX, changed = true;
                if (ImGui::Selectable("Nothing", mask == 0))
                    mask = 0, changed = true;
                ImGui::Separator();
                const auto labels = PhysicsEditing::LayerLabels(settings);
                for (uint32_t i = 0; i < 32 && i < labels.size(); ++i)
                {
                    bool on = mask & (1u << i);
                    if (ImGui::Checkbox(labels[i].c_str(), &on))
                        mask = on ? mask | (1u << i) : mask & ~(1u << i), changed = true;
                }
                ImGui::EndCombo();
            }
            if (PropertyGrid::RowReset(mask != UINT32_MAX))
                mask = UINT32_MAX, changed = true;
            return changed;
        }
    } // namespace

    void ComponentEditors::DrawParticleSystem(Entity entity, ComponentUI& ui)
    {
        auto       c        = entity.GetComponent<ParticleSystemComponent>();
        const auto authored = c;
        Scene*     scene    = ui.GetScene();
        const auto system   = scene ? Edit::ResolveSystem(*scene, entity, ui.GetAssets()) : nullptr;
        const ParticleSystemComponent defaults;

        if (PropertyGrid::BeginGroup("System"))
        {
            ui.AssetPicker("System", AssetType::VfxSystem, c.System);
            if (PropertyGrid::Row("Edit", "Edit the system in the VFX system editor"))
            {
                ImGui::BeginDisabled(!c.System.IsValid());
                if (ImGui::Button(ICON_FA_PEN "  Open in the system editor", {-FLT_MIN, 0}))
                    ui.OpenVfxSystem(c.System);
                ImGui::EndDisabled();
            }
            PropertyGrid::EndGroup();
        }

        if (PropertyGrid::BeginGroup("Playback"))
        {
            PropertyGrid::Checkbox("Auto-play on start", c.AutoActivate, &defaults.AutoActivate);
            PropertyGrid::Checkbox("Loop", c.Loop, &defaults.Loop);
            PropertyGrid::Checkbox("Prewarm", c.Prewarm, &defaults.Prewarm);
            PropertyGrid::Float("Simulation speed", c.TimeScale, 0.01f, 0.0f, 100.0f, "%.2fx", &defaults.TimeScale);
            const char* seeds[] = {"From the system", "Fixed", "Random per instance"};
            int         seed    = c.RandomSeed ? 2 : c.SeedOverride ? 1 : 0;
            const int   none    = 0;
            if (PropertyGrid::Combo("Seed", seeds, 3, seed, &none))
            {
                c.RandomSeed = seed == 2;
                if (seed == 1 && !c.SeedOverride)
                    c.SeedOverride = 1;
                if (seed != 1)
                    c.SeedOverride.reset();
            }
            if (c.SeedOverride && !c.RandomSeed)
            {
                int value = int(*c.SeedOverride);
                if (PropertyGrid::Int("Seed value", value, 0, INT32_MAX))
                    c.SeedOverride = uint32_t(std::max(0, value));
            }
            PropertyGrid::Checkbox("Preview in edit mode", c.PreviewInEditMode, &defaults.PreviewInEditMode);
            PropertyGrid::EndGroup();
        }

        if (system)
        {
            const auto params = Edit::ListUserParameters(c, system->GetData());
            if ((!params.Rows.empty() || !params.Warnings.empty()) && PropertyGrid::BeginGroup("User parameters"))
            {
                for (const auto& warning : params.Warnings)
                    if (PropertyGrid::Row(ICON_FA_TRIANGLE_EXCLAMATION, "Dropped on the next edit; ignored at runtime"))
                    {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextColored(ComponentEditors::ErrorColor, "%s", warning.c_str());
                        ImGui::PopTextWrapPos();
                    }
                for (const auto& row : params.Rows)
                {
                    ImGui::PushID(row.Spec.Name.c_str());
                    const auto  dot   = row.Spec.Name.find('.');
                    const auto  label = dot == std::string::npos ? row.Spec.Name : row.Spec.Name.substr(dot + 1);
                    if (PropertyGrid::Row(label.c_str(), "Override the system's default for this instance", row.Overridden))
                    {
                        bool overridden = row.Overridden;
                        if (ImGui::Checkbox("##override", &overridden))
                            c = overridden ? Edit::SetOverride(c, system->GetData(), row.Spec.Name, row.Value) :
                                             Edit::ResetOverride(c, system->GetData(), row.Spec.Name);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Override");
                        ImGui::SameLine();
                        auto value = row.Value;
                        ImGui::BeginDisabled(!row.Overridden);
                        const float reserve = row.Overridden ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
                        if (ValueField("##value", value, row.Spec, ImGui::GetContentRegionAvail().x - reserve))
                            c = Edit::SetOverride(c, system->GetData(), row.Spec.Name, value);
                        ImGui::EndDisabled();
                        if (PropertyGrid::RowReset(row.Overridden))
                            c = Edit::ResetOverride(c, system->GetData(), row.Spec.Name);
                    }
                    ImGui::PopID();
                }
                PropertyGrid::EndGroup();
            }
        }
        else if (c.System.IsValid() && !c.UserParameters.empty() && PropertyGrid::BeginGroup("User parameters"))
        {
            PropertyGrid::Text("Overrides", std::to_string(c.UserParameters.size()) + " stored; the system is not loaded", true);
            PropertyGrid::EndGroup();
        }

        if (PropertyGrid::BeginGroup("Layers", nullptr, nullptr, false))
        {
            CollisionMask(c.CollisionMask);
            int       layer = int(c.FieldLayer);
            const int zero  = 0;
            if (PropertyGrid::Int("Force field layer", layer, 0, 31, &zero))
                c.FieldLayer = uint32_t(std::clamp(layer, 0, 31));
            PropertyGrid::EndGroup();
        }

        // The scene viewport's handles: read-only unless "Edit asset in viewport" is on, which edits
        // the system asset itself (every instance) and leaves it unsaved until it is saved.
        if (auto* handles = ui.GetVfxHandles(); handles && c.System.IsValid() && PropertyGrid::BeginGroup("Viewport handles"))
        {
            auto& choice = handles->ChoiceFor(entity.GetUUID());
            bool  edit   = choice.Edit;
            if (PropertyGrid::Row("Edit asset in viewport",
                                  "Off: the handles only show the shapes. On: dragging them edits the system asset (every "
                                  "instance) through its system editor, which stays unsaved until you save it",
                                  edit))
            {
                if (ImGui::Checkbox("##editasset", &edit))
                    handles->SetEdit(entity.GetUUID(), c.System, edit);
            }
            const auto source = handles->Resolve(c.System, false);
            if (source.Document && source.Context)
            {
                const auto& data = source.Document->Data();
                if (!VfxSystemEditing::FindHandle(data, choice.Emitter) && !data.Emitters.empty())
                    choice.Emitter = data.Emitters.front().Guid;
                if (PropertyGrid::Row("Emitter", "Whose module and bounds get handles"))
                {
                    const auto* current = VfxSystemEditing::FindHandle(data, choice.Emitter);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##vfxemitter", current ? current->Name.c_str() : "None"))
                    {
                        for (const auto& h : data.Emitters)
                            if (ImGui::Selectable(h.Name.c_str(), h.Guid == choice.Emitter))
                                choice.Emitter = h.Guid, choice.Use.clear();
                        ImGui::EndCombo();
                    }
                }
                const auto uses = VfxGizmos::UsesWithHandles(data, *source.Context, choice.Emitter);
                if (PropertyGrid::Row("Module", "The module whose spatial inputs get handles"))
                {
                    std::string label = "Bounds only";
                    for (const auto& [guid, name] : uses)
                        if (guid == choice.Use)
                            label = name;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##vfxuse", label.c_str()))
                    {
                        if (ImGui::Selectable("Bounds only", choice.Use.empty()))
                            choice.Use.clear();
                        for (const auto& [guid, name] : uses)
                            if (ImGui::Selectable(name.c_str(), guid == choice.Use))
                                choice.Use = guid;
                        ImGui::EndCombo();
                    }
                }
                if (choice.Edit && source.Document->IsDirty())
                    PropertyGrid::Text("Asset", "Unsaved: save it in the system editor (Ctrl+S)", true);
            }
            if (!handles->Message().empty() && PropertyGrid::Row(ICON_FA_TRIANGLE_EXCLAMATION, "Why edit mode is off"))
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ComponentEditors::ErrorColor, "%s", handles->Message().c_str());
                ImGui::PopTextWrapPos();
            }
            PropertyGrid::EndGroup();
        }

        // Preview in the edited scene: runtime state only, never the component.
        const bool editMode = scene && !scene->GetVfxWorld().IsPlaying();
        if (editMode && c.System.IsValid() && PropertyGrid::BeginGroup("Preview"))
        {
            const auto state = Edit::State(*scene, entity);
            if (PropertyGrid::Row("Playback", "Simulates in the scene viewport; Stop returns to the authored state"))
            {
                ImGui::BeginDisabled(!c.PreviewInEditMode);
                if (ImGui::SmallButton(state == Edit::PreviewState::Playing ? ICON_FA_PAUSE : ICON_FA_PLAY))
                    state == Edit::PreviewState::Playing ? Edit::Pause(*scene, entity) : Edit::Play(*scene, entity);
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT))
                    Edit::Restart(*scene, entity);
                ImGui::SameLine();
                ImGui::BeginDisabled(state == Edit::PreviewState::Authored);
                if (ImGui::SmallButton(ICON_FA_STOP))
                    Edit::Stop(*scene, entity);
                ImGui::EndDisabled();
                ImGui::SameLine();
                float age = float(Edit::Age(*scene, entity));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##scrub", &age, 0.0f, std::max(5.0f, age), "%.2f s"))
                    Edit::Seek(*scene, entity, age);
                ImGui::EndDisabled();
            }
            if (!c.PreviewInEditMode)
                PropertyGrid::Text("Note", "Preview in edit mode is off", true);
            for (const auto& count : Edit::Counts(*scene, entity))
                PropertyGrid::Text(count.Name.c_str(), std::to_string(count.Count) + " / " + std::to_string(count.Capacity) + " particles", true);
            PropertyGrid::EndGroup();
        }

        if (c != authored)
            entity.SetComponent<ParticleSystemComponent>(c);
    }
} // namespace Kans
