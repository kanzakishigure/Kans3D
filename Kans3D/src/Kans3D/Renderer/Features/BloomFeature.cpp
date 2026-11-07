#include "kspch.h"
#include "BloomFeature.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    std::vector<glm::uvec2> BloomFeature::Levels(glm::uvec2 viewport)
    {
        // Seven levels at 1080p; doubling height adds one. No width-dependent diffusion.
        const int count =
            std::clamp(int(std::floor(std::log2(std::max(1u, viewport.y) / 1080.0f * (540.0f / 8.0f)))) + 1, 1, 10);
        auto                    size = glm::max(viewport / 2u, glm::uvec2(1));
        std::vector<glm::uvec2> levels {size};
        for (int i = 1; i < count; ++i)
        {
            auto next = glm::max(size / 2u, glm::uvec2(1));
            if (std::min(next.x, next.y) < 8)
                break;
            levels.push_back(next);
            size = next;
        }
        return levels;
    }
    void BloomFeature::Prepare(RenderView& view, RenderFrame& frame)
    {
        view.BloomMipCount     = 0;
        const auto b           = view.GetPostProcessSettings().Bloom;
        frame.DeferToneMapping = b.Enabled && b.Intensity > 0;
    }
    void BloomFeature::AddPasses(RenderExtensionPoint, const RenderFeatureContext& context)
    {
        auto*      frame = context.Frame;
        const auto b     = context.View.GetPostProcessSettings().Bloom;
        if (!frame || !b.Enabled || b.Intensity == 0)
            return;
        auto&       graph    = context.Graph;
        const float exposure = context.View.GetPostProcessSettings().Exposure;
        const auto  hdr      = graph.ImportTexture("SceneHDR", frame->Output.GetHDR());
        if (!frame->PostProcessHDR)
        {
            const auto display = graph.ImportTexture("BloomDisplayInput", frame->Output.GetColors()[0]);
            graph.AddPass("Bloom HDR prefill",
                          {{display, GraphAccess::SampledRead}, {hdr, GraphAccess::ColorWrite}},
                          [frame, exposure](auto& c) {
                              frame->Services.GetToneMapping().Record(c.Device,
                                                                      c.Commands,
                                                                      frame->Output.GetColors()[0],
                                                                      {frame->Output.GetHDR()},
                                                                      exposure,
                                                                      true,
                                                                      frame->View.GetPostProcessSettings().ToneMapping);
                          });
        }
        auto dirt = b.DirtTexture.IsValid() && b.DirtIntensity > 0 && frame->Assets() ?
                        frame->Assets()->GetTextureResource(b.DirtTexture) :
                        RenderHardware::Handle<RenderHardware::Texture> {};
        frame->View.BloomMipCount = uint32_t(Levels(context.View.Viewport).size());
        AddBloomPasses(graph, hdr, context.View.Viewport, b, exposure, dirt);
        frame->PostProcessHDR = true;
    }
    GraphTexture BloomFeature::AddBloomPasses(RenderGraph&                                    graph,
                                              GraphTexture                                    hdr,
                                              glm::uvec2                                      viewport,
                                              const BloomSettings&                            settings,
                                              float                                           exposure,
                                              RenderHardware::Handle<RenderHardware::Texture> dirt)
    {
        using namespace RenderHardware;
        const auto b = settings.Validated();
        if (!b.Enabled || b.Intensity == 0)
            return hdr;
        const auto levels = Levels(viewport);
        Parameters parameters;
        parameters.Curve   = {b.Threshold, b.Threshold * b.Knee, b.Clamp, ToneMappingPass::EffectiveExposure(exposure)};
        parameters.Options = {0, b.Scatter, b.HighQualityFiltering ? 1 : 0, b.Intensity};
        parameters.Tint    = glm::vec4(b.Tint, dirt ? b.DirtIntensity : 0);
        auto create        = [&](std::string name, glm::uvec2 size) {
            TextureDesc desc;
            desc.Width       = size.x;
            desc.Height      = size.y;
            desc.PixelFormat = Format::RGBA16Float;
            desc.Usage       = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::Sampled |
                               RenderHardware::TextureUsage::CopySource;
            return graph.CreateTexture(std::move(name), desc);
        };
        auto add = [&](std::string  name,
                       unsigned     stage,
                       GraphTexture source,
                       GraphTexture fine,
                       GraphTexture target,
                       glm::uvec2   size,
                       GraphTexture dirtID) {
            auto p       = parameters;
            p.OutputSize = {float(size.x), float(size.y), 0, 0};
            p.Options.x  = float(stage);
            std::vector<GraphUse> uses {{source, GraphAccess::SampledRead}};
            if (fine != source)
                uses.push_back({fine, GraphAccess::SampledRead});
            if (dirtID != source && dirtID != fine)
                uses.push_back({dirtID, GraphAccess::SampledRead});
            uses.push_back({target, stage >= 4 ? GraphAccess::ColorReadWrite : GraphAccess::ColorWrite});
            graph.AddPass(std::move(name), std::move(uses), [this, stage, source, fine, target, dirtID, p](auto& c) {
                Record(c, stage, source, fine, target, dirtID, p);
            });
        };
        std::vector<GraphTexture> down;
        for (size_t i = 0; i < levels.size(); ++i)
        {
            const auto image  = create("BloomDown" + std::to_string(i), levels[i]);
            const auto source = i ? down.back() : hdr;
            add(i ? (i == 1 ? "Bloom Downsample Karis 1" : "Bloom Downsample 13-tap " + std::to_string(i)) :
                    "Bloom Prefilter",
                i ? (i == 1 ? 1 : 2) : 0,
                source,
                source,
                image,
                levels[i],
                source);
            down.push_back(image);
        }
        auto bloom = down.back();
        for (int i = int(down.size()) - 2; i >= 0; --i)
        {
            auto up = create("BloomUp" + std::to_string(i), levels[i]);
            // Keep the distribution over screen-space scales anchored at 1080p. Missing
            // subpixel fine levels fold their convex weights into level zero; extra fine
            // levels at higher resolutions pass through until the reference scale. Each
            // reconstruction remains lerp(fine, tent(coarse), scatter), with no energy gain.
            const float offset   = std::log2(std::max(1u, viewport.y) / 1080.0f);
            const float previous = i ? std::max(0.0f, float(i) - offset) : 0.0f;
            const float exponent = std::max(0.0f, float(i + 1) - offset) - previous;
            parameters.Options.y = exponent == 0 ? 1.0f : std::pow(b.Scatter, exponent);
            add("Bloom Upsample tent " + std::to_string(i), 3, bloom, down[i], up, levels[i], bloom);
            bloom = up;
        }
        const auto dirtID = dirt ? graph.ImportTexture("BloomDirt", std::move(dirt)) : bloom;
        add("Bloom Composite", b.Mode == BloomSettings::Additive ? 4 : 5, bloom, bloom, hdr, viewport, dirtID);
        return bloom;
    }
    void BloomFeature::Record(RenderGraphContext& context,
                              unsigned            stage,
                              GraphTexture        source,
                              GraphTexture        fine,
                              GraphTexture        target,
                              GraphTexture        dirt,
                              const Parameters&   parameters)
    {
        using namespace RenderHardware;
        auto& device = context.Device;
        if (!m_Shader)
        {
            auto built = CreateInternalShader(device,
                                              "Jimenez bloom",
                                              R"(#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p*2-1,0,1);
})",
                                              R"(#version 450
layout(set=0,binding=0,std140) uniform Parameters { vec4 OutputSize; vec4 Curve; vec4 Options; vec4 Tint; } p;
layout(set=0,binding=1) uniform sampler2D InputColor;
layout(set=0,binding=2) uniform sampler2D FineColor;
layout(set=0,binding=3) uniform sampler2D Dirt;
layout(location=0) out vec4 Color;
float luma(vec3 c) { return dot(c,vec3(0.2126,0.7152,0.0722)); }
vec3 sampleAt(vec2 uv, vec2 offset) { return texture(InputColor,uv+offset/vec2(textureSize(InputColor,0))).rgb; }
vec3 prefilter(vec3 c) {
    c = max(c,vec3(0))*p.Curve.w;
    float brightness = max(c.r,max(c.g,c.b));
    float soft = clamp(brightness-p.Curve.x+p.Curve.y,0,2*p.Curve.y);
    soft = soft*soft/max(4*p.Curve.y,1e-8);
    c *= max(soft,brightness-p.Curve.x)/max(brightness,1e-8);
    // Clamp in exposed luminance units, then return to the scene's radiance units.
    c *= min(1,p.Curve.z/max(luma(c),1e-8));
    return min(c/p.Curve.w,vec3(65472));
}
vec3 group4(vec3 a, vec3 b, vec3 c, vec3 d, bool karis) {
    if (!karis) return (a+b+c+d)*0.25;
    vec4 w = 1/(1+vec4(luma(a),luma(b),luma(c),luma(d))*p.Curve.w);
    return (a*w.x+b*w.y+c*w.z+d*w.w)/dot(w,vec4(1));
}
vec3 down13(vec2 uv, bool karis) {
    vec3 a=sampleAt(uv,vec2(-2,-2)), b=sampleAt(uv,vec2(0,-2)), c=sampleAt(uv,vec2(2,-2));
    vec3 d=sampleAt(uv,vec2(-2,0)), e=sampleAt(uv,vec2(0,0)), f=sampleAt(uv,vec2(2,0));
    vec3 g=sampleAt(uv,vec2(-2,2)), h=sampleAt(uv,vec2(0,2)), i=sampleAt(uv,vec2(2,2));
    vec3 j=sampleAt(uv,vec2(-1,-1)), k=sampleAt(uv,vec2(1,-1));
    vec3 l=sampleAt(uv,vec2(-1,1)), m=sampleAt(uv,vec2(1,1));
    return 0.125*(group4(a,b,d,e,karis)+group4(b,c,e,f,karis)+group4(d,e,g,h,karis)+group4(e,f,h,i,karis))
          +0.5*group4(j,k,l,m,karis);
}
vec3 tent(vec2 uv) {
    vec3 c=sampleAt(uv,vec2(0))*4;
    c+=2*(sampleAt(uv,vec2(-1,0))+sampleAt(uv,vec2(1,0))+sampleAt(uv,vec2(0,-1))+sampleAt(uv,vec2(0,1)));
    c+=sampleAt(uv,vec2(-1,-1))+sampleAt(uv,vec2(1,-1))+sampleAt(uv,vec2(-1,1))+sampleAt(uv,vec2(1,1));
    return c/16;
}
vec3 bicubic(vec2 uv) {
    vec2 size=vec2(textureSize(InputColor,0)), xy=uv*size-0.5, f=fract(xy), base=floor(xy);
    vec2 w0=(1-f)*(1-f)*(1-f)/6;
    vec2 w1=(3*f*f*f-6*f*f+4)/6;
    vec2 w2=(-3*f*f*f+3*f*f+3*f+1)/6;
    vec2 w3=f*f*f/6;
    vec2 g0=w0+w1, g1=w2+w3;
    vec2 h0=(base-1+w1/g0+0.5)/size, h1=(base+1+w3/g1+0.5)/size;
    return texture(InputColor,h0).rgb*g0.x*g0.y + texture(InputColor,vec2(h1.x,h0.y)).rgb*g1.x*g0.y
         + texture(InputColor,vec2(h0.x,h1.y)).rgb*g0.x*g1.y + texture(InputColor,h1).rgb*g1.x*g1.y;
}
void main() {
    vec2 uv=gl_FragCoord.xy/p.OutputSize.xy;
    int stage=int(p.Options.x);
    vec3 c;
    if (stage==0) {
        c=0.25*(prefilter(sampleAt(uv,vec2(-0.5,-0.5)))+prefilter(sampleAt(uv,vec2(0.5,-0.5)))
               +prefilter(sampleAt(uv,vec2(-0.5,0.5)))+prefilter(sampleAt(uv,vec2(0.5,0.5))));
    } else if (stage<=2) c=down13(uv,stage==1);
    else if (stage==3) c=mix(texture(FineColor,uv).rgb,tent(uv),p.Options.y);
    else {
        c=p.Options.z!=0 ? bicubic(uv) : texture(InputColor,uv).rgb;
        c*=p.Tint.rgb*p.Options.w;
        if (p.Tint.w>0) c*=1+p.Tint.w*texture(Dirt,uv).rgb;
        c=min(c,vec3(65472));
    }
    Color=vec4(c,stage==5 ? p.Options.w : 1);
})");
            if (auto* error = std::get_if<AssetError>(&built))
                throw std::runtime_error(error->Message);
            m_Shader = std::get<Handle<RenderHardware::Shader>>(std::move(built));
            m_Layout = device.CreateBindGroupLayout({0,
                                                     {{0, BindingType::UniformBuffer},
                                                      {1, BindingType::CombinedTextureSampler},
                                                      {2, BindingType::CombinedTextureSampler},
                                                      {3, BindingType::CombinedTextureSampler}}});
            SamplerDesc sampler;
            sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
            m_Sampler                         = device.CreateSampler(sampler);
        }
        const unsigned kind     = stage < 4 ? 0 : stage - 3;
        auto&          pipeline = m_Pipelines[kind];
        if (!pipeline)
        {
            PipelineDesc desc;
            desc.Program         = m_Shader;
            desc.DebugName       = kind == 0 ? "Bloom/Filter" : kind == 1 ? "Bloom/Add" : "Bloom/Dirt";
            desc.GroupLayouts    = {m_Layout};
            desc.ColorFormats    = {Format::RGBA16Float};
            desc.Rasterizer.Cull = CullMode::None;
            desc.Blends.resize(1);
            if (kind)
            {
                auto& blend            = desc.Blends[0];
                blend.Enabled          = true;
                blend.WriteMask        = 7;
                blend.DestinationColor = kind == 1 ? BlendFactor::One : BlendFactor::OneMinusSourceAlpha;
            }
            pipeline = device.CreatePipeline(desc);
        }
        auto buffer = device.CreateBuffer({sizeof(parameters), BufferUsage::Uniform, MemoryUsage::Upload});
        buffer->Write(0, &parameters, sizeof(parameters));
        BindGroupDesc group;
        group.Layout           = m_Layout;
        group.Entries          = {{0, buffer},
                                  {1, {}, 0, 0, context.Texture(source), m_Sampler},
                                  {2, {}, 0, 0, context.Texture(fine), m_Sampler},
                                  {3, {}, 0, 0, context.Texture(dirt), m_Sampler}};
        auto&         commands = context.Commands;
        RenderingDesc render;
        render.Colors.push_back({context.Texture(target), {}, kind ? LoadOp::Load : LoadOp::Discard});
        commands.BeginRendering(render);
        const auto size = parameters.OutputSize;
        commands.SetViewport({0, 0, size.x, size.y});
        commands.SetScissor({0, 0, uint32_t(size.x), uint32_t(size.y)});
        commands.BindPipeline(pipeline);
        commands.BindGroup(device.CreateBindGroup(group));
        commands.Draw(3);
        commands.EndRendering();
    }
} // namespace Kans
