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
        RenderHardware::Handle<RenderHardware::Sampler>                         Compare, Raw, CookieSampler;
        RenderHardware::Handle<RenderHardware::Texture>                         Spots, Cookies;
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
    struct SpotShadowTile
    {
        uint32_t Light = 0, X = 0, Y = 0, Resolution = 0;
        bool     operator==(const SpotShadowTile& b) const
        {
            return Light == b.Light && X == b.X && Y == b.Y && Resolution == b.Resolution;
        }
    };
    // Screen coverage, distance, UUID priority. A bounded power-of-two atlas reduces the
    // lowest-priority large tiles first; never allocates more than eight maps per view.
    std::vector<SpotShadowTile> AllocateSpotShadows(const SceneInfo&, uint32_t size = 2048, uint32_t cap = 8);
    glm::mat4                   SpotViewProjection(const SpotLight& light);
    // Initialized all-lit descriptors; no render pass is needed for the immutable fallback.
    std::shared_ptr<ShadowResources> CreateShadowFallback(RenderHardware::Device& device);
    // Custom depth draws main geometry only, using the first enabled DepthOnly pass,
    // otherwise ShadowCaster, otherwise built-in Standard depth. No main-depth occlusion.
    GraphTexture                     BuildCustomDepthPass(RenderGraph& graph, RenderFrame& frame);
    void                             BuildShadowPasses(RenderGraph& graph, RenderFrame& frame);
} // namespace Kans
