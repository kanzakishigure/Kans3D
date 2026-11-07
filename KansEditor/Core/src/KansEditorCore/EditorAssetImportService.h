#pragma once
#include <atomic>
#include <future>
#include <optional>
#include <thread>
#include "Kans3D/Project/Project.h"

namespace Kans
{
    // Build target the editor imports into and loads from.
    inline constexpr const char* EditorAssetTarget = "vulkan12";

    enum class EditorAssetImportState
    {
        Idle,
        Building,
        Succeeded,
        Failed,
        Cancelled
    };

    struct EditorAssetImportSnapshot
    {
        EditorAssetImportState               State = EditorAssetImportState::Idle;
        SourceAssetID                        Source;
        std::string                          Target;
        bool                                 CancelRequested = false;
        std::optional<PublicationPhase>      Phase;
        std::optional<AssetBuildGraphResult> Result;
        std::optional<AssetError>            Error;
    };

    // Runs one asset build at a time on a worker thread for the editor. All
    // methods must be called from the thread that created the service; project
    // operations other than polling are rejected while a build runs.
    class EditorAssetImportService
    {
    public:
        [[nodiscard]] static std::variant<Scope<EditorAssetImportService>, AssetError> Create(Scope<Project> project);
        ~EditorAssetImportService();
        EditorAssetImportService(const EditorAssetImportService&)            = delete;
        EditorAssetImportService& operator=(const EditorAssetImportService&) = delete;

        [[nodiscard]] std::variant<std::monostate, AssetError> Start(SourceAssetID source, AssetBuildOptions options);
        // Check all published sources for a target in one background session.
        // Source in the snapshot is invalid; Result.Root summarizes the session,
        // including its final Registry and per-source failure warnings.
        [[nodiscard]] std::variant<std::monostate, AssetError> StartOutdated(AssetBuildOptions options);
        // True once when a started build finishes (successfully or not).
        [[nodiscard]] std::variant<bool, AssetError>                      Poll();
        [[nodiscard]] std::variant<std::monostate, AssetError>            Cancel();
        [[nodiscard]] std::variant<std::monostate, AssetError>            Shutdown();
        [[nodiscard]] std::variant<EditorAssetImportSnapshot, AssetError> GetSnapshot() const;

        // Owner thread, idle only. While a worker owns Project these return
        // Cancelled ("Project is busy importing") before reading project state.
        // Unavailable from Start until Poll consumes completion, including when
        // the worker has finished but has not been polled. The importer-name
        // schema overload also works while a build runs.
        [[nodiscard]] std::variant<SourceInventory, AssetError>      InspectSource(SourceAssetID source) const;
        [[nodiscard]] std::variant<ImportSettingsSchema, AssetError> GetSettingsSchema(SourceAssetID source) const;
        [[nodiscard]] std::variant<ImportSettingsSchema, AssetError>
        GetSettingsSchema(const std::string& importerName) const;
        [[nodiscard]] std::variant<std::vector<SourceAssetMetadata>, AssetError> GetSources() const;
        [[nodiscard]] std::variant<std::monostate, AssetError>                   RefreshSources();
        [[nodiscard]] std::variant<std::monostate, AssetError>          MoveSourceAsset(SourceAssetID    source,
                                                                                        const AssetPath& destination);
        [[nodiscard]] std::variant<SourceAssetDeletionPlan, AssetError> PlanSourceAssetDeletion(SourceAssetID source);
        [[nodiscard]] std::variant<std::monostate, AssetError>          DeleteSourceAsset(SourceAssetID source);
        [[nodiscard]] std::variant<std::monostate, AssetError>
        SetSourceImportConfiguration(SourceAssetID source, const SourceImportConfiguration& configuration);
        // Owner thread, idle only; uses EditorAssetTarget. Mutations persist without
        // starting a build: call Start(modelSource, options) afterwards.
        [[nodiscard]] std::variant<std::vector<ModelMaterialSlot>, AssetError>
        GetModelMaterialSlots(SourceAssetID source) const;
        [[nodiscard]] std::variant<std::monostate, AssetError>
        SetModelMaterialRemap(SourceAssetID source, const std::string& slot, std::optional<AssetID> material);
        [[nodiscard]] std::variant<ModelMaterialExtractionResult, AssetError>
        ExtractModelMaterials(SourceAssetID                   source,
                              const std::filesystem::path&    directory,
                              const std::vector<std::string>& slots = {});
        // The published Registry after a source deletion or refresh, consumed once.
        [[nodiscard]] std::variant<std::optional<Ref<const AssetRegistry>>, AssetError> ConsumeRuntimeRegistryChange();
        // Safe while a build runs.
        Ref<const AssetRegistry> GetRuntimeAssetRegistry() const { return m_Project->GetRuntimeAssetRegistry(); }
        [[nodiscard]] std::variant<std::vector<AssetError>, AssetError> GetProjectWarnings() const;

        const ProjectConfig&         GetConfig() const { return m_Config; }
        const std::filesystem::path& GetProductDirectory() const { return m_ProductDirectory; }

    private:
        explicit EditorAssetImportService(Scope<Project> project);
        std::optional<AssetError> CheckOwner() const;
        std::optional<AssetError> CheckIdle() const;
        std::variant<std::monostate, AssetError> StartJob(SourceAssetID source, AssetBuildOptions options);
        struct Job
        {
            std::atomic<bool> Cancel {false};
            std::atomic<int>  Phase {-1};
        };
        Scope<Project>                                               m_Project;
        ProjectConfig                                                m_Config;
        std::filesystem::path                                        m_ProductDirectory;
        std::thread::id                                              m_Owner;
        bool                                                         m_Closed = false;
        Ref<const AssetRegistry>                                     m_RegistryChange;
        Ref<Job>                                                     m_Job;
        std::future<std::variant<AssetBuildGraphResult, AssetError>> m_Future;
        EditorAssetImportSnapshot                                    m_Snapshot;
    };
} // namespace Kans
