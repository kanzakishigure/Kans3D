#include "kspch.h"
#include "Kans3D/Renderer/2D/UIRenderer.h"
#include <glm/gtc/matrix_transform.hpp>
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/UI/UICaret.h"
namespace Kans
{
    namespace RH = RenderHardware;
    namespace
    {
        const char* VertexSource   = R"(#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 inUV;
layout(location=2) in vec4 inColor;
layout(location=3) in vec4 inRect;
layout(location=4) in vec4 inMetrics;
layout(location=5) in vec4 inOutline;
layout(location=6) in vec4 inShadow;
layout(location=7) in vec4 inEffects;
layout(set=0,binding=0,std140) uniform Draw { mat4 MVP; vec4 Clip; vec4 Flags; } d;
layout(location=0) out vec2 uv;
layout(location=1) out vec4 color;
layout(location=2) flat out vec4 rect;
layout(location=3) flat out vec4 metrics;
layout(location=4) flat out vec4 outline;
layout(location=5) flat out vec4 shadow;
layout(location=6) flat out vec4 effects;
layout(location=7) out vec2 canvasPosition;
void main() {
    uv=inUV; color=inColor; rect=inRect; metrics=inMetrics;
    outline=inOutline; shadow=inShadow; effects=inEffects; canvasPosition=position;
    gl_Position=d.MVP*vec4(position,0,1);
})";
        const char* FragmentSource = R"(#version 450
#include <Kans3D/MSDF.glsl>
layout(set=0,binding=0,std140) uniform Draw { mat4 MVP; vec4 Clip; vec4 Flags; } d;
layout(set=0,binding=1) uniform sampler2D image;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=2) flat in vec4 rect;
layout(location=3) flat in vec4 metrics;
layout(location=4) flat in vec4 outline;
layout(location=5) flat in vec4 shadow;
layout(location=6) flat in vec4 effects;
layout(location=7) in vec2 canvasPosition;
layout(location=0) out vec4 outputColor;
vec3 linearColor(vec3 c) { return mix(c/12.92,pow((max(c,vec3(0))+.055)/1.055,vec3(2.4)),greaterThan(c,vec3(.04045))); }
vec4 tint(vec4 c) { return vec4(d.Flags.x>0 ? linearColor(c.rgb) : c.rgb,c.a); }
// Stay half a texel inside the glyph so bilinear taps never mix in a neighbouring glyph's channels.
vec4 inner() { vec2 h=.5/vec2(textureSize(image,0)); return vec4(rect.xy+h,rect.zw-h); }
float sdf(vec2 p) { vec4 r=inner(); vec2 q=clamp(p,r.xy,r.zw); return texture(image,q).a-length((p-q)*vec2(textureSize(image,0)))/metrics.x; }
void main() {
    if (any(lessThan(canvasPosition,d.Clip.xy)) || any(greaterThanEqual(canvasPosition,d.Clip.zw))) discard;
    vec4 c=tint(color);
    if(metrics.z<.5) {
        vec2 p=metrics.w>.5 ? mix(rect.xy,rect.zw,fract(uv)) : uv;
        outputColor=texture(image,p)*c;
    } else {
        float range=msdfScreenPixelRange(uv,vec2(textureSize(image,0)),metrics.x);
        float pixelsPerEm=range*metrics.y/metrics.x;
        vec4 distance=texture(image,clamp(uv,inner().xy,inner().zw));
        float fill=msdfCoverage(distance,range);
        if(any(lessThan(uv,rect.xy)) || any(greaterThan(uv,rect.zw))) fill=0;
        distance.a=sdf(uv);
        float edge=effects.x>0 ? msdfOutline(distance,range,effects.x*pixelsPerEm) : 0;
        float shade=msdfSoftShadow(sdf(uv-effects.zw*metrics.y/vec2(textureSize(image,0))),range,0,effects.y*pixelsPerEm);
        vec4 o=tint(outline), s=tint(shadow);
        float fa=fill*c.a, oa=edge*o.a*(1-fa), sa=shade*s.a*(1-fa)*(1-edge*o.a), a=fa+oa+sa;
        outputColor=vec4(a>0 ? (c.rgb*fa+o.rgb*oa+s.rgb*sa)/a : vec3(0),a*metrics.w);
    }
    if(outputColor.a<=.001) discard;
})";
        template<class F>
        void Walk(const std::vector<UIRuntimeWidget>& nodes, F&& f)
        {
            for (auto& w : nodes)
            {
                f(w);
                Walk(w.Children, f);
            }
        }
    } // namespace
    bool UIRenderer::Prepare(std::vector<ViewUI>& uis, RenderAssetCache* assets, glm::uvec2 viewport)
    {
        bool complete = true;
        m_Stats       = {};
        if (!m_White)
        {
            uint32_t pixel = 0xffffffff;
            // Use the renderer device, including standalone GPU fixtures.
            RH::TextureDesc d;
            d.Width = d.Height = 1;
            d.Usage            = RH::TextureUsage::Sampled | RH::TextureUsage::CopyDestination;
            m_White            = m_Renderer.GetDevice()->CreateTexture(d);
            auto staging =
                m_Renderer.GetDevice()->CreateBuffer({4, RH::BufferUsage::CopySource, RH::MemoryUsage::Upload});
            staging->Write(0, &pixel, 4);
            auto commands = m_Renderer.GetDevice()->CreateCommandList();
            commands->CopyBufferToTextureRegion(staging, 0, m_White, 0, 0, 1, 1);
            m_Renderer.GetDevice()->Submit(commands);
            m_WhiteSampler = m_Renderer.GetDevice()->CreateSampler({});
        }
        auto fontsByID = [&](AssetID id) -> Ref<const FontAsset> {
            if (!assets)
                return {};
            auto s = assets->GetAssetManager().Request<FontAsset>(id);
            complete &= s.State != AssetLoadState::Loading;
            return s.IsReady() ? s.Asset : Ref<const FontAsset>();
        };
        auto& fonts = FontSystem::Default();
        fonts.SetResolver(fontsByID);
        for (auto& ui : uis)
        {
            if (!ui.Instance)
            {
                complete = false;
                continue;
            }
            auto refresh = ui.Instance->Refresh();
            if (std::holds_alternative<AssetError>(refresh))
                complete = false;
            if (!ui.Instance->Data())
                continue;
            ui.Instance->Update(
                ui.Screen ? glm::vec2(viewport) : ui.Instance->Data()->ReferenceResolution, &fonts, fontsByID);
            Walk(ui.Instance->Widgets(), [&](const auto& w) {
                if (w.Visible && (w.Data.Type == UIWidgetType::Image || UIInteractive(w.Data.Type)) && assets)
                {
                    auto id = w.HasVisualTexture ? w.VisualTexture :
                                                   AssetID::FromString(UIProperty<std::string>(w.Data, "Texture", ""));
                    if (id.IsValid())
                        complete &=
                            assets->GetAssetManager().Request<TextureAsset>(id).State != AssetLoadState::Loading;
                }
                for (auto& q : w.Text.Glyphs)
                    if (!m_Atlases.count(q.Font.get()))
                        m_Atlases[q.Font.get()] = std::make_unique<GpuFontAtlas>(m_Renderer.GetDevice(), q.Font);
            });
        }
        fonts.Rasterize(16);
        for (auto& [font, atlas] : m_Atlases)
            atlas->Flush();
        for (auto& ui : uis)
        {
            if (!ui.Instance || !ui.Instance->Data())
                continue;
            ui.Instance->Update(
                ui.Screen ? glm::vec2(viewport) : ui.Instance->Data()->ReferenceResolution, &fonts, fontsByID);
            Walk(ui.Instance->Widgets(), [&](const auto& w) {
                if (w.Visible)
                    for (auto& q : w.Text.Glyphs)
                        complete &= q.Ready || q.Position.x == q.Position.z || q.Position.y == q.Position.w;
            });
            Build(ui, assets);
        }
        fonts.SetResolver({});
        for (auto it = m_Geometry.begin(); it != m_Geometry.end();)
            if (it->second.Instance.expired())
                it = m_Geometry.erase(it);
            else
                ++it;
        return complete;
    }
    void UIRenderer::Build(const ViewUI& ui, RenderAssetCache* assets)
    {
        Geometry g;
        g.Instance = ui.Instance;
        std::vector<Vertex>                                 vertices;
        std::map<const Texture2D*, RH::Handle<RH::Sampler>> samplers;
        auto                                                quad = [&](const UIRuntimeWidget&  w,
                                                                       glm::vec4               box,
                                                                       glm::vec4               uv,
                                                                       glm::vec4               color,
                                                                       RH::Handle<RH::Texture> texture,
                                                                       RH::Handle<RH::Sampler> sampler,
                                                                       glm::vec4               uvBounds,
                                                                       glm::vec4               metrics = glm::vec4(0),
                                                                       glm::vec4               outline = glm::vec4(0),
                                                                       glm::vec4               shadow  = glm::vec4(0),
                                                                       glm::vec4               effects = glm::vec4(0)) {
            auto clip = w.Clip;
            if (w.Data.Type == UIWidgetType::InputField)
                clip = {glm::max(glm::vec2(clip), glm::vec2(w.Bounds)),
                        glm::min(glm::vec2(clip.z, clip.w), glm::vec2(w.Bounds.z, w.Bounds.w))};
            if (!texture || box.x == box.z || box.y == box.w || clip.z <= clip.x || clip.w <= clip.y)
                return;
            if (g.Batches.empty() || g.Batches.back().Texture != texture || g.Batches.back().Sampler != sampler ||
                g.Batches.back().Clip != clip)
                g.Batches.push_back({texture, sampler, clip, uint32_t(vertices.size()), 0});
            const glm::vec2 pos[] = {{box.x, box.y}, {box.z, box.y}, {box.z, box.w}, {box.x, box.w}};
            const glm::vec2 tex[] = {{uv.x, uv.y}, {uv.z, uv.y}, {uv.z, uv.w}, {uv.x, uv.w}};
            if (metrics.z > .5f)
                metrics.w = w.Opacity; // Apply inherited opacity after composing text fill/effects.
            else
                color.a *= w.Opacity;
            for (int i : {0, 1, 2, 2, 3, 0})
                vertices.push_back({glm::vec2(w.Transform * glm::vec4(pos[i], 0, 1)),
                                    tex[i],
                                    color,
                                    uvBounds,
                                    metrics,
                                    outline,
                                    shadow,
                                    effects});
            g.Batches.back().Count += 6;
            ++m_Stats.Quads;
        };
        Walk(ui.Instance->Widgets(), [&](const auto& w) {
            if (!w.Visible)
                return;
            const auto& p = w.Data.Properties;
            if (w.Data.Type == UIWidgetType::Panel)
            {
                auto c = UIVec4(p["BackgroundColor"]) * w.Tint;
                if (c.a > 0)
                    quad(w, {0, 0, w.Size.x, w.Size.y}, {0, 0, 1, 1}, c, m_White, m_WhiteSampler, {});
            }
            else if (w.Data.Type == UIWidgetType::Image || UIInteractive(w.Data.Type))
            {
                auto id      = w.HasVisualTexture ? w.VisualTexture :
                                                    AssetID::FromString(UIProperty<std::string>(w.Data, "Texture", ""));
                auto image   = assets && id.IsValid() ? assets->GetTexture(id) : Ref<Texture2D>();
                auto texture = image ? image->GetResource() : m_White;
                auto sampler = m_WhiteSampler;
                if (image)
                {
                    auto& cached = samplers[image.get()];
                    if (!cached)
                        cached = m_Renderer.GetDevice()->CreateSampler(image->GetSampling());
                    sampler = cached;
                }
                glm::vec2 texSize = image ? glm::vec2(image->GetWidth(), image->GetHeight()) : glm::vec2(1);
                auto      uv = UIVec4(p["UVRect"], {0, 0, 1, 1}), color = UIVec4(p["Color"], glm::vec4(1)) * w.Tint;
                auto      mode = UIProperty<std::string>(w.Data, "DrawMode", "Simple");
                std::swap(uv.y, uv.w); // Image UVRect is min/max in the engine bottom-left texture convention.
                glm::vec2 min(0), max = w.Size, source = texSize * glm::abs(glm::vec2(uv.z - uv.x, uv.w - uv.y));
                if (UIProperty<bool>(w.Data, "PreserveAspect", false) && source.x > 0 && source.y > 0)
                {
                    auto size = source * std::min(w.Size.x / source.x, w.Size.y / source.y);
                    min       = (w.Size - size) * .5f;
                    max       = min + size;
                }
                if (mode == "Sliced")
                {
                    auto  b  = glm::max(UIVec4(p["Borders"]), glm::vec4(0));
                    float sx = b.x + b.z > source.x ? source.x / std::max(.001f, b.x + b.z) : 1,
                          sy = b.y + b.w > source.y ? source.y / std::max(.001f, b.y + b.w) : 1;
                    b *= glm::vec4(sx, sy, sx, sy);
                    auto  border = b * ui.Instance->PixelScale();
                    float fx     = std::min(1.f, (max.x - min.x) / std::max(.001f, border.x + border.z)),
                          fy     = std::min(1.f, (max.y - min.y) / std::max(.001f, border.y + border.w));
                    float x[]    = {min.x, min.x + border.x * fx, max.x - border.z * fx, max.x},
                          y[]    = {min.y, min.y + border.y * fy, max.y - border.w * fy, max.y};
                    float u[]    = {uv.x,
                                    uv.x + (uv.z - uv.x) * b.x / std::max(.001f, source.x),
                                    uv.z - (uv.z - uv.x) * b.z / std::max(.001f, source.x),
                                    uv.z};
                    float v[]    = {uv.y,
                                    uv.y + (uv.w - uv.y) * b.y / std::max(.001f, source.y),
                                    uv.w - (uv.w - uv.y) * b.w / std::max(.001f, source.y),
                                    uv.w};
                    for (int j = 0; j < 3; ++j)
                        for (int i = 0; i < 3; ++i)
                            quad(w,
                                 {x[i], y[j], x[i + 1], y[j + 1]},
                                 {u[i], v[j], u[i + 1], v[j + 1]},
                                 color,
                                 texture,
                                 sampler,
                                 uv);
                }
                else if (mode == "Tiled")
                {
                    auto repeat = (max - min) / glm::max(source * ui.Instance->PixelScale(), glm::vec2(.001f));
                    quad(w, {min, max}, {0, 0, repeat.x, repeat.y}, color, texture, sampler, uv, {0, 0, 0, 1});
                }
                else
                    quad(w, {min, max}, uv, color, texture, sampler, uv);
            }
            if (w.Data.Type == UIWidgetType::Text || w.Data.Type == UIWidgetType::InputField)
            {
                auto style = UITextStyle(w.Data, ui.Instance->PixelScale());
                style.Color *= w.Tint;
                if (w.Data.Type == UIWidgetType::InputField && w.Anchor != w.Caret)
                {
                    auto lo = std::min(w.Anchor, w.Caret), hi = std::max(w.Anchor, w.Caret);
                    for (size_t i = 1; i < w.Text.Carets.size(); ++i)
                    {
                        auto a = w.Text.Carets[i - 1], b = w.Text.Carets[i];
                        if (a.Byte < lo || a.Byte >= hi || a.Line != b.Line)
                            continue;
                        auto start = a.Position - w.TextScroll, end = b.Position - w.TextScroll;
                        quad(w,
                             {start.x, start.y - w.Text.SizeUsed, end.x, end.y + w.Text.SizeUsed * .2f},
                             {0, 0, 1, 1},
                             {.2f, .45f, .9f, .45f},
                             m_White,
                             m_WhiteSampler,
                             {});
                    }
                }
                for (auto& q : w.Text.Glyphs)
                {
                    if (!q.Ready || q.Position.x == q.Position.z || q.Position.y == q.Position.w)
                        continue;
                    auto found = m_Atlases.find(q.Font.get());
                    if (found == m_Atlases.end() || q.Page >= found->second->Pages().size())
                        continue;
                    auto& atlas = *found->second;
                    auto  size  = w.Text.SizeUsed;
                    auto  extra =
                        (glm::abs(style.Shadow.Offset) + glm::vec2(style.Shadow.Softness + style.Outline.Width)) * size;
                    float emUV     = float(q.Font->GetData().Settings.GlyphSize) / GlyphAtlas::PageSize;
                    auto  texExtra = extra / std::max(.001f, size) * emUV;
                    quad(w,
                         q.Position + glm::vec4(-extra, extra) - glm::vec4(w.TextScroll, w.TextScroll),
                         q.UV + glm::vec4(-texExtra.x, texExtra.y, texExtra.x, -texExtra.y),
                         style.Color,
                         atlas.Pages()[q.Page],
                         atlas.Sampler(),
                         {q.UV.x, q.UV.w, q.UV.z, q.UV.y},
                         {float(q.Font->GetData().Settings.PixelRange),
                          float(q.Font->GetData().Settings.GlyphSize),
                          1,
                          0},
                         style.Outline.Color,
                         style.Shadow.Color,
                         {style.Outline.Width, style.Shadow.Softness, style.Shadow.Offset});
                }
                if (w.Data.Type == UIWidgetType::InputField && w.CaretVisible)
                {
                    auto p = UICaretPosition(w) - w.TextScroll;
                    quad(w,
                         {p.x, p.y - w.Text.SizeUsed, p.x + 1.5f, p.y + w.Text.SizeUsed * .2f},
                         {0, 0, 1, 1},
                         style.Color,
                         m_White,
                         m_WhiteSampler,
                         {});
                }
            }
        });
        if (!vertices.empty())
        {
            g.Vertices = m_Renderer.GetDevice()->CreateBuffer(
                {vertices.size() * sizeof(Vertex), RH::BufferUsage::Vertex, RH::MemoryUsage::Upload});
            g.Vertices->Write(0, vertices.data(), vertices.size() * sizeof(Vertex));
        }
        m_Stats.Batches += g.Batches.size();
        m_Geometry[ui.Instance.get()] = std::move(g);
    }
    void UIRenderer::Draw(RH::CommandList&         commands,
                          const ViewUI&            ui,
                          const glm::mat4&         projection,
                          const RH::RenderingDesc& target,
                          glm::uvec2               viewport)
    {
        auto found = m_Geometry.find(ui.Instance.get());
        if (found == m_Geometry.end() || !found->second.Vertices)
            return;
        auto& g      = found->second;
        auto  device = m_Renderer.GetDevice();
        if (!m_Layout)
            m_Layout = device->CreateBindGroupLayout(
                {0, {{0, RH::BindingType::UniformBuffer}, {1, RH::BindingType::CombinedTextureSampler}}});
        RH::PipelineDesc p;
        p.Program         = m_Renderer.GetInternalProgram("UI", VertexSource, FragmentSource);
        p.GroupLayouts    = {m_Layout};
        p.VertexBindings  = {{0, sizeof(Vertex), false}};
        p.Attributes      = {{0, 0, offsetof(Vertex, Position), RH::VertexFormat::Float2},
                             {1, 0, offsetof(Vertex, UV), RH::VertexFormat::Float2},
                             {2, 0, offsetof(Vertex, Color), RH::VertexFormat::Float4},
                             {3, 0, offsetof(Vertex, UVBounds), RH::VertexFormat::Float4},
                             {4, 0, offsetof(Vertex, Metrics), RH::VertexFormat::Float4},
                             {5, 0, offsetof(Vertex, Outline), RH::VertexFormat::Float4},
                             {6, 0, offsetof(Vertex, Shadow), RH::VertexFormat::Float4},
                             {7, 0, offsetof(Vertex, Effects), RH::VertexFormat::Float4}};
        p.Rasterizer.Cull = ui.DoubleSided || ui.Screen ? RH::CullMode::None : RH::CullMode::Back;
        // Canvas +Y points down; mapping it to a world canvas reverses triangle winding.
        p.Rasterizer.Front = RH::FrontFace::Clockwise;
        p.HasDepthStencil  = bool(target.Depth.Target);
        if (p.HasDepthStencil)
            p.DepthStencilFormat = target.Depth.Target->GetDesc().PixelFormat;
        p.DepthStencil.DepthTest    = !ui.Screen && !ui.AlwaysOnTop;
        p.DepthStencil.DepthWrite   = false;
        p.DepthStencil.DepthCompare = RH::CompareOp::LessEqual;
        for (auto& color : target.Colors)
            p.ColorFormats.push_back(color.Target->GetDesc().PixelFormat);
        p.Samples = target.Colors[0].Target->GetDesc().Samples;
        p.Blends.resize(p.ColorFormats.size());
        for (auto& b : p.Blends)
        {
            b.Enabled          = true;
            b.SourceColor      = RH::BlendFactor::SourceAlpha;
            b.DestinationColor = RH::BlendFactor::OneMinusSourceAlpha;
            b.SourceAlpha      = RH::BlendFactor::One;
            b.DestinationAlpha = RH::BlendFactor::OneMinusSourceAlpha;
        }
        commands.BindPipeline(m_Renderer.CachedPipeline(p));
        commands.BindVertexBuffers({{0, g.Vertices, 0}});
        for (auto& b : g.Batches)
        {
            struct Uniform
            {
                glm::mat4 MVP;
                glm::vec4 Clip, Flags;
            } data {projection * ui.Transform, b.Clip, {ui.Screen ? 1.f : 0.f, 0, 0, 0}};
            auto buffer = device->CreateBuffer({sizeof(data), RH::BufferUsage::Uniform, RH::MemoryUsage::Upload});
            buffer->Write(0, &data, sizeof(data));
            commands.BindGroup(device->CreateBindGroup(
                {m_Layout, {{0, buffer, 0, sizeof(data), {}, {}}, {1, {}, 0, 0, b.Texture, b.Sampler}}}));
            glm::vec2 min(0), max(viewport);
            if (ui.Screen)
            {
                min = glm::clamp(glm::floor(glm::vec2(b.Clip)), min, max);
                max = glm::clamp(glm::ceil(glm::vec2(b.Clip.z, b.Clip.w)), min, max);
            }
            commands.SetScissor({int32_t(min.x), int32_t(min.y), uint32_t(max.x - min.x), uint32_t(max.y - min.y)});
            commands.Draw(b.Count, 1, b.First);
        }
        commands.SetScissor({0, 0, viewport.x, viewport.y});
    }
    std::vector<RH::Handle<RH::Texture>> UIRenderer::Textures() const
    {
        std::vector<RH::Handle<RH::Texture>> out;
        for (auto& [key, g] : m_Geometry)
            for (auto& b : g.Batches)
                if (std::find(out.begin(), out.end(), b.Texture) == out.end())
                    out.push_back(b.Texture);
        return out;
    }
} // namespace Kans
