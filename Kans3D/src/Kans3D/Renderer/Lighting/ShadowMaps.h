#pragma once
#include "Kans3D/Renderer/RenderView.h"

namespace Kans
{
    class RenderGraph;
    struct RenderFrame;
    struct ShadowResources
    {
        RenderHardware::Handle<RenderHardware::Texture>                         Directional;
        std::array<RenderHardware::Handle<RenderHardware::Texture>, 4>          Points;
        RenderHardware::Handle<RenderHardware::Sampler>                         Compare, Raw;
        std::map<std::string, RenderHardware::Handle<RenderHardware::Pipeline>> DepthPipelines;
    };
    struct ShadowCascade
    {
        glm::mat4 ViewProjection {1};
        float     Split = 0, BlendStart = 0, Radius = 0, TexelSize = 0, DepthRange = 0;
    };
    std::array<float, 4> ShadowSplits(float nearPlane, float farPlane, uint32_t count, float lambda);
    std::array<ShadowCascade, 4>
                          BuildShadowCascades(const RenderCamera& camera, glm::vec3 direction, ShadowSettings settings);
    std::vector<uint32_t> SelectPointShadows(const SceneInfo& scene, uint32_t cap = 4);
    // Initialized all-lit descriptors; no render pass is needed for the immutable fallback.
    std::shared_ptr<ShadowResources> CreateShadowFallback(RenderHardware::Device& device);
    // Custom depth draws main geometry only, using the first enabled DepthOnly pass,
    // otherwise ShadowCaster, otherwise built-in Standard depth. No main-depth occlusion.
    GraphTexture                     BuildCustomDepthPass(RenderGraph& graph, RenderFrame& frame);
    void                             BuildShadowPasses(RenderGraph& graph, RenderFrame& frame);
} // namespace Kans
