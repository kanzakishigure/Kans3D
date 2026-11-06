#pragma once
#include "Kans3D/Renderer/2D/GpuFontAtlas.h"
#include "Kans3D/Renderer/RenderView.h"
#include "Kans3D/Renderer/Resource/SelectionTools.h"
namespace Kans
{
    class Renderer;
    class TextRenderer
    {
    public:
        explicit TextRenderer(Renderer& renderer) : m_Renderer(renderer) {}
        void                                                         BeginFrame();
        void                                                         Upload(const std::vector<ViewTextDraw>&);
        void                                                         Draw(RenderHardware::CommandList&,
                                                                          const ViewTextDraw&,
                                                                          const glm::mat4& deviceViewProjection,
                                                                          const RenderHardware::RenderingDesc&,
                                                                          bool      shadow       = false,
                                                                          glm::vec4 pointLight   = {},
                                                                          float     constantBias = 0,
                                                                          float     slopeBias    = 0,
                                                                          bool      point        = false);
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> Textures() const;
        const GpuMesh&                                               BoundsQuad();
        MeshDraw BoundsDraw(const ViewTextDraw&, const RenderCamera&, uint32_t id);

    private:
        struct Batch
        {
            Ref<const FontAsset> Font;
            uint32_t             Page = 0, First = 0, Count = 0;
        };
        struct Geometry
        {
            std::weak_ptr<TextLayoutCache>                 Layout;
            uint64_t                                       Revision = 0;
            RenderHardware::Handle<RenderHardware::Buffer> Vertices;
            std::vector<Batch>                             Batches;
        };
        Geometry&                                                 Prepare(const ViewTextDraw&);
        Renderer&                                                 m_Renderer;
        uint64_t                                                  m_Frame = 0;
        std::map<const FontAsset*, std::unique_ptr<GpuFontAtlas>> m_Atlases;
        std::map<const TextLayoutCache*, Geometry>                m_Geometry;
        RenderHardware::Handle<RenderHardware::BindGroupLayout>   m_Layout;
        std::optional<GpuMesh>                                    m_BoundsQuad;
    };
} // namespace Kans
