#include "kspch.h"
#include "EditorAssetImportService.h"
#include <chrono>
#include "Kans3D/Asset/Product/ModelProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"

namespace Kans
{
    EditorAssetImportService::EditorAssetImportService(Scope<Project> project) :
        m_Project(std::move(project)), m_Config(m_Project->GetConfig()),
        m_ProductDirectory(m_Project->GetProductDirectory()), m_Owner(std::this_thread::get_id())
    {}

    std::variant<Scope<EditorAssetImportService>, AssetError> EditorAssetImportService::Create(Scope<Project> project)
    {
        if (!project)
            return AssetError {AssetErrorCode::InvalidArgument, "Import service requires a Project"};
        return Scope<EditorAssetImportService>(new EditorAssetImportService(std::move(project)));
    }

    EditorAssetImportService::~EditorAssetImportService()
    {
        if (m_Job)
            m_Job->Cancel.store(true);
        if (m_Future.valid())
            m_Future.wait();
    }

    std::optional<AssetError> EditorAssetImportService::CheckOwner() const
    {
        if (std::this_thread::get_id() != m_Owner)
            return AssetError {AssetErrorCode::InvalidArgument, "Import service must be used from its owner thread"};
        if (m_Closed)
            return AssetError {AssetErrorCode::Cancelled, "Import service is closed"};
        return {};
    }

    std::optional<AssetError> EditorAssetImportService::CheckIdle() const
    {
        if (auto error = CheckOwner())
            return error;
        if (m_Future.valid())
            return AssetError {AssetErrorCode::Cancelled, "Project is busy importing"};
        return {};
    }

    std::variant<std::monostate, AssetError> EditorAssetImportService::Start(SourceAssetID     source,
                                                                             AssetBuildOptions options)
    {
        if (auto error = CheckIdle())
            return *error;
        const auto metadata = m_Project->GetSourceAssetDatabase().FindByID(source);
        if (!metadata.IsValid() || !metadata.Exists)
            return AssetError {AssetErrorCode::NotFound, "Import source is absent or Missing"};
        return StartJob(source, std::move(options));
    }

    std::variant<std::monostate, AssetError> EditorAssetImportService::StartOutdated(AssetBuildOptions options)
    {
        if (auto error = CheckIdle())
            return *error;
        if (!IsProductTarget(options.Target))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid build target"};
        return StartJob({}, std::move(options));
    }

    std::variant<std::monostate, AssetError> EditorAssetImportService::StartJob(SourceAssetID     source,
                                                                                AssetBuildOptions options)
    {
        auto job         = CreateRef<Job>();
        auto observer    = std::move(options.Progress);
        options.Progress = [job, observer = std::move(observer)](PublicationPhase phase) {
            job->Phase.store(static_cast<int>(phase));
            const bool observed = !observer || observer(phase);
            return observed && !job->Cancel.load();
        };
        auto cancelObserver     = std::move(options.CancelRequested);
        options.CancelRequested = [job, cancelObserver = std::move(cancelObserver)] {
            return job->Cancel.load() || (cancelObserver && cancelObserver());
        };
        m_Snapshot        = {};
        m_Snapshot.State  = EditorAssetImportState::Building;
        m_Snapshot.Source = source;
        m_Snapshot.Target = options.Target;
        m_Job             = job;
        m_Future          = std::async(std::launch::async, [project = m_Project.get(), source, options]() {
            try
            {
                return source.IsValid() ? project->BuildAsset(source, options) : project->BuildPublishedAssets(options);
            }
            catch (const std::exception& error)
            {
                return std::variant<AssetBuildGraphResult, AssetError>(
                    AssetError {AssetErrorCode::IoError, error.what()});
            }
        });
        return std::monostate {};
    }

    std::variant<bool, AssetError> EditorAssetImportService::Poll()
    {
        if (auto error = CheckOwner())
            return *error;
        if (!m_Future.valid() || m_Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return false;
        auto result = m_Future.get();
        if (auto* error = std::get_if<AssetError>(&result))
        {
            m_Snapshot.State = error->Code == AssetErrorCode::Cancelled ? EditorAssetImportState::Cancelled :
                                                                          EditorAssetImportState::Failed;
            m_Snapshot.Error = std::move(*error);
        }
        else
        {
            m_Snapshot.State  = EditorAssetImportState::Succeeded;
            m_Snapshot.Result = std::get<AssetBuildGraphResult>(std::move(result));
        }
        return true;
    }

    std::variant<std::monostate, AssetError> EditorAssetImportService::Cancel()
    {
        if (auto error = CheckOwner())
            return *error;
        if (m_Future.valid())
            m_Job->Cancel.store(true);
        return std::monostate {};
    }

    std::variant<std::monostate, AssetError> EditorAssetImportService::Shutdown()
    {
        if (std::this_thread::get_id() != m_Owner)
            return AssetError {AssetErrorCode::InvalidArgument, "Import shutdown requires its owner thread"};
        if (m_Closed)
            return std::monostate {};
        if (m_Job)
            m_Job->Cancel.store(true);
        if (m_Future.valid())
        {
            m_Future.wait();
            (void)Poll();
        }
        m_Closed = true;
        return std::monostate {};
    }

    std::variant<EditorAssetImportSnapshot, AssetError> EditorAssetImportService::GetSnapshot() const
    {
        if (std::this_thread::get_id() != m_Owner)
            return AssetError {AssetErrorCode::InvalidArgument, "Import snapshot requires its owner thread"};
        auto snapshot = m_Snapshot;
        if (m_Job)
        {
            snapshot.CancelRequested = m_Job->Cancel.load();
            if (const int phase = m_Job->Phase.load(); phase >= 0)
                snapshot.Phase = static_cast<PublicationPhase>(phase);
        }
        return snapshot;
    }

    std::variant<ImportSettingsSchema, AssetError>
    EditorAssetImportService::GetSettingsSchema(const std::string& name) const
    {
        if (auto error = CheckOwner())
            return *error;
        auto registry = CreateDefaultImporterRegistry();
        if (auto* error = std::get_if<AssetError>(&registry))
            return *error;
        return std::get<ImporterRegistry>(registry).GetSettingsSchema(name);
    }

    std::variant<ImportSettingsSchema, AssetError>
    EditorAssetImportService::GetSettingsSchema(SourceAssetID source) const
    {
        if (auto error = CheckIdle())
            return *error;
        auto metadata = m_Project->GetSourceAssetDatabase().FindByID(source);
        if (!metadata.IsValid() || !metadata.Exists)
            return AssetError {AssetErrorCode::NotFound, "Inspection source is absent or Missing"};
        auto registry = CreateDefaultImporterRegistry();
        if (auto* error = std::get_if<AssetError>(&registry))
            return *error;
        return std::get<ImporterRegistry>(registry).GetSettingsSchemaForSource(metadata.Path.ToString(),
                                                                               metadata.Import.Importer);
    }

    std::variant<SourceInventory, AssetError> EditorAssetImportService::InspectSource(SourceAssetID source) const
    {
        if (auto error = CheckIdle())
            return *error;
        auto metadata = m_Project->GetSourceAssetDatabase().FindByID(source);
        if (!metadata.IsValid() || !metadata.Exists)
            return AssetError {AssetErrorCode::NotFound, "Inspection source is absent or Missing"};
        auto registry = CreateDefaultImporterRegistry();
        if (auto* error = std::get_if<AssetError>(&registry))
            return *error;
        auto resolved =
            std::get<ImporterRegistry>(registry).Resolve(metadata.Path.ToString(), metadata.Import.Importer);
        if (auto* error = std::get_if<AssetError>(&resolved))
            return *error;
        ImportContext context;
        context.SourceID   = source;
        context.AssetRoot  = m_Config.AssetDirectory;
        context.SourceFile = m_Config.AssetDirectory / std::filesystem::u8path(metadata.Path.ToString());
        context.Settings   = metadata.Import.Settings;
        context.Target     = EditorAssetTarget;
        auto inspected     = std::get<RegisteredImporter>(resolved).Importer->Inspect(context);
        if (auto* inventory = std::get_if<SourceInventory>(&inspected); inventory && !inventory->Materials.empty())
        {
            const auto lower = [](std::string text) {
                std::transform(
                    text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                return text;
            };
            // Read only published material products. Current remaps take priority;
            // ties use the registry's stable AssetID order, independent of scan order.
            // Own-source materials cannot be remap inputs without a dependency cycle.
            std::map<std::string, AssetID> materials;
            auto                           products = m_Project->GetRuntimeAssetRegistry()->GetAllProducts();
            std::sort(products.begin(), products.end(), [](const auto& a, const auto& b) {
                return a.Metadata.assetID < b.Metadata.assetID;
            });
            for (const auto& product : products)
                if (product.Target == EditorAssetTarget && product.Metadata.Type == AssetType::Material &&
                    product.Metadata.assetID.GetSourceID() != source)
                {
                    auto bytes = ReadRegisteredProduct(m_ProductDirectory, product);
                    if (auto* data = std::get_if<std::vector<std::byte>>(&bytes))
                    {
                        auto decoded = DecodeMaterialProduct(data->data(), data->size());
                        if (auto* material = std::get_if<MaterialData>(&decoded))
                            materials.emplace(lower(material->Name), product.Metadata.assetID);
                    }
                }
            for (auto& slot : inventory->Materials)
                if (!slot.SuggestedMaterial)
                    if (auto found = materials.find(lower(slot.SourceName)); found != materials.end())
                        slot.SuggestedMaterial = found->second;
        }
        return inspected;
    }

    std::variant<std::vector<SourceAssetMetadata>, AssetError> EditorAssetImportService::GetSources() const
    {
        if (auto error = CheckIdle())
            return *error;
        return m_Project->GetSourceAssetDatabase().GetAllSources();
    }

    std::variant<std::monostate, AssetError> EditorAssetImportService::RefreshSources()
    {
        if (auto error = CheckIdle())
            return *error;
        auto refreshed = m_Project->RefreshAssets();
        if (auto* error = std::get_if<AssetError>(&refreshed))
            return *error;
        m_RegistryChange = m_Project->GetRuntimeAssetRegistry();
        return std::monostate {};
    }

    std::variant<std::monostate, AssetError> EditorAssetImportService::MoveSourceAsset(SourceAssetID    source,
                                                                                       const AssetPath& destination)
    {
        if (auto error = CheckIdle())
            return *error;
        return m_Project->MoveSourceAsset(source, destination);
    }

    std::variant<SourceAssetDeletionPlan, AssetError>
    EditorAssetImportService::PlanSourceAssetDeletion(SourceAssetID source)
    {
        if (auto error = CheckIdle())
            return *error;
        return m_Project->PlanSourceAssetDeletion(source);
    }

    std::variant<std::monostate, AssetError> EditorAssetImportService::DeleteSourceAsset(SourceAssetID source)
    {
        if (auto error = CheckIdle())
            return *error;
        auto deleted = m_Project->DeleteSourceAsset(source);
        if (auto* error = std::get_if<AssetError>(&deleted))
            return std::move(*error);
        m_RegistryChange = std::get<Ref<const AssetRegistry>>(std::move(deleted));
        return std::monostate {};
    }

    std::variant<std::monostate, AssetError>
    EditorAssetImportService::SetSourceImportConfiguration(SourceAssetID                    source,
                                                           const SourceImportConfiguration& configuration)
    {
        if (auto error = CheckIdle())
            return *error;
        return m_Project->SetSourceImportConfiguration(source, configuration);
    }

    std::variant<std::vector<ModelMaterialSlot>, AssetError>
    EditorAssetImportService::GetModelMaterialSlots(SourceAssetID source) const
    {
        if (auto error = CheckIdle())
            return *error;
        return m_Project->GetModelMaterialSlots(source, EditorAssetTarget);
    }

    std::variant<std::monostate, AssetError>
    EditorAssetImportService::SetModelMaterialRemap(SourceAssetID          source,
                                                    const std::string&     slot,
                                                    std::optional<AssetID> material)
    {
        if (auto error = CheckIdle())
            return *error;
        return m_Project->SetModelMaterialRemap(source, slot, material, EditorAssetTarget);
    }

    std::variant<ModelMaterialExtractionResult, AssetError>
    EditorAssetImportService::ExtractModelMaterials(SourceAssetID                   source,
                                                    const std::filesystem::path&    directory,
                                                    const std::vector<std::string>& slots)
    {
        if (auto error = CheckIdle())
            return *error;
        auto result      = m_Project->ExtractModelMaterials(source, directory, slots, EditorAssetTarget);
        m_RegistryChange = m_Project->GetRuntimeAssetRegistry();
        return result;
    }

    std::variant<std::optional<Ref<const AssetRegistry>>, AssetError>
    EditorAssetImportService::ConsumeRuntimeRegistryChange()
    {
        if (auto error = CheckOwner())
            return *error;
        if (!m_RegistryChange)
            return std::optional<Ref<const AssetRegistry>> {};
        return std::optional<Ref<const AssetRegistry>>(std::exchange(m_RegistryChange, nullptr));
    }

    std::variant<std::vector<AssetError>, AssetError> EditorAssetImportService::GetProjectWarnings() const
    {
        if (auto error = CheckIdle())
            return *error;
        return m_Project->GetAssetWarnings();
    }
} // namespace Kans
