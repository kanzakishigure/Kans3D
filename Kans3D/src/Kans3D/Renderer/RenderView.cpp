#include "kspch.h"
#include "RenderView.h"
#include <algorithm>
#include "Kans3D/Renderer/Lighting/ShadowMaps.h"
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
                                   (LightRadiance(scene.pointLight) != glm::vec3(0) ? 1 : 0) :
                                   std::min(scene.pointLights.size(), size_t(EnginePointLightLimit));
        for (size_t i = 0; i < count; ++i)
        {
            const auto& light       = scene.pointLights.empty() ? scene.pointLight : scene.pointLights[i];
            auto        radiance                   = LightRadiance(light, ToCandela(light.Intensity, light.Unit));
            frame.PointPositions[i] = glm::vec4(light.Position, 1);
            frame.PointRadiance[i]                 = glm::vec4(radiance, 0);
            frame.PointSpecular[i]                 = glm::vec4(glm::vec3(light.SpecularScale), 0);
            frame.PointDiffuseScales[i / 4][i % 4] = light.DiffuseScale;
            frame.PointRanges[i / 4][i % 4]        = std::max(0.0f, light.Range);
            if (i < EngineLightCount)
            {
                frame.LightPositions[i] = glm::vec4(light.Position, 1);
                frame.LightColors[i]    = glm::vec4(radiance * light.DiffuseScale, 0);
                frame.LightDiffuse[i]   = frame.LightColors[i];
            }
        }
        const auto& light           = scene.dirLight;
        const float directionLength = glm::length(light.Dirction);
        if ((scene.HasDirectionalLight || LightRadiance(light) != glm::vec3(0)) && directionLength > 1e-6f)
        {
            frame.DirectionalDirection = glm::vec4(light.Dirction / directionLength, 1);
            frame.DirectionalRadiance  = glm::vec4(LightRadiance(light), 0);
            frame.DirectionalSpecular  = glm::vec4(glm::vec3(light.SpecularScale), 0);
            frame.DirectionalScales.x  = light.DiffuseScale;
        }
        size_t spots = std::min(scene.spotLights.size(), size_t(EngineSpotLightLimit));
        for (size_t i = 0; i < spots; ++i)
        {
            const auto& spot           = scene.spotLights[i];
            float       outer          = std::clamp(spot.OuterConeAngle, .01f, 89.0f),
                        inner          = std::clamp(spot.InnerConeAngle, 0.0f, outer);
            frame.SpotPositionRange[i] = glm::vec4(spot.Position, std::max(0.0f, spot.Range));
            auto direction = glm::length(spot.Direction) > 1e-6f ? glm::normalize(spot.Direction) : glm::vec3(0, 0, -1);
            frame.SpotDirectionOuter[i] = glm::vec4(direction, std::cos(glm::radians(outer)));
            frame.SpotRadianceInner[i] =
                glm::vec4(LightRadiance(spot, ToCandela(spot.Intensity, spot.Unit, SpotSolidAngle(inner, outer))),
                          std::cos(glm::radians(inner)));
            frame.SpotScales[i] = glm::vec4(spot.DiffuseScale, spot.SpecularScale, 0, 0);
            frame.SpotViewProjection[i]  = SpotViewProjection(spot);
            float angle                  = glm::radians(std::isfinite(spot.CookieRotation) ? spot.CookieRotation : 0.f);
            auto  scale                  = glm::max(spot.CookieScale, glm::vec2(.001f));
            frame.SpotCookieTransform[i] = {std::cos(angle) / scale.x,
                                            std::sin(angle) / scale.x,
                                            -std::sin(angle) / scale.y,
                                            std::cos(angle) / scale.y};
        }
        const size_t areas = std::min(scene.areaLights.size(), size_t(EngineAreaLightLimit));
        for (size_t i = 0; i < areas; ++i)
        {
            const auto& a = scene.areaLights[i];
            const float width =
                a.Shape == AreaLightShape::Disc ? 2 * std::max(.001f, a.Radius) : std::max(.001f, a.Width);
            const float height         = a.Shape == AreaLightShape::Disc ? width : std::max(.001f, a.Height);
            frame.AreaPositionRange[i] = glm::vec4(a.Position, std::max(0.f, a.Range));
            frame.AreaRightWidth[i]    = glm::vec4(a.Right, width);
            frame.AreaUpHeight[i]      = glm::vec4(a.Up, height);
            const float area = a.Shape == AreaLightShape::Disc ? LightPi * width * width * .25f : width * height;
            frame.AreaRadiance[i] =
                glm::vec4(LightRadiance(a, AreaLightRadiance(a.Intensity, a.Unit, area, a.TwoSided)),
                          a.Shape == AreaLightShape::Disc ? 1 : 0);
            frame.AreaScales[i] = {a.DiffuseScale,
                                   a.SpecularScale,
                                   a.TwoSided ? 1 : 0,
                                   a.SourceTexture.IsValid() || a.SourceTextureHandle ? 1 : 0};
        }
        frame.LightingInfo = {float(count),
                              scene.sceneCamera.camera.ResolvePostProcess(scene.PostProcess).Exposure,
                              float(spots),
                              float(areas)};
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
        view.FrameTimeDelta               = scene.GetFrameTimeDelta();
        view.Viewport                     = viewport;
        view.Scene.sceneCamera.camera     = entity.GetComponent<CameraComponent>().SceneCamera;
        view.Scene.sceneCamera.Position   = glm::vec3(transform[3]);
        view.Scene.sceneCamera.viewMatrix = glm::inverse(transform);
        return view;
    }
} // namespace Kans
