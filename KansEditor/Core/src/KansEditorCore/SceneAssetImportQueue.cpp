#include "kspch.h"
#include "SceneAssetImportQueue.h"
#include "Kans3D/Scene/SceneAssetReferences.h"

namespace Kans
{
    std::variant<std::monostate, AssetError> SceneAssetImportQueue::Collect(EditorAssetImportService& service,
                                                                            const Scene&              scene)
    {
        auto sources = service.GetSources();
        // The initial sweep owns Project until Poll consumes completion. The caller
        // repeats collection then, using the newly scanned source database.
        if (auto error = std::get_if<AssetError>(&sources))
        {
            if (error->Code == AssetErrorCode::Cancelled)
                return std::monostate {};
            return *error;
        }
        try
        {
            std::set<SourceAssetID> referenced, published;
            for (auto id : CollectSceneAssetReferences(scene))
                referenced.insert(id.GetSourceID());
            for (const auto& record : service.GetRuntimeAssetRegistry()->GetAllProducts())
                if (record.Target == EditorAssetTarget)
                    published.insert(record.Metadata.assetID.GetSourceID());
            for (const auto& source : std::get<std::vector<SourceAssetMetadata>>(sources))
            {
                Revision revision {source.FileSize, source.last_edit_time, source.Exists};
                auto [old, inserted] = m_Revisions.emplace(source.sourceAssetID, revision);
                if (!inserted && (old->second.Size != revision.Size || old->second.Time != revision.Time ||
                                  old->second.Exists != revision.Exists))
                {
                    m_AutoImported.erase(source.sourceAssetID);
                    old->second = revision;
                }
                if (source.Exists && !published.count(source.sourceAssetID) &&
                    (referenced.count(source.sourceAssetID) || source.Path.GetPath().extension() == ".shaderprog") &&
                    m_AutoImported.insert(source.sourceAssetID).second)
                    if (std::find(m_AutoImports.begin(), m_AutoImports.end(), source.sourceAssetID) ==
                        m_AutoImports.end())
                        m_AutoImports.push_back(source.sourceAssetID);
            }
            return std::monostate {};
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData, error.what()};
        }
    }

    std::variant<std::monostate, AssetError> SceneAssetImportQueue::StartNext(EditorAssetImportService& service)
    {
        if (m_AutoImports.empty())
            return std::monostate {};
        auto snapshot = service.GetSnapshot();
        if (auto error = std::get_if<AssetError>(&snapshot))
            return *error;
        if (std::get<EditorAssetImportSnapshot>(snapshot).State == EditorAssetImportState::Building)
            return std::monostate {};
        // A prior root may have published a queued source as a dependency.
        std::set<SourceAssetID> published;
        for (const auto& record : service.GetRuntimeAssetRegistry()->GetAllProducts())
            if (record.Target == EditorAssetTarget)
                published.insert(record.Metadata.assetID.GetSourceID());
        while (!m_AutoImports.empty())
        {
            const auto source = m_AutoImports.front();
            m_AutoImports.pop_front();
            if (published.count(source))
                continue;
            AssetBuildOptions options;
            options.Target = EditorAssetTarget;
            return service.Start(source, options);
        }
        return std::monostate {};
    }
} // namespace Kans
