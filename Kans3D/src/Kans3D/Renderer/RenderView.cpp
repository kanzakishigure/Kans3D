#include "kspch.h"
#include "RenderView.h"
#include <algorithm>
#include "Kans3D/Scene/Entity.h"

namespace Kans
{
    void
    ExtractMeshDraws(RenderView& view, const ViewMeshSubmission& submission, SceneViewMode mode, const GpuMesh* mesh)
    {
        if (!mesh)
            return;
        const auto& component = submission.Mesh;
        for (size_t i = 0; i < mesh->Sections.size(); ++i)
        {
            if (component.Section && *component.Section != i)
                continue;
            const auto& section  = mesh->Sections[i];
            const auto  slot     = section.MaterialSlot;
            const auto  material = slot < component.Materials.size() && component.Materials[slot].IsValid() ?
                                       component.Materials[slot] :
                                       mesh->MaterialSlots[slot].DefaultMaterial;
            view.Draws.push_back({component.Mesh,
                                  submission.Transform,
                                  material,
                                  mode,
                                  submission.Stencil,
                                  section.LocalBounds,
                                  {},
                                  false,
                                  uint32_t(i),
                                  submission.EntityID,
                                  submission.InstanceRoot,
                                  {},
                                  false,
                                  component.CastShadows,
                                  component.ReceiveShadows,
                                  {},
                                  {},
                                  component.RenderCustomDepth,
                                  component.CustomStencil});
        }
    }
    Frustum::Frustum(const glm::mat4& matrix)
    {
        const auto rows = glm::transpose(matrix);
        m_Planes        = {
            rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2]};
    }
    bool Frustum::Intersects(const Bounds& bounds) const
    {
        if (!bounds.IsValid())
            return true; // Unavailable bounds must never hide geometry.
        for (const auto& plane : m_Planes)
        {
            const glm::vec3 support(plane.x >= 0 ? bounds.Max.x : bounds.Min.x,
                                    plane.y >= 0 ? bounds.Max.y : bounds.Min.y,
                                    plane.z >= 0 ? bounds.Max.z : bounds.Min.z);
            const float     distance = glm::dot(glm::vec3(plane), support) + plane.w;
            const float     tolerance =
                1e-5f * (glm::dot(glm::abs(glm::vec3(plane)), glm::abs(support)) + std::abs(plane.w) + 1);
            if (std::isfinite(distance) && distance < -tolerance)
                return false;
        }
        return true;
    }
    void ExtractDrawLists(RenderView& view, const Frustum* frustum)
    {
        view.Culling                = {};
        view.Culling.SectionProxies = view.Draws.size();
        view.MaterialOrder.clear();
        view.Opaque.clear();
        view.Mask.clear();
        view.Blend.clear();
        for (size_t i = 0; i < view.Draws.size(); ++i)
        {
            const auto& draw = view.Draws[i];
            if (frustum && !frustum->Intersects(draw.HasWorldBounds ? draw.WorldBounds :
                                                                      draw.LocalBounds.Transformed(draw.Transform)))
                continue;
            ++view.Culling.InFrustum;
            if (!draw.Ready)
                continue;
            const auto center = glm::vec3(draw.Transform * glm::vec4(draw.LocalBounds.Center(), 1));
            const auto offset = center - view.Scene.sceneCamera.Position;
            view.MaterialOrder.push_back({i, draw.Render, glm::dot(offset, offset)});
        }
        for (size_t i = 0; i < view.Texts.size(); ++i)
        {
            const auto& text = view.Texts[i];
            if (frustum && !frustum->Intersects(text.WorldBounds))
                continue;
            if (text.Style.Text.empty() || !view.Highlights.Isolate.empty())
                continue;
            const auto transform = TextWorldTransform(
                text.Style, text.Transform, view.Scene.sceneCamera.Position, view.Scene.sceneCamera.viewMatrix);
            const auto local = text.Layout->LocalBounds();
            const auto center =
                local.IsValid() ? glm::vec3(transform * glm::vec4(local.Center(), 1)) : glm::vec3(transform[3]);
            const auto          offset = center - view.Scene.sceneCamera.Position;
            MaterialRenderState render;
            render.AlphaMode = MaterialAlphaMode::Blend;
            view.MaterialOrder.push_back({i, render, glm::dot(offset, offset), true, text.Style.AlwaysOnTop});
        }
        for (size_t i = 0; i < view.UIs.size(); ++i)
        {
            const auto& ui = view.UIs[i];
            if (ui.Screen || (frustum && !frustum->Intersects(ui.WorldBounds)))
                continue;
            auto                offset = ui.WorldBounds.Center() - view.Scene.sceneCamera.Position;
            MaterialRenderState render;
            render.AlphaMode = MaterialAlphaMode::Blend;
            view.MaterialOrder.push_back({i, render, glm::dot(offset, offset), false, ui.AlwaysOnTop, true});
        }
        std::stable_sort(view.MaterialOrder.begin(), view.MaterialOrder.end(), [](const auto& a, const auto& b) {
            if (a.AlwaysOnTop != b.AlwaysOnTop)
                return !a.AlwaysOnTop;
            bool aBlend = IsTransparentMaterial(a.Render.AlphaMode);
            bool bBlend = IsTransparentMaterial(b.Render.AlphaMode);
            if (aBlend != bBlend)
                return !aBlend;
            return aBlend && a.DistanceSquared > b.DistanceSquared;
        });
        for (const auto& draw : view.MaterialOrder)
        {
            if (draw.Text || draw.UI)
                continue;
            auto& list = IsTransparentMaterial(draw.Render.AlphaMode)     ? view.Blend :
                         draw.Render.AlphaMode == MaterialAlphaMode::Mask ? view.Mask :
                                                                            view.Opaque;
            list.push_back(draw.DrawIndex);
        }
    }
    EngineFrameUniforms MakeLightingUniforms(const SceneInfo& scene)
    {
        EngineFrameUniforms frame;
        frame.CameraPosition = glm::vec4(scene.sceneCamera.Position, 1);
        const size_t count   = scene.pointLights.empty() ?
                                   (scene.pointLight.Diffuse_Intensity != glm::vec3(0) ? 1 : 0) :
                                   std::min(scene.pointLights.size(), size_t(EnginePointLightLimit));
        for (size_t i = 0; i < count; ++i)
        {
            const auto& light       = scene.pointLights.empty() ? scene.pointLight : scene.pointLights[i];
            frame.PointPositions[i] = glm::vec4(light.Position, 1);
            frame.PointRadiance[i]  = glm::vec4(light.Diffuse_Intensity, 0);
            frame.PointSpecular[i]  = glm::vec4(light.Specular_Intensity, 0);
            if (i < EngineLightCount)
            {
                frame.LightPositions[i] = frame.PointPositions[i];
                frame.LightColors[i]    = frame.PointRadiance[i];
                frame.LightDiffuse[i]   = frame.PointRadiance[i];
            }
        }
        const auto& light           = scene.dirLight;
        const float directionLength = glm::length(light.Dirction);
        if ((scene.HasDirectionalLight || light.Diffuse_Intensity != glm::vec3(0) ||
             light.Ambient_Intensity != glm::vec3(0)) &&
            directionLength > 1e-6f)
        {
            frame.DirectionalDirection = glm::vec4(light.Dirction / directionLength, 1);
            frame.DirectionalRadiance  = glm::vec4(light.Diffuse_Intensity, 0);
            frame.DirectionalSpecular  = glm::vec4(light.Specular_Intensity, 0);
        }
        frame.DirectionalAmbient = glm::vec4(light.Ambient_Intensity, 0);
        frame.LightingInfo       = {
            float(count), scene.sceneCamera.camera.ResolvePostProcess(scene.PostProcess).Exposure, 0, 0};
        return frame;
    }
} // namespace Kans

namespace Kans
{
    std::optional<RenderView> MakeRuntimeView(Scene& scene, glm::uvec2 viewport)
    {
        const auto entity = scene.GetCameraEntity();
        if (!entity)
            return std::nullopt;
        const auto transform = scene.GetWorldTransform(entity);
        RenderView view;
        view.Scene.PostProcess            = scene.GetPostProcessSettings();
        view.Viewport                     = viewport;
        view.Scene.sceneCamera.camera     = entity.GetComponent<CameraComponent>().SceneCamera;
        view.Scene.sceneCamera.Position   = glm::vec3(transform[3]);
        view.Scene.sceneCamera.viewMatrix = glm::inverse(transform);
        return view;
    }
} // namespace Kans
