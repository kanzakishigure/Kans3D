#include "kspch.h"
#include "Kans3D/Renderer/2D/TextRenderer.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    namespace RH = RenderHardware;
    namespace
    {
        const char* Vertex         = R"(#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec2 inUV;
layout(location=2) in vec4 inColor;
layout(location=3) in vec4 inRect;
layout(location=4) in vec2 inMetrics;
layout(set=0,binding=0,std140) uniform Draw { mat4 MVP; mat4 World; vec4 Outline; vec4 Shadow; vec4 Effects; vec4 Light; } d;
layout(location=0) out vec2 uv;
layout(location=1) out vec4 color;
layout(location=2) flat out vec4 rect;
layout(location=3) flat out vec2 metrics;
layout(location=4) out vec3 worldPosition;
void main()
{
    uv = inUV;
    color = inColor;
    rect = inRect;
    metrics = inMetrics;
    worldPosition = (d.World * vec4(position, 1)).xyz;
    gl_Position = d.MVP * vec4(position, 1);
}
)";
        const char* Fragment       = R"(#version 450
#include <Kans3D/MSDF.glsl>
layout(set=0,binding=0,std140) uniform Draw { mat4 MVP; mat4 World; vec4 Outline; vec4 Shadow; vec4 Effects; vec4 Light; } d;
layout(set=0,binding=1) uniform sampler2D atlas;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=2) flat in vec4 rect;
layout(location=3) flat in vec2 metrics;
layout(location=4) in vec3 worldPosition;
layout(location=0) out vec4 outputColor;
// Stay half a texel inside the glyph so bilinear taps never mix in a neighbouring glyph's channels.
vec4 inner() { vec2 h=.5/vec2(textureSize(atlas,0)); return vec4(rect.xy+h,rect.zw-h); }
float sdf(vec2 p)
{
    vec4 r = inner();
    vec2 clampedUV = clamp(p, r.xy, r.zw);
    // Extend the alpha distance beyond the glyph padding without reading a neighbouring glyph.
    return texture(atlas, clampedUV).a - length((p-clampedUV) * vec2(textureSize(atlas, 0))) / metrics.x;
}
void main(){
    float range=msdfScreenPixelRange(uv,vec2(textureSize(atlas,0)),metrics.x);
    float pixelsPerEm=range*metrics.y/metrics.x;
    vec4 sampleDistance=texture(atlas,clamp(uv,inner().xy,inner().zw));
    float fill=msdfCoverage(sampleDistance,range);
    if(any(lessThan(uv,rect.xy))||any(greaterThan(uv,rect.zw))) fill=0;
    sampleDistance.a=sdf(uv);
    float outline=d.Effects.x>0 ? msdfOutline(sampleDistance,range,d.Effects.x*pixelsPerEm) : 0;
    float shadow=msdfSoftShadow(sdf(uv-d.Effects.zw*metrics.y/vec2(textureSize(atlas,0))),range,0,d.Effects.y*pixelsPerEm);
    float fa=fill*color.a, oa=outline*d.Outline.a*(1-fa), sa=shadow*d.Shadow.a*(1-fa)*(1-outline*d.Outline.a);
    float a=fa+oa+sa;
    if(a<=0.001) discard;
    outputColor=vec4((color.rgb*fa+d.Outline.rgb*oa+d.Shadow.rgb*sa)/a,a);
}
)";
        const char* ShadowFragment = R"(#version 450
#include <Kans3D/MSDF.glsl>
layout(set=0,binding=0,std140) uniform Draw { mat4 MVP; mat4 World; vec4 Outline; vec4 Shadow; vec4 Effects; vec4 Light; } d;
layout(set=0,binding=1) uniform sampler2D atlas;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=2) flat in vec4 rect;
layout(location=3) flat in vec2 metrics;
layout(location=4) in vec3 worldPosition;
void main(){
    if(any(lessThan(uv,rect.xy))||any(greaterThan(uv,rect.zw))) discard;
    float range=msdfScreenPixelRange(uv,vec2(textureSize(atlas,0)),metrics.x);
    vec2 h=.5/vec2(textureSize(atlas,0));
    if(msdfCoverage(texture(atlas,clamp(uv,rect.xy+h,rect.zw-h)),range)*color.a<0.5) discard;
    gl_FragDepth=d.Light.w>0 ? length(worldPosition-d.Light.xyz)/d.Light.w : gl_FragCoord.z;
}
)";
        struct VertexData
        {
            glm::vec3 Position;
            glm::vec2 UV;
            glm::vec4 Color, Rect;
            glm::vec2 Metrics;
        };
        struct DrawData
        {
            glm::mat4 MVP, World;
            glm::vec4 Outline, Shadow, Effects, Light;
        };
    } // namespace
    void TextRenderer::BeginFrame()
    {
        static uint64_t frame = 0;
        m_Frame = ++frame;
        FontSystem::Default().BeginFrame(m_Frame);
        for (auto it = m_Geometry.begin(); it != m_Geometry.end();)
            if (it->second.Layout.expired())
                it = m_Geometry.erase(it);
            else
                ++it;
    }
    void TextRenderer::Upload(const std::vector<ViewTextDraw>& texts)
    {
        // UI1 owns the work budget. Project fonts, including fallback assets, are tracked by the layouts.
        for (const auto& text : texts)
            for (const auto& q : text.Layout->Get().Glyphs)
            {
                if (!m_Atlases.count(q.Font.get()))
                    m_Atlases[q.Font.get()] = std::make_unique<GpuFontAtlas>(m_Renderer.GetDevice(), q.Font);
            }
        FontSystem::Default().Rasterize(16);
        for (auto& [font, atlas] : m_Atlases)
            atlas->Flush();
    }
    TextRenderer::Geometry& TextRenderer::Prepare(const ViewTextDraw& text)
    {
        auto& geometry = m_Geometry[text.Layout.get()];
        if (geometry.Revision == text.Layout->Revision())
            return geometry;
        geometry          = {};
        geometry.Layout   = text.Layout;
        geometry.Revision = text.Layout->Revision();
        std::map<std::pair<const FontAsset*, uint32_t>, std::vector<VertexData>> pages;
        for (const auto& q : text.Layout->Get().Glyphs)
        {
            if (!q.Ready || q.Position.x == q.Position.z || q.Position.y == q.Position.w)
                continue;
            auto&           vertices = pages[{q.Font.get(), q.Page}];
            const auto&     c        = text.Style;
            const float     size     = text.Layout->Get().SizeUsed;
            const float     emUV     = float(q.Font->GetData().Settings.GlyphSize) / GlyphAtlas::PageSize;
            const glm::vec2 extra = (glm::abs(c.Shadow.Offset) + glm::vec2(c.Shadow.Softness + c.Outline.Width)) * size;
            const glm::vec2 min(q.Position.x - extra.x, q.Position.w - extra.y),
                max(q.Position.z + extra.x, q.Position.y + extra.y);
            const glm::vec2 uvMin = glm::vec2(q.UV.x, q.UV.w) - extra / size * emUV;
            const glm::vec2 uvMax = glm::vec2(q.UV.z, q.UV.y) + extra / size * emUV;
            const glm::vec4 rect  = glm::vec4(q.UV.x, q.UV.w, q.UV.z, q.UV.y) +
                                    glm::vec4(.5f, .5f, -.5f, -.5f) / float(GlyphAtlas::PageSize);
            for (auto t :
                 {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1), glm::vec2(0, 1), glm::vec2(1, 0), glm::vec2(1, 1)})
                vertices.push_back({glm::vec3(glm::mix(min, max, t), 0),
                                    glm::mix(uvMin, uvMax, t),
                                    c.Color,
                                    rect,
                                    {q.Font->GetData().Settings.PixelRange, q.Font->GetData().Settings.GlyphSize}});
        }
        std::vector<VertexData> vertices;
        for (auto& [key, page] : pages)
        {
            Ref<const FontAsset> font;
            for (auto& q : text.Layout->Get().Glyphs)
                if (q.Font.get() == key.first)
                {
                    font = q.Font;
                    break;
                }
            geometry.Batches.push_back({font, key.second, uint32_t(vertices.size()), uint32_t(page.size())});
            vertices.insert(vertices.end(), page.begin(), page.end());
        }
        if (!vertices.empty())
        {
            geometry.Vertices = m_Renderer.GetDevice()->CreateBuffer(
                {vertices.size() * sizeof(VertexData), RH::BufferUsage::Vertex, RH::MemoryUsage::Upload});
            geometry.Vertices->Write(0, vertices.data(), vertices.size() * sizeof(VertexData));
        }
        return geometry;
    }
    void TextRenderer::Draw(RH::CommandList&         commands,
                            const ViewTextDraw&      text,
                            const glm::mat4&         projection,
                            const RH::RenderingDesc& target,
                            bool                     shadow,
                            glm::vec4                pointLight,
                            float                    constantBias,
                            float                    slopeBias,
                            bool                     point)
    {
        auto& g = Prepare(text);
        if (!g.Vertices)
            return;
        auto device = m_Renderer.GetDevice();
        if (!m_Layout)
            m_Layout = device->CreateBindGroupLayout(
                {0, {{0, RH::BindingType::UniformBuffer}, {1, RH::BindingType::CombinedTextureSampler}}});
        RH::PipelineDesc p;
        p.Program =
            m_Renderer.GetInternalProgram(shadow ? "TextShadow" : "Text3D", Vertex, shadow ? ShadowFragment : Fragment);
        p.GroupLayouts                 = {m_Layout};
        p.VertexBindings               = {{0, sizeof(VertexData), false}};
        p.Attributes                   = {{0, 0, offsetof(VertexData, Position), RH::VertexFormat::Float3},
                                          {1, 0, offsetof(VertexData, UV), RH::VertexFormat::Float2},
                                          {2, 0, offsetof(VertexData, Color), RH::VertexFormat::Float4},
                                          {3, 0, offsetof(VertexData, Rect), RH::VertexFormat::Float4},
                                          {4, 0, offsetof(VertexData, Metrics), RH::VertexFormat::Float2}};
        p.Rasterizer.Cull              = text.Style.DoubleSided ? RH::CullMode::None : RH::CullMode::Back;
        p.Rasterizer.Front             = point ? RH::FrontFace::Clockwise : RH::FrontFace::CounterClockwise;
        p.Rasterizer.DepthBiasConstant = constantBias;
        p.Rasterizer.DepthBiasSlope    = slopeBias;
        p.HasDepthStencil              = bool(target.Depth.Target);
        if (p.HasDepthStencil)
            p.DepthStencilFormat = target.Depth.Target->GetDesc().PixelFormat;
        p.DepthStencil.DepthTest    = shadow || !text.Style.AlwaysOnTop;
        p.DepthStencil.DepthWrite   = shadow;
        p.DepthStencil.DepthCompare = RH::CompareOp::LessEqual;
        for (auto& color : target.Colors)
            p.ColorFormats.push_back(color.Target->GetDesc().PixelFormat);
        p.Samples = target.Colors.empty() ? 1 : target.Colors[0].Target->GetDesc().Samples;
        p.Blends.resize(p.ColorFormats.size());
        for (auto& b : p.Blends)
        {
            b.Enabled          = !shadow;
            b.SourceColor      = RH::BlendFactor::SourceAlpha;
            b.DestinationColor = RH::BlendFactor::OneMinusSourceAlpha;
            b.SourceAlpha      = RH::BlendFactor::One;
            b.DestinationAlpha = RH::BlendFactor::OneMinusSourceAlpha;
        }
        commands.BindPipeline(m_Renderer.CachedPipeline(p));
        DrawData data {projection * text.Transform,
                       text.Transform,
                       text.Style.Outline.Color,
                       text.Style.Shadow.Color,
                       {text.Style.Outline.Width,
                        text.Style.Shadow.Softness,
                        text.Style.Shadow.Offset.x,
                        text.Style.Shadow.Offset.y},
                       pointLight};
        auto     buffer = device->CreateBuffer({sizeof(data), RH::BufferUsage::Uniform, RH::MemoryUsage::Upload});
        buffer->Write(0, &data, sizeof(data));
        commands.BindVertexBuffers({{0, g.Vertices, 0}});
        for (auto& batch : g.Batches)
        {
            auto& atlas = *m_Atlases.at(batch.Font.get());
            if (batch.Page >= atlas.Pages().size())
                continue;
            RH::BindGroupDesc group;
            group.Layout  = m_Layout;
            group.Entries = {{0, buffer, 0, sizeof(data), {}, {}},
                             {1, {}, 0, 0, atlas.Pages()[batch.Page], atlas.Sampler()}};
            commands.BindGroup(device->CreateBindGroup(group));
            commands.Draw(batch.Count, 1, batch.First);
        }
    }
    std::vector<RH::Handle<RH::Texture>> TextRenderer::Textures() const
    {
        std::vector<RH::Handle<RH::Texture>> result;
        for (auto& [font, atlas] : m_Atlases)
            result.insert(result.end(), atlas->Pages().begin(), atlas->Pages().end());
        return result;
    }
    const GpuMesh& TextRenderer::BoundsQuad()
    {
        if (!m_BoundsQuad)
        {
            GpuMesh m;
            m.Device      = m_Renderer.GetDevice();
            m.VertexCount = 4;
            m.IndexCount  = 6;
            MeshVertexData vertices[4];
            vertices[0].Position = {0, 0, 0};
            vertices[1].Position = {1, 0, 0};
            vertices[2].Position = {0, 1, 0};
            vertices[3].Position = {1, 1, 0};
            uint32_t indices[6]  = {0, 1, 2, 2, 1, 3};
            m.Vertices = m.Device->CreateBuffer({sizeof(vertices), RH::BufferUsage::Vertex, RH::MemoryUsage::Upload});
            m.Vertices->Write(0, vertices, sizeof(vertices));
            m.Indices = m.Device->CreateBuffer({sizeof(indices), RH::BufferUsage::Index, RH::MemoryUsage::Upload});
            m.Indices->Write(0, indices, sizeof(indices));
            m.LocalBounds.Expand(glm::vec3(0));
            m.LocalBounds.Expand({1, 1, 0});
            m.Sections.push_back({0, 6, 0, m.LocalBounds});
            m.MaterialSlots.push_back({});
            m_BoundsQuad = std::move(m);
        }
        return *m_BoundsQuad;
    }
    MeshDraw TextRenderer::BoundsDraw(const ViewTextDraw& text, const RenderCamera& camera, uint32_t id)
    {
        const auto local = text.Layout->LocalBounds();
        const glm::vec4 b(local.Min.x, local.Min.y, local.Max.x, local.Max.y);
        auto world     = TextWorldTransform(text.Style, text.Transform, camera.Position, camera.viewMatrix);
        auto transform = world * glm::translate(glm::mat4(1), glm::vec3(b.x, b.y, 0)) *
                         glm::scale(glm::mat4(1), glm::vec3(b.z - b.x, b.w - b.y, 1));
        return {&BoundsQuad(), transform, id, 0};
    }
} // namespace Kans
