#pragma once
#include "Kans3D/Renderer/RenderView.h"

namespace Kans
{
    class RenderGraph;
    struct RenderFrame;
    struct StaticShadowKey
    {
        uint64_t       World = 0, Casters = 0;
        glm::mat4      Matrix {1};
        glm::vec4      Position {0};
        ShadowSettings Settings;
        float          Split = 0;
        bool           operator==(const StaticShadowKey& b) const;
    };
    struct StaticShadowCache
    {
        RenderHardware::Handle<RenderHardware::Texture> Depth;
        StaticShadowKey                                 Key;
        uint64_t                                        Casters = 0;
        bool                                            Valid   = false;
        uint32_t                                        Face = 0, X = 0, Y = 0;
        bool Matches(const StaticShadowKey& key) const { return Valid && Key == key; }
    };
    struct StaticShadowImage
    {
        RenderHardware::Handle<RenderHardware::Texture> Live, Depth;
    };
    struct ShadowResources
    {
        RenderHardware::Handle<RenderHardware::Texture>                         Directional;
        std::array<RenderHardware::Handle<RenderHardware::Texture>, 4>          Points;
        RenderHardware::Handle<RenderHardware::Sampler>                         Compare, Raw, CookieSampler;
        RenderHardware::Handle<RenderHardware::Texture>                         Spots, Cookies;
        std::map<std::string, RenderHardware::Handle<RenderHardware::Pipeline>> DepthPipelines;
        std::map<std::string, StaticShadowCache>                                StaticCaches;
        std::map<const RenderHardware::Texture*, StaticShadowImage>             StaticImages;
        std::string                                                             StaticCasterKey;
        std::vector<std::shared_ptr<void>>                                      StaticCasterResources;
        uint64_t                                                                StaticRevision = 0;
        void                                                                    InvalidateStaticCaches()
        {
            for (auto& item : StaticCaches)
                item.second.Valid = false;
        }
        uint64_t StaticCacheBytes() const
        {
            uint64_t bytes = 0;
            for (const auto& item : StaticImages)
                if (item.second.Depth)
                {
                    const auto& d = item.second.Depth->GetDesc();
                    bytes += uint64_t(d.Width) * d.Height * 4 *
                             (d.Dimension == RenderHardware::TextureDimension::Cube ? 6 : 1);
                }
            return bytes;
        }
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
