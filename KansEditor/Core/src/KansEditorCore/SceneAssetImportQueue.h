#pragma once
#include <deque>
#include <map>
#include <set>
#include "KansEditorCore/EditorAssetImportService.h"

namespace Kans
{
    class Scene;
    // Owner-thread policy shared by EditorLayer and headless project/scene clients.
    class SceneAssetImportQueue
    {
    public:
        std::variant<std::monostate, AssetError> Collect(EditorAssetImportService& service, const Scene& scene);
        // Matches the sources against the references the last Collect captured, without
        // reading the scene again: for a sweep's completion, which adds sources, not references.
        std::variant<std::monostate, AssetError> Rematch(EditorAssetImportService& service);
        std::variant<std::monostate, AssetError> StartNext(EditorAssetImportService& service);
        size_t                                   PendingCount() const { return m_AutoImports.size(); }
        bool                                     Empty() const { return m_AutoImports.empty(); }

    private:
        std::variant<std::monostate, AssetError> CollectSources(EditorAssetImportService& service);
        // Capture scene references at the scene-change event, even while a build
        // owns Project. Only source matching is deferred until the worker finishes.
        std::set<SourceAssetID> m_Referenced;
        bool                    m_CollectPending = false;
        struct Revision
        {
            uint64_t                        Size;
            std::filesystem::file_time_type Time;
            bool                            Exists;
        };
        std::map<SourceAssetID, Revision> m_Revisions;
        std::deque<SourceAssetID>         m_AutoImports;
        std::set<SourceAssetID>           m_AutoImported;
    };
} // namespace Kans
