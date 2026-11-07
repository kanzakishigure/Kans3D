#include "KansEditorCore/UI/SourceBuild.h"
#include "KansEditorCore/EditorAssetImportService.h"

namespace Kans
{
    std::string BuildSavedSource(EditorAssetImportService* service, const std::filesystem::path& file)
    {
        if (!service)
            return "Saved.";
        const auto canonical = [](const std::filesystem::path& path) {
            std::error_code error;
            auto            result = std::filesystem::weakly_canonical(path, error);
            return error ? path : result;
        };
        const auto target = canonical(file);
        // A new file is not a source until the sources are scanned again.
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            auto sources = service->GetSources();
            if (std::holds_alternative<std::vector<SourceAssetMetadata>>(sources))
                for (const auto& source : std::get<std::vector<SourceAssetMetadata>>(sources))
                    if (source.Exists &&
                        canonical(service->GetConfig().AssetDirectory / source.Path.GetPath()) == target)
                    {
                        AssetBuildOptions options;
                        options.Target = EditorAssetTarget;
                        auto started   = service->Start(source.sourceAssetID, options);
                        return std::holds_alternative<AssetError>(started) ?
                                   "Saved; build not started: " + std::get<AssetError>(started).Message :
                                   "Saved; building...";
                    }
            (void)service->RefreshSources();
        }
        return "Saved, but the file is not a project asset.";
    }
} // namespace Kans
