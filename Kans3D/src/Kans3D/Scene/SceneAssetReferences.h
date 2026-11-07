#pragma once
#include <set>
#include "Kans3D/Asset/Asset.h"

namespace Kans
{
    class Scene;
    // Includes scene settings, prefab overrides and generated entities. Invalid IDs are omitted.
    // Does not load assets, expand prefabs, mutate the scene or write files.
    std::set<AssetID> CollectSceneAssetReferences(const Scene& scene);
} // namespace Kans
