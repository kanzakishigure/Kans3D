#include <algorithm>
#include <map>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "KansEditorCore/Components/MaterialAssignment.h"
#include "UI/EditorIcons.h"
#include "UI/MaterialSlotRow.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    // Mesh and prefab edits notify from their pickers rather than through a
    // per-frame PatchComponent: any prefab notification reconciles the instance.

    namespace
    {
        std::string Parts(size_t count) { return std::to_string(count) + (count == 1 ? " part" : " parts"); }
    } // namespace

    void DrawPrefabInstance(Entity root, ComponentUI& ui)
    {
        Scene* scene    = ui.GetScene();
        auto&  instance = root.GetComponent<PrefabInstanceComponent>();
        if (PropertyGrid::Begin("##Prefab"))
        {
            AssetID chosen = instance.Prefab;
            if (ui.AssetPicker("Prefab", AssetType::Model, chosen))
                root.PatchComponent<PrefabInstanceComponent>([&](PrefabInstanceComponent& p) {
                    p.Prefab = chosen;
                    p.Overrides.clear(); // Overrides refer to the previous prefab's nodes.
                });
            PropertyGrid::End();
        }
        if (!instance.Prefab.IsValid())
        {
            ImGui::TextDisabled("Pick an imported model, or drop a model file on the field.");
            return;
        }
        if (instance.LegacyPending || !instance.Source)
        {
            ImGui::TextDisabled("Loading prefab...");
            return;
        }
        if (!scene)
            return;

        // Replace a material on every part of this instance that uses it.
        struct Use
        {
            Entity   Part;
            uint32_t Slot;
        };
        std::vector<std::pair<AssetID, std::vector<Use>>> materials;
        for (Entity part : scene->GetMeshEntities(root))
        {
            if (!part.HasComponent<PrefabOwnedComponent>())
                continue;
            const auto& key   = part.GetComponent<PrefabOwnedComponent>().NodeKey;
            auto        state = instance.Nodes.find(key);
            if (state == instance.Nodes.end())
                continue;
            const auto& base = state->second.BaseMesh.Materials;
            for (uint32_t slot = 0; slot < base.size(); ++slot)
            {
                auto found = std::find_if(materials.begin(), materials.end(),
                                          [&](const auto& item) { return item.first == base[slot]; });
                if (found == materials.end())
                    found = materials.insert(materials.end(), {base[slot], {}});
                found->second.push_back({part, slot});
            }
        }
        if (PropertyGrid::BeginGroup(("Materials (" + std::to_string(materials.size()) + ")").c_str()))
        {
            // A row stands for an imported material and replaces it on every part using it.
            PropertyGrid::BeginFree();
            for (const auto& [imported, uses] : materials)
            {
                ImGui::PushID(imported.ToString().c_str());
                // Shown as replaced only when every use carries the same replacement.
                AssetID current = uses.front().Part.GetComponent<MeshComponent>().Materials.at(uses.front().Slot);
                for (const auto& use : uses)
                    if (use.Part.GetComponent<MeshComponent>().Materials.at(use.Slot) != current)
                        current = imported;
                SlotParts parts;
                for (const auto& use : uses)
                    parts.push_back({use.Part.GetUUID(), use.Slot});
                AssetID chosen;
                if (Widgets::MaterialSlotRow(
                        ui, ui.MaterialName(imported), Parts(uses.size()), parts, imported, current, chosen))
                    for (const auto& use : uses)
                        scene->SetPrefabMaterialOverride(
                            root, use.Part.GetComponent<PrefabOwnedComponent>().NodeKey, use.Slot, chosen);
                ImGui::PopID();
            }
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        size_t removed = 0;
        for (const auto& [key, change] : instance.Overrides)
            removed += change.Removed ? 1 : 0;
        if (removed && ImGui::Button((ICON_FA_ROTATE_LEFT "  Restore " + std::to_string(removed) + " removed part" +
                                      (removed > 1 ? "s" : ""))
                                         .c_str(),
                                     {-FLT_MIN, 0.0f}))
            for (const auto& [key, change] : std::map(instance.Overrides))
                if (change.Removed)
                    scene->SetPrefabNodeRemoved(root, key, false);
        if (!instance.Overrides.empty() &&
            ImGui::Button(ICON_FA_ROTATE_RIGHT "  Revert all overrides", {-FLT_MIN, 0.0f}))
            for (const auto& [key, change] : std::map(instance.Overrides))
                scene->ClearPrefabOverride(root, key);
    }

    void DrawMeshMaterials(Entity                               entity,
                           ComponentUI&                         ui,
                           Scene&                               scene,
                           const MeshAsset*                     data,
                           Entity                               root,
                           bool                                 owned,
                           const std::vector<MeshMaterialSlot>& slots);

    void DrawMesh(Entity entity, ComponentUI& ui)
    {
        Scene*        scene  = ui.GetScene();
        AssetManager* assets = ui.GetAssets();
        auto&         mesh   = entity.GetComponent<MeshComponent>();
        Entity        root   = scene ? scene->GetPrefabInstanceRoot(entity) : Entity {};
        const bool    owned  = root && entity.HasComponent<PrefabOwnedComponent>();
        Ref<const MeshAsset> data;
        if (assets && mesh.Mesh.IsValid())
            if (auto loaded = assets->Request<MeshAsset>(mesh.Mesh); loaded.IsReady())
                data = loaded.Asset;
        if (PropertyGrid::BeginGroup("Mesh"))
        {
            if (owned)
                PropertyGrid::Text("Mesh", mesh.Section ? "Section " + std::to_string(*mesh.Section) : "All sections");
            else
            {
                AssetID chosen = mesh.Mesh;
                if (ui.AssetPicker("Mesh", AssetType::StaticMesh, chosen))
                    entity.PatchComponent<MeshComponent>([&](MeshComponent& m) {
                        m.Mesh = chosen;
                        m.Materials.clear();
                    });
            }
            if (data)
            {
                const auto& d = data->GetData();
                PropertyGrid::Text("Geometry",
                                   std::to_string(d.Vertices.size()) + " vertices, " +
                                       std::to_string(d.Indices.size() / 3) + " triangles" +
                                       (d.HasSmoothNormals() ? ", smooth normals" : ""),
                                   true);
            }
            PropertyGrid::EndGroup();
        }
        const auto slots =
            mesh.Mesh.IsValid() && scene ? scene->GetMeshSlots(entity) : std::vector<MeshMaterialSlot> {};
        if (mesh.Mesh.IsValid() && scene && slots.empty())
            ImGui::TextDisabled("Loading mesh...");
        if (!slots.empty())
            DrawMeshMaterials(entity, ui, *scene, data.get(), root, owned, slots);
        // A prefab part's shadow flags come from the prefab and are restored when it
        // re-expands; use the material's receive_shadows for parts instead.
        if (!owned && PropertyGrid::BeginGroup("Lighting"))
        {
            bool cast = mesh.CastShadows, receive = mesh.ReceiveShadows;
            if (PropertyGrid::Checkbox("Cast shadows", cast) | PropertyGrid::Checkbox("Receive shadows", receive))
                entity.PatchComponent<MeshComponent>([&](MeshComponent& m) {
                    m.CastShadows    = cast;
                    m.ReceiveShadows = receive;
                });
            PropertyGrid::EndGroup();
        }
        // Read by post-process materials; folded until used. Unlike the shadow flags, a
        // prefab part keeps these as a prefab override, so they are editable on parts too.
        if (PropertyGrid::BeginGroup("Rendering", nullptr, nullptr, mesh.RenderCustomDepth))
        {
            bool custom  = mesh.RenderCustomDepth;
            int  stencil = mesh.CustomStencil;
            if (PropertyGrid::Checkbox("Custom depth", custom) |
                (custom && PropertyGrid::Int("Custom stencil", stencil, 0, 255)))
                entity.PatchComponent<MeshComponent>([&](MeshComponent& m) {
                    m.RenderCustomDepth = custom;
                    m.CustomStencil     = uint8_t(std::clamp(stencil, 0, 255));
                });
            PropertyGrid::EndGroup();
        }
    }

    void DrawMeshMaterials(Entity                               entity,
                           ComponentUI&                         ui,
                           Scene&                               scene,
                           const MeshAsset*                     data,
                           Entity                               root,
                           bool                                 owned,
                           const std::vector<MeshMaterialSlot>& slots)
    {
        const auto& mesh = entity.GetComponent<MeshComponent>();
        // A part shows only the slots its sections use.
        std::vector<uint32_t> shown;
        if (mesh.Section && data && *mesh.Section < data->GetData().Sections.size())
            shown.push_back(data->GetData().Sections[*mesh.Section].MaterialSlot);
        if (shown.empty())
            for (uint32_t slot = 0; slot < slots.size(); ++slot)
                shown.push_back(slot);
        const MeshComponent* base = nullptr;
        if (owned)
        {
            const auto& nodes = root.GetComponent<PrefabInstanceComponent>().Nodes;
            auto        state = nodes.find(entity.GetComponent<PrefabOwnedComponent>().NodeKey);
            if (state != nodes.end())
                base = &state->second.BaseMesh;
        }
        // Sections per slot, for the part counts.
        std::vector<size_t> sections(slots.size(), 0);
        if (data)
            for (const auto& section : data->GetData().Sections)
                if (section.MaterialSlot < sections.size())
                    ++sections[section.MaterialSlot];
        if (!PropertyGrid::BeginGroup(("Materials (" + std::to_string(shown.size()) + ")").c_str()))
            return;
        PropertyGrid::BeginFree();
        for (uint32_t slot : shown)
        {
            ImGui::PushID(int(slot));
            const AssetID imported =
                base && slot < base->Materials.size() ? base->Materials[slot] : slots[slot].DefaultMaterial;
            const AssetID current =
                slot < mesh.Materials.size() && mesh.Materials[slot].IsValid() ? mesh.Materials[slot] : imported;
            AssetID    chosen;
            // GetMeshSlots names unnamed slots "Slot <index>", 0-based like the scene file and scripts.
            const auto detail = owned ? "Slot " + std::to_string(slot) : Parts(sections[slot]);
            if (Widgets::MaterialSlotRow(
                    ui, slots[slot].Name, detail, {{entity.GetUUID(), slot}}, imported, current, chosen))
                AssignMaterial(scene, entity, slot, chosen);
            ImGui::PopID();
        }
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }
} // namespace Kans::ComponentEditors
