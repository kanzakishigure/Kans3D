#include "kspch.h"
#include "RenderFrame.h"
#include "Shading/InternalShader.h"
#include "Temporal.h"
namespace Kans
{
    void RenderFrame::BuildGtao(RenderGraph& graph)
    {
        using namespace RenderHardware;
        if (Samples != 1 || !View.Environment.AmbientOcclusion.Enabled)
            return;
        auto&      state    = Output.GetTemporalState();
        auto       device   = GetDevice();
        const auto settings = View.Environment.AmbientOcclusion.Validated();
        const auto size     = settings.FullResolution ? View.Viewport : (View.Viewport + glm::uvec2(1)) / 2u;
        if (!state.AORaw || state.AORaw->GetDesc().Width != size.x || state.AORaw->GetDesc().Height != size.y)
        {
            TextureDesc d;
            d.Width       = size.x;
            d.Height      = size.y;
            d.PixelFormat = Format::RG16Float;
            d.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::Storage |
                            RenderHardware::TextureUsage::CopySource;
            state.AORaw   = device->CreateTexture(d);
            for (auto& image : state.AOHistory)
                image = device->CreateTexture(d);
            state.AOValid = false;
        }
        if (!state.AOLayout)
        {
            state.AOLayout = device->CreateBindGroupLayout({0,
                                                            {{0, BindingType::UniformBuffer},
                                                             {1, BindingType::CombinedTextureSampler},
                                                             {2, BindingType::CombinedTextureSampler},
                                                             {3, BindingType::CombinedTextureSampler},
                                                             {4, BindingType::CombinedTextureSampler},
                                                             {5, BindingType::CombinedTextureSampler},
                                                             {6, BindingType::StorageTexture}}});
            SamplerDesc sampler;
            sampler.U = sampler.V = AddressMode::ClampToEdge;
            sampler.Min = sampler.Mag = sampler.Mip = Filter::Nearest;
            sampler.MaxLod                          = 0;
            state.AOSampler                         = device->CreateSampler(sampler);
            const std::string                common = R"(#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0,std140) uniform Params {
    mat4 inverseProjection; mat4 view; mat4 inverseView; mat4 previousView;
    vec4 search; vec4 shape; vec4 temporal;
} p;
layout(set=0,binding=1) uniform sampler2D Depth;
layout(set=0,binding=2) uniform sampler2D Normal;
layout(set=0,binding=3) uniform sampler2D Raw;
layout(set=0,binding=4) uniform sampler2D History;
layout(set=0,binding=5) uniform sampler2D Velocity;
layout(set=0,binding=6,rg16f) uniform writeonly image2D Result;
vec3 unproject(vec2 uv, float depth) {
    vec4 v=p.inverseProjection*vec4(uv*2-1,depth,1);
    return v.xyz/v.w;
}
vec3 position(vec2 uv) { return unproject(uv,textureLod(Depth,uv,0).r); }
vec3 decode(vec2 v) {
    vec2 f=v*2-1; vec3 n=vec3(f,1-abs(f.x)-abs(f.y));
    float t=clamp(-n.z,0,1); n.xy+=mix(vec2(t),vec2(-t),greaterThanEqual(n.xy,vec2(0)));
    return normalize(mat3(p.view)*normalize(n));
}
)";
            const std::array<std::string, 2> bodies {R"(
void main() {
    ivec2 id=ivec2(gl_GlobalInvocationID.xy), size=imageSize(Result);
    if(any(greaterThanEqual(id,size))) return;
    vec2 uv=(vec2(id)+.5)/vec2(size);
    vec4 packed=textureLod(Normal,uv,0);
    vec3 P=position(uv);
    if(packed.a!=1 || textureLod(Depth,uv,0).r>=1) { imageStore(Result,id,vec4(1,0,0,0)); return; }
    vec3 N=decode(packed.rg), V=normalize(-P);
    float visibility=0;
    vec2 texel=1.0/vec2(textureSize(Depth,0));
    // Radius in view-space metres, projected to UV. Perspective and orthographic projections both work.
    float centerDepth=textureLod(Depth,uv,0).r;
    float footprint=length(unproject(uv+vec2(texel.x,0),centerDepth)-P);
    float radiusPixels=min(p.search.x/max(footprint,1e-6),float(max(size.x,size.y))*2);
    for(int slice=0;slice<int(p.search.y);++slice) {
        float angle=3.14159265359*(float(slice)+p.temporal.x)/p.search.y;
        vec2 direction=vec2(cos(angle),sin(angle));
        vec3 D=normalize(unproject(uv+direction*texel,centerDepth)-P);
        vec3 axis=normalize(cross(D,V));
        vec3 T=normalize(cross(V,axis));
        vec3 projected=N-axis*dot(N,axis);
        float nLength=length(projected);
        float n=atan(dot(projected,T),dot(projected,V));
        float horizon[2];
        horizon[0]=cos(n+1.57079632679); horizon[1]=cos(n-1.57079632679);
        for(int side=0;side<2;++side) {
            float signSide=side==0?1.0:-1.0;
            float base=horizon[side];
            for(int step=0;step<int(p.search.z);++step) {
                float fraction=(float(step)+.5)/p.search.z;
                vec2 sampleUV=uv+signSide*direction*texel*max(1.0,fraction*fraction*radiusPixels);
                if(any(lessThan(sampleUV,vec2(0)))||any(greaterThan(sampleUV,vec2(1)))) continue;
                vec3 delta=position(sampleUV)-P;
                float distance=length(delta);
                float h=dot(delta,V)/max(distance,1e-6);
                float falloff=clamp((p.search.x-distance)/(p.search.x*p.shape.z),0,1);
                h=mix(base,h,falloff);
                // Samples below the current horizon relax it: thin foreground features do not become solid walls.
                horizon[side]=h>horizon[side]?h:mix(horizon[side],h,p.shape.w);
            }
        }
        // GTAO (Jimenez et al. 2016): analytic cosine-weighted visibility integral in the slice plane.
        float h1=n+clamp(acos(clamp(horizon[0],-1,1))-n,-1.57079632679,1.57079632679);
        float h2=n+clamp(-acos(clamp(horizon[1],-1,1))-n,-1.57079632679,1.57079632679);
        visibility+=nLength*.25*((-cos(2*h1-n)+cos(n)+2*h1*sin(n))+(-cos(2*h2-n)+cos(n)+2*h2*sin(n)));
    }
    float ao=pow(clamp(1-p.shape.x*(1-visibility/p.search.y),0,1),p.shape.y);
    imageStore(Result,id,vec4(ao,-P.z,0,1));
})",
                                                     R"(
void main() {
    ivec2 id=ivec2(gl_GlobalInvocationID.xy), size=imageSize(Result);
    if(any(greaterThanEqual(id,size))) return;
    vec2 uv=(vec2(id)+.5)/vec2(size);
    vec4 center=texelFetch(Raw,id,0);
    if(center.g<=0) { imageStore(Result,id,center); return; }
    float sum=0, weights=0, lo=1, hi=0;
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        ivec2 q=clamp(id+ivec2(x,y),ivec2(0),size-1);
        vec4 sampleAO=texelFetch(Raw,q,0);
        float w=exp(-float(x*x+y*y)*.25)*exp(-abs(sampleAO.g-center.g)/max(.01,center.g*.01))*float(sampleAO.g>0);
        sum+=sampleAO.r*w; weights+=w;
        if(w>.1) { lo=min(lo,sampleAO.r); hi=max(hi,sampleAO.r); }
    }
    float ao=sum/max(weights,1e-6);
    vec2 oldUV=uv+textureLod(Velocity,uv,0).rg;
    if(p.temporal.y>0 && all(greaterThanEqual(oldUV,vec2(0))) && all(lessThanEqual(oldUV,vec2(1)))) {
        vec4 old=textureLod(History,oldUV,0);
        float expected=-(p.previousView*p.inverseView*vec4(position(uv),1)).z;
        if(old.g>0 && abs(old.g-expected)<max(.02,abs(expected)*.02))
            ao=mix(ao,clamp(old.r,lo,hi),.85);
    }
    imageStore(Result,id,vec4(ao,center.g,0,1));
})"};
            for (unsigned i = 0; i < 2; ++i)
            {
                const auto& built = GetInternalComputeShaderData(i ? "GTAO denoise temporal" : "GTAO horizon search",
                                                                 common + bodies[i]);
                if (auto* error = std::get_if<AssetError>(&built))
                    throw std::runtime_error(error->Message);
                state.AOPipeline[i] = device->CreateComputePipeline(
                    {device->CreateShader(std::get<ShaderData>(built)), {state.AOLayout}, 0, "GTAO"});
            }
        }
        struct Parameters
        {
            glm::mat4 InverseProjection, View, InverseView, PreviousView;
            glm::vec4 Search, Shape, Temporal;
        };
        const bool taa   = View.EffectiveAA == AntiAliasing::TAA;
        const auto phase = GtaoSample(state.AOFrame, 0, 0, settings).x * settings.Slices / glm::pi<float>();
        Parameters p {glm::inverse(SceneProjection()),
                      View.Scene.sceneCamera.viewMatrix,
                      glm::inverse(View.Scene.sceneCamera.viewMatrix),
                      state.AOPreviousView,
                      {settings.Radius, settings.Slices, settings.Steps, 0},
                      {settings.Intensity, settings.Power, settings.Falloff, settings.Thickness},
                      {phase, !taa && state.AOValid && !state.Reset && settings == state.AOSettings ? 1.f : 0.f, 0, 0}};
        auto       uniform = device->CreateBuffer({sizeof(p), BufferUsage::Uniform, MemoryUsage::Upload});
        uniform->Write(0, &p, sizeof(p));
        auto previous = state.AOHistory[state.AOFrame % 2], result = state.AOHistory[(state.AOFrame + 1) % 2];
        AmbientOcclusion = result;
        const auto depth = Output.GetDepth(), normal = Output.GetNormalRoughness(), velocity = Output.GetVelocity();
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto                                           output = i ? result : state.AORaw;
            std::vector<GraphUse>                                uses;
            const std::array<Handle<RenderHardware::Texture>, 5> inputs {
                depth, normal, state.AORaw, previous, velocity};
            // Raw is unused by the horizon pipeline; bind depth there to avoid read/write aliasing.
            auto bound = inputs;
            if (!i)
                bound[2] = depth;
            for (auto& image : bound)
            {
                auto id = graph.ImportTexture("GTAO input", image);
                if (std::none_of(uses.begin(), uses.end(), [&](auto u) { return u.Texture == id; }))
                    uses.push_back({id, GraphAccess::SampledRead});
            }
            const auto out = graph.ImportTexture(i ? "GTAO filtered" : "GTAO raw", output);
            uses.push_back({out, GraphAccess::StorageWrite});
            graph.ExportTexture(out);
            auto       layout   = state.AOLayout;
            auto       pipeline = state.AOPipeline[i];
            auto       sampler  = state.AOSampler;
            const auto pass     = graph.AddPass(i ? "GTAO depth-aware denoise and temporal" : "GTAO horizon search",
                                                uses,
                                                [uniform, bound, output, layout, pipeline, sampler, size](auto& c) {
                                                BindGroupDesc group;
                                                group.Layout = layout;
                                                group.Entries.push_back({0, uniform});
                                                for (unsigned j = 0; j < bound.size(); ++j)
                                                    group.Entries.push_back({j + 1, {}, 0, 0, bound[j], sampler});
                                                group.Entries.push_back({6, {}, 0, 0, output});
                                                group.Entries.back().Subresource = TextureSubresource {};
                                                c.Commands.BindComputePipeline(pipeline);
                                                c.Commands.BindGroup(c.Device.CreateBindGroup(group));
                                                c.Commands.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
                                                });
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        }
        state.AOSettings     = settings;
        state.AOValid        = true;
        state.AOPreviousView = View.Scene.sceneCamera.viewMatrix;
        ++state.AOFrame;
    }
} // namespace Kans
