#include "kspch.h"
#include "Kans3D/Renderer/Passes/AntiAliasingPass.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    void ReconstructionFilter::Record(RenderHardware::Device&                                device,
                                      RenderHardware::CommandList&                           commands,
                                      const RenderHardware::Handle<RenderHardware::Texture>& source,
                                      const RenderHardware::Handle<RenderHardware::Texture>& target,
                                      float                                                  radius)
    {
        using namespace RenderHardware;
        if (!std::isfinite(radius) || radius < 1)
            return; // Integer pixel centres leave only the centre tap below radius one.
        radius = std::min(radius, 4.f);
        if (!m_Pipeline)
        {
            auto built = CreateInternalShader(device,
                                              "Radial Blackman-Harris reconstruction",
                                              R"(#version 450
void main() {
    vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    gl_Position=vec4(p*2-1,0,1);
})",
                                              R"(#version 450
layout(set=0,binding=0,std140) uniform Kernel { vec4 Info; vec4 Taps[81]; } k;
layout(set=0,binding=1) uniform sampler2D InputColor;
layout(location=0) out vec4 Color;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy), last=textureSize(InputColor,0)-1;
    vec4 center=texelFetch(InputColor,p,0);
    vec3 sum=center.rgb*k.Taps[0].z;
    for(int i=1;i<int(k.Info.x);++i) {
        ivec2 at=clamp(p+ivec2(k.Taps[i].xy),ivec2(0),last);
        sum+=texelFetch(InputColor,at,0).rgb*k.Taps[i].z;
    }
    Color=vec4(sum,center.a);
})");
            if (auto* error = std::get_if<AssetError>(&built))
                throw std::runtime_error(error->Message);
            m_Layout = device.CreateBindGroupLayout(
                {0, {{0, BindingType::UniformBuffer}, {1, BindingType::CombinedTextureSampler}}});
            SamplerDesc sampling;
            sampling.U = sampling.V = sampling.W = AddressMode::ClampToEdge;
            m_Sampler                            = device.CreateSampler(sampling);
            PipelineDesc desc;
            desc.Program         = std::get<Handle<RenderHardware::Shader>>(std::move(built));
            desc.GroupLayouts    = {m_Layout};
            desc.ColorFormats    = {Format::RGBA16Float};
            desc.Rasterizer.Cull = CullMode::None;
            desc.Blends.resize(1);
            m_Pipeline = device.CreatePipeline(desc);
        }
        if (m_Radius != radius)
        {
            struct Kernel
            {
                glm::vec4                 Info {0};
                std::array<glm::vec4, 81> Taps {};
            } kernel;
            size_t count    = 1;
            kernel.Taps[0]  = {0, 0, 1, 0};
            double    total = 1;
            const int bound = int(std::floor(radius));
            for (int y = -bound; y <= bound; ++y)
                for (int x = -bound; x <= bound; ++x)
                {
                    const double distance = std::sqrt(double(x * x + y * y));
                    if ((!x && !y) || distance > radius)
                        continue;
                    const double a = 3.14159265358979323846 * distance / radius;
                    const double weight =
                        .35875 + .48829 * std::cos(a) + .14128 * std::cos(2 * a) + .01168 * std::cos(3 * a);
                    kernel.Taps[count++] = {float(x), float(y), float(weight), 0};
                    total += weight;
                }
            for (size_t i = 0; i < count; ++i)
                kernel.Taps[i].z /= float(total);
            kernel.Info.x = float(count);
            // Replace rather than overwrite: previous commands may still reference the old kernel.
            m_Kernel = device.CreateBuffer({sizeof(kernel), BufferUsage::Uniform, MemoryUsage::Upload});
            m_Kernel->Write(0, &kernel, sizeof(kernel));
            m_Group.reset();
            m_Radius = radius;
        }
        if (!m_Group || m_Source != source)
        {
            m_Group  = device.CreateBindGroup({m_Layout, {{0, m_Kernel}, {1, {}, 0, 0, source, m_Sampler}}});
            m_Source = source;
        }
        if (m_Render.Colors.empty())
            m_Render.Colors.resize(1);
        m_Render.Colors[0].Target = target;
        m_Render.Colors[0].Load   = LoadOp::Discard;
        commands.BeginRendering(m_Render);
        commands.SetViewport({0, 0, float(target->GetDesc().Width), float(target->GetDesc().Height)});
        commands.SetScissor({0, 0, target->GetDesc().Width, target->GetDesc().Height});
        commands.BindPipeline(m_Pipeline);
        commands.BindGroup(m_Group);
        commands.Draw(3);
        commands.EndRendering();
    }

    namespace
    {
        constexpr const char* Vertex          = R"(#version 450
void main() {
    vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    gl_Position=vec4(p*2-1,0,1);
})";
        constexpr const char* ResolveFragment = R"(#version 450
layout(set=0,binding=0) uniform sampler2DMS InputColor;
layout(location=0) out vec4 Color;
float luma(vec3 c) { return dot(c,vec3(0.2126,0.7152,0.0722)); }
void main() {
    vec3 mapped=vec3(0); float alpha=0;
    int count=textureSamples(InputColor);
    for(int i=0;i<count;++i) {
        vec4 c=texelFetch(InputColor,ivec2(gl_FragCoord.xy),i);
        mapped += HDR ? max(c.rgb,vec3(0))/(1+luma(max(c.rgb,vec3(0)))) : c.rgb;
        alpha+=c.a;
    }
    mapped/=float(count);
    // Inverse of c/(1+luma(c)). Equivalent to sum(c*w)/sum(w), w=1/(1+luma).
    Color=vec4(HDR ? mapped/max(1-luma(mapped),1e-6) : mapped, alpha/float(count));
})";
        // FXAA quality: local contrast rejection, dominant edge, bidirectional
        // endpoint search (quality 12 schedule), and subpixel alias removal.
        constexpr const char* FxaaFragment = R"(#version 450
layout(set=0,binding=0) uniform sampler2D InputColor;
layout(location=0) out vec4 Color;
float luma(vec3 c) { return dot(c,vec3(0.299,0.587,0.114)); }
float lum(vec2 uv) { return luma(texture(InputColor,uv).rgb); }
void main() {
    vec2 rcp=1.0/vec2(textureSize(InputColor,0)), uv=gl_FragCoord.xy*rcp;
    vec4 center=texelFetch(InputColor,ivec2(gl_FragCoord.xy),0);
    float m=luma(center.rgb), n=lum(uv+vec2(0,-rcp.y)), s=lum(uv+vec2(0,rcp.y));
    float w=lum(uv+vec2(-rcp.x,0)), e=lum(uv+vec2(rcp.x,0));
    float lo=min(m,min(min(n,s),min(w,e))), hi=max(m,max(max(n,s),max(w,e))), range=hi-lo;
    if(range<max(0.0312,hi*0.125)) { Color=center; return; }
    float nw=lum(uv+vec2(-rcp.x,-rcp.y)), ne=lum(uv+vec2(rcp.x,-rcp.y));
    float sw=lum(uv+vec2(-rcp.x,rcp.y)), se=lum(uv+vec2(rcp.x,rcp.y));
    float horizontal=abs(nw+sw-2*w)+2*abs(n+s-2*m)+abs(ne+se-2*e);
    float vertical=abs(nw+ne-2*n)+2*abs(w+e-2*m)+abs(sw+se-2*s);
    bool hor=horizontal>=vertical;
    float sideA=hor?n:w, sideB=hor?s:e;
    float gradA=sideA-m, gradB=sideB-m;
    bool first=abs(gradA)>=abs(gradB);
    float gradient=max(abs(gradA),abs(gradB))*0.25;
    float localAverage=0.5*(m+(first?sideA:sideB));
    float stepLength=(hor?rcp.y:rcp.x)*(first?-1:1);
    vec2 edgeUV=uv;
    if(hor) edgeUV.y+=stepLength*0.5; else edgeUV.x+=stepLength*0.5;
    vec2 along=hor?vec2(rcp.x,0):vec2(0,rcp.y);
    vec2 a=edgeUV-along, b=edgeUV+along;
    float endA=lum(a)-localAverage, endB=lum(b)-localAverage;
    bool doneA=abs(endA)>=gradient, doneB=abs(endB)>=gradient;
    const float steps[11]=float[](1.0,1.5,2.0,2.0,2.0,2.0,2.0,2.0,4.0,8.0,8.0);
    for(int i=0;i<11 && !(doneA&&doneB);++i) {
        if(!doneA) { a-=along*steps[i]; endA=lum(a)-localAverage; doneA=abs(endA)>=gradient; }
        if(!doneB) { b+=along*steps[i]; endB=lum(b)-localAverage; doneB=abs(endB)>=gradient; }
    }
    float distA=hor?uv.x-a.x:uv.y-a.y, distB=hor?b.x-uv.x:b.y-uv.y;
    bool closerA=distA<distB;
    float edgeOffset=0.5-min(distA,distB)/max(distA+distB,1e-6);
    if(((closerA?endA:endB)<0)==(m<localAverage)) edgeOffset=0;
    float sub=clamp(abs((2*(n+s+w+e)+nw+ne+sw+se)/12-m)/range,0,1);
    sub=sub*sub*(3-2*sub); sub=sub*sub*0.75;
    float offset=max(edgeOffset,sub);
    vec2 finalUV=uv;
    if(hor) finalUV.y+=offset*stepLength; else finalUV.x+=offset*stepLength;
    Color=vec4(texture(InputColor,finalUV).rgb,center.a);
})";
    } // namespace
    void AntiAliasingPass::Resolve(RenderHardware::Device&                                device,
                                   RenderHardware::CommandList&                           commands,
                                   const RenderHardware::Handle<RenderHardware::Texture>& source,
                                   const RenderHardware::Handle<RenderHardware::Texture>& target,
                                   bool                                                   hdr)
    {
        Record(device, commands, source, target, hdr ? 0 : 1);
    }
    void AntiAliasingPass::FXAA(RenderHardware::Device&                                device,
                                RenderHardware::CommandList&                           commands,
                                const RenderHardware::Handle<RenderHardware::Texture>& source,
                                const RenderHardware::Handle<RenderHardware::Texture>& target)
    {
        Record(device, commands, source, target, 2);
    }
    void AntiAliasingPass::Record(RenderHardware::Device&                                device,
                                  RenderHardware::CommandList&                           commands,
                                  const RenderHardware::Handle<RenderHardware::Texture>& source,
                                  const RenderHardware::Handle<RenderHardware::Texture>& target,
                                  unsigned                                               kind)
    {
        using namespace RenderHardware;
        if (!m_Layout)
        {
            m_Layout = device.CreateBindGroupLayout({0, {{0, BindingType::CombinedTextureSampler}}});
            SamplerDesc sampling;
            sampling.U = sampling.V = sampling.W = AddressMode::ClampToEdge;
            m_Sampler                            = device.CreateSampler(sampling);
        }
        auto& pipeline = m_Pipelines[{kind, target->GetDesc().PixelFormat}];
        if (!pipeline)
        {
            std::string fragment = kind == 2 ? FxaaFragment : ResolveFragment;
            if (kind != 2)
                fragment.insert(fragment.find('\n') + 1, kind == 0 ? "#define HDR true\n" : "#define HDR false\n");
            auto built = CreateInternalShader(device, kind == 2 ? "FXAA quality" : "MSAA resolve", Vertex, fragment);
            if (auto* error = std::get_if<AssetError>(&built))
                throw std::runtime_error(error->Message);
            PipelineDesc desc;
            desc.Program         = std::get<Handle<RenderHardware::Shader>>(std::move(built));
            desc.GroupLayouts    = {m_Layout};
            desc.ColorFormats    = {target->GetDesc().PixelFormat};
            desc.Rasterizer.Cull = CullMode::None;
            desc.Blends.resize(1);
            pipeline = device.CreatePipeline(desc);
        }
        RenderingDesc render;
        render.Colors.push_back({target, {}, LoadOp::Discard});
        commands.BeginRendering(render);
        commands.BindPipeline(pipeline);
        commands.BindGroup(device.CreateBindGroup({m_Layout, {{0, {}, 0, 0, source, m_Sampler}}}));
        commands.Draw(3);
        commands.EndRendering();
    }
} // namespace Kans
