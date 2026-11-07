#include "kspch.h"
#include "SceneAssetReferences.h"
#include <yaml-cpp/yaml.h>
#include "SceneSerializer.h"

namespace Kans
{
    std::set<AssetID> CollectSceneAssetReferences(const Scene& scene)
    {
        std::set<AssetID> references;
        // Use the persistence encoder rather than a second component/field list:
        // every future component serializing an AssetID is automatically covered,
        // including IDs nested in inline controllers, prefab overrides and settings.
        const auto visit = [&](const auto& self, const YAML::Node& node) -> void {
            if (node.IsScalar())
            {
                const auto id = AssetID::FromString(node.Scalar());
                if (id.IsValid())
                    references.insert(id);
            }
            else if (node.IsMap())
                for (const auto& entry : node)
                    self(self, entry.second);
            else if (node.IsSequence())
                for (const auto& entry : node)
                    self(self, entry);
        };
        visit(visit, SceneSerializer::SerializeNode(scene, true));
        return references;
    }
} // namespace Kans
