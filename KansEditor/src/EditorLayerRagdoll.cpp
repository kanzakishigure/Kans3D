#include <functional>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Editor/EditorAssetImportService.h"
#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "EditorLayer.h"
#include "Physics/RagdollEditing.h"
#include "UI/SourceBuild.h"

namespace Kans
{
    namespace
    {
        // Characters of a file name that are not allowed on some systems.
        std::string SafeStem(std::string stem)
        {
            for (char& c : stem)
                if (std::string("\\/:*?\"<>|").find(c) != std::string::npos)
                    c = '_';
            return stem;
        }
    } // namespace

    void EditorLayer::BindRagdolls()
    {
        PropertiesPanel::RagdollActions actions;
        actions.Open   = [this](AssetID ragdoll) { OpenRagdollAsset(ragdoll); };
        actions.Create = [this](Entity entity) { CreateRagdollFor(entity); };
        m_PropertiesPanel.SetRagdollActions(std::move(actions));
        m_PropertiesPanel.SetOpenVfxSystem([this](AssetID system) {
            const auto file = SourceFileOf(system);
            if (file.empty())
                Record(AssetError {AssetErrorCode::NotFound, "The VFX system has no source file to edit"});
            else
                m_PendingVfxSystems.push_back(file);
        });
        m_RagdollEditor.SetSavedCallback([this](const std::filesystem::path& file) {
            CORE_INFO("{}: {}", file.filename().u8string(), BuildSavedSource(m_AssetImports.get(), file));
            m_ContentBrowserPanel.Refresh();
        });
        m_RagdollEditor.SetPreviewRenderer(
            [this](const Ref<Scene>& scene, const EditorCamera& camera, glm::uvec2 size, const std::vector<GizmoDraw>& gizmo) {
                return RenderRagdollPreview(scene, camera, size, gizmo);
            });
        m_ContentBrowserPanel.SetCreateRagdollCallback(
            [this](const std::filesystem::path& model) { CreateRagdollForModel(model); });
        m_ContentBrowserPanel.SetCreateRagdollFromSkeletonCallback([this](AssetID skeleton) { CreateRagdollForSkeleton(skeleton); });
        m_ViewportPanel.SetWheelSelection([this](Entity vehicle) { return m_PropertiesPanel.GetSelectedWheel(vehicle); });
    }

    uintptr_t EditorLayer::RenderRagdollPreview(const Ref<Scene>& scene, const EditorCamera& camera, glm::uvec2 size,
                                                const std::vector<GizmoDraw>& gizmo)
    {
        if (!m_Viewport || !m_Assets)
            return 0;
        m_RagdollPreview.SetAssetManager(m_Assets.get());
        return m_RagdollPreview.Render(scene, camera, size, gizmo, m_ActiveScene.get());
    }

    void EditorLayer::OpenRagdollFile(const std::filesystem::path& file, Entity character)
    {
        m_RagdollEditor.SetAssetManager(m_Assets.get());
        std::optional<RagdollEditing::PreviewSource> source;
        // From a character: its skeleton, model and controller (pose matching follows it).
        if (character && character.HasComponent<AnimatorComponent>())
        {
            const auto& a = character.GetComponent<AnimatorComponent>();
            if (a.SkeletonSource)
            {
                RagdollEditing::PreviewSource s;
                s.Skeleton       = a.Skeleton;
                s.SkeletonSource = a.SkeletonSource;
                s.Controller     = a.Controller;
                if (character.HasComponent<PrefabInstanceComponent>())
                    s.Model = character.GetComponent<PrefabInstanceComponent>().Prefab;
                source = s;
            }
        }
        const auto open = [this, file, source] {
            if (!m_RagdollEditor.Open(file, source))
                Record(AssetError {AssetErrorCode::InvalidData, m_RagdollEditor.Status()});
        };
        // Another ragdoll with unsaved edits asks first.
        if (m_RagdollEditor.IsOpen() && m_RagdollEditor.Session().GetFile() != file)
            m_RagdollEditor.RequestClose(open);
        else
            open();
    }

    void EditorLayer::OpenRagdollAsset(AssetID ragdoll)
    {
        const auto file = SourceFileOf(ragdoll);
        if (file.empty())
        {
            Record(AssetError {AssetErrorCode::NotFound, "The ragdoll asset has no source file to edit"});
            return;
        }
        OpenRagdollFile(file, m_HierarchyPanel.GetSelected());
    }

    void EditorLayer::CreateRagdollFor(Entity character)
    {
        if (!m_ActiveScene || !character || m_Play.IsActive())
            return;
        const auto products = m_AssetImports ? m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts() :
                                               std::vector<ProductRecord> {};
        auto       found    = RagdollEditing::SourceFromEntity(*m_ActiveScene, character, m_Assets.get(), products,
                                                               EditorAssetTarget);
        if (const auto* reason = std::get_if<std::string>(&found))
        {
            Record(AssetError {AssetErrorCode::NotFound, *reason});
            return;
        }
        auto&      created   = std::get<RagdollEditing::CreateSource>(found);
        const auto model     = SourceFileOf(created.Source.Skeleton);
        auto       directory = model.empty() ? m_ContentBrowserPanel.GetCurrentFolder() : model.parent_path();
        if (directory.empty() && m_AssetImports)
            directory = m_AssetImports->GetConfig().AssetDirectory;
        const std::string stem =
            SafeStem((model.empty() ? character.GetComponent<TagComponent>().Tag : model.stem().u8string()) + "_Ragdoll");
        const UUID owner = character.GetUUID();
        m_RagdollEditor.SetAssetManager(m_Assets.get());
        m_RagdollEditor.BeginCreate(directory, stem, created.Source, created.Mesh, [this, owner](const std::filesystem::path& file) {
            m_PendingRagdolls.push_back({file, owner});
        });
    }

    void EditorLayer::CreateRagdollForModel(const std::filesystem::path& file)
    {
        const auto sourceID = FindModelSource(file);
        if (!sourceID || !m_Assets || !m_AssetImports)
        {
            Record(AssetError {AssetErrorCode::NotFound, file.filename().u8string() + " is not an imported model"});
            return;
        }
        BeginRagdollFromImport(file, *sourceID, {});
    }

    void EditorLayer::CreateRagdollForSkeleton(AssetID skeleton)
    {
        const auto file = SourceFileOf(skeleton);
        if (!skeleton.IsValid() || file.empty() || !m_Assets || !m_AssetImports)
        {
            Record(AssetError {AssetErrorCode::NotFound, "The skeleton has no imported source"});
            return;
        }
        BeginRagdollFromImport(file, skeleton.GetSourceID(), skeleton);
    }

    void EditorLayer::BeginRagdollFromImport(const std::filesystem::path& file, SourceAssetID import, AssetID skeleton)
    {
        auto found = RagdollEditing::SourceFromImport(m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts(), import,
                                                      skeleton, *m_Assets, EditorAssetTarget);
        if (const auto* reason = std::get_if<std::string>(&found))
        {
            Record(AssetError {AssetErrorCode::NotFound, file.filename().u8string() + ": " + *reason});
            return;
        }
        auto& created = std::get<RagdollEditing::CreateSource>(found);
        m_RagdollEditor.SetAssetManager(m_Assets.get());
        m_RagdollEditor.BeginCreate(file.parent_path(), SafeStem(file.stem().u8string() + "_Ragdoll"), created.Source,
                                    created.Mesh, {});
    }

    void EditorLayer::AssignPendingRagdolls()
    {
        bool assigned = false;
        m_PendingRagdolls.erase(std::remove_if(m_PendingRagdolls.begin(),
                                               m_PendingRagdolls.end(),
                                               [&](const PendingRagdoll& pending) {
                                                   const AssetID ragdoll = FindPublished(pending.File, AssetType::Ragdoll);
                                                   if (!ragdoll.IsValid())
                                                       return false;
                                                   if (Entity e = m_ActiveScene ? m_ActiveScene->GetEntityByUUID(pending.Entity) : Entity {})
                                                   {
                                                       if (!e.HasComponent<RagdollComponent>())
                                                           e.AddComponent<RagdollComponent>();
                                                       e.PatchComponent<RagdollComponent>(
                                                           [&](RagdollComponent& c) { c.Asset = ragdoll; });
                                                       assigned = true;
                                                   }
                                                   return true;
                                               }),
                                m_PendingRagdolls.end());
        // The assignment is one undoable step.
        if (assigned && !m_Play.IsActive())
            m_History.Record(*m_ActiveScene);
    }
} // namespace Kans
