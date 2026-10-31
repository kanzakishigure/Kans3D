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
        m_RagdollEditor.SetSavedCallback([this](const std::filesystem::path& file) {
            CORE_INFO("{}: {}", file.filename().u8string(), BuildSavedSource(m_AssetImports.get(), file));
            m_ContentBrowserPanel.Refresh();
        });
        m_RagdollEditor.SetPreviewRenderer(
            [this](const Ref<Scene>& scene, const EditorCamera& camera, glm::uvec2 size) {
                return RenderRagdollPreview(scene, camera, size);
            });
        m_ContentBrowserPanel.SetCreateRagdollCallback(
            [this](const std::filesystem::path& model) { CreateRagdollForModel(model); });
        m_ViewportPanel.SetWheelSelection([this](Entity vehicle) { return m_PropertiesPanel.GetSelectedWheel(vehicle); });
    }

    uintptr_t EditorLayer::RenderRagdollPreview(const Ref<Scene>& scene, const EditorCamera& camera, glm::uvec2 size)
    {
        if (!scene || !m_Viewport || !m_Assets)
            return 0;
        if (!m_RagdollPreview)
        {
            m_RagdollPreview = CreateScope<ViewportClient>();
            m_RagdollPreview->SetAssetManager(m_Assets.get());
        }
        if (m_RagdollPreview->GetScene() != scene)
        {
            // The editor's environment lights the preview.
            if (m_ActiveScene && scene->GetRenderWorld().EnvironmentPath.empty())
            {
                scene->GetRenderWorld().EnvironmentPath = m_ActiveScene->GetRenderWorld().EnvironmentPath;
                scene->GetRenderWorld().EnvironmentRoot = m_ActiveScene->GetRenderWorld().EnvironmentRoot;
            }
            m_RagdollPreview->SetScene(scene);
            m_RagdollPreview->PrepareEnvironment();
        }
        // Outside a run the model shows its bind pose.
        if (!scene->GetPhysicsWorld())
            scene->UpdateModelPoses();
        m_RagdollPreview->Resize(size);
        m_RagdollPreview->RenderEditor(camera);
        return m_RagdollPreview->GetOutput();
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
        if (!m_ActiveScene || !character || m_Play.IsActive() || !character.HasComponent<AnimatorComponent>())
            return;
        const auto& a = character.GetComponent<AnimatorComponent>();
        if (!a.SkeletonSource)
        {
            Record(AssetError {AssetErrorCode::NotFound, "The Animator's skeleton is not loaded yet"});
            return;
        }
        RagdollEditing::PreviewSource source;
        source.Skeleton       = a.Skeleton;
        source.SkeletonSource = a.SkeletonSource;
        source.Controller     = a.Controller;
        if (character.HasComponent<PrefabInstanceComponent>())
            source.Model = character.GetComponent<PrefabInstanceComponent>().Prefab;
        // The skinned mesh fits the bodies' radii.
        Ref<const MeshAsset>        mesh;
        std::function<void(Entity)> find = [&](Entity e) {
            if (mesh)
                return;
            if (auto m = m_ActiveScene->ResolveMesh(e); m && !m->GetData().Skin.empty() && m->GetData().Skeleton == a.Skeleton)
                mesh = m;
            for (Entity child : m_ActiveScene->GetChildren(e))
                find(child);
        };
        find(character);
        const auto model     = SourceFileOf(a.Skeleton);
        auto       directory = model.empty() ? m_ContentBrowserPanel.GetCurrentFolder() : model.parent_path();
        if (directory.empty() && m_AssetImports)
            directory = m_AssetImports->GetConfig().AssetDirectory;
        const std::string stem =
            SafeStem((model.empty() ? character.GetComponent<TagComponent>().Tag : model.stem().u8string()) + "_Ragdoll");
        const UUID owner = character.GetUUID();
        m_RagdollEditor.SetAssetManager(m_Assets.get());
        m_RagdollEditor.BeginCreate(directory, stem, source, mesh, [this, owner](const std::filesystem::path& file) {
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
        RagdollEditing::PreviewSource source;
        AssetID                       meshID;
        for (const auto& product : m_AssetImports->GetRuntimeAssetRegistry()->GetAllProducts())
        {
            if (product.Metadata.assetID.GetSourceID() != *sourceID || product.Target != EditorAssetTarget)
                continue;
            if (product.Metadata.Type == AssetType::Skeleton)
                source.Skeleton = product.Metadata.assetID;
            else if (product.Metadata.Type == AssetType::Model)
                source.Model = product.Metadata.assetID;
            else if (product.Metadata.Type == AssetType::Mesh && !meshID.IsValid())
                meshID = product.Metadata.assetID;
        }
        if (!source.Skeleton.IsValid())
        {
            Record(AssetError {AssetErrorCode::NotFound, file.filename().u8string() + " has no skeleton"});
            return;
        }
        auto skeleton = m_Assets->Load<SkeletonAsset>(source.Skeleton);
        if (const auto* error = std::get_if<AssetError>(&skeleton))
            return Record(*error);
        source.SkeletonSource = std::get<Ref<const SkeletonAsset>>(skeleton);
        Ref<const MeshAsset> mesh;
        if (meshID.IsValid())
            if (auto loaded = m_Assets->Load<MeshAsset>(meshID); std::holds_alternative<Ref<const MeshAsset>>(loaded))
                if (const auto& m = std::get<Ref<const MeshAsset>>(loaded); m->GetData().Skeleton == source.Skeleton)
                    mesh = m;
        m_RagdollEditor.SetAssetManager(m_Assets.get());
        m_RagdollEditor.BeginCreate(file.parent_path(), SafeStem(file.stem().u8string() + "_Ragdoll"), source, mesh, {});
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
