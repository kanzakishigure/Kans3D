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
        std::variant<std::monostate, AssetError> StartNext(EditorAssetImportService& service);
        bool                                     Empty() const { return m_AutoImports.empty(); }

    private:
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
