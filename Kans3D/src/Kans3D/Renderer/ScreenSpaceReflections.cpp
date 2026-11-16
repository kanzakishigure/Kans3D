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
    mat4 previousClipFromView; mat4 previousInverseProjection;
    vec4 search; vec4 shape; vec4 reprojection;
} p;
layout(set=0,binding=1) uniform sampler2D HiZ;
layout(set=0,binding=2) uniform sampler2D Normal;
layout(set=0,binding=3) uniform sampler2D Color;
layout(set=0,binding=4) uniform sampler2D Surface;
layout(set=0,binding=5,rgba16f) uniform writeonly image2D Result;
layout(set=0,binding=6) uniform sampler2D Velocity;
layout(set=0,binding=7) uniform sampler2D PreviousDepth;
layout(set=0,binding=8,rgba32f) uniform writeonly image2D Ray;
layout(set=0,binding=9) uniform sampler2D PreviousNormal;
vec3 unproject(vec2 uv,float z) {
    vec4 q=p.inverseProjection*vec4(uv*2-1,z,1); return q.xyz/q.w;
}
vec3 normalAt(vec2 uv) {
    vec2 f=textureLod(Normal,uv,0).rg*2-1;
    vec3 n=vec3(f,1-abs(f.x)-abs(f.y));
    float t=clamp(-n.z,0,1); n.xy+=mix(vec2(t),vec2(-t),greaterThanEqual(n.xy,vec2(0)));
    return normalize(mat3(p.view)*normalize(n));
}
// Velocity is previousUV-currentUV without jitter. Both histories retain raster jitter.
bool historyAt(vec2 uv,vec3 position,out vec2 oldUV) {
    oldUV=uv;
    if(p.reprojection.z<.5) return false;
    if(p.reprojection.w<.5) return true; // Analytic, history-free trace harness.
    vec2 velocity=textureLod(Velocity,uv,0).rg;
    oldUV=uv+velocity+p.reprojection.xy;
    if(any(lessThan(oldUV,vec2(0)))||any(greaterThanEqual(oldUV,vec2(1)))||length(velocity)>.25) return false;
    vec4 old=p.previousClipFromView*vec4(position,1);
    if(old.w<=0) return false;
    float depth=textureLod(PreviousDepth,oldUV,0).r;
    if(depth>=1) return false;
    vec4 actual=p.previousInverseProjection*vec4(oldUV*2-1,depth,1);
    vec4 expected=p.previousInverseProjection*vec4(oldUV*2-1,old.z/old.w,1);
    if(abs(actual.z/actual.w-expected.z/expected.w)>max(.02,abs(expected.z/expected.w)*.005)) return false;
    vec4 n=textureLod(Normal,uv,0), oldN=textureLod(PreviousNormal,oldUV,0);
    // Octahedral normal comparison in world space also rejects replaced/disoccluded surfaces.
    vec2 f=n.rg*2-1, g=oldN.rg*2-1;
    vec3 a=vec3(f,1-abs(f.x)-abs(f.y)), b=vec3(g,1-abs(g.x)-abs(g.y));
    a.xy+=mix(vec2(clamp(-a.z,0,1)),vec2(-clamp(-a.z,0,1)),greaterThanEqual(a.xy,vec2(0)));
    b.xy+=mix(vec2(clamp(-b.z,0,1)),vec2(-clamp(-b.z,0,1)),greaterThanEqual(b.xy,vec2(0)));
    return oldN.a==1 && dot(normalize(a),normalize(b))>.9 && abs(n.b-oldN.b)<.1;
}
float distribution(float nh,float roughness) {
    float a=max(roughness*roughness,.0001), a2=a*a;
    float d=(1-nh*nh)+a2*nh*nh;
    return a2/(3.14159265359*d*d);
}
float smith(float nv,float roughness) {
    float a=max(roughness*roughness,.0001);
    return 2*nv/max(nv+sqrt(a*a+(1-a*a)*nv*nv),1e-8);
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
    imageStore(Ray,id,vec4(0));
    if(p.reprojection.z<.5) return;
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
    float pdf=roughness==0?1:distribution(max(dot(n,h),0),roughness)*smith(dot(n,v),roughness)/(4*dot(n,v));
    imageStore(Ray,id,vec4(start+direction*distance,pdf));
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
            vec3 hitPosition=unproject(hit.xy,z);
            vec2 oldUV;
            if(historyAt(hit.xy,hitPosition,oldUV)) {
                // SceneHDR is scene-linear without pre-exposure (R7a, as in FSR): exposure ratio is 1.
                imageStore(Result,id,vec4(textureLod(Color,oldUV,0).rgb*p.search.w,confidence));
                imageStore(Ray,id,vec4(hitPosition,pdf));
            }
        }
        // First visible surface terminates traversal even if invalid: no leaking through occluders.
        return;
    }
})";
        return source;
    }

    const std::string& SsrResolveShaderSource()
    {
        static const std::string source = [] {
            const auto& trace = SsrTraceShaderSource();
            const auto  begin = trace.find("vec3 unproject(");
            const auto  end   = trace.find("// Heitz", begin);
            return std::string(R"(
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0,std140) uniform Params {
    mat4 projection; mat4 inverseProjection; mat4 view;
    mat4 previousClipFromView; mat4 previousInverseProjection;
    vec4 search; vec4 shape; vec4 reprojection;
} p;
layout(set=0,binding=1) uniform sampler2D Source;
layout(set=0,binding=2) uniform sampler2D Normal;
layout(set=0,binding=3) uniform sampler2D HiZ;
layout(set=0,binding=4) uniform sampler2D Ray;
layout(set=0,binding=5) uniform sampler2D Velocity;
layout(set=0,binding=6) uniform sampler2D History;
layout(set=0,binding=7) uniform sampler2D PreviousDepth;
layout(set=0,binding=8) uniform sampler2D PreviousNormal;
layout(set=0,binding=9,rgba16f) uniform writeonly image2D Result;
layout(set=0,binding=10) uniform sampler2D Surface;
)") + trace.substr(begin, end - begin) +
                   R"(
float surfaceWeight(vec2 uv,vec2 other,vec3 origin,vec3 n,float roughness) {
    vec4 packed=textureLod(Normal,other,0);
    if(packed.a!=1 || abs(packed.b-roughness)>.1 || textureLod(Surface,other,0).a<.5) return 0;
    vec3 position=unproject(other,textureLod(HiZ,other,0).r);
    // Plane distance preserves slanted receivers without leaking across depth/normal edges.
    float plane=abs(dot(position-origin,n));
    return pow(max(0,dot(n,normalAt(other))),32)*exp(-plane/max(.01,abs(origin.z)*.002));
}
void main() {
    ivec2 id=ivec2(gl_GlobalInvocationID.xy), size=imageSize(Result);
    if(any(greaterThanEqual(id,size))) return;
    imageStore(Result,id,vec4(0));
    vec2 uv=(vec2(id)+.5)/vec2(size);
    vec4 packed=textureLod(Normal,uv,0);
    float depth=textureLod(HiZ,uv,0).r, roughness=packed.b;
    if(packed.a!=1 || depth>=1 || roughness>p.search.y || textureLod(Surface,uv,0).a<.5) return;
    vec3 origin=unproject(uv,depth), n=normalAt(uv);
    vec4 center=texelFetch(Source,id,0);
#if SSR_STAGE == 0
    // Stachowiak ratio estimator: sum(L * BRDF*cos/PDF) / sum(BRDF*cos/PDF).
    // Confidence includes misses in the denominator, avoiding bright hit-only bias.
    if(roughness<.02) { imageStore(Result,id,center); return; }
    vec3 v=p.projection[3][3]==1?vec3(0,0,1):normalize(-origin);
    float nv=max(dot(n,v),.001);
    vec3 f0=textureLod(Surface,uv,0).rgb;
    vec3 sum=vec3(0); float weights=0, hits=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        ivec2 q=id+ivec2(x,y);
        if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,size))) continue;
        vec2 other=(vec2(q)+.5)/vec2(size);
        vec4 ray=texelFetch(Ray,q,0), radiance=texelFetch(Source,q,0);
        if(ray.w<=0) continue;
        vec3 l=normalize(ray.xyz-origin), h=normalize(l+v);
        float nl=max(dot(n,l),0), nh=max(dot(n,h),0), vh=max(dot(v,h),0);
        if(nl<=0 || nh<=0) continue;
        vec3 fresnel=f0+(1-f0)*pow(1-vh,5);
        float brdf=distribution(nh,roughness)*smith(nv,roughness)*smith(nl,roughness)
                   *dot(fresnel,vec3(.2126,.7152,.0722))/(4*nv);
        float w=brdf/max(ray.w,1e-10)*surfaceWeight(uv,other,origin,n,roughness);
        weights+=w; hits+=w*radiance.a; sum+=w*radiance.a*radiance.rgb;
    }
    imageStore(Result,id,vec4(sum/max(hits,1e-10),hits/max(weights,1e-10)));
#elif SSR_STAGE == 1
    vec2 oldUV;
    // A current miss immediately returns to IBL: never resurrect stale reflected hits.
    if(center.a<=0 || p.shape.z<.5 || !historyAt(uv,origin,oldUV)) {
        imageStore(Result,id,center); return;
    }
    vec3 mean=vec3(0), square=vec3(0); float count=0;
    float loAlpha=1, hiAlpha=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        ivec2 q=clamp(id+ivec2(x,y),ivec2(0),size-1);
        vec2 other=(vec2(q)+.5)/vec2(size);
        vec4 c=texelFetch(Source,q,0);
        if(c.a<=0 || surfaceWeight(uv,other,origin,n,roughness)<.1) continue;
        mean+=c.rgb; square+=c.rgb*c.rgb; count+=1;
        loAlpha=min(loAlpha,c.a); hiAlpha=max(hiAlpha,c.a);
    }
    mean/=max(count,1); square/=max(count,1);
    vec3 sigma=sqrt(max(square-mean*mean,vec3(0)));
    vec4 old=textureLod(History,oldUV,0);
    old.rgb=clamp(old.rgb,mean-1.5*sigma,mean+1.5*sigma);
    old.a=clamp(old.a,loAlpha,hiAlpha);
    float weight=.9*exp(-length(textureLod(Velocity,uv,0).rg)*40);
    imageStore(Result,id,mix(center,old,weight));
#else
    if(roughness<.02 || center.a<=0) { imageStore(Result,id,center); return; }
    vec3 sum=vec3(0); float hits=0, weights=0;
    float radius=mix(.5,1.5,roughness);
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        ivec2 q=id+ivec2(x,y);
        if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,size))) continue;
        vec2 other=(vec2(q)+.5)/vec2(size);
        float w=exp(-float(x*x+y*y)/(2*radius*radius))*surfaceWeight(uv,other,origin,n,roughness);
        vec4 c=texelFetch(Source,q,0);
        sum+=c.rgb*c.a*w; hits+=c.a*w; weights+=w;
    }
    imageStore(Result,id,vec4(sum/max(hits,1e-10),hits/max(weights,1e-10)));
#endif
})";
        }();
        return source;
    }

    void RenderFrame::BuildReflections(RenderGraph& graph)
    {
        using namespace RenderHardware;
        auto&      state    = Output.GetTemporalState();
        const auto settings = View.Environment.ScreenSpaceReflections.Validated();
        const auto size     = settings.HalfResolution ? (View.Viewport + glm::uvec2(1)) / 2u : View.Viewport;
        auto       device   = GetDevice();
        const auto ensure   = [&](Handle<RenderHardware::Texture>& image,
                                  glm::uvec2                       extent,
                                  Format                           format,
                                  RenderHardware::TextureUsage     usage) {
            if (!image || image->GetDesc().Width != extent.x || image->GetDesc().Height != extent.y)
            {
                TextureDesc d;
                d.Width               = extent.x;
                d.Height              = extent.y;
                d.PixelFormat         = format;
                d.Usage               = usage;
                image                 = device->CreateTexture(d);
                state.ReflectionValid = false;
            }
        };
        const auto sampledCopy = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
        ensure(state.ReflectionColor,
               View.Viewport,
               Format::RGBA16Float,
               sampledCopy | RenderHardware::TextureUsage::CopySource);
        ensure(state.ReflectionDepth, View.Viewport, Format::R32Float, sampledCopy);
        ensure(state.ReflectionNormal, View.Viewport, Format::RGBA16Float, sampledCopy);
        for (auto& history : state.ReflectionHistory)
            ensure(history,
                   size,
                   Format::RGBA16Float,
                   RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::Storage |
                       RenderHardware::TextureUsage::CopySource);
        ensure(state.ReflectionFiltered,
               size,
               Format::RGBA16Float,
               RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::Storage |
                   RenderHardware::TextureUsage::CopySource);
        const bool valid         = state.ReflectionValid && !state.Reset && state.ReflectionSettings == settings;
        HiZ                      = BuildHiZ(graph, *device, Output.GetDepth(), state.HiZ);
        const auto     normal    = graph.ImportTexture("SceneNormalRoughness", Output.GetNormalRoughness());
        const auto     surface   = graph.ImportTexture("SSR receiver surface", ReflectionSurface);
        const auto     velocity  = graph.ImportTexture("SceneVelocity", Output.GetVelocity());
        const auto     color     = graph.ImportTexture("SSR previous final scene HDR", state.ReflectionColor);
        const auto     oldDepth  = graph.ImportTexture("SSR previous depth", state.ReflectionDepth);
        const auto     oldNormal = graph.ImportTexture("SSR previous normal roughness", state.ReflectionNormal);
        SsrTraceInputs inputs {
            HiZ, normal, color, surface, SceneProjection(), View.Scene.sceneCamera.viewMatrix, View.Viewport, settings};
        inputs.Velocity                  = velocity;
        inputs.PreviousDepth             = oldDepth;
        inputs.PreviousNormal            = oldNormal;
        inputs.PreviousClipFromView      = state.ReflectionPreviousViewProjection * glm::inverse(inputs.View);
        inputs.PreviousInverseProjection = state.ReflectionPreviousInverseProjection;
        inputs.JitterDelta  = (state.ReflectionPreviousJitter - Output.GetJitter()) / glm::vec2(View.Viewport);
        inputs.HistoryValid = valid;
        ReflectionTrace     = BuildSsrTrace(graph, *device, state, inputs);
        if (!state.ReflectionResolveLayout)
        {
            BindGroupLayoutDesc desc;
            desc.Group = 0;
            desc.Entries.push_back({0, BindingType::UniformBuffer});
            for (unsigned i = 1; i <= 10; ++i)
                desc.Entries.push_back({i, i == 9 ? BindingType::StorageTexture : BindingType::CombinedTextureSampler});
            state.ReflectionResolveLayout = device->CreateBindGroupLayout(desc);
            for (unsigned i = 0; i < 3; ++i)
            {
                const std::string shader =
                    "#version 450\n#define SSR_STAGE " + std::to_string(i) + "\n" + SsrResolveShaderSource();
                const auto& built = GetInternalComputeShaderData("SSR resolve stage " + std::to_string(i), shader);
                if (auto* error = std::get_if<AssetError>(&built))
                    throw std::runtime_error(error->Message);
                state.ReflectionResolvePipeline[i] =
                    device->CreateComputePipeline({device->CreateShader(std::get<ShaderData>(built)),
                                                   {state.ReflectionResolveLayout},
                                                   0,
                                                   "SSR denoise"});
            }
        }
        struct Parameters
        {
            glm::mat4 Projection, InverseProjection, View, PreviousClipFromView, PreviousInverseProjection;
            glm::vec4 Search, Shape, Reprojection;
        };
        const Parameters p {inputs.Projection,
                            glm::inverse(inputs.Projection),
                            inputs.View,
                            inputs.PreviousClipFromView,
                            inputs.PreviousInverseProjection,
                            {settings.MaxSteps, settings.MaxRoughness, settings.Thickness, settings.Intensity},
                            {0, 100, float(valid), 0},
                            {inputs.JitterDelta, float(valid), 1}};
        auto             uniform = device->CreateBuffer({sizeof(p), BufferUsage::Uniform, MemoryUsage::Upload});
        uniform->Write(0, &p, sizeof(p));
        auto       desc     = state.ReflectionFiltered->GetDesc();
        const auto resolved = graph.CreateTexture("SSR ratio estimator radiance confidence", desc);
        // BuildSsrTrace increments ReflectionFrame; alternate histories without reading the write target.
        const auto old =
            graph.ImportTexture("SSR temporal previous", state.ReflectionHistory[(state.ReflectionFrame - 1) % 2]);
        const auto next = graph.ImportTexture("SSR temporal next", state.ReflectionHistory[state.ReflectionFrame % 2]);
        const auto filtered = graph.ImportTexture("SSR filtered reflection", state.ReflectionFiltered);
        const auto ray      = graph.ImportTexture("SSR ray endpoint PDF", state.ReflectionRay);
        const std::array<GraphTexture, 3> sources {ReflectionTrace, resolved, next}, outputs {resolved, next, filtered};
        const std::array<const char*, 3>  names {
            "SSR ratio estimator resolve", "SSR temporal variance clamp", "SSR roughness spatial filter"};
        for (unsigned i = 0; i < 3; ++i)
        {
            const std::array<GraphTexture, 9> images {
                sources[i], normal, HiZ, ray, velocity, old, oldDepth, oldNormal, surface};
            std::vector<GraphUse> uses;
            for (auto image : images)
                uses.push_back({image, GraphAccess::SampledRead});
            uses.push_back({outputs[i], GraphAccess::StorageWrite});
            const auto pass =
                graph.AddPass(names[i],
                              GraphPassKind::Compute,
                              uses,
                              [=,
                               layout   = state.ReflectionResolveLayout,
                               pipeline = state.ReflectionResolvePipeline[i],
                               sampler  = state.ReflectionSampler](auto& c) {
                                  BindGroupDesc group;
                                  group.Layout = layout;
                                  group.Entries.push_back({0, uniform});
                                  for (unsigned j = 0; j < 8; ++j)
                                      group.Entries.push_back({j + 1, {}, 0, 0, c.Texture(images[j]), sampler});
                                  group.Entries.push_back({9, {}, 0, 0, c.Texture(outputs[i])});
                                  group.Entries.back().Subresource = TextureSubresource {};
                                  group.Entries.push_back({10, {}, 0, 0, c.Texture(surface), sampler});
                                  c.Commands.BindComputePipeline(pipeline);
                                  c.Commands.BindGroup(c.Device.CreateBindGroup(group));
                                  c.Commands.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
                              });
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        }
        graph.ExportTexture(next);
        graph.ExportTexture(filtered);
        Reflections              = state.ReflectionFiltered;
        state.ReflectionSettings = settings;
    }

    void RenderFrame::CaptureReflectionHistory(RenderGraph& graph)
    {
        using namespace RenderHardware;
        auto& state = Output.GetTemporalState();
        // Capture after transparents and before FSR/tonemapping. Histories stay at render resolution,
        // scene-linear and unexposed, independent of R7a's manual/automatic exposure value.
        const std::array<GraphTexture, 3> sources {
            graph.ImportTexture("SceneHDR", Output.GetHDR()),
            HiZ,
            graph.ImportTexture("SceneNormalRoughness", Output.GetNormalRoughness())};
        const std::array<GraphTexture, 3> outputs {
            graph.ImportTexture("SSR previous final scene HDR", state.ReflectionColor),
            graph.ImportTexture("SSR previous depth", state.ReflectionDepth),
            graph.ImportTexture("SSR previous normal roughness", state.ReflectionNormal)};
        std::vector<GraphUse> uses;
        for (unsigned i = 0; i < 3; ++i)
        {
            uses.push_back({sources[i], GraphAccess::TransferRead});
            uses.push_back({outputs[i], GraphAccess::TransferWrite});
            graph.ExportTexture(outputs[i]);
        }
        const auto projection     = SceneProjection();
        const auto viewProjection = projection * View.Scene.sceneCamera.viewMatrix;
        const auto jitter         = Output.GetJitter();
        const auto pass           = graph.AddPass("SSR final HDR and geometry history", uses, [=, &state](auto& c) {
            for (unsigned i = 0; i < 3; ++i)
                c.Commands.CopyTexture(c.Texture(sources[i]), {}, c.Texture(outputs[i]));
            state.ReflectionPreviousViewProjection    = viewProjection;
            state.ReflectionPreviousInverseProjection = glm::inverse(projection);
            state.ReflectionPreviousJitter            = jitter;
            state.ReflectionValid                     = true;
        });
        if (pass)
            graph.OrderAfter(pass, pass - 1);
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
        auto rayDesc        = state.ReflectionRaw->GetDesc();
        rayDesc.PixelFormat = Format::RGBA32Float;
        if (!state.ReflectionRay || state.ReflectionRay->GetDesc().Width != size.x ||
            state.ReflectionRay->GetDesc().Height != size.y)
            state.ReflectionRay = device.CreateTexture(rayDesc);
        const auto ray    = graph.ImportTexture("SSR ray endpoint PDF", state.ReflectionRay);
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
                                                                    {5, BindingType::StorageTexture},
                                                                    {6, BindingType::CombinedTextureSampler},
                                                                    {7, BindingType::CombinedTextureSampler},
                                                                    {8, BindingType::StorageTexture},
                                                                    {9, BindingType::CombinedTextureSampler}}});
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
            glm::mat4 Projection, InverseProjection, View, PreviousClipFromView, PreviousInverseProjection;
            glm::vec4 Search, Shape, Reprojection;
        };
        Parameters p {inputs.Projection,
                      glm::inverse(inputs.Projection),
                      inputs.View,
                      inputs.PreviousClipFromView,
                      inputs.PreviousInverseProjection,
                      {settings.MaxSteps, settings.MaxRoughness, settings.Thickness, settings.Intensity},
                      {state.ReflectionFrame++ % 64, 100, 0, 0},
                      {inputs.JitterDelta, float(inputs.HistoryValid), float(inputs.Velocity != uint32_t(-1))}};
        auto       uniform = device.CreateBuffer({sizeof(p), BufferUsage::Uniform, MemoryUsage::Upload});
        uniform->Write(0, &p, sizeof(p));
        const auto normal = inputs.Normal, surface = inputs.Surface, color = inputs.Color, HiZ = inputs.HiZ;
        const auto            velocity  = inputs.Velocity == uint32_t(-1) ? surface : inputs.Velocity;
        const auto            oldDepth  = inputs.PreviousDepth == uint32_t(-1) ? HiZ : inputs.PreviousDepth;
        const auto            oldNormal = inputs.PreviousNormal == uint32_t(-1) ? normal : inputs.PreviousNormal;
        std::vector<GraphUse> uses;
        for (auto image : {HiZ, normal, color, surface, velocity, oldDepth, oldNormal})
            if (std::none_of(uses.begin(), uses.end(), [&](auto u) { return u.Texture == image; }))
                uses.push_back({image, GraphAccess::SampledRead});
        uses.push_back({trace, GraphAccess::StorageWrite});
        uses.push_back({ray, GraphAccess::StorageWrite});
        auto pass = graph.AddPass("SSR Hi-Z VNDF trace",
                                  GraphPassKind::Compute,
                                  uses,
                                  [=,
                                   layout   = state.ReflectionLayout,
                                   pipeline = state.ReflectionPipeline,
                                   sampler  = state.ReflectionSampler](auto& c) {
                                      BindGroupDesc group;
                                      group.Layout = layout;
                                      group.Entries.push_back({0, uniform});
                                      const std::array<GraphTexture, 4> images {HiZ, normal, color, surface};
                                      for (unsigned i = 0; i < images.size(); ++i)
                                          group.Entries.push_back({i + 1, {}, 0, 0, c.Texture(images[i]), sampler});
                                      group.Entries.push_back({5, {}, 0, 0, result});
                                      group.Entries.back().Subresource = TextureSubresource {};
                                      group.Entries.push_back({6, {}, 0, 0, c.Texture(velocity), sampler});
                                      group.Entries.push_back({7, {}, 0, 0, c.Texture(oldDepth), sampler});
                                      group.Entries.push_back({8, {}, 0, 0, c.Texture(ray)});
                                      group.Entries.back().Subresource = TextureSubresource {};
                                      group.Entries.push_back({9, {}, 0, 0, c.Texture(oldNormal), sampler});
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
