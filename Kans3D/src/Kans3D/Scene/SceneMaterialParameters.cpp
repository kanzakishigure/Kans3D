#include "kspch.h"
#include <algorithm>
#include <cmath>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Asset/Runtime/MaterialParameterCollection.h"
#include "Scene.h"

namespace Kans
{
    namespace
    {
        template<class Map>
        std::pair<typename Map::mapped_type*, const MaterialParameterDefinition*>
        FindParameter(Map& active, std::string_view collection, std::string_view name)
        {
            for (auto& [id, runtime] : active)
            {
                const auto& data = runtime.Source->GetData();
                if (data.Name == collection)
                    for (const auto& p : data.Parameters)
                        if (p.Name == name)
                            return {&runtime, &p};
            }
            return {};
        }

        template<class Collection>
        bool AssignParameter(Collection& runtime, const MaterialParameterDefinition& p, glm::vec4 value)
        {
            const auto key      = MaterialParameterKey(runtime.Source->GetData().Name, p);
            const auto previous = runtime.Overrides.find(key);
            if (value == (previous == runtime.Overrides.end() ? p.Default : previous->second))
                return false;
            if (value == p.Default)
                runtime.Overrides.erase(key);
            else
                runtime.Overrides[key] = value;
            return true;
        }
    } // namespace

    bool Scene::ResolveMaterialParameterCollections(const std::vector<AssetID>& ids,
                                                    MaterialParameterMap&       resolved) const
    {
        if (!m_PrefabAssets)
            return true;
        std::set<std::string_view> names;
        std::set<uint64_t>         keys;
        bool                       unique = true;
        for (const auto& id : ids)
        {
            auto        result = m_PrefabAssets->Load<MaterialParameterCollectionAsset>(id);
            const auto* asset  = std::get_if<Ref<const MaterialParameterCollectionAsset>>(&result);
            if (!asset)
                continue;
            const auto& data = (*asset)->GetData();
            if (!names.insert(data.Name).second ||
                std::any_of(data.Parameters.begin(), data.Parameters.end(), [&](const auto& p) {
                    return keys.count(MaterialParameterKey(data.Name, p));
                }))
            {
                unique = false;
                continue;
            }
            auto& runtime       = resolved[id];
            runtime.Source      = *asset;
            const auto previous = m_ActiveMaterialParameters.find(id);
            for (const auto& p : data.Parameters)
            {
                const auto key = MaterialParameterKey(data.Name, p);
                keys.insert(key);
                if (previous != m_ActiveMaterialParameters.end())
                {
                    const auto value = previous->second.Overrides.find(key);
                    if (value != previous->second.Overrides.end())
                        runtime.Overrides.emplace(key, value->second);
                }
            }
        }
        return unique;
    }

    bool Scene::SetMaterialParameterCollections(std::vector<AssetID> ids)
    {
        if (ids.size() > MaterialCollectionLimit)
            return false;
        std::set<AssetID> unique;
        for (const auto& id : ids)
            if (!id.IsValid() || !unique.insert(id).second)
                return false;
        if (ids == m_MaterialCollectionIDs)
            return true;
        MaterialParameterMap resolved;
        if (!ResolveMaterialParameterCollections(ids, resolved))
            return false;
        m_MaterialCollectionIDs         = std::move(ids);
        m_ActiveMaterialParameters      = std::move(resolved);
        m_MaterialParametersNeedRefresh = false;
        m_MaterialParameterSnapshot.reset();
        return true;
    }

    void Scene::RefreshMaterialParameterCollections() const
    {
        if (!m_MaterialParametersNeedRefresh)
            return;
        MaterialParameterMap resolved;
        ResolveMaterialParameterCollections(m_MaterialCollectionIDs, resolved);
        // Load completion can also notify observers. Preserve the snapshot when definitions are unchanged.
        const bool changed =
            resolved.size() != m_ActiveMaterialParameters.size() ||
            !std::equal(
                resolved.begin(), resolved.end(), m_ActiveMaterialParameters.begin(), [](const auto& a, const auto& b) {
                    return a.first == b.first && a.second.Source == b.second.Source;
                });
        if (changed)
        {
            m_ActiveMaterialParameters = std::move(resolved);
            m_MaterialParameterSnapshot.reset();
        }
        m_MaterialParametersNeedRefresh = false;
    }

    Ref<const MaterialParameterCollectionAsset> Scene::GetMaterialParameterCollection(AssetID id) const
    {
        RefreshMaterialParameterCollections();
        const auto found = m_ActiveMaterialParameters.find(id);
        return found == m_ActiveMaterialParameters.end() ? nullptr : found->second.Source;
    }

    const MaterialParameterDefinition* Scene::FindMaterialParameter(std::string_view collection,
                                                                    std::string_view name) const
    {
        RefreshMaterialParameterCollections();
        return FindParameter(m_ActiveMaterialParameters, collection, name).second;
    }

    std::optional<glm::vec4> Scene::GetMaterialParameter(std::string_view collection, std::string_view name) const
    {
        RefreshMaterialParameterCollections();
        const auto [runtime, p] = FindParameter(m_ActiveMaterialParameters, collection, name);
        if (!p)
            return std::nullopt;
        const auto found = runtime->Overrides.find(MaterialParameterKey(collection, *p));
        return found == runtime->Overrides.end() ? p->Default : found->second;
    }

    bool Scene::SetMaterialParameter(std::string_view collection, std::string_view name, glm::vec4 value)
    {
        RefreshMaterialParameterCollections();
        const auto [runtime, p] = FindParameter(m_ActiveMaterialParameters, collection, name);
        if (!p || p->Type != MaterialParameterType::Vec4 || !IsFiniteMaterialParameter(value))
            return false;
        if (AssignParameter(*runtime, *p, value))
            m_MaterialParameterSnapshot.reset();
        return true;
    }

    bool Scene::SetMaterialParameter(std::string_view collection, std::string_view name, float value)
    {
        RefreshMaterialParameterCollections();
        const auto [runtime, p] = FindParameter(m_ActiveMaterialParameters, collection, name);
        if (!p || p->Type != MaterialParameterType::Scalar || !std::isfinite(value))
            return false;
        if (AssignParameter(*runtime, *p, glm::vec4(value, 0, 0, 0)))
            m_MaterialParameterSnapshot.reset();
        return true;
    }

    std::shared_ptr<const MaterialParameterUniforms> Scene::CaptureMaterialParameters() const
    {
        RefreshMaterialParameterCollections();
        if (m_MaterialParameterSnapshot || m_ActiveMaterialParameters.empty())
            return m_MaterialParameterSnapshot;
        size_t count = 0;
        for (const auto& [id, runtime] : m_ActiveMaterialParameters)
            count += runtime.Source->GetData().Parameters.size();
        if (count == 0)
            return {};
        std::vector<std::pair<uint64_t, glm::vec4>> values;
        values.reserve(count);
        for (const auto& [id, runtime] : m_ActiveMaterialParameters)
        {
            const auto& data = runtime.Source->GetData();
            for (const auto& p : data.Parameters)
            {
                const auto key   = MaterialParameterKey(data.Name, p);
                const auto found = runtime.Overrides.find(key);
                values.emplace_back(key, found == runtime.Overrides.end() ? p.Default : found->second);
            }
        }
        std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        auto packed = std::make_shared<MaterialParameterUniforms>();
        for (const auto& [key, value] : values)
        {
            const auto slot           = packed->Count.x++;
            auto&      words          = packed->Keys[slot / 2];
            words[(slot % 2) * 2]     = uint32_t(key);
            words[(slot % 2) * 2 + 1] = uint32_t(key >> 32);
            packed->Values[slot]      = value;
        }
        m_MaterialParameterSnapshot = std::move(packed);
        return m_MaterialParameterSnapshot;
    }
} // namespace Kans
