#pragma once

#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Asset/Importer/AssetBuildService.h"
#include "Kans3D/Asset/SourceAssetDatabase.h"
#include "Kans3D/Asset/Storage/AssetProvider.h"
#include "Kans3D/Physics/PhysicsSettings.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace Kans
{

    // 相对目录以 RootDirectory 为基准。
    class ProjectConfig
    {
    public:
        PhysicsSettings       Physics;
        std::string           Name;
        std::filesystem::path RootDirectory;
        std::filesystem::path AssetDirectory = "assets";
        std::filesystem::path CacheDirectory = "Cache";
    };

    class ProjectOpenOptions
    {
    public:
        // 留空时使用 CacheDirectory/SourceAssetDatabase.cache.yaml，否则须为绝对路径。
        std::filesystem::path SourceAssetCachePath;
    };

    struct AssetBuildGraphResult
    {
        // A successful root is retained when dependent builds fail. Warnings
        // report each failed/skipped dependent by source path and reason.
        AssetBuildResult Root;
        // Sources built before the root because it reads or references them.
        std::vector<AssetBuildResult> Dependencies;
        // Successful additional sources built after the root with RebuildDependents.
        std::vector<AssetBuildResult> Dependents;
        // BuildPublishedAssets: successful checks, once per source, dependencies
        // first. Root is the batch summary (invalid Source), with final Registry
        // and warnings for failed sources. Its Disposition says whether anything
        // published, not whether every source succeeded. Single-source builds leave this empty.
        std::vector<AssetBuildResult> CheckedSources;
    };

    struct SourceAssetDeletionPlan
    {
        SourceAssetMetadata Source;
        // Sources whose Products reference the deleted source; they keep working
        // but lose that reference until rebuilt.
        std::vector<SourceAssetMetadata> Dependents;
    };

    struct ModelMaterialSlot
    {
        std::string            Key, DisplayName;
        AssetID                EmbeddedMaterial;
        std::optional<AssetID> Remap;
    };

    struct ExtractedModelMaterial
    {
        std::string           SlotKey;
        std::filesystem::path File;
        SourceAssetID         Source;
        AssetID               Material;
    };

    struct ModelMaterialExtractionResult
    {
        // Includes every file written, even if registration or mapping later failed.
        std::vector<ExtractedModelMaterial> Created;
        std::vector<std::string>            Skipped;
        bool                                RemapsApplied = false;
        std::optional<AssetError>           Error;
    };

    // Owns a project's source database and Product store. Operations on one
    // project must be called from one thread at a time; the project directory is
    // locked against other processes while it is open.
    class Project
    {
    public:
        Project(const Project&)            = delete;
        Project& operator=(const Project&) = delete;
        ~Project();

        // 校验目录并扫描源资产；缓存读写失败与单个文件的错误记录为警告。
        [[nodiscard]] static std::variant<std::unique_ptr<Project>, AssetError>
        Open(const ProjectConfig& config, const ProjectOpenOptions& options = {});

        // 重新扫描并按 ID 合并；消失的源保留为 Missing，相同 ID 恢复。
        // 路径身份冲突时保留现有数据库及缓存。
        [[nodiscard]] std::variant<std::monostate, AssetError> RefreshAssets();
        [[nodiscard]] std::variant<std::monostate, AssetError> SaveAssetCache() const;

        // Moves a source and its sidecar into an existing directory inside the
        // asset root. Identity and published Products are unchanged.
        [[nodiscard]] std::variant<std::monostate, AssetError>          MoveSourceAsset(SourceAssetID    id,
                                                                                        const AssetPath& destination);
        [[nodiscard]] std::variant<SourceAssetDeletionPlan, AssetError> PlanSourceAssetDeletion(SourceAssetID id) const;
        // Unpublishes the source's Products, then deletes the source and its sidecar.
        [[nodiscard]] std::variant<Ref<const AssetRegistry>, AssetError> DeleteSourceAsset(SourceAssetID id);

        // Writes the sidecar, then updates the database.
        [[nodiscard]] std::variant<std::monostate, AssetError>
        SetSourceImportConfiguration(SourceAssetID id, const SourceImportConfiguration& configuration);

        // Slots from the last published import for target; does not parse current source content.
        [[nodiscard]] std::variant<std::vector<ModelMaterialSlot>, AssetError>
        GetModelMaterialSlots(SourceAssetID source, const std::string& target = "default") const;
        // Persists settings only. Caller builds the model afterwards.
        [[nodiscard]] std::variant<std::monostate, AssetError>
        SetModelMaterialRemap(SourceAssetID          source,
                              const std::string&     slot,
                              std::optional<AssetID> material,
                              const std::string&     target = "default");
        // Relative directories are relative to the asset root. Empty slots selects all.
        // All remaps commit in one sidecar write after every source is registered.
        // Later failures return a report with Error and Created, without changing remaps.
        [[nodiscard]] std::variant<ModelMaterialExtractionResult, AssetError>
        ExtractModelMaterials(SourceAssetID                   source,
                              const std::filesystem::path&    directory,
                              const std::vector<std::string>& slots  = {},
                              const std::string&              target = "default");

        // Builds the source after building every source it reads or references.
        // RebuildDependents also updates its published dependents for the target.
        [[nodiscard]] std::variant<AssetBuildGraphResult, AssetError> BuildAsset(SourceAssetID            id,
                                                                                 const AssetBuildOptions& options);
        // Uses a custom importer set instead of the default importers.
        [[nodiscard]] std::variant<AssetBuildGraphResult, AssetError>
        BuildAsset(SourceAssetID id, const AssetBuildOptions& options, const ImporterRegistry& importers);

        // One session over sources with published Products for options.Target.
        // Fingerprints skip current sources; a failed source warns and does not
        // stop other roots. Cancellation stops the session, retaining publications.
        [[nodiscard]] std::variant<AssetBuildGraphResult, AssetError>
        BuildPublishedAssets(const AssetBuildOptions& options);

        Ref<const AssetRegistry>     GetRuntimeAssetRegistry() const;
        // Project must outlive the provider. Do not build concurrently through another coordinator.
        // Runtime cache repair uses the same BuildAsset path as ordinary project imports.
        [[nodiscard]] Ref<IAssetProvider> CreateAssetProvider();
        std::filesystem::path        GetProductDirectory() const { return m_Config.CacheDirectory / "Products"; }
        const ProductStore&          GetProductStore() const { return *m_ProductStore; }
        const ProjectConfig&         GetConfig() const { return m_Config; } // 目录已解析为绝对路径。
        const std::filesystem::path& GetSourceAssetCachePath() const { return m_SourceAssetCachePath; }
        const SourceAssetDatabase&   GetSourceAssetDatabase() const { return m_SourceAssetDatabase; }
        // 最近一次打开、刷新或修改过程中产生的警告。
        const std::vector<AssetError>& GetAssetWarnings() const { return m_AssetWarnings; }

    private:
        struct BuildSession;

        Project() = default;
        std::variant<std::monostate, AssetError>   ScanAssets();
        std::variant<std::monostate, AssetError>   SaveSourceCache() const;
        std::variant<std::monostate, AssetError>   RecoverInterruptedMove();
        std::variant<AssetBuildResult, AssetError> BuildSource(SourceAssetID id, BuildSession& session);
        std::variant<AssetBuildResult, AssetError> BuildSourceOnce(SourceAssetID id, BuildSession& session);
        std::variant<SourceAssetID, AssetError>    FindSourceByFile(const std::filesystem::path& file) const;
        std::variant<std::monostate, AssetError>   SaveSidecar(const SourceAssetMetadata& metadata);
        std::filesystem::path                      SourceFile(const SourceAssetMetadata& metadata) const;

        ProjectConfig           m_Config;
        std::filesystem::path   m_SourceAssetCachePath;
        SourceAssetDatabase     m_SourceAssetDatabase;
        std::vector<AssetError> m_AssetWarnings;
        Scope<ProductStoreLock> m_Lock;
        Scope<ProductStore>     m_ProductStore;
        FileDigestCache         m_Digests;
    };

} // namespace Kans
