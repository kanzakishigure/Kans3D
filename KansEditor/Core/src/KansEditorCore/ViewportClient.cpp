#include "KansEditorCore/ViewportClient.h"
#include "Kans3D/Renderer/Features/BloomFeature.h"
#include "KansEditorCore/Renderer/EditorRenderFeatures.h"
#include "Kans3D/Renderer/Features/MaterialPassFeature.h"
#include "Kans3D/Renderer/RenderWorld.h"

namespace Kans
{
    namespace
    {
        // Editor overlays; game views (the scene camera) render without them.
        constexpr const char* EditorOnlyFeatures[] = {"EditorGrid", "EditorSelection", "EditorPreview", "TransformGizmo"};
    } // namespace

    ViewportClient::ViewportClient() :
        m_Renderer(Renderer::Acquire()), m_Target(std::make_unique<ViewTarget>(m_Renderer->GetDevice()))
    {
        // Authored material passes first, then the editor features, in their drawing order.
        m_Features.Register("MaterialOutline",
                            std::make_unique<MaterialPassFeature>("Outline", RenderExtensionPoint::AfterTransparent));
        RegisterDefaultEditorFeatures(m_Features);
        // Post-processing reads the camera's settings (the editor or the scene camera).
        m_Features.Register("Bloom", std::make_unique<BloomFeature>());
    }

    ViewportClient::~ViewportClient()
    {
        m_Last = {};
        m_Target.reset();
        m_Renderer->GetDevice()->WaitIdle();
    }

    void ViewportClient::SetScene(Ref<Scene> scene)
    {
        if (m_Scene && scene && scene != m_Scene)
        {
            const auto& previous         = m_Scene->GetRenderWorld();
            auto&       world            = scene->GetRenderWorld();
            world.Settings               = previous.Settings;
            world.EnvironmentPath        = previous.EnvironmentPath;
            world.EnvironmentRoot        = previous.EnvironmentRoot;
        }
        m_Scene = std::move(scene);
        if (m_Scene && m_Assets)
        {
            m_Scene->SetPrefabAssetManager(&m_Assets->GetAssetManager());
            m_Scene->GetRenderWorld().SetAssetManager(&m_Assets->GetAssetManager());
        }
    }

    void ViewportClient::SetAssetManager(AssetManager* assets)
    {
        // Results and views hold cache leases; drop them before the cache.
        m_Last   = {};
        m_Assets = assets ? m_Renderer->AcquireAssetCache(*assets) : nullptr;
        if (m_Scene)
        {
            m_Scene->SetPrefabAssetManager(assets);
            m_Scene->GetRenderWorld().SetAssetManager(assets);
        }
    }

    void ViewportClient::PrepareEnvironment(EnvironmentPrecomputeSizes sizes)
    {
        if (!m_Scene)
            return;
        m_EnvironmentSizes  = sizes;
        m_EnvironmentSource = m_Scene->GetRenderWorld().EnvironmentPath;
        if (auto lighting = m_Renderer->PrepareEnvironment(m_EnvironmentSource, sizes))
            m_Lighting = std::move(lighting);
    }

    void ViewportClient::RefreshEnvironment()
    {
        // The world names its environment; follow a change made elsewhere.
        const auto& source = m_Scene->GetRenderWorld().EnvironmentPath;
        if (m_EnvironmentSource.empty() || m_EnvironmentSource == source)
            return;
        m_EnvironmentSource = source;
        if (auto lighting = m_Renderer->PrepareEnvironment(m_EnvironmentSource, m_EnvironmentSizes))
            m_Lighting = std::move(lighting);
    }

    void ViewportClient::Resize(glm::uvec2 size)
    {
        m_Size = size;
        m_Target->Resize(size);
    }

    void ViewportClient::RenderEditor(const EditorCamera& camera)
    {
        RenderView view;
        view.Scene.sceneCamera.Position   = camera.GetPosition();
        view.Scene.sceneCamera.viewMatrix = camera.GetViewMatrix();
        view.Scene.sceneCamera.camera     = camera;
        view.DebugMode                    = m_ViewMode;
        view.Grid                         = m_Grid;
        view.AntiAliasing                 = camera.GetAntiAliasing();
        view.Debug                        = m_Debug;
        // Orthographic front/back and side views get the grid plane they look at.
        if (camera.IsOrthographic())
        {
            const glm::vec3 forward = glm::abs(camera.Forward());
            const auto      plane   = forward.y >= forward.x && forward.y >= forward.z ? ViewGrid::Plane::XZ :
                                      forward.z >= forward.x                         ? ViewGrid::Plane::XY :
                                                                                       ViewGrid::Plane::YZ;
            view.Grids = {{plane, glm::vec3(0.0f)}};
        }
        Render(std::move(view), true);
    }

    bool ViewportClient::RenderGame()
    {
        if (!m_Scene)
            return false;
        auto runtime = MakeRuntimeView(*m_Scene, m_Size);
        if (!runtime)
            return false;
        RenderView view;
        view.Scene     = runtime->Scene;
        view.DebugMode = SceneViewMode::Lit;
        m_Highlights           = {}; // Editor intents never reach a game view.
        m_LastEditorHighlights = {};
        m_Preview.reset();
        m_Gizmo.clear();
        Render(std::move(view), false);
        return true;
    }

    void ViewportClient::Render(RenderView view, bool editorFeatures)
    {
        if (!m_Scene)
            return;
        for (const char* name : EditorOnlyFeatures)
            m_Features.SetEnabled(name, editorFeatures);
        RefreshEnvironment();
        view.Viewport            = m_Size;
        view.Assets              = m_Assets;
        view.Features            = &m_Features;
        view.Time                = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_StartTime).count();
        view.ExtractWorld        = true;
        view.DrawEnvironment     = true;
        // The scene's environment settings apply; the editor's maps are reused when the scene
        // has none (or names the same map).
        view.UseWorldEnvironment = true;
        view.EnvironmentMaps     = {m_Lighting->Maps.IrradianceMap, m_Lighting->Maps.RadianceMap, m_EnvironmentSource};
        view.BrdfLUT             = m_Lighting->BrdfLUT;
        view.Highlights          = std::move(m_Highlights);
        if (editorFeatures)
            m_LastEditorHighlights = view.Highlights;
        view.Preview             = std::move(m_Preview);
        if (editorFeatures)
            view.Gizmo = std::move(m_Gizmo);
        m_Gizmo.clear();
        m_Highlights             = {};
        m_Preview.reset();
        m_Last = m_Renderer->Render(m_Scene->GetRenderWorld(), view, *m_Target);
    }

    ScenePickHit ViewportClient::Pick(const glm::mat4& viewProjection, glm::uvec2 pixel)
    {
        if (!m_Scene)
            return {};
        RenderView view;
        view.Assets                       = m_Assets;
        view.Highlights.Isolate           = m_LastEditorHighlights.Isolate;
        view.Viewport                     = m_Size;
        view.Scene.sceneCamera.camera     = Camera(viewProjection);
        view.Scene.sceneCamera.viewMatrix = glm::mat4(1);
        return m_Renderer->PickSceneSurface(m_Scene->GetRenderWorld(), view, pixel);
    }
} // namespace Kans
