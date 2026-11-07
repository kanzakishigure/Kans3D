#include "kspch.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Renderer/RenderView.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Scene/Scene.h"
namespace Kans
{
    void Scene::UpdateUI(const UIInputState& input, const UIViewport& viewport, float seconds)
    {
        m_UIViewport = viewport;
        GetRenderWorld().Synchronize();
        auto         runtime = MakeRuntimeView(*this, glm::uvec2(viewport.Pixels()));
        RenderCamera camera {};
        camera.viewMatrix = glm::mat4(1);
        if (runtime)
            camera = runtime->Scene.sceneCamera;
        auto surfaces = GetRenderWorld().CollectUIs(camera);
        if (!runtime)
            surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [](auto& ui) { return !ui.Screen; }),
                           surfaces.end());
        auto assets = GetRenderWorld().GetAssetManager();
        m_UI.SetFonts(nullptr, [assets](AssetID id) -> Ref<const FontAsset> {
            if (!assets)
                return {};
            auto loaded = assets->Request<FontAsset>(id);
            return loaded.IsReady() ? loaded.Asset : Ref<const FontAsset>();
        });
        m_UI.Process(input,
                     surfaces,
                     viewport,
                     runtime ? camera.camera.GetProjectionMatrix() * camera.viewMatrix : glm::mat4(1),
                     seconds);
    }
} // namespace Kans
