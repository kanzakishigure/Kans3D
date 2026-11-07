#pragma once
#include <string>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Scene/Entity.h"

namespace Kans
{
    class AssetManager;
    class Scene;

    // Puts `material` in `entity`'s material slot, as a per-instance override: a prefab
    // part records a prefab override, a mesh or editable mesh its own slot. An invalid
    // material restores the imported one. False when the entity has no such mesh.
    bool AssignMaterial(Scene& scene, Entity entity, uint32_t slot, AssetID material);

    // A mesh slot drawn with a given material.
    struct MaterialUse
    {
        UUID        Entity = 0;
        uint32_t    Slot   = 0;
        std::string EntityName, SlotName;
        bool        Overridden = false; // Assigned on this entity rather than the mesh's or prefab's own.
    };
    // Every mesh slot in `scene` drawn with `material`, in hierarchy order. A prefab
    // part counts only the slot of the section it draws (its mesh from `assets` or
    // the prefab instance).
    std::vector<MaterialUse> FindMaterialUses(const Scene& scene, AssetID material, AssetManager* assets = nullptr);
} // namespace Kans
