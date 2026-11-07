#include <imgui.h>
#include "ComponentEditors.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    namespace
    {
        void ListEntities(Scene& scene, Entity entity, int depth, uint64_t& id, bool& changed)
        {
            const uint64_t uuid = uint64_t(entity.GetUUID());
            ImGui::PushID(&uuid);
            ImGui::Indent(float(depth) * 10.0f);
            if (ImGui::Selectable(entity.GetComponent<TagComponent>().Tag.c_str(), id == uuid))
                id = uuid, changed = true;
            ImGui::Unindent(float(depth) * 10.0f);
            ImGui::PopID();
            for (Entity child : scene.GetChildren(entity))
                ListEntities(scene, child, depth + 1, id, changed);
        }

        bool InputParameter(std::string& value)
        {
            char buffer[128] = {};
            value.copy(buffer, sizeof(buffer) - 1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (!ImGui::InputText("##parameter", buffer, sizeof(buffer)))
                return false;
            value = buffer;
            return true;
        }
    } // namespace

    bool EntityField(const char* label, uint64_t& id, Scene* scene)
    {
        if (!PropertyGrid::Row(label))
            return false;
        Entity      target  = scene && id ? scene->GetEntityByUUID(UUID(id)) : Entity {};
        std::string current = !id ? "(none)" : target ? target.GetComponent<TagComponent>().Tag : "(missing)";
        bool        changed = false;
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##entity", current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("(none)", id == 0))
                id = 0, changed = true;
            if (scene)
                for (Entity root : scene->GetRootEntities())
                    ListEntities(*scene, root, 0, id, changed);
            ImGui::EndCombo();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload("entity_payload");
                payload && payload->DataSize == sizeof(uint64_t))
                id = *static_cast<const uint64_t*>(payload->Data), changed = true;
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        return changed;
    }

    void DrawMaterialParameterDrivers(Entity entity, ComponentUI& ui)
    {
        ImGui::TextDisabled("Writes a vec3/vec4 parameter of this model's materials every frame,\n"
                            "from another entity's transform (e.g. one attached to a bone).");
        entity.PatchComponent<MaterialParameterDriversComponent>([&](MaterialParameterDriversComponent& c) {
            const char* quantities[] = {"World position", "World forward (+Z)", "World up (+Y)", "World right (+X)"};
            for (size_t i = 0; i < c.Bindings.size(); ++i)
            {
                auto& binding = c.Bindings[i];
                ImGui::PushID(int(i));
                if (PropertyGrid::Begin("##Driver"))
                {
                    if (PropertyGrid::Row("Parameter", "A vec3 or vec4 uniform declared by the material's shader"))
                        InputParameter(binding.Parameter);
                    uint64_t source = uint64_t(binding.SourceEntity);
                    if (EntityField("Source", source, ui.GetScene()))
                        binding.SourceEntity = UUID(source);
                    int quantity = int(binding.Quantity);
                    if (PropertyGrid::Combo("Value", quantities, 4, quantity))
                        binding.Quantity = MaterialParameterQuantity(quantity);
                    PropertyGrid::End();
                }
                if (ImGui::SmallButton(ICON_FA_TRASH " Remove"))
                    c.Bindings.erase(c.Bindings.begin() + long(i)), --i;
                ImGui::Separator();
                ImGui::PopID();
            }
            if (ImGui::SmallButton(ICON_FA_PLUS " Binding"))
                c.Bindings.push_back({});
        });
    }
} // namespace Kans::ComponentEditors
