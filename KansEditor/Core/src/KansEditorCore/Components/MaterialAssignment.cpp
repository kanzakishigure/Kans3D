#include "KansEditorCore/Components/MaterialAssignment.h"
#include <algorithm>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans
{
    bool AssignMaterial(Scene& scene, Entity entity, uint32_t slot, AssetID material)
    {
        if (!entity)
            return false;
        if (entity.HasComponent<MeshComponent>())
        {
            const Entity root = scene.GetPrefabInstanceRoot(entity);
            if (root && entity.HasComponent<PrefabOwnedComponent>())
            {
                scene.SetPrefabMaterialOverride(
                    root, entity.GetComponent<PrefabOwnedComponent>().NodeKey, slot, material);
                return true;
            }
            entity.PatchComponent<MeshComponent>([&](MeshComponent& mesh) {
                mesh.Materials.resize(std::max<size_t>(mesh.Materials.size(), slot + 1));
                mesh.Materials[slot] = material;
            });
            return true;
        }
        if (entity.HasComponent<EditableMeshComponent>())
        {
            entity.PatchComponent<EditableMeshComponent>([&](EditableMeshComponent& mesh) {
                mesh.Materials.resize(std::max<size_t>(mesh.Materials.size(), slot + 1));
                mesh.Materials[slot] = material;
            });
            return true;
        }
        return false;
    }

    namespace
    {
        std::vector<uint32_t> AllSlots(size_t count)
        {
            std::vector<uint32_t> all(count);
            for (uint32_t i = 0; i < count; ++i)
                all[i] = i;
            return all;
        }

        // The slots a mesh entity draws: its section's slot, or all of them.
        std::vector<uint32_t> DrawnSlots(const Scene& scene, Entity entity, size_t count, AssetManager* assets)
        {
            const auto& mesh = entity.GetComponent<MeshComponent>();
            if (!mesh.Section)
                return AllSlots(count);
            Ref<const MeshAsset> data;
            if (assets)
                if (const auto loaded = assets->Request<MeshAsset>(mesh.Mesh); loaded.IsReady())
                    data = loaded.Asset;
            if (const Entity root = !data ? scene.GetPrefabInstanceRoot(entity) : Entity {})
            {
                const auto& sources = root.GetComponent<PrefabInstanceComponent>().MeshSources;
                if (const auto found = sources.find(mesh.Mesh); found != sources.end())
                    data = found->second;
            }
            if (data && *mesh.Section < data->GetData().Sections.size())
                return {data->GetData().Sections[*mesh.Section].MaterialSlot};
            return AllSlots(count);
        }
    } // namespace

    std::vector<MaterialUse> FindMaterialUses(const Scene& scene, AssetID material, AssetManager* assets)
    {
        std::vector<MaterialUse> uses;
        if (!material.IsValid())
            return uses;
        const auto visit = [&](auto&& self, Entity entity) -> void {
            const std::vector<AssetID>* own = nullptr;
            // What a slot draws when nothing is assigned here: a prefab part's imported
            // materials, otherwise the mesh's own.
            const std::vector<AssetID>* base  = nullptr;
            const auto                  slots = scene.GetMeshSlots(entity);
            std::vector<uint32_t>       drawn;
            if (entity.HasComponent<MeshComponent>())
            {
                own   = &entity.GetComponent<MeshComponent>().Materials;
                drawn = DrawnSlots(scene, entity, slots.size(), assets);
                if (const Entity root = scene.GetPrefabInstanceRoot(entity);
                    root && entity.HasComponent<PrefabOwnedComponent>())
                {
                    const auto& nodes = root.GetComponent<PrefabInstanceComponent>().Nodes;
                    if (const auto state = nodes.find(entity.GetComponent<PrefabOwnedComponent>().NodeKey);
                        state != nodes.end())
                        base = &state->second.BaseMesh.Materials;
                }
            }
            else if (entity.HasComponent<EditableMeshComponent>())
            {
                own   = &entity.GetComponent<EditableMeshComponent>().Materials;
                drawn = AllSlots(std::max(slots.size(), own->size()));
            }
            for (const uint32_t slot : drawn)
            {
                const AssetID fallback  = base && slot < base->size() && (*base)[slot].IsValid() ? (*base)[slot] :
                                          slot < slots.size() ? slots[slot].DefaultMaterial :
                                                                AssetID {};
                const bool    assigned  = slot < own->size() && (*own)[slot].IsValid();
                const AssetID effective = assigned ? (*own)[slot] : fallback;
                if (effective != material)
                    continue;
                MaterialUse use;
                use.Entity     = entity.GetUUID();
                use.Slot       = slot;
                use.EntityName = entity.GetComponent<TagComponent>().Tag;
                use.SlotName   = slot < slots.size() ? slots[slot].Name : "Slot " + std::to_string(slot);
                use.Overridden = assigned && (*own)[slot] != fallback;
                uses.push_back(std::move(use));
            }
            for (Entity child : scene.GetChildren(entity))
                self(self, child);
        };
        for (Entity root : scene.GetRootEntities())
            visit(visit, root);
        return uses;
    }
} // namespace Kans
