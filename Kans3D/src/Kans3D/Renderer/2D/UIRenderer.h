#pragma once
#include "Kans3D/Renderer/2D/GpuFontAtlas.h"
#include "Kans3D/Renderer/RenderView.h"
namespace Kans
{
    class Renderer;
    // Rectangular clips intersect in canvas space. Rotated panels use their canvas AABB,
    // not a rotated/stencil mask. Screen batches also use scissors; world batches clip in the fragment shader.
    class UIRenderer
    {
    public:
        explicit UIRenderer(Renderer& renderer) : m_Renderer(renderer) {}
        bool Prepare(std::vector<ViewUI>&, RenderAssetCache*, glm::uvec2 viewport);
        void Draw(RenderHardware::CommandList&,
                  const ViewUI&,
                  const glm::mat4& deviceProjection,
                  const RenderHardware::RenderingDesc&,
                  glm::uvec2 viewport);
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> Textures() const;
        struct Statistics
        {
            size_t Batches = 0, Quads = 0;
        };
        const Statistics& Stats() const { return m_Stats; }

    private:
        struct Vertex
        {
            glm::vec2 Position, UV;
            glm::vec4 Color, UVBounds, Metrics, Outline, Shadow, Effects;
        };
        struct Batch
        {
            RenderHardware::Handle<RenderHardware::Texture> Texture;
            RenderHardware::Handle<RenderHardware::Sampler> Sampler;
            glm::vec4                                       Clip;
            uint32_t                                        First = 0, Count = 0;
        };
        struct Geometry
        {
            std::weak_ptr<UIInstance>                      Instance;
            RenderHardware::Handle<RenderHardware::Buffer> Vertices;
            std::vector<Batch>                             Batches;
        };
        void                                                      Build(const ViewUI&, RenderAssetCache*);
        Renderer&                                                 m_Renderer;
        std::map<const UIInstance*, Geometry>                     m_Geometry;
        std::map<const FontAsset*, std::unique_ptr<GpuFontAtlas>> m_Atlases;
        RenderHardware::Handle<RenderHardware::Texture>           m_White;
        RenderHardware::Handle<RenderHardware::Sampler>           m_WhiteSampler;
        RenderHardware::Handle<RenderHardware::BindGroupLayout>   m_Layout;
        Statistics                                                m_Stats;
    };
} // namespace Kans
