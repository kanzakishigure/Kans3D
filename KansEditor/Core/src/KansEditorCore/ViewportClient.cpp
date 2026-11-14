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

    ViewportClient::ViewportClient(std::filesystem::path visibilityFile) :
        m_Renderer(Renderer::Acquire()), m_Target(std::make_unique<ViewTarget>(m_Renderer->GetDevice())),
        m_Visibility(std::move(visibilityFile))
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
        m_Last     = {};
        m_Snapshot = {};
        m_Target.reset();
        m_Renderer->GetDevice()->WaitIdle();
    }

    void ViewportClient::SetScene(Ref<Scene> scene, std::string visibilityKey)
    {
        if (m_Scene && scene && scene != m_Scene)
        {
            const auto& previous         = m_Scene->GetRenderWorld();
            auto&       world            = scene->GetRenderWorld();
            world.Settings               = previous.Settings;
            world.EnvironmentPath        = previous.EnvironmentPath;
            world.EnvironmentRoot        = previous.EnvironmentRoot;
        }
        if (scene && (scene != m_Scene || !visibilityKey.empty()))
            m_Visibility.Load(*scene, std::move(visibilityKey));
        if (!scene)
            m_Visibility.Unload();
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
        // Allocate inside the render boundary so a resize failure is retried next frame.
    }

    void ViewportClient::RenderEditor(const EditorCamera& camera, const std::string& name)
    {
        m_Target->TryRender(name, [&] {
            RenderView view;
            if (m_Scene && !m_Scene->GetPhysicsWorld())
                view.HiddenEntities = m_Visibility.Hidden();
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
                                          forward.z >= forward.x                           ? ViewGrid::Plane::XY :
                                                                                             ViewGrid::Plane::YZ;
                view.Grids              = {{plane, glm::vec3(0.0f)}};
            }
            Render(std::move(view), true);
        });
    }

    bool ViewportClient::RenderGame()
    {
        bool       hasCamera = false;
        const bool rendered  = m_Target->TryRender("Game view / camera preview", [&] {
            if (!m_Scene)
                return;
            auto runtime = MakeRuntimeView(*m_Scene, m_Size);
            if (!runtime)
                return;
            RenderView view;
            view.Scene             = runtime->Scene;
            view.DebugMode         = SceneViewMode::Lit;
            m_Highlights           = {}; // Editor intents never reach a game view.
            m_LastEditorHighlights = {};
            m_Preview.reset();
            m_Gizmo.clear();
            hasCamera = true;
            Render(std::move(view), false);
        });
        return !rendered || hasCamera;
    }

    void ViewportClient::Render(RenderView view, bool editorFeatures)
    {
        if (!m_Scene)
            return;
        m_LastWasEditor = editorFeatures;
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

    bool ViewportClient::RequestSnapshot()
    {
        using namespace RenderHardware;
        const auto& colors = m_Target->GetColors();
        if (!m_Scene || colors.empty() || !colors[0] || m_Size.x == 0 || m_Size.y == 0 ||
            colors[0]->GetDesc().PixelFormat != Format::RGBA8Unorm)
            return false;
        const auto& device = m_Renderer->GetDevice();
        const auto  size   = glm::uvec2(colors[0]->GetDesc().Width, colors[0]->GetDesc().Height);
        m_Snapshot = device->CreateBuffer({size_t(size.x) * size.y * 4, BufferUsage::CopyDestination, MemoryUsage::Readback});
        auto commands = device->CreateCommandList();
        commands->CopyTextureToBuffer(colors[0], {}, m_Snapshot);
        device->Submit(commands);
        m_SnapshotSize = size;
        return true;
    }

    std::optional<ViewportClient::Snapshot> ViewportClient::PollSnapshot()
    {
        if (!m_Snapshot || !m_Snapshot->IsReadbackReady())
            return std::nullopt;
        Snapshot shot;
        shot.Width  = m_SnapshotSize.x;
        shot.Height = m_SnapshotSize.y;
        shot.Pixels = m_Snapshot->Read(0, size_t(shot.Width) * shot.Height * 4);
        m_Snapshot  = {};
        const size_t row = size_t(shot.Width) * 4;
        if (m_Renderer->GetDevice()->GetCoordinateConventions().Origin == RenderHardware::FramebufferOrigin::LowerLeft)
            for (size_t y = 0; y < shot.Height / 2; ++y)
                std::swap_ranges(shot.Pixels.begin() + y * row,
                                 shot.Pixels.begin() + (y + 1) * row,
                                 shot.Pixels.begin() + (shot.Height - y - 1) * row);
        for (size_t i = 3; i < shot.Pixels.size(); i += 4)
            shot.Pixels[i] = 255;
        return shot;
    }

    const std::unordered_set<UUID>& ViewportClient::HiddenInEditor() const
    {
        static const std::unordered_set<UUID> empty;
        // Overlay gathering uses the exact flattened snapshot of the image on screen.
        return m_Scene && !m_Scene->GetPhysicsWorld() && m_LastWasEditor ? m_Last.View.HiddenHierarchy : empty;
    }

    ScenePickHit ViewportClient::Pick(const glm::mat4& viewProjection, glm::uvec2 pixel)
    {
        if (!m_Scene)
            return {};
        RenderView view;
        view.Assets                       = m_Assets;
        view.Highlights.Isolate           = m_LastEditorHighlights.Isolate;
        if (m_LastWasEditor && !m_Scene->GetPhysicsWorld())
            view.HiddenEntities = m_Visibility.Hidden();
        view.Viewport                     = m_Size;
        view.Scene.sceneCamera.camera     = Camera(viewProjection);
        view.Scene.sceneCamera.viewMatrix = glm::mat4(1);
        return m_Renderer->PickSceneSurface(m_Scene->GetRenderWorld(), view, pixel);
    }
} // namespace Kans
