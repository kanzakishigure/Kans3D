#include "kspch.h"
#include "ModelData.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace Kans
{
    ModelAsset::ModelAsset(ModelData data) : m_Data(std::move(data)), m_EditorData(m_Data)
    {
        if (m_Data.Entities.empty())
            m_Data.Entities = BuildPrefabEntities(m_Data);
        for (auto& node : m_EditorData.Nodes)
        {
            std::vector<AssetID> parts;
            for (AssetID mesh : node.Meshes)
            {
                bool mapped = false;
                for (const auto& identity : m_Data.SectionIdentities)
                    if (identity.Mesh == mesh)
                    {
                        parts.push_back(identity.OldMesh);
                        mapped = true;
                    }
                if (!mapped)
                    parts.push_back(mesh);
            }
            node.Meshes = std::move(parts);
        }
    }

    std::vector<PrefabNodeData> BuildPrefabEntities(const ModelData& data)
    {
        std::vector<PrefabNodeData> result;
        std::vector<int64_t>        parents;
        // KMOD v2 can be loaded while StartOutdated is still re-importing it.
        // Its inferred keys must exactly match ModelImporter's v3 keys, including
        // duplicate-name disambiguation, so that upgrade cannot change UUIDs.
        std::vector<std::string>      paths, keys;
        std::map<std::string, size_t> counts, occurrences;
        for (const auto& node : data.Nodes)
        {
            paths.push_back((node.Parent < 0 ? std::string() : paths.at(node.Parent)) + "/" + node.Name);
            keys.push_back("node:" + std::to_string(node.Name.size()) + ":" + node.Name);
            ++counts[keys.back()];
        }
        for (size_t i = 0; i < keys.size(); ++i)
            if (counts[keys[i]] > 1)
                keys[i] += ":" + std::to_string(paths[i].size()) + ":" + paths[i];
        for (auto& key : keys)
            if (const auto occurrence = occurrences[key]++; occurrence)
                key += "#" + std::to_string(occurrence);
        const std::array<float, 16> identity {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (size_t i = 0; i < data.Nodes.size(); ++i)
        {
            const auto&   node   = data.Nodes[i];
            const auto    key    = node.Key.empty() ? keys[i] : node.Key;
            const int64_t parent = result.size();
            parents.push_back(parent);
            result.push_back(
                {key, node.Name, node.Parent < 0 ? -1 : parents.at(node.Parent), node.LocalTransform, {}, {}, {}});
            for (auto mesh : node.Meshes)
            {
                bool mapped = false;
                for (const auto& part : data.SectionIdentities)
                    if (part.Mesh == mesh)
                    {
                        result.push_back({key + "/part:" + std::to_string(part.ObjectKey.size()) + ":" + part.ObjectKey,
                                          part.ObjectKey,
                                          parent,
                                          identity,
                                          mesh,
                                          part.Section,
                                          {}});
                        mapped = true;
                    }
                if (!mapped)
                    result.push_back({key + "/mesh:" + mesh.ToString(), node.Name, parent, identity, mesh, {}, {}});
            }
        }
        return result;
    }

    bool IsValidMaterialRenderState(const MaterialRenderState& state)
    {
        return (uint32_t(state.AlphaMode) <= uint32_t(MaterialAlphaMode::Dithered) ||
                state.AlphaMode == MaterialAlphaMode::Additive ||
                state.AlphaMode == MaterialAlphaMode::Premultiplied) &&
               std::isfinite(state.AlphaCutoff) && state.AlphaCutoff >= 0;
    }

    std::variant<std::monostate, AssetError> ValidateMaterialData(const MaterialData& data)
    {
        std::set<std::string> passNames;
        if (data.DisabledPasses.size() > 32)
            return AssetError {AssetErrorCode::InvalidData, "Too many disabled passes"};
        for (const auto& name : data.DisabledPasses)
            if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos ||
                !passNames.insert(name).second)
                return AssetError {AssetErrorCode::InvalidData, "Invalid disabled pass name"};
        if ((!data.Shader.IsValid() && data.Shader != AssetID {}) || data.Name.size() > 4096 ||
            data.Name.find('\0') != std::string::npos || data.Textures.size() > 1000000 ||
            !std::all_of(data.BaseColor.begin(), data.BaseColor.end(), [](float v) { return std::isfinite(v); }) ||
            !std::isfinite(data.Metallic) || !std::isfinite(data.Roughness) || !IsValidMaterialRenderState(data.Render))
            return AssetError {AssetErrorCode::InvalidData, "Invalid material Product data"};
        for (const auto& binding : data.Textures)
            if (!binding.Texture.IsValid() || binding.Texture == data.Shader || !binding.Semantic ||
                binding.Semantic > MaxMaterialTextureSemantic || binding.UVChannel >= 8)
                return AssetError {AssetErrorCode::InvalidData, "Invalid material Texture binding"};
        if (data.Bindings)
        {
            if (!data.Shader.IsValid())
                return AssetError {AssetErrorCode::InvalidData, "Material binding recipe requires a Shader"};
            for (const auto& assignment : data.Bindings->Textures)
                if (assignment.TextureIndex >= data.Textures.size())
                    return AssetError {AssetErrorCode::InvalidData, "Material recipe references an absent Texture"};
        }
        return std::monostate {};
    }
} // namespace Kans
