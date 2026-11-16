#include "kspch.h"
#include "Passes/SsrTrace.h"
#include "RenderFrame.h"
#include "Shading/InternalShader.h"
#include "Temporal.h"

namespace Kans
{
    const std::string& SsrTraceShaderSource()
    {
        static const std::string source = R"(#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0,std140) uniform Params {
    mat4 projection; mat4 inverseProjection; mat4 view;
    vec4 search; vec4 shape;
} p;
layout(set=0,binding=1) uniform sampler2D HiZ;
layout(set=0,binding=2) uniform sampler2D Normal;
layout(set=0,binding=3) uniform sampler2D Color;
layout(set=0,binding=4) uniform sampler2D Surface;
layout(set=0,binding=5,rgba16f) uniform writeonly image2D Result;
vec3 unproject(vec2 uv,float z) {
    vec4 q=p.inverseProjection*vec4(uv*2-1,z,1); return q.xyz/q.w;
}
vec3 normalAt(vec2 uv) {
    vec2 f=textureLod(Normal,uv,0).rg*2-1;
    vec3 n=vec3(f,1-abs(f.x)-abs(f.y));
    float t=clamp(-n.z,0,1); n.xy+=mix(vec2(t),vec2(-t),greaterThanEqual(n.xy,vec2(0)));
    return normalize(mat3(p.view)*normalize(n));
}
// Heitz 2018 GGX visible normal distribution, with alpha = perceptual roughness squared.
vec3 vndf(vec3 v,float roughness,vec2 u) {
    float a=max(roughness*roughness,.0001);
    vec3 vh=normalize(vec3(a*v.xy,v.z));
    float len=dot(vh.xy,vh.xy);
    vec3 t1=len>0?vec3(-vh.y,vh.x,0)/sqrt(len):vec3(1,0,0), t2=cross(vh,t1);
    float r=sqrt(u.x), phi=6.28318530718*u.y;
    float x=r*cos(phi), y=r*sin(phi), s=.5*(1+vh.z);
    y=(1-s)*sqrt(max(0,1-x*x))+s*y;
    vec3 nh=x*t1+y*t2+sqrt(max(0,1-x*x-y*y))*vh;
    return normalize(vec3(a*nh.xy,max(0,nh.z)));
}
float ign(vec2 pixel) { return fract(52.9829189*fract(dot(pixel,vec2(.06711056,.00583715)))); }
void main() {
    ivec2 id=ivec2(gl_GlobalInvocationID.xy), size=imageSize(Result);
    if(any(greaterThanEqual(id,size))) return;
    imageStore(Result,id,vec4(0));
    vec2 uv=(vec2(id)+.5)/vec2(size);
    vec4 packed=textureLod(Normal,uv,0);
    float depth=textureLod(HiZ,uv,0).r, roughness=packed.b;
    // Receiver weight threshold .02 excludes black metals; unsupported surfaces have zero coverage.
    if(packed.a!=1 || depth>=1 || roughness>p.search.y || textureLod(Surface,uv,0).a<.5) return;
    vec3 origin=unproject(uv,depth), n=normalAt(uv);
    vec3 v=p.projection[3][3]==1?vec3(0,0,1):normalize(-origin);
    if(dot(n,v)<=0) return;
    vec3 tangent=normalize(cross(abs(n.z)<.999?vec3(0,0,1):vec3(0,1,0),n)), bitangent=cross(n,tangent);
    mat3 basis=mat3(tangent,bitangent,n);
    vec2 random=vec2(ign(vec2(id)+p.shape.x*vec2(5.588238,5.588238)),
                     ign(vec2(id.yx)+p.shape.x*vec2(13.371,7.173)+19));
    vec3 h=roughness==0?n:basis*vndf(transpose(basis)*v,roughness,random);
    vec3 direction=reflect(-v,h);
    if(dot(n,direction)<=0) return;
    // Clip the view-space ray against the near plane, then walk its projective screen segment.
    vec3 start=origin+n*.002;
    float distance=p.shape.y;
    float nearZ=unproject(vec2(.5),0).z;
    if(direction.z>0) distance=min(distance,(nearZ-start.z)/direction.z*.999);
    if(distance<=.002) return;
    vec4 a=p.projection*vec4(start,1), b=p.projection*vec4(start+direction*distance,1);
    vec3 A=a.xyz/a.w, B=b.xyz/b.w;
    A.xy=A.xy*.5+.5; B.xy=B.xy*.5+.5;
    vec3 D=B-A;
    vec2 pixels=vec2(textureSize(HiZ,0));
    float speed=max(abs(D.x)*pixels.x,abs(D.y)*pixels.y);
    if(speed<1) return;
    float end=1;
    for(int axis=0;axis<2;++axis) {
        if(D[axis]>0) end=min(end,(1-A[axis])/D[axis]);
        else if(D[axis]<0) end=min(end,-A[axis]/D[axis]);
    }
    // Start beyond the source texel to prevent self reflection.
    float t=1.5/speed, epsilon=.001/speed;
    int level=0, maxLevel=textureQueryLevels(HiZ)-1;
    for(int step=0;step<int(p.search.x) && t<end;++step) {
        vec3 q=A+D*t;
        if(any(lessThan(q.xy,vec2(0)))||any(greaterThanEqual(q.xy,vec2(1)))||q.z<0||q.z>1) break;
        float scale=exp2(float(level));
        ivec2 mipSize=textureSize(HiZ,level);
        ivec2 cell=min(ivec2(q.xy*pixels/scale),mipSize-1);
        vec2 lo=vec2(cell)*scale/pixels;
        vec2 hi=vec2(cell+1)*scale/pixels;
        if(cell.x==mipSize.x-1) hi.x=1;
        if(cell.y==mipSize.y-1) hi.y=1;
        vec2 boundary=mix(lo,hi,greaterThan(D.xy,vec2(0)));
        vec2 dt=vec2(1e20);
        if(abs(D.x)>1e-12) dt.x=(boundary.x-q.x)/D.x;
        if(abs(D.y)>1e-12) dt.y=(boundary.y-q.y)/D.y;
        float next=min(end,t+max(0,min(dt.x,dt.y)));
        float z=texelFetch(HiZ,cell,level).r;
        // An entire cell in front of its nearest surface can be skipped; otherwise descend.
        if(z>=1 || max(q.z,A.z+D.z*next)<z) {
            t=next+epsilon; level=min(level+1,maxLevel); continue;
        }
        if(level>0) { --level; continue; }
        float hitT=t;
        if(abs(D.z)>1e-10) hitT=clamp((z-A.z)/D.z,t,next);
        vec3 hit=A+D*hitT;
        float gap=unproject(hit.xy,z).z-unproject(hit.xy,hit.z).z;
        vec4 hitNormal=textureLod(Normal,hit.xy,0);
        // A ray may enter the depth slab, but must never tunnel behind it or hit a back face.
        if(gap>=-.001 && gap<=p.search.z && hitNormal.a==1 && dot(normalAt(hit.xy),direction)<-.05) {
            float border=min(min(hit.x,hit.y),min(1-hit.x,1-hit.y));
            float confidence=smoothstep(0,.08,border)*smoothstep(.02,.2,1-max(0,dot(direction,v)));
            imageStore(Result,id,vec4(textureLod(Color,hit.xy,0).rgb*p.search.w,confidence));
        }
        // First visible surface terminates traversal even if invalid: no leaking through occluders.
        return;
    }
})";
        return source;
    }

    void RenderFrame::BuildReflections(RenderGraph& graph)
    {
        using namespace RenderHardware;
        const auto settings = View.Environment.ScreenSpaceReflections.Validated();
        auto&      state    = Output.GetTemporalState();
        auto       device   = GetDevice();
        HiZ                 = BuildHiZ(graph, *device, Output.GetDepth(), state.HiZ);
        // Snapshot before any transparent draw and before the composite writes HDR again.
        auto colorDesc   = Output.GetHDR()->GetDesc();
        colorDesc.Usage  = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
        const auto color = graph.CreateTexture("SSR opaque HDR snapshot", colorDesc);
        const auto hdr   = graph.ImportTexture("SceneHDR", Output.GetHDR());
        auto       pass =
            graph.AddPass("SSR opaque HDR snapshot",
                          {{hdr, GraphAccess::TransferRead}, {color, GraphAccess::TransferWrite}},
                          [hdr, color](auto& c) { c.Commands.CopyTexture(c.Texture(hdr), {}, c.Texture(color)); });
        if (pass)
            graph.OrderAfter(pass, pass - 1);
        const auto normal  = graph.ImportTexture("SceneNormalRoughness", Output.GetNormalRoughness());
        const auto surface = graph.ImportTexture("SSR receiver surface", ReflectionSurface);
        ReflectionTrace    = BuildSsrTrace(graph,
                                           *device,
                                           state,
                                           {HiZ,
                                            normal,
                                            color,
                                            surface,
                                            SceneProjection(),
                                            View.Scene.sceneCamera.viewMatrix,
                                            View.Viewport,
                                            settings});
        Reflections        = state.ReflectionRaw;
    }

    GraphTexture BuildSsrTrace(RenderGraph&            graph,
                               RenderHardware::Device& device,
                               TemporalState&          state,
                               const SsrTraceInputs&   inputs)
    {
        using namespace RenderHardware;
        const auto settings = inputs.Settings.Validated();
        const auto size     = settings.HalfResolution ? (inputs.Viewport + glm::uvec2(1)) / 2u : inputs.Viewport;
        if (!state.ReflectionRaw || state.ReflectionRaw->GetDesc().Width != size.x ||
            state.ReflectionRaw->GetDesc().Height != size.y)
        {
            TextureDesc d;
            d.Width             = size.x;
            d.Height            = size.y;
            d.PixelFormat       = Format::RGBA16Float;
            d.Usage             = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::Storage |
                                  RenderHardware::TextureUsage::CopySource;
            state.ReflectionRaw = device.CreateTexture(d);
        }
        const auto result = state.ReflectionRaw;
        const auto trace  = graph.ImportTexture("SSR raw radiance confidence", result);
        if (!state.ReflectionPipeline)
        {
            state.ReflectionLayout = device.CreateBindGroupLayout({0,
                                                                   {{0, BindingType::UniformBuffer},
                                                                    {1, BindingType::CombinedTextureSampler},
                                                                    {2, BindingType::CombinedTextureSampler},
                                                                    {3, BindingType::CombinedTextureSampler},
                                                                    {4, BindingType::CombinedTextureSampler},
                                                                    {5, BindingType::StorageTexture}}});
            SamplerDesc sampler;
            sampler.U = sampler.V = AddressMode::ClampToEdge;
            sampler.Min = sampler.Mag = sampler.Mip = Filter::Nearest;
            sampler.MaxLod                          = 32;
            state.ReflectionSampler                 = device.CreateSampler(sampler);
            const auto& source                      = SsrTraceShaderSource();
            const auto& built                       = GetInternalComputeShaderData("SSR Hi-Z VNDF trace", source);
            if (auto* error = std::get_if<AssetError>(&built))
                throw std::runtime_error(error->Message);
            state.ReflectionPipeline = device.CreateComputePipeline(
                {device.CreateShader(std::get<ShaderData>(built)), {state.ReflectionLayout}, 0, "SSR trace"});
        }
        struct Parameters
        {
            glm::mat4 Projection, InverseProjection, View;
            glm::vec4 Search, Shape;
        };
        Parameters p {inputs.Projection,
                      glm::inverse(inputs.Projection),
                      inputs.View,
                      {settings.MaxSteps, settings.MaxRoughness, settings.Thickness, settings.Intensity},
                      {state.ReflectionFrame++ % 64, 100, 0, 0}};
        auto       uniform = device.CreateBuffer({sizeof(p), BufferUsage::Uniform, MemoryUsage::Upload});
        uniform->Write(0, &p, sizeof(p));
        const auto normal = inputs.Normal, surface = inputs.Surface, color = inputs.Color, HiZ = inputs.HiZ;
        auto       pass = graph.AddPass("SSR Hi-Z VNDF trace",
                                        GraphPassKind::Compute,
                                        {{HiZ, GraphAccess::SampledRead},
                                         {normal, GraphAccess::SampledRead},
                                         {color, GraphAccess::SampledRead},
                                         {surface, GraphAccess::SampledRead},
                                         {trace, GraphAccess::StorageWrite}},
                                        [uniform,
                                         pyramid = HiZ,
                                         normal,
                                         color,
                                         surface,
                                         result,
                                         layout   = state.ReflectionLayout,
                                         pipeline = state.ReflectionPipeline,
                                         sampler  = state.ReflectionSampler,
                                         size](auto& c) {
                                      BindGroupDesc group;
                                      group.Layout = layout;
                                      group.Entries.push_back({0, uniform});
                                      const std::array<GraphTexture, 4> images {pyramid, normal, color, surface};
                                      for (unsigned i = 0; i < images.size(); ++i)
                                          group.Entries.push_back({i + 1, {}, 0, 0, c.Texture(images[i]), sampler});
                                      group.Entries.push_back({5, {}, 0, 0, result});
                                      group.Entries.back().Subresource = TextureSubresource {};
                                      c.Commands.BindComputePipeline(pipeline);
                                      c.Commands.BindGroup(c.Device.CreateBindGroup(group));
                                      c.Commands.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
                                        });
        if (pass)
            graph.OrderAfter(pass, pass - 1);
        graph.ExportTexture(trace);
        return trace;
    }
} // namespace Kans
