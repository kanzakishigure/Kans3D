#include <imgui.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include "Kans3D/Asset/Importer/MaterialSource.h"
#include "Kans3D/Asset/Storage/AssetProvider.h"
#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Project/ProjectFile.h"
#include "Kans3D/Project/RecentProjects.h"
#include "Kans3D/Scene/SceneSerializer.h"
#include "Kans3D/Timeline/TimelineProduct.h"
#include "Components/ComponentEditors.h"
#include "Components/MaterialAssignment.h"
#include "EditorLayer.h"
#include "Launcher/LauncherModel.h"
#include "Physics/PhysicsEditing.h"

namespace Kans
{
    namespace
    {
        // "<stem><ext>", or "<stem>2<ext>", ... when taken.
        std::filesystem::path
        UniqueFile(const std::filesystem::path& directory, const std::string& stem, const char* extension)
        {
            auto file = directory / (stem + extension);
            for (int i = 2; std::filesystem::exists(file); ++i)
                file = directory / (stem + std::to_string(i) + extension);
            return file;
        }
    } // namespace

    void EditorLayer::OpenProject()
    {
        auto loaded = LoadProjectFile(m_ProjectFile);
        if (const auto* error = std::get_if<AssetError>(&loaded))
        {
            CORE_ERROR("Cannot load project: {}", error->Message);
            std::exit(EXIT_FAILURE);
        }
        const auto& file = std::get<ProjectFile>(loaded);
        const auto config = MakeProjectConfig(m_ProjectFile.parent_path(), file);
        m_StartupScene = file.StartupScene;
        std::error_code ec;
        std::filesystem::current_path(config.RootDirectory, ec);
        if (ec)
        {
            CORE_ERROR("Cannot enter project '{}': {}", config.RootDirectory.string(), ec.message());
            std::exit(EXIT_FAILURE);
        }
        KansFileSystem::SetProject(config);
        m_ProjectName = file.Name;
        m_LayoutFile  = Launcher::LayoutFile(config.RootDirectory, config.CacheDirectory);
        Application::Get().GetWindow().SetTitle(file.Name + " - Kans3D Editor");
        RecentProjects recent(RecentProjects::DefaultStorageFile());
        if (auto error = recent.Load())
            CORE_WARN("Recent projects: {}", error->Message);
        recent.Touch(m_ProjectFile, file.Name, static_cast<int64_t>(std::time(nullptr)));
        if (auto error = recent.Save())
            CORE_WARN("Cannot save recent projects: {}", error->Message);
        auto opened = Project::Open(config);
        if (const auto* error = std::get_if<AssetError>(&opened))
        {
            CORE_ERROR("Cannot open project: {}", error->Message);
            std::exit(EXIT_FAILURE);
        }
        auto project = std::move(std::get<std::unique_ptr<Project>>(opened));
        for (const auto& warning : project->GetAssetWarnings())
            CORE_WARN("Project asset cache: {}", warning.Message);
        auto controller = EditorAssetImportService::Create(std::move(project));
        if (const auto* error = std::get_if<AssetError>(&controller))
        {
            CORE_ERROR("Cannot create project import controller: {}", error->Message);
            return;
        }
        m_AssetImports = std::get<Scope<EditorAssetImportService>>(std::move(controller));
        m_ProductImportPanel.Bind(m_AssetImports.get());
        m_MaterialEditorPanel.Bind(m_AssetImports.get());
        m_TimelinePanel.Bind(m_AssetImports.get());
        m_PhysicsMaterialPanel.Bind(m_AssetImports.get());
        m_ProjectSettingsPanel.Bind(m_ProjectFile);
        m_UIDesignerPanel.Bind(m_AssetImports.get());
        ComponentEditors::SetMotionPresetDirectories(SecondaryMotionEditing::BuiltInPresetDirectory(),
                                                     SecondaryMotionEditing::ProjectPresetDirectory(config.RootDirectory));
        m_ContentBrowserPanel.SetOpenFileCallback([this](const std::filesystem::path& path) {
            if (path.extension() == ".kans")
                ConfirmDiscard("opening " + path.filename().u8string(), [this, path] { Record(OpenScene(path)); });
            else if (path.extension() == ".material")
                OpenMaterial(path);
            else if (path.extension() == ".physmat")
                OpenPhysicsMaterial(path);
            else if (path.extension() == ".vfxmodule")
                OpenVfxModule(path);
            else if (path.extension() == ".cloth")
                OpenClothFile(path);
            else if (path.extension() == ".ragdoll")
                OpenRagdollFile(path, m_HierarchyPanel.GetSelected());
            else if (path.extension() == ".kui")
            {
                if (m_UIDesignerPanel.Open(path))
                    m_Panels.Open(PanelID::UIDesigner);
            }
            else
                OpenImporter(path);
        });
        m_ContentBrowserPanel.SetCreateMaterialCallback(
            [this](const std::filesystem::path& directory) { CreateMaterial(directory); });
        m_ContentBrowserPanel.SetCreateUICallback([this](const std::filesystem::path& directory) {
            // A new screen at 1920 x 1080, scaling with the screen, opened in the designer.
            UIData data;
            data.ScaleMode  = UIScaleMode::ScaleWithScreenSize;
            const auto file = UniqueFile(directory, "NewUI", ".kui");
            if (auto text = SerializeUISource(data); std::holds_alternative<std::string>(text))
            {
                std::ofstream(file, std::ios::binary) << std::get<std::string>(text);
                if (m_UIDesignerPanel.Open(file))
                    m_UIDesignerPanel.Save(), m_Panels.Open(PanelID::UIDesigner);
            }
        });
        m_ContentBrowserPanel.SetCreateTimelineCallback([this](const std::filesystem::path& directory) {
            TimelineData data;
            data.Length = 5;
            YAML::Emitter out;
            out << EncodeTimelineNode(data);
            std::ofstream(UniqueFile(directory, "NewTimeline", ".timeline"), std::ios::binary) << out.c_str() << '\n';
        });
        m_ContentBrowserPanel.SetCreatePhysicsMaterialCallback([this](const std::filesystem::path& directory) {
            auto created = PhysicsEditing::CreatePhysicsMaterialFile(directory);
            if (const auto* error = std::get_if<AssetError>(&created))
                return Record(*error);
            // Import it (so colliders can pick it) and open it for editing.
            OpenPhysicsMaterial(std::get<std::filesystem::path>(created));
            m_PhysicsMaterialPanel.Save();
        });
        m_ContentBrowserPanel.SetCreateVfxModuleCallback(
            [this](const std::filesystem::path& directory) { m_VfxModuleCreate.Open(directory); });
        m_ContentBrowserPanel.SetImportCallback([this](const std::filesystem::path& path) { ImportSource(path); });
        m_ContentBrowserPanel.SetImportSettingsCallback(
            [this](const std::filesystem::path& path) { OpenImporter(path); });
        m_ContentBrowserPanel.SetExtractMaterialsCallback(
            [this](const std::filesystem::path& path) { ExtractMaterials(path); });
        m_ContentBrowserPanel.SetCreateInstanceCallback(
            [this](const std::filesystem::path& path) { CreateMaterialInstance(path); });
        m_PropertiesPanel.SetMaterialActions(
            {[this](const std::filesystem::path& file) { OpenMaterial(file); },
             [this](const std::filesystem::path& file) { m_ContentBrowserPanel.Reveal(file); },
             [this](AssetID parent, const std::filesystem::path& source, const SlotParts& parts) {
                 CreateSlotInstance(parent, source, parts);
             }});
        m_ContentBrowserPanel.SetErrorCallback(
            [this](const std::string& message) { Record(AssetError {AssetErrorCode::InvalidArgument, message}); });
        m_ContentBrowserPanel.Bind(m_AssetImports.get());
        m_ProductImportPanel.SetPlaceCallback([this](const std::filesystem::path& file) { PlaceAsset(file); });
        m_ProductImportPanel.SetOpenMaterialCallback([this](const std::filesystem::path& file) { OpenMaterial(file); });
        m_HierarchyPanel.SetFileDropCallback(
            [this](const std::filesystem::path& file, Entity parent) { PlaceAsset(file, parent); });
        CORE_INFO("Opened project '{}'", m_AssetImports->GetConfig().Name);
        // Re-import products built by older importers (up-to-date sources are only
        // checked). Models appear once the sweep's registry is applied in Poll.
        AssetBuildOptions options;
        options.Target = EditorAssetTarget;
        Record(m_AssetImports->StartOutdated(options));
    }

    std::variant<std::monostate, AssetError> EditorLayer::RefreshProductAssets(Ref<const AssetRegistry> snapshot)
    {
        if (!m_AssetImports || !m_Viewport)
            return AssetError {AssetErrorCode::NotFound, "Project/scene renderer is unavailable"};
        if (!snapshot)
            snapshot = m_AssetImports->GetRuntimeAssetRegistry();
        auto choices = CollectAssetChoices(*snapshot);
        m_MaterialEditorPanel.SetAssetChoices(choices);
        m_AnimatorGraphPanel.SetAssetChoices(choices);
        m_TimelinePanel.SetAssetChoices(choices);
        m_UIDesignerPanel.SetAssetChoices(choices);
        m_RagdollEditor.SetAssetChoices(choices);
        m_PropertiesPanel.SetAssetChoices(std::move(choices));
        if (m_Assets)
        {
            auto changed = m_Assets->SetRegistry(std::move(snapshot));
            if (const auto* error = std::get_if<AssetError>(&changed))
                return *error;
            return std::monostate {};
        }
        auto provider = LooseFileAssetProvider::Open(m_AssetImports->GetProductDirectory());
        if (const auto* error = std::get_if<AssetError>(&provider))
            return *error;
        AssetManagerSpecification spec;
        spec.Target = EditorAssetTarget;
        m_Assets    = CreateScope<AssetManager>(
            std::get<Ref<LooseFileAssetProvider>>(std::move(provider)), std::move(snapshot), std::move(spec));
        m_Viewport->SetAssetManager(m_Assets.get());
        m_MaterialPreview->SetResources(m_Viewport->GetAssetCache(), m_Viewport->GetEnvironment());
        m_ThumbnailRenderer->SetResources(m_Viewport->GetAssetCache(), m_Viewport->GetEnvironment());
        m_PropertiesPanel.SetAssetManager(m_Assets.get());
        m_MaterialEditorPanel.SetAssetManager(m_Assets.get());
        m_TimelinePanel.SetAssetManager(m_Assets.get());
        m_MaterialEditorPanel.SetUsageQuery([this](AssetID material) {
            return m_ActiveScene ? FindMaterialUses(*m_ActiveScene, material, m_Assets.get()) :
                                   std::vector<MaterialUse> {};
        });
        m_MaterialEditorPanel.SetSelectCallback([this](UUID entity) {
            if (m_ActiveScene)
                m_HierarchyPanel.SetSelected(m_ActiveScene->GetEntityByUUID(entity));
        });
        return std::monostate {};
    }

    std::vector<EditorAssetChoice> EditorLayer::CollectAssetChoices(const AssetRegistry& registry) const
    {
        std::unordered_map<SourceAssetID, std::filesystem::path> sources;
        bool                                                     listed = false;
        if (auto  all  = m_AssetImports->GetSources();
            auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&all))
        {
            listed = true;
            for (const auto& source : *list)
                if (source.Exists)
                    sources.emplace(source.sourceAssetID, source.Path.GetPath());
        }
        const auto& assetRoot = m_AssetImports->GetConfig().AssetDirectory;

        std::vector<EditorAssetChoice> choices;
        for (const auto& record : registry.GetAllProducts())
        {
            if (record.Target != EditorAssetTarget)
                continue;
            const AssetID     id = record.Metadata.assetID;
            EditorAssetChoice choice;
            choice.ID        = id;
            choice.Type      = record.Metadata.Type;
            const auto found = sources.find(id.GetSourceID());
            if (found != sources.end())
            {
                std::error_code error;
                choice.SourcePath = std::filesystem::weakly_canonical(assetRoot / found->second, error);
                choice.Name       = found->second.generic_u8string();
            }
            else if (listed)
                continue; // Products left behind by a deleted or moved source are not offered.
            else
                choice.Name = id.GetSourceID().ToString();
            // Products other than a source's main Model are distinguished by LocalID.
            if (choice.Type != AssetType::Model)
                choice.Name += " #" + std::to_string(id.GetLocalID());
            choices.push_back(std::move(choice));
        }
        std::sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return a.Name < b.Name; });
        return choices;
    }

    AssetID EditorLayer::FindPublishedModel(SourceAssetID source) const
    {
        for (const auto& record : m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts())
            if (record.Metadata.assetID.GetSourceID() == source && record.Metadata.Type == AssetType::Model &&
                record.Target == EditorAssetTarget)
                return record.Metadata.assetID;
        return {};
    }

    std::optional<SourceAssetID> EditorLayer::FindModelSource(const std::filesystem::path& file)
    {
        if (!m_AssetImports)
            return std::nullopt;
        auto sources = m_AssetImports->GetSources();
        if (const auto* error = std::get_if<AssetError>(&sources))
        {
            Record(*error);
            return std::nullopt;
        }
        std::error_code error;
        const auto      dropped = std::filesystem::weakly_canonical(file, error);
        const auto&     root    = m_AssetImports->GetConfig().AssetDirectory;
        for (const auto& source : std::get<std::vector<SourceAssetMetadata>>(sources))
        {
            if (!source.Exists || std::filesystem::weakly_canonical(root / source.Path.GetPath(), error) != dropped)
                continue;
            const auto importer = CreateDefaultImporterRegistry();
            const auto selected = std::get<ImporterRegistry>(importer).Resolve(file, source.Import.Importer);
            if (std::holds_alternative<AssetError>(selected) ||
                std::get<RegisteredImporter>(selected).Name != "ModelImporter")
                return std::nullopt;
            return source.sourceAssetID;
        }
        return std::nullopt;
    }

    void EditorLayer::PlaceAsset(const std::filesystem::path& file, Entity parent, std::optional<glm::vec3> position)
    {
        if (!m_AssetImports || !m_ActiveScene)
            return;
        const auto source = FindModelSource(file);
        if (!source)
        {
            OpenImporter(file); // Not a model: show it in the import panel instead.
            return;
        }
        // Always build: it is quick when up to date and picks up edited sources.
        m_PendingPlacement         = *source;
        m_PendingPlacementParent   = parent ? parent.GetUUID() : UUID(0);
        m_PendingPlacementPosition = position;
        m_PendingPlacementWaiting  = false;
        auto snapshot              = m_AssetImports->GetSnapshot();
        if (const auto* state = std::get_if<EditorAssetImportSnapshot>(&snapshot);
            state && state->State == EditorAssetImportState::Building)
        {
            m_PendingPlacementWaiting = true; // e.g. the project-open re-import sweep
            return;
        }
        StartPendingPlacementBuild();
    }

    void EditorLayer::StartPendingPlacementBuild()
    {
        m_PendingPlacementWaiting = false;
        AssetBuildOptions options;
        options.Target = EditorAssetTarget;
        auto started   = m_AssetImports->Start(*m_PendingPlacement, options);
        if (const auto* failure = std::get_if<AssetError>(&started))
        {
            Record(*failure);
            m_PendingPlacement.reset();
        }
    }

    void EditorLayer::ExtractMaterials(const std::filesystem::path& file)
    {
        const auto source = FindModelSource(file);
        if (!source)
        {
            Record(AssetError {AssetErrorCode::InvalidArgument, "Not a model source: " + file.filename().u8string()});
            return;
        }
        std::error_code error;
        const auto      folder =
            std::filesystem::weakly_canonical(file, error).parent_path().lexically_relative(
                std::filesystem::weakly_canonical(m_AssetImports->GetConfig().AssetDirectory, error)) /
            "Materials";
        auto result = m_AssetImports->ExtractModelMaterials(*source, folder);
        if (const auto* failure = std::get_if<AssetError>(&result))
        {
            Record(*failure);
            return;
        }
        const auto& extracted = std::get<ModelMaterialExtractionResult>(result);
        for (const auto& created : extracted.Created)
            CLIENT_INFO_TAG("Editor", "Extracted material {}", created.File.u8string());
        if (extracted.Error)
            Record(*extracted.Error);
        else if (extracted.Created.empty())
            Record(AssetError {AssetErrorCode::InvalidArgument, "Every material of this model already uses a project material"});
        if (extracted.RemapsApplied && !extracted.Created.empty())
        {
            AssetBuildOptions options;
            options.Target = EditorAssetTarget;
            if (auto started = m_AssetImports->Start(*source, options); std::holds_alternative<AssetError>(started))
                Record(std::get<AssetError>(started));
        }
        m_ContentBrowserPanel.Refresh();
        OpenImporter(file); // Shows the slots and the build result.
    }

    Ref<Texture2D> EditorLayer::MaterialThumbnail(const std::filesystem::path& file, uint32_t size)
    {
        if (!m_AssetImports || !m_Assets || !m_ThumbnailRenderer)
            return nullptr;
        std::error_code error;
        const auto      key   = std::filesystem::weakly_canonical(file, error).u8string();
        auto&           entry = m_MaterialThumbnails[key];
        if (!entry.Material.IsValid())
        {
            if (m_ThumbnailBudget <= 0)
                return entry.Texture;
            --m_ThumbnailBudget; // Finding the source walks the source list.
            auto sources = m_AssetImports->GetSources();
            if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
                for (const auto& source : *list)
                    if (std::filesystem::weakly_canonical(
                            m_AssetImports->GetConfig().AssetDirectory / source.Path.GetPath(), error)
                            .u8string() == key)
                        for (const auto& product : m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts())
                            if (product.Metadata.assetID.GetSourceID() == source.sourceAssetID &&
                                product.Metadata.Type == AssetType::Material && product.Target == EditorAssetTarget)
                                entry.Material = product.Metadata.assetID;
            if (!entry.Material.IsValid())
                return nullptr; // Not imported yet; looked up again after the next import.
        }
        // Re-render when the material is rebuilt (a new asset) or a larger image is wanted.
        const auto status = m_Assets->Request<MaterialDataAsset>(entry.Material);
        if (status.IsReady() && (status.Asset != entry.Rendered || size > entry.Size) && m_ThumbnailBudget > 0)
        {
            --m_ThumbnailBudget;
            MaterialPreviewOptions options;
            options.Size = {size, size};
            if (auto image = m_ThumbnailRenderer->RenderThumbnail(entry.Material, options))
            {
                TextureData data;
                data.Width      = image->Size.x;
                data.Height     = image->Size.y;
                data.Format     = TexturePixelFormat::RGBA8;
                data.ColorSpace = TextureColorSpace::Linear; // Already display-ready.
                std::vector<std::byte> pixels(image->Pixels.size());
                std::memcpy(pixels.data(), image->Pixels.data(), pixels.size());
                data.Mips = {{std::move(pixels)}};
                if (auto uploaded = Texture2D::CreateFromProduct(data);
                    std::holds_alternative<Ref<Texture2D>>(uploaded))
                {
                    entry.Texture  = std::get<Ref<Texture2D>>(uploaded);
                    entry.Rendered = status.Asset;
                    entry.Size     = size;
                }
            }
        }
        if (status.Error)
            entry.Material = {}; // Deleted or unpublished: find it again.
        return entry.Texture;
    }

    AssetID EditorLayer::FindPublishedMaterial(const std::filesystem::path& file)
    {
        if (!m_AssetImports)
            return {};
        std::error_code error;
        const auto      wanted  = std::filesystem::weakly_canonical(file, error);
        auto            sources = m_AssetImports->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& source : *list)
                if (std::filesystem::weakly_canonical(m_AssetImports->GetConfig().AssetDirectory / source.Path.GetPath(),
                                                      error) == wanted)
                    for (const auto& product : m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts())
                        if (product.Metadata.assetID.GetSourceID() == source.sourceAssetID &&
                            product.Metadata.Type == AssetType::Material && product.Target == EditorAssetTarget)
                            return product.Metadata.assetID;
        return {};
    }

    void EditorLayer::CreateMaterialInstance(const std::filesystem::path& file)
    {
        const AssetID parent = FindPublishedMaterial(file);
        if (!parent.IsValid())
        {
            Record(AssetError {AssetErrorCode::NotFound, "Import the material before creating an instance of it"});
            return;
        }
        if (const auto target = WriteMaterialInstance(parent, file.parent_path(), file.stem().u8string());
            !target.empty())
            OpenMaterial(target);
    }

    std::filesystem::path
    EditorLayer::WriteMaterialInstance(AssetID parent, const std::filesystem::path& directory, const std::string& stem)
    {
        std::filesystem::path target;
        for (int index = 0; index < 1000; ++index)
        {
            target = directory / (stem + "_Instance" + (index ? std::to_string(index) : std::string()) + ".material");
            std::error_code error;
            if (!std::filesystem::exists(target, error))
                break;
        }
        MaterialSource source;
        source.Name   = target.stem().u8string();
        source.Parent = parent;
        {
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            output << SerializeMaterialSource(source);
            if (!output)
            {
                Record(AssetError {AssetErrorCode::IoError, "Cannot write " + target.u8string()});
                return {};
            }
        }
        Record(m_AssetImports->RefreshSources());
        auto sources = m_AssetImports->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
        {
            std::error_code error;
            for (const auto& meta : *list)
                if (std::filesystem::weakly_canonical(m_AssetImports->GetConfig().AssetDirectory / meta.Path.GetPath(),
                                                      error) == std::filesystem::weakly_canonical(target, error))
                {
                    AssetBuildOptions options;
                    options.Target = EditorAssetTarget;
                    if (auto started = m_AssetImports->Start(meta.sourceAssetID, options);
                        std::holds_alternative<AssetError>(started))
                        Record(std::get<AssetError>(started));
                }
        }
        m_ContentBrowserPanel.Refresh();
        return target;
    }

    void EditorLayer::CreateSlotInstance(AssetID parent, const std::filesystem::path& source, const SlotParts& parts)
    {
        if (!m_AssetImports)
            return;
        // A material inside a model is named after the model and the material.
        std::string stem = source.stem().u8string();
        if (source.extension() != ".material" && m_Assets)
            if (const auto material = m_Assets->Request<MaterialDataAsset>(parent);
                material.IsReady() && !material.Asset->GetData().Name.empty())
            {
                std::string name = material.Asset->GetData().Name;
                for (char& c : name)
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || (c & 0x80)))
                        c = '_';
                stem += "_" + name;
            }
        const auto target = WriteMaterialInstance(parent, source.parent_path(), stem);
        if (target.empty())
            return;
        m_PendingSlotInstances.push_back({target, parts});
        OpenMaterial(target);
    }

    void EditorLayer::AssignPendingSlotInstances()
    {
        auto assigned = [&](const auto& pending) {
            const AssetID instance = FindPublishedMaterial(pending.first);
            if (!instance.IsValid())
                return false;
            if (m_ActiveScene)
                for (const auto& [id, slot] : pending.second)
                    if (Entity entity = m_ActiveScene->GetEntityByUUID(id))
                        AssignMaterial(*m_ActiveScene, entity, slot, instance);
            return true;
        };
        m_PendingSlotInstances.erase(
            std::remove_if(m_PendingSlotInstances.begin(), m_PendingSlotInstances.end(), assigned),
            m_PendingSlotInstances.end());
    }

    void EditorLayer::ImportSource(const std::filesystem::path& file)
    {
        if (!m_AssetImports)
            return;
        auto sources = m_AssetImports->GetSources();
        if (const auto* error = std::get_if<AssetError>(&sources))
        {
            Record(*error);
            return;
        }
        std::error_code error;
        const auto      target = std::filesystem::weakly_canonical(file, error);
        const auto&     root   = m_AssetImports->GetConfig().AssetDirectory;
        for (const auto& source : std::get<std::vector<SourceAssetMetadata>>(sources))
            if (source.Exists && std::filesystem::weakly_canonical(root / source.Path.GetPath(), error) == target)
            {
                AssetBuildOptions options;
                options.Target = EditorAssetTarget;
                Record(m_AssetImports->Start(source.sourceAssetID, options));
                return;
            }
        Record(AssetError {AssetErrorCode::NotFound, file.filename().u8string() + " is not a project asset"});
    }

    void EditorLayer::PlacePendingModel()
    {
        if (!m_PendingPlacement || !m_ActiveScene)
            return;
        const auto source = *m_PendingPlacement;
        m_PendingPlacement.reset();
        const AssetID model = FindPublishedModel(source);
        if (!model.IsValid())
            return; // The import failed; the import panel shows why.
        std::string name = "Model";
        if (auto  sources = m_AssetImports->GetSources();
            auto* list    = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& metadata : *list)
                if (metadata.sourceAssetID == source)
                    name = metadata.Path.GetPath().stem().u8string();
        // The instance's parts appear once the prefab system has the prefab's data.
        auto entity = m_ActiveScene->InstantiatePrefab(model, {}, m_PendingPlacementParent);
        entity.GetComponent<TagComponent>().Tag = name;
        if (m_PendingPlacementPosition)
        {
            // Keep the model's world position under the parent.
            glm::mat4 world = m_ActiveScene->GetWorldTransform(entity);
            world[3]        = glm::vec4(*m_PendingPlacementPosition, 1.0f);
            m_ActiveScene->SetWorldTransform(entity, world);
        }
        m_PendingPlacementParent = 0;
        m_PendingPlacementPosition.reset();
        m_HierarchyPanel.SetSelected(entity);
    }

    std::variant<std::monostate, AssetError> EditorLayer::ActivateScene(Ref<Scene> scene, std::filesystem::path path)
    {
        if (!scene || !m_Viewport)
            return AssetError {AssetErrorCode::InvalidArgument, "Scene/renderer unavailable"};
        const auto viewport = m_Viewport->GetSize();
        scene->OnViewportResize(std::max(1u, viewport.x), std::max(1u, viewport.y));
        // Leaving a playing scene drops its runtime state; the edit-time state is not kept.
        if (m_ActiveScene)
            m_Play.Discard(*m_ActiveScene);
        FinishClothPaint();
        m_PendingCloth.clear();
        m_ActiveScene = std::move(scene);
        m_Viewport->SetScene(m_ActiveScene);
        m_HierarchyPanel.SetScene(m_ActiveScene);
        m_ViewportPanel.SetContext(m_ActiveScene, m_Viewport.get());
        m_CameraEntity       = {};
        m_SecondCameraEntity = {};
        m_ScenePath          = std::move(path);
        m_History.Reset(*m_ActiveScene);
        m_EditPending = false;
        return std::monostate {};
    }

    std::variant<std::monostate, AssetError> EditorLayer::NewScene() { return ActivateScene(CreateRef<Scene>(), {}); }

    namespace
    {
        std::variant<std::filesystem::path, AssetError> SceneFilePath(const EditorAssetImportService* project,
                                                                      const std::filesystem::path&    path)
        {
            if (path.empty() || path.extension() != ".kans")
                return AssetError {AssetErrorCode::InvalidArgument, "Choose a .kans scene file"};
            std::error_code error;
            auto            root = project ? project->GetConfig().RootDirectory : std::filesystem::current_path(error);
            if (error)
                return AssetError {AssetErrorCode::IoError, error.message()};
            return (root / path).lexically_normal();
        }
    } // namespace

    std::variant<std::monostate, AssetError> EditorLayer::OpenScene(const std::filesystem::path& path)
    {
        auto resolved = SceneFilePath(m_AssetImports.get(), path);
        if (const auto* error = std::get_if<AssetError>(&resolved))
            return *error;
        auto fullPath  = std::get<std::filesystem::path>(std::move(resolved));
        auto candidate = SceneSerializer::DeserializeChecked(fullPath);
        if (const auto* error = std::get_if<AssetError>(&candidate))
            return *error;
        auto activated = ActivateScene(std::get<Ref<Scene>>(std::move(candidate)), std::move(fullPath));
        if (!std::holds_alternative<std::monostate>(activated))
            return activated;
        // Post-processing comes from the scene settings; the editor view starts with the scene
        // camera's anti-aliasing, which the viewport's camera menu can still change.
        if (Entity camera = m_ActiveScene->GetCameraEntity())
            m_ViewportPanel.GetCamera().SetAntiAliasing(
                camera.GetComponent<CameraComponent>().SceneCamera.GetAntiAliasing());
        QueueMissingSceneAssets();
        return activated;
    }

    void EditorLayer::QueueMissingSceneAssets()
    {
        if (!m_AssetImports || !m_ActiveScene)
            return;
        std::set<SourceAssetID> referenced;
        const auto              add = [&](AssetID id) {
            if (id.IsValid())
                referenced.insert(id.GetSourceID());
        };
        for (Entity entity : m_ActiveScene->GetMeshEntities())
        {
            if (entity.HasComponent<EditableMeshComponent>())
                for (auto material : entity.GetComponent<EditableMeshComponent>().Materials)
                    add(material);
            if (entity.HasComponent<MeshComponent>())
                for (auto material : entity.GetComponent<MeshComponent>().Materials)
                    add(material);
        }
        std::function<void(Entity)> visit = [&](Entity entity) {
            if (entity.HasComponent<TextComponent>())
                add(entity.GetComponent<TextComponent>().Font);
            if (entity.HasComponent<PrefabInstanceComponent>())
                add(entity.GetComponent<PrefabInstanceComponent>().Prefab);
            for (Entity child : m_ActiveScene->GetChildren(entity))
                visit(child);
        };
        for (Entity root : m_ActiveScene->GetRootEntities())
            visit(root);
        // Sources with published products are rebuilt by the open-time sweep instead.
        for (const auto& record : m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts())
            referenced.erase(record.Metadata.assetID.GetSourceID());
        auto sources = m_AssetImports->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& source : *list)
                if (source.Exists && referenced.count(source.sourceAssetID))
                    QueueAutoImport(source.sourceAssetID);
        StartNextAutoImport();
    }

    void EditorLayer::QueueUnimportedShaders()
    {
        if (!m_AssetImports)
            return;
        std::set<SourceAssetID> published;
        for (const auto& record : m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts())
            published.insert(record.Metadata.assetID.GetSourceID());
        auto sources = m_AssetImports->GetSources();
        if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
            for (const auto& source : *list)
                if (source.Exists && source.Path.GetPath().extension() == ".shaderprog" &&
                    !published.count(source.sourceAssetID) && !m_AutoImported.count(source.sourceAssetID))
                {
                    CORE_INFO("Importing shader {}", source.Path.GetPath().generic_u8string());
                    QueueAutoImport(source.sourceAssetID);
                }
        StartNextAutoImport();
    }

    void EditorLayer::QueueAutoImport(SourceAssetID source)
    {
        if (m_AutoImported.insert(source).second)
            m_AutoImports.push_back(source);
    }

    void EditorLayer::StartNextAutoImport()
    {
        if (m_AutoImports.empty() || m_PendingPlacement)
            return;
        auto snapshot = m_AssetImports->GetSnapshot();
        if (const auto* state = std::get_if<EditorAssetImportSnapshot>(&snapshot);
            !state || state->State == EditorAssetImportState::Building)
            return; // Retried when the running build completes.
        AssetBuildOptions options;
        options.Target = EditorAssetTarget;
        const auto source = m_AutoImports.front();
        m_AutoImports.pop_front();
        Record(m_AssetImports->Start(source, options));
    }

    std::variant<std::monostate, AssetError> EditorLayer::SaveScene(const std::filesystem::path& path)
    {
        if (m_Play.IsActive())
            return AssetError {AssetErrorCode::InvalidArgument, "Stop play mode before saving the scene"};
        m_TimelinePanel.StopPreview(); // Saves the scene, not a previewed frame.
        auto resolved = SceneFilePath(m_AssetImports.get(), path);
        if (const auto* error = std::get_if<AssetError>(&resolved))
            return *error;
        auto fullPath = std::get<std::filesystem::path>(std::move(resolved));
        auto saved    = SceneSerializer(m_ActiveScene).SerializeChecked(fullPath);
        if (const auto* error = std::get_if<AssetError>(&saved))
            return *error;
        m_ScenePath = std::move(fullPath);
        m_History.MarkSaved();
        return std::monostate {};
    }

    void EditorLayer::Record(const std::variant<std::monostate, AssetError>& result)
    {
        if (const auto* error = std::get_if<AssetError>(&result))
        {
            m_SceneFileError = error->Message;
            CORE_ERROR("Editor: {}", error->Message);
        }
    }

    void EditorLayer::SaveCurrentScene()
    {
        if (m_ScenePath.empty())
            m_SceneDialog = SceneDialog::Save;
        else
            Record(SaveScene(m_ScenePath));
    }

    void EditorLayer::ConfirmDiscard(std::string what, std::function<void()> action)
    {
        // Play mode edits a copy; stopping restores the scene whose changes are at stake.
        if (m_Play.IsActive())
            StopPlay();
        m_Unsaved.Request(m_History.IsModified(), std::move(what), std::move(action));
        m_UnsavedPromptRequested = m_Unsaved.Asking();
    }

    void EditorLayer::DrawUnsavedPrompt()
    {
        if (m_UnsavedPromptRequested)
        {
            ImGui::OpenPopup("Unsaved Changes");
            m_UnsavedPromptRequested = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;
        const auto scene = m_ScenePath.empty() ? std::string("the untitled scene") : m_ScenePath.filename().u8string();
        ImGui::Text("Save changes to %s before %s?", scene.c_str(), m_Unsaved.What().c_str());
        ImGui::TextDisabled("Your changes will be lost if you don't save them.");
        ImGui::Dummy({0, EditorTheme::Px(8.0f)});
        std::optional<UnsavedChanges::Choice> choice;
        const ImVec2 button {EditorTheme::Px(110.0f), 0.0f};
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Colors().Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Colors().AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Colors().AccentPressed);
        if (ImGui::Button("Save", button) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
            choice = UnsavedChanges::Choice::Save;
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", button))
            choice = UnsavedChanges::Choice::Discard;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            choice = UnsavedChanges::Choice::Cancel;
        if (choice)
        {
            ImGui::CloseCurrentPopup();
            m_Unsaved.Resolve(*choice, [this] {
                if (m_ScenePath.empty())
                {
                    // An untitled scene needs a name first; the action waits for that save.
                    m_SceneDialog = SceneDialog::Save;
                    return UnsavedChanges::SaveResult::NeedsPath;
                }
                auto saved = SaveScene(m_ScenePath);
                Record(saved);
                return std::holds_alternative<std::monostate>(saved) ? UnsavedChanges::SaveResult::Saved :
                                                                       UnsavedChanges::SaveResult::Failed;
            });
        }
        ImGui::EndPopup();
    }

    void EditorLayer::DrawSceneFileDialogs()
    {
        // Opened here, not where requested, so the popup shares this ID stack.
        if (m_SceneDialog != SceneDialog::None)
        {
            std::error_code error;
            const auto      root = m_AssetImports ? m_AssetImports->GetConfig().RootDirectory : std::filesystem::current_path(error);
            m_SceneFileDialog.Open(m_SceneDialog == SceneDialog::Open ? SceneFileDialog::Mode::Open : SceneFileDialog::Mode::Save,
                                   root,
                                   m_ScenePath.empty() ? KansFileSystem::GetAssetFolder() / "scenes" : m_ScenePath);
            m_SceneDialog = SceneDialog::None;
        }
        bool savedAs = false;
        m_SceneFileDialog.Draw([&](const std::filesystem::path& file) -> std::optional<std::string> {
            const bool saving = m_SceneFileDialog.GetMode() == SceneFileDialog::Mode::Save;
            auto       result = saving ? SaveScene(file) : OpenScene(file);
            if (const auto* error = std::get_if<AssetError>(&result))
                return error->Message;
            savedAs = saving;
            return std::nullopt;
        });
        // An action waiting for "Save" on an untitled scene goes on once it is saved, and is
        // dropped if the dialog was dismissed.
        if (savedAs)
            m_Unsaved.SavedAs();
        else if (m_Unsaved.WaitingForSaveAs() && !m_SceneFileDialog.IsOpen() && m_SceneDialog == SceneDialog::None)
            m_Unsaved.SaveCancelled();
        DrawUnsavedPrompt();
    }
    void EditorLayer::OpenPhysicsMaterial(const std::filesystem::path& file)
    {
        if (m_PhysicsMaterialPanel.IsDirty() && m_PhysicsMaterialPanel.GetFile() != file)
            m_PhysicsMaterialPanel.Save(); // Switching files keeps the edits instead of dropping them.
        if (m_PhysicsMaterialPanel.Open(file))
            m_Panels.Open(PanelID::PhysicsMaterial);
        else
            Record(AssetError {AssetErrorCode::InvalidData, "Cannot open " + file.filename().u8string()});
    }

    void EditorLayer::OpenVfxModule(const std::filesystem::path& file)
    {
        std::error_code error;
        for (auto& editor : m_VfxModuleEditors)
            if (editor->IsOpen() && std::filesystem::equivalent(editor->GetFile(), file, error))
            {
                ImGui::SetWindowFocus(editor->WindowName().c_str());
                return;
            }
        auto panel = std::make_unique<VfxModuleEditorPanel>();
        panel->Bind(m_AssetImports.get());
        panel->SetOpenCallback([this](const std::filesystem::path& copy) { m_PendingVfxModules.push_back(copy); });
        if (m_AssetImports)
        {
            panel->SetProjectDirectory(m_AssetImports->GetConfig().AssetDirectory / "VFX");
            // The file of a project source, by source ID or path.
            const auto sources = [this]() {
                auto listed = m_AssetImports->GetSources();
                return std::holds_alternative<std::vector<SourceAssetMetadata>>(listed) ?
                           std::get<std::vector<SourceAssetMetadata>>(listed) :
                           std::vector<SourceAssetMetadata> {};
            };
            panel->SetModuleResolver([this, sources](AssetID id) {
                for (const auto& source : sources())
                    if (source.sourceAssetID == id.GetSourceID())
                        return Vfx::DecodeModule(
                            YAML::LoadFile((m_AssetImports->GetConfig().AssetDirectory / source.Path.GetPath()).string()));
                throw std::invalid_argument("Missing VFX script " + id.ToString());
            });
            // Emitters and systems whose products depend on the module's products.
            panel->SetUsageQuery([this, sources](const std::filesystem::path& module) -> std::optional<std::vector<std::string>> {
                auto registry = m_AssetImports->GetRuntimeAssetRegistry();
                if (!registry)
                    return std::nullopt;
                const auto     all = sources();
                SourceAssetID  id;
                std::error_code ec;
                for (const auto& source : all)
                    if (std::filesystem::equivalent(m_AssetImports->GetConfig().AssetDirectory / source.Path.GetPath(), module, ec))
                        id = source.sourceAssetID;
                if (!id.IsValid())
                    return std::nullopt;
                std::set<SourceAssetID> users;
                for (const auto& record : registry->GetAllProducts())
                    if (record.Metadata.Type == AssetType::VfxEmitter || record.Metadata.Type == AssetType::VfxSystem)
                        for (const auto& dependency : record.Metadata.Dependencies)
                            if (dependency.GetSourceID() == id)
                                users.insert(record.Metadata.assetID.GetSourceID());
                std::vector<std::string> names;
                for (const auto& source : all)
                    if (users.count(source.sourceAssetID))
                        names.push_back(source.Path.ToString());
                return names;
            });
        }
        if (panel->Open(file))
            m_VfxModuleEditors.push_back(std::move(panel));
        else
            Record(AssetError {AssetErrorCode::InvalidData, panel->Status()});
    }

    void EditorLayer::DrawVfxModuleEditors()
    {
        for (auto& editor : m_VfxModuleEditors)
            editor->Draw();
        m_VfxModuleEditors.erase(std::remove_if(m_VfxModuleEditors.begin(), m_VfxModuleEditors.end(),
                                                [](const auto& editor) { return !editor->IsOpen() && !editor->IsAskingToClose(); }),
                                 m_VfxModuleEditors.end());
        if (auto created = m_VfxModuleCreate.Draw())
        {
            m_ContentBrowserPanel.Refresh();
            m_PendingVfxModules.push_back(*created);
        }
        auto pending = std::move(m_PendingVfxModules);
        m_PendingVfxModules.clear();
        for (const auto& file : pending)
            OpenVfxModule(file);
    }
} // namespace Kans
