#include "EditorLayer.h"
#include <imgui.h>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "KansEditorCore/Components/MaterialAssignment.h"

// TestInclude
#include "Kans3D/Core/Log/Log.h"
#include "KansEditorCore/Renderer/EditorRenderFeatures.h"
#include <Kans3D/Core/Hash.h>
#include <Kans3D/Core/UUID.h>

namespace Kans
{
    EditorLayer::EditorLayer(std::filesystem::path                          projectFile,
                             std::shared_ptr<Launcher::WindowPlacementFlow> placements,
                             InProcessHubOpener                             openHubInProcess) :
        Layer("EditorLayer"), m_ProjectFile(std::move(projectFile)), m_Placements(std::move(placements)),
        m_OpenHubInProcess(std::move(openHubInProcess))
    {}

    void EditorLayer::OnAttach()
    {
        PROFILE_FUCTION();
        // Before anything logs, so the console shows the whole session.
        m_Console.Attach();
        RegisterPanels();
        m_Panels.InstallSettingsHandler();
        m_ContentBrowserPanel.InstallSettingsHandler();
        // The splash keeps the window's own titlebar, so it can be moved and closed while loading.
        m_Titlebar.Attach(Application::Get().GetWindow());
        Application::Get().GetWindow().SetTitle(m_ProjectFile.stem().u8string() + " - Kans3D Editor");
        // Closing the window asks about unsaved changes like File > Exit; nothing is unsaved
        // before the editor has started.
        Application::Get().SetCloseRequestHandler([this] {
            if (!m_Startup.Finished() || !m_History.IsModified())
                return true;
            ConfirmDiscard("closing the editor", [] { Application::Get().Close(); });
            return false;
        });
        BuildStartup();
    }

    void EditorLayer::BuildStartup()
    {
        // Weights roughly follow how long each stage takes on a typical project.
        m_Startup.Add("Opening the project", 3.0f, [this] { return OpenProject(); });
        m_Startup.Add("Loading project scripts", 1.0f, [this] {
            LoadProjectScripts();
            return std::nullopt;
        });
        m_Startup.Add("Loading editor resources", 1.0f, [] {
            EditorResources::Init();
            return std::nullopt;
        });
        m_Startup.Add("Creating the scene", 1.0f, [this] {
            CreateDefaultScene();
            return std::nullopt;
        });
        m_Startup.Add("Preparing the environment", 4.0f, [this] {
            CreateViewport();
            return std::nullopt;
        });
        m_Startup.Add("Loading project assets", 2.0f, [this] {
            Record(m_StartupAssets = RefreshProductAssets());
            return std::nullopt;
        });
        m_Startup.Add("Opening the startup scene", 3.0f, [this] {
            if (!m_StartupScene.empty() && std::holds_alternative<std::monostate>(m_StartupAssets))
                Record(OpenScene(KansFileSystem::GetProjectRoot() / m_StartupScene));
            // Imports of what the scene misses run in the background, in the progress popup.
            QueueMissingSceneAssets();
            return std::nullopt;
        });
        m_Startup.Add("Restoring the layout", 1.0f, [this] {
            LoadLayout();
            return std::nullopt;
        });
    }

    void EditorLayer::CreateDefaultScene()
    {
        m_ActiveScene = CreateRef<Scene>("Untitled");
        m_ActiveScene->OnViewportResize((uint32_t)Application::Get().GetWindow().GetWidth(),
                                        (uint32_t)Application::Get().GetWindow().GetHeight());
        auto pointlight = m_ActiveScene->CreateEntity("PointLight");

        auto& plightCMP              = pointlight.AddComponent<PointLightComponent>();
        plightCMP.Diffuse_Intensity  = glm::vec3(1.0);
        plightCMP.Specular_Intensity = glm::vec3(1.0);
        plightCMP.Ambient_Intensity  = glm::vec3(1.0);
        pointlight.PatchComponent<TransformComponent>([](auto& t) { t.Position = {20.0f, 20.0f, 1.0f}; });

        auto  DirLight            = m_ActiveScene->CreateEntity("DirLight");
        auto& dirCMP              = DirLight.AddComponent<DirLightComponent>();
        dirCMP.Diffuse_Intensity  = glm::vec3(1.0);
        dirCMP.Specular_Intensity = glm::vec3(1.0);
        dirCMP.Ambient_Intensity  = glm::vec3(1.0);

        m_CameraEntity = m_ActiveScene->CreateEntity("mainCamera");
        m_CameraEntity.PatchComponent<TransformComponent>([](auto& t) { t.Position = {0.0f, 0.7f, 1.5f}; });
        auto& cmp = m_CameraEntity.AddComponent<CameraComponent>();
        cmp.SceneCamera.SetViewportSize(1920, 1080);

        m_HierarchyPanel.SetScene(m_ActiveScene);
        m_History.Reset(*m_ActiveScene);
    }

    void EditorLayer::CreateViewport()
    {
        m_Viewport = CreateScope<ViewportClient>(m_ProjectConfig.RootDirectory / m_ProjectConfig.CacheDirectory /
                                                 "Editor/Visibility.yaml");
        m_Viewport->SetScene(m_ActiveScene);
        // The Hierarchy's eye column edits the visibility the editor view renders with.
        m_HierarchyPanel.SetVisibility(&m_Viewport->Visibility());
        // Scenes without environment settings show the editor's default map; a scene's own
        // map is relative to the project directory.
        const auto defaultEnvironment = KansFileSystem::GetResoucesFolder() / "Environment/studio_country_hall_2k.hdr";
        m_ActiveScene->GetRenderWorld().EnvironmentPath = defaultEnvironment;
        m_ActiveScene->GetRenderWorld().EnvironmentRoot = std::filesystem::current_path();
        m_PropertiesPanel.SetDefaultEnvironment(defaultEnvironment);
        m_Viewport->PrepareEnvironment();
        // Material previews share the viewport's renderer and environment; their
        // asset cache is bound when the project's assets are (RefreshProductAssets).
        m_MaterialPreview =
            CreateScope<MaterialPreviewRenderer>(m_Viewport->GetRenderer(), nullptr, m_Viewport->GetEnvironment());
        m_ThumbnailRenderer =
            CreateScope<MaterialPreviewRenderer>(m_Viewport->GetRenderer(), nullptr, m_Viewport->GetEnvironment());
        m_MaterialEditorPanel.SetPreviewRenderer(m_MaterialPreview.get());
        m_ContentBrowserPanel.SetMaterialThumbnailProvider(
            [this](const std::filesystem::path& file, uint32_t size) { return MaterialThumbnail(file, size); });
        m_PropertiesPanel.SetMaterialThumbnailProvider(
            [this](const std::filesystem::path& file, uint32_t size) { return MaterialThumbnail(file, size); });
        m_PropertiesPanel.SetBrowserSelection([this] { return m_ContentBrowserPanel.GetSelectedFiles(); });
        m_ViewportPanel.SetHighlightSource(
            [this](ViewHighlights& highlights) { m_PropertiesPanel.AppendSlotHighlights(highlights); });
        m_ViewportPanel.SetContext(m_ActiveScene, m_Viewport.get());
        m_ViewportPanel.SetFileDropCallback(
            [this](const std::filesystem::path& file, std::optional<glm::vec3> position) {
                if (file.extension() == ".kans")
                    ConfirmDiscard("opening " + file.filename().u8string(), [this, file] { Record(OpenScene(file)); });
                else if (file.has_extension())
                    PlaceAsset(file, {}, position);
            });
        m_ViewportPanel.SetMaterialDropCallback([this](Entity part, uint32_t slot, const std::filesystem::path& file) {
            const AssetID material = FindPublishedMaterial(file);
            if (!material.IsValid())
                Record(AssetError {AssetErrorCode::InvalidArgument,
                                   file.filename().u8string() + " is not imported yet; try again once it is."});
            else if (m_ActiveScene)
                AssignMaterial(*m_ActiveScene, part, slot, material);
        });
        m_ViewportPanel.SetModelResolver([this](const std::filesystem::path& file) {
            const auto source = file.has_extension() ? FindModelSource(file) : std::nullopt;
            return source ? FindPublishedModel(*source) : AssetID {};
        });
    }

    void EditorLayer::LoadLayout()
    {
        // ImGui keeps the pointer and saves through it until the context goes away, after this layer.
        static std::string layout;
        layout = m_LayoutFile.u8string();
        std::error_code ec;
        std::filesystem::create_directories(m_LayoutFile.parent_path(), ec);
        auto& io       = ImGui::GetIO();
        io.IniFilename = layout.c_str();
        // Explicit, now that every settings handler is installed and the content browser is bound.
        if (std::filesystem::exists(m_LayoutFile, ec))
            ImGui::LoadIniSettingsFromDisk(layout.c_str());
    }

    void EditorLayer::OnDetach()
    {
        Application::Get().SetCloseRequestHandler({});
        m_Titlebar.Detach();
        if (m_Placements)
            m_Placements->Remember(Application::Get().GetWindow().GetPlacement());
        if (m_ThumbnailWrite.valid())
            m_ThumbnailWrite.wait();
        m_Panels.UninstallSettingsHandler();
        m_ContentBrowserPanel.UninstallSettingsHandler();
        m_ContentBrowserPanel.Bind(nullptr);
        m_ContentBrowserPanel.SetOpenFileCallback({});
        m_ContentBrowserPanel.SetCreateMaterialCallback({});
        m_ContentBrowserPanel.SetImportCallback({});
        m_ContentBrowserPanel.SetImportSettingsCallback({});
        m_ContentBrowserPanel.SetExtractMaterialsCallback({});
        m_ContentBrowserPanel.SetErrorCallback({});
        m_ProductImportPanel.Bind(nullptr);
        m_TimelinePanel.StopPreview();
        m_TimelinePanel.Bind(nullptr);
        m_PhysicsMaterialPanel.Bind(nullptr);
        m_ProjectSettingsPanel.Bind({});
        m_UIDesignerPanel.Bind(nullptr);
        m_MaterialEditorPanel.Bind(nullptr);
        m_VfxModuleEditors.clear(); // Waits for a running module compile.
        m_VfxSystemEditors.clear();
        if (m_AssetImports)
        {
            if (const auto task = m_ImportProgress.Task())
                m_Progress.Dismiss(*task); // Its cancel action refers to the service.
            auto stopped = m_AssetImports->Shutdown();
            if (const auto* error = std::get_if<AssetError>(&stopped))
                CORE_ERROR("Project import shutdown: {}", error->Message);
        }
        if (m_ActiveScene)
            m_Play.Discard(*m_ActiveScene);
        // The ragdoll editor's preview scene and its renderer hold asset leases.
        m_RagdollEditor.SetPreviewRenderer({});
        m_RagdollEditor.SetAssetManager(nullptr);
        m_RagdollEditor.Preview().SetSource({}, nullptr);
        m_RagdollPreview.Reset();
        if (m_Viewport)
        {
            m_ViewportPanel.SetContext({}, nullptr);
            if (m_MaterialPreview)
                m_MaterialPreview->SetResources({}, {});
            if (m_ThumbnailRenderer)
                m_ThumbnailRenderer->SetResources({}, {});
            m_Viewport->SetAssetManager(nullptr);
            m_PropertiesPanel.SetAssetManager(nullptr);
            m_MaterialEditorPanel.SetAssetManager(nullptr);
            m_TimelinePanel.SetAssetManager(nullptr);
            m_MaterialEditorPanel.SetUsageQuery({});
            m_MaterialEditorPanel.SetSelectCallback({});
            m_MaterialEditorPanel.SetPreviewRenderer(nullptr);
            m_ContentBrowserPanel.SetMaterialThumbnailProvider({});
            m_PropertiesPanel.SetMaterialThumbnailProvider({});
            m_PropertiesPanel.SetBrowserSelection({});
            m_PropertiesPanel.SetMaterialActions({});
            m_PendingSlotInstances.clear();
            m_ViewportPanel.SetHighlightSource({});
            m_ViewportPanel.SetMaterialDropCallback({});
            m_MaterialThumbnails.clear();
            m_MaterialPreview.reset();
            m_ThumbnailRenderer.reset();
            m_HierarchyPanel.SetVisibility(nullptr);
            m_Viewport.reset();
        }
        m_Assets.reset();
        m_HierarchyPanel.SetScene({});

        PROFILE_FUCTION();
        CLIENT_INFO_TAG("Editor", "editor layer call detach");

        EditorResources::ShutDown();

        m_ActiveScene.reset();
        m_AssetImports.reset();
        m_Console.Detach();
    }

    void EditorLayer::OnUpdate(TimeStep ts)
    {
        PROFILE_FUCTION();
        // The startup splash runs the startup stages (OnImGuiRender); nothing else runs until then.
        if (!m_Startup.Finished())
            return;
        PollScriptBuild();
        if (m_AssetImports)
        {
            // Consume source operations before Poll: a just-completed build may
            // publish a newer catalog than this retained immutable notification.
            auto changed = m_AssetImports->ConsumeRuntimeRegistryChange();
            if (const auto* error = std::get_if<AssetError>(&changed))
                Record(*error);
            else if (const auto& snapshot = std::get<std::optional<Ref<const AssetRegistry>>>(changed); snapshot)
            {
                Record(RefreshProductAssets(*snapshot));
                AssignPendingSlotInstances();
                AssignPendingCloth();
                AssignPendingRagdolls();
                QueueMissingSceneAssets(); // Includes newly discovered scene sources.
            }
            auto completed = m_AssetImports->Poll();
            // Before anything starts the next queued import: the reporter sees every terminal state.
            m_ImportProgress.Poll(m_Progress, *m_AssetImports, m_SceneAssetImports);
            if (const auto* error = std::get_if<AssetError>(&completed))
                Record(*error);
            else if (std::get<bool>(completed))
            {
                // Dependencies can be published even when the root build fails.
                Record(RefreshProductAssets());
                AssignPendingSlotInstances();
                AssignPendingCloth();
                AssignPendingRagdolls();
                if (m_PendingPlacement && m_PendingPlacementWaiting)
                    StartPendingPlacementBuild();
                else
                    PlacePendingModel();
                // The open-time sweep may have discovered previously unknown scene sources. The scene's
                // references were captured when it opened; serializing it again here froze the editor.
                Record(m_SceneAssetImports.Rematch(*m_AssetImports));
                if (!m_Play.IsActive())
                    m_History.Record(*m_ActiveScene);
            }
        }

        StartNextAutoImport();

        // The scene only runs in play mode; IK and saved shape keys of models without an
        // Animator still show while editing.
        m_TimelinePanel.StopPreviewIfHidden();
        if (m_Play.IsActive())
        {
            // The game's UI sees the pointer over the viewport image and the keys typed into it.
            auto input = m_UIInput;
            m_ViewportPanel.GameInput(input);
            m_Play.FeedUIInput(input, m_ViewportPanel.GameViewport());
            m_UIInput.Buttons = input.Buttons; // Held buttons carry over to the next frame.
            m_UIInput.NextFrame();
        }
        else
            m_UIInput = {};
        const uint64_t loads   = m_Play.GetSceneLoadCount();
        const bool     updated = m_Play.Update(*m_ActiveScene, ts);
        // A script loaded another scene: the selection named entities of the one it replaced.
        if (m_Play.GetSceneLoadCount() != loads)
        {
            m_HierarchyPanel.SetSelected({});
            QueueMissingSceneAssets();
        }
        if (!updated && !m_Play.IsActive())
        {
            m_ActiveScene->UpdateModelPoses();
            // Springs and cloth on the edit pose; nothing is written to the scene.
            SecondaryMotionEditing::StepPreview(*m_ActiveScene, double(ts));
            m_ActiveScene->UpdateVfxPreview(float(ts), m_ViewportPanel.GetCamera().GetPosition());
        }
        // The viewport panel renders the scene during the UI frame.
    }

    void EditorLayer::OnEvent(Event& e)
    {
        // Editor shortcuts are handled in the ImGui frame (HandleShortcuts). While playing, keys and
        // text typed with the viewport focused also reach the game's UI (the pointer comes from ImGui).
        const auto type = e.GetEventType();
        if (m_Play.IsActive() && m_ViewportPanel.IsFocused() &&
            (type == EventType::KeyPressed || type == EventType::KeyReleased || type == EventType::KeyTyped))
            m_UIInput.Feed(e);
        else if (type == EventType::KeyReleased)
            m_UIInput.Feed(e); // A key held when focus left still has to come up.
    }

} // namespace Kans
