#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <glm/glm.hpp>
#include "KansEditorCore/EditorCamera.h"
#include "Kans3D/Renderer/RenderFeature.h"
#include "Kans3D/Renderer/RenderView.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Renderer/ViewTarget.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans
{
    class AssetManager;

    // One editor viewport: its render target, its feature set (editor overlays
    // plus authored material passes), its asset and environment leases, and the
    // per-frame intents (highlight, drag preview, view mode). Each frame builds a
    // fresh RenderView and renders the bound scene's world through Renderer::Render.
    class ViewportClient
    {
    public:
        ViewportClient();
        ~ViewportClient();
        ViewportClient(const ViewportClient&)            = delete;
        ViewportClient& operator=(const ViewportClient&) = delete;

        // Binds a scene. Its world takes the previous world's render settings and
        // environment, so reloading or playing a scene keeps the viewport's look.
        void              SetScene(Ref<Scene> scene);
        const Ref<Scene>& GetScene() const { return m_Scene; }

        // The manager must outlive its use here; pass nullptr before destroying it.
        void                                     SetAssetManager(AssetManager* assets);
        const std::shared_ptr<RenderAssetCache>& GetAssetCache() const { return m_Assets; }

        // Loads the environment named by the bound world (IBL, skybox, BRDF LUT).
        void                                        PrepareEnvironment(EnvironmentPrecomputeSizes sizes = {});
        const std::shared_ptr<EnvironmentLighting>& GetEnvironment() const { return m_Lighting; }
        const std::shared_ptr<Renderer>&            GetRenderer() const { return m_Renderer; }

        void       Resize(glm::uvec2 size);
        glm::uvec2 GetSize() const { return m_Size; }
        uintptr_t  GetOutput() const { return m_Target->GetOutput(0); }

        RenderFeatureSet& GetFeatures() { return m_Features; }
        void              SetViewMode(SceneViewMode mode) { m_ViewMode = mode; }
        // The editor view's grid look; its plane follows the camera (see RenderEditor).
        void              SetGrid(const GridSettings& grid) { m_Grid = grid; }
        // Per-pass captures of the editor view (Frame Graph window); off by default.
        void SetDebugCapture(GraphCaptureSettings settings) { m_Debug = std::move(settings); }
        // The editor view's capture store: display textures and pixel probes.
        std::shared_ptr<GraphCapture> GetGraphCapture() { return m_Target->GetGraphCapture(); }
        // Consumed by the next rendered frame.
        void SetHighlights(ViewHighlights highlights) { m_Highlights = std::move(highlights); }
        void SetPreview(AssetID prefab, const glm::mat4& transform) { m_Preview = PrefabPreview {prefab, transform}; }
        // Transform handles for the next editor frame (drawn by the TransformGizmo feature).
        void SetGizmo(std::vector<GizmoDraw> draws) { m_Gizmo = std::move(draws); }

        // Renders through the editor camera, or through the scene's primary camera
        // with no editor features (false when the scene has no camera).
        void RenderEditor(const EditorCamera& camera);
        bool RenderGame();

        ScenePickHit        Pick(const glm::mat4& viewProjection, glm::uvec2 pixel);
        const RenderResult& GetLastResult() const { return m_Last; }

    private:
        void Render(RenderView view, bool editorFeatures);
        void RefreshEnvironment();

        std::shared_ptr<Renderer>            m_Renderer;
        std::unique_ptr<ViewTarget>          m_Target;
        RenderFeatureSet                     m_Features;
        Ref<Scene>                           m_Scene;
        std::shared_ptr<RenderAssetCache>    m_Assets;
        std::shared_ptr<EnvironmentLighting> m_Lighting = std::make_shared<EnvironmentLighting>();
        EnvironmentPrecomputeSizes           m_EnvironmentSizes;
        std::filesystem::path                m_EnvironmentSource;
        glm::uvec2                           m_Size {0, 0};
        SceneViewMode                        m_ViewMode = SceneViewMode::Lit;
        GridSettings                         m_Grid;
        GraphCaptureSettings                 m_Debug;
        ViewHighlights                       m_Highlights;
        ViewHighlights                       m_LastEditorHighlights;
        std::optional<PrefabPreview>         m_Preview;
        std::vector<GizmoDraw>               m_Gizmo;
        RenderResult                         m_Last;

        std::chrono::steady_clock::time_point m_StartTime = std::chrono::steady_clock::now();
    };
} // namespace Kans
