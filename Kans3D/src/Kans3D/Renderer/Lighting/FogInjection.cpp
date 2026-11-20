#include "kspch.h"
#include "FogInjection.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Kans3D/VFX/VfxGpu.h"

namespace Kans
{
    using namespace RenderHardware;
    std::vector<FogInjectionSource> SelectFogInjectionSources(std::vector<FogInjectionSource> sources)
    {
        sources.erase(std::remove_if(sources.begin(),
                                     sources.end(),
                                     [](const auto& s) {
                                         return !std::isfinite(s.Density) || s.Density <= 0 ||
                                                !std::isfinite(s.RadiusScale) || s.RadiusScale <= 0 ||
                                                (!s.Resolution.x && !s.Capacity);
                                     }),
                      sources.end());
        std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
            return std::make_tuple(uint64_t(a.EntityID), a.EmitterIndex, a.RendererIndex) <
                   std::make_tuple(uint64_t(b.EntityID), b.EmitterIndex, b.RendererIndex);
        });
        uint32_t remaining = FogParticleInjectionLimit, grids = 0;
        for (auto& s : sources)
        {
            if (s.Resolution.x)
            {
                // Bound grid dispatches too; one trilinear lookup per froxel per selected grid.
                s.Count = grids++ < FogVolumeLimit ? 1 : 0;
                continue;
            }
            s.Count = std::min(s.Capacity, remaining);
            remaining -= s.Count;
        }
        sources.erase(std::remove_if(sources.begin(), sources.end(), [](const auto& s) { return !s.Count; }),
                      sources.end());
        return sources;
    }
    std::vector<FogInjectionSource> CollectFogInjectionSources(RenderFrame& frame)
    {
        std::vector<FogInjectionSource> sources;
        auto                            world = frame.World.GetVfxWorld();
        if (!world)
            return sources;
        for (const auto& [id, instance] : world->Instances())
        {
            if (frame.View.HiddenHierarchy.count(id))
                continue;
            uint32_t emitterIndex = 0;
            for (const auto& e : instance->Emitters())
            {
                const auto ei = emitterIndex++;
                if (!e.GPU || !e.Failure.empty() || e.HiddenByDistance)
                    continue;
                const auto& settings = e.Compiled->Settings;
                for (uint32_t ri = 0; ri < settings.Renderers.size(); ++ri)
                {
                    const auto& r = settings.Renderers[ri];
                    if (!r.InjectVolumetricFog || r.FogDensity <= 0)
                        continue;
                    FogInjectionSource s;
                    s.EntityID      = id;
                    s.EmitterIndex  = ei;
                    s.RendererIndex = ri;
                    s.Owner         = instance->OwnerTransform();
                    s.LocalSpace    = settings.SimulationSpace == Vfx::Space::Local;
                    s.Density       = r.FogDensity;
                    s.RadiusScale   = r.FogRadiusScale;
                    s.Albedo        = r.FogAlbedo;
                    if (!r.FogGrid.empty())
                    {
                        auto binding = settings.DataInterfaces.find(r.FogGrid);
                        auto storage = e.GPU->Grids.find(r.FogGrid);
                        if (binding == settings.DataInterfaces.end() || binding->second.Kind != "Grid3D" ||
                            storage == e.GPU->Grids.end())
                            continue;
                        const auto& grid = binding->second;
                        auto        density =
                            std::find_if(grid.GridAttributes.begin(), grid.GridAttributes.end(), [&](const auto& a) {
                                return a.Name == r.FogDensityAttribute && a.Kind == Vfx::Type::Float;
                            });
                        if (density == grid.GridAttributes.end())
                            continue;
                        s.Resolution    = grid.Resolution;
                        s.WorldBounds   = grid.WorldBounds;
                        s.DensityOffset = uint32_t(density - grid.GridAttributes.begin());
                        s.Attributes    = storage->second.Values[storage->second.Read];
                    }
                    else
                    {
                        auto offset = [&](const char* binding) -> std::optional<uint32_t> {
                            auto name = r.Bindings.find(binding);
                            if (name == r.Bindings.end())
                                return {};
                            for (const auto& a : e.Compiled->Attributes)
                                if (a.Name == name->second)
                                    return a.Offset;
                            return {};
                        };
                        auto pos = offset("Position"), size = offset("SpriteSize"), color = offset("Color");
                        if (!pos || !size || !color)
                            continue;
                        s.PositionOffset = *pos;
                        s.SizeOffset     = *size;
                        s.ColorOffset    = *color;
                        s.Capacity       = settings.Capacity;
                        s.Attributes     = e.GPU->Attributes;
                        s.Flags          = e.GPU->Flags;
                        if (settings.Target == Vfx::SimTarget::CPU)
                        {
                            std::vector<uint32_t> flags(std::min(settings.Capacity, FogParticleInjectionLimit), 0);
                            for (auto slot : e.Alive)
                                if (slot < flags.size())
                                    flags[slot] = 1;
                            s.Flags = frame.GetDevice()->CreateBuffer(
                                {flags.size() * 4, BufferUsage::Storage, MemoryUsage::Upload});
                            s.Flags->Write(0, flags.data(), flags.size() * 4);
                        }
                    }
                    sources.push_back(std::move(s));
                }
            }
        }
        return SelectFogInjectionSources(std::move(sources));
    }
    namespace
    {
        struct Parameters
        {
            glm::mat4  InverseView, InverseProjection, Projection, Owner, WorldToGrid;
            glm::vec4  Grid, Medium, Albedo, Bounds;
            glm::uvec4 Offsets, Info, Resolution;
        };
        constexpr const char* Common          = R"(
layout(local_size_x=64) in;
layout(set=0,binding=0,std140) uniform Parameters {
    mat4 inverseView,inverseProjection,projection,owner,worldToGrid;
    vec4 grid,medium,albedo,bounds; uvec4 offsets,info,resolution;
} p;
// Flatten bounded 2D dispatch rows; large/high-quality views can exceed 65535 groups in X.
uint invocationIndex(){return gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*gl_WorkGroupSize.x;}
float sliceDepth(float z){return .1*(exp(log(1+p.grid.w/.1)*z/p.grid.z)-1);}
float depthSlice(float z){return log(1+max(0,z)/.1)/log(1+p.grid.w/.1)*p.grid.z;}
vec3 sampleWorld(ivec3 cell){
    uvec3 v=uvec3(cell)*uvec3(1597334677u,3812015801u,2798796415u)+p.info.z*747796405u;
    uint h=(v.x^v.y^v.z)*2891336453u;h=(h^(h>>16))*2246822519u;
    vec3 j=(vec3(h&1023u,(h>>10)&1023u,(h>>20)&1023u)+.5)/1024.0;
    float depth=mix(sliceDepth(float(cell.z)),sliceDepth(float(cell.z+1)),j.z);
    vec2 uv=(vec2(cell.xy)+j.xy)/p.grid.xy;
    vec4 a=p.inverseProjection*vec4(uv*2-1,0,1),b=p.inverseProjection*vec4(uv*2-1,1,1);
    vec3 lo=a.xyz/a.w,hi=b.xyz/b.w;
    return (p.inverseView*vec4(mix(lo,hi,(-depth-lo.z)/(hi.z-lo.z)),1)).xyz;
}
uint cellIndex(ivec3 c){return uint((c.z*int(p.grid.y)+c.y)*int(p.grid.x)+c.x);}
)";
        constexpr const char* ParticleInputs  = R"(
layout(set=0,binding=1,std430) readonly buffer Attributes { vec4 attributes[]; };
layout(set=0,binding=2,std430) readonly buffer Flags { uint flags[]; };
)";
        constexpr const char* SumsBinding     = R"(
layout(set=0,binding=4,std430) buffer Sums { uvec4 sums[]; };
)";
        constexpr const char* InjectionImages = R"(
layout(set=0,binding=5,rgba16f) uniform image3D media;
layout(set=0,binding=6,rgba16f) uniform image3D injected;
)";
        // Keep the std430 strides in sync with the shader structs.
        constexpr uint32_t    ParticleWorkBytes = 48, SliceWorkBytes = 24;
        constexpr const char* WorkTypes = R"(
struct ParticleWork { vec4 sphere,tint; uint end,sliceCount; };
struct SliceWork { uint x,y,width,height,end,z; };
// 64 lanes, eight cells per lane. This also bounds the size of the flattened indirect dispatch.
const uint CellsPerGroup=gl_WorkGroupSize.x*8u;
)";
        constexpr const char* Bounds    = R"(
layout(set=0,binding=7,std430) writeonly buffer Particles { ParticleWork particles[]; };
layout(set=0,binding=8,std430) writeonly buffer Slices { SliceWork slices[]; };

shared vec4 sphere,tint,viewSphere;
shared vec2 depthShift,sectionExtent;
shared uint firstSlice,sliceCount;
shared uint laneEnds[64];

void loadParticle(uint i){
    sliceCount=0u;
    particles[i].end=0u;
    if(flags[i]==0u)return;
    uint cap=p.offsets.w;
    vec3 center=attributes[p.offsets.x*cap+i].xyz;
    vec2 size=attributes[p.offsets.y*cap+i].xy;
    tint=clamp(attributes[p.offsets.z*cap+i],0,1);
    if(tint.a==0||any(isnan(tint)))return;
    if(p.info.y!=0u)center=(p.owner*vec4(center,1)).xyz;
    float radius=max(abs(size.x),abs(size.y))*.5*p.medium.y;
    if(p.info.y!=0u)radius*=max(length(p.owner[0].xyz),max(length(p.owner[1].xyz),length(p.owner[2].xyz)));
    if(!(radius>0)||isnan(radius)||isinf(radius)||any(isnan(center))||any(isinf(center)))return;

    mat4 view=inverse(p.inverseView);
    vec3 v=(view*vec4(center,1)).xyz;
    float depth=-v.z;
    // Preserve the original slice limit, including for scaled camera transforms.
    if(depth+radius<=0||depth-radius>=p.grid.w)return;
    firstSlice=uint(clamp(floor(depthSlice(depth-radius)),0,p.grid.z-1));
    uint lastSlice=uint(clamp(floor(depthSlice(depth+radius)),0,p.grid.z-1));
    sliceCount=lastSlice-firstSlice+1u;
    sphere=vec4(center,radius);

    // A camera's affine transform maps the world sphere to an ellipsoid. Split each
    // view-space axis into its component along the depth direction and its tangent.
    mat3 rows=transpose(mat3(view));
    float depthScale=length(rows[2]);
    vec3 axis=-rows[2]/depthScale;
    vec2 along=vec2(dot(rows[0],axis),dot(rows[1],axis));
    // Allow for roundoff in sampleWorld's inverse transforms and the view transform.
    float paddedRadius=radius+1e-6*max(radius,max(max(abs(center.x),abs(center.y)),
                                               max(abs(center.z),length(p.inverseView[3].xyz))));
    depthShift=along*paddedRadius;
    sectionExtent=vec2(length(rows[0]-axis*along.x),length(rows[1]-axis*along.y))*paddedRadius;
    viewSphere=vec4(v.xy,depth,depthScale*paddedRadius);
}

void includeProjection(vec2 nLo,vec2 nHi,float wLo,float wHi,inout vec2 lo,inout vec2 hi){
    lo=min(lo,mix(nLo/wHi,wLo>0?nLo/wLo:vec2(-1),lessThan(nLo,vec2(0))));
    hi=max(hi,mix(nHi/wHi,wLo>0?nHi/wLo:vec2(1),greaterThan(nHi,vec2(0))));
}

SliceWork sliceBounds(uint z){
    SliceWork empty=SliceWork(0,0,0,0,0,z);
    float nearDepth=sliceDepth(float(z)),farDepth=sliceDepth(float(z+1u));
    // Fog also samples between the eye and the raster near plane. Clamp to a
    // positive depth below the earliest jittered sample, including in slice zero.
    nearDepth=max(mix(nearDepth,farDepth,.25/1024.0),viewSphere.z-viewSphere.w);
    farDepth=min(farDepth,viewSphere.z+viewSphere.w);
    if(nearDepth>farDepth)return empty;

    vec2 u=clamp((vec2(nearDepth,farDepth)-viewSphere.z)/viewSphere.w,-1,1);
    // Maximum sphere cross-section over the slab occurs at its closest depth to the center.
    float closest=clamp(0,u.x,u.y);
    vec2 extent=sectionExtent*sqrt(max(0,(1-closest)*(1+closest)));
    vec2 shift0=depthShift*u.x,shift1=depthShift*u.y;
    vec2 lo=viewSphere.xy+min(shift0,shift1)-extent;
    vec2 hi=viewSphere.xy+max(shift0,shift1)+extent;
    vec2 mid=.5*lo+.5*hi,halfSize=.5*hi-.5*lo;

    // Project the rectangle at both end depths. In perspective, a signed edge
    // can be extremal at the far depth. Orthographic W is constant.
    mat4 projection=p.projection;
    vec4 a=projection*vec4(mid,-nearDepth,1),b=projection*vec4(mid,-farDepth,1);
    vec2 xyRadius=abs(projection[0].xy)*halfSize.x+abs(projection[1].xy)*halfSize.y;
    vec2 ndcLo,ndcHi;
    float wRadius=dot(abs(vec2(projection[0].w,projection[1].w)),halfSize);
    if(wRadius==0&&min(a.w,b.w)>0){
        ndcLo=min((a.xy-xyRadius)/a.w,(b.xy-xyRadius)/b.w);
        ndcHi=max((a.xy+xyRadius)/a.w,(b.xy+xyRadius)/b.w);
    }else{
        // Custom projections can also vary W across XY or reverse its sign.
        // Bound each sign separately, since both can reconstruct samples.
        float wLo=min(a.w,b.w)-wRadius,wHi=max(a.w,b.w)+wRadius;
        vec2 nLo=min(a.xy,b.xy)-xyRadius,nHi=max(a.xy,b.xy)+xyRadius;
        if(!(wHi>0||wLo<0))return SliceWork(0,0,uint(p.grid.x),uint(p.grid.y),0,z);
        ndcLo=vec2(3.402823466e38);ndcHi=-ndcLo;
        if(wHi>0)includeProjection(nLo,nHi,wLo,wHi,ndcLo,ndcHi);
        if(wLo<0)includeProjection(-nHi,-nLo,-wHi,-wLo,ndcLo,ndcHi);
    }
    // Saturate infinities before adding an outward rounding margin. An
    // indeterminate edge still keeps the part of the slice on screen.
    ndcLo=clamp(mix(ndcLo,vec2(-1),isnan(ndcLo)),-2,2);
    ndcHi=clamp(mix(ndcHi,vec2(1),isnan(ndcHi)),-2,2);
    ndcLo-=2e-5*(1+abs(ndcLo));
    ndcHi+=2e-5*(1+abs(ndcHi));
    if(any(greaterThan(ndcLo,vec2(1)))||any(lessThan(ndcHi,vec2(-1))))return empty;
    uvec2 first=uvec2(min((clamp(ndcLo,-1,1)*.5+.5)*p.grid.xy,p.grid.xy-1));
    uvec2 last=uvec2(min((clamp(ndcHi,-1,1)*.5+.5)*p.grid.xy,p.grid.xy-1));
    uvec2 size=last-first+1u;
    return SliceWork(first.x,first.y,size.x,size.y,0,z);
}

void main(){
    uint i=gl_WorkGroupID.x,lane=gl_LocalInvocationIndex;
    if(lane==0u)loadParticle(i);
    barrier();
    if(sliceCount==0u)return;
    // At most 128 fog slices, so each lane computes at most two rectangles.
    uint perLane=(sliceCount+gl_WorkGroupSize.x-1u)/gl_WorkGroupSize.x;
    uint start=lane*perLane,stop=min(start+perLane,sliceCount),total=0u;
    SliceWork work[2];
    for(uint s=start;s<stop;++s){
        SliceWork rect=sliceBounds(firstSlice+s);
        total+=(rect.width*rect.height+CellsPerGroup-1u)/CellsPerGroup;
        rect.end=total;
        work[s-start]=rect;
    }
    laneEnds[lane]=total;
    barrier();
    if(lane==0u){
        for(uint j=1u;j<gl_WorkGroupSize.x;++j)laneEnds[j]+=laneEnds[j-1u];
        particles[i]=ParticleWork(sphere,tint,laneEnds[gl_WorkGroupSize.x-1u],sliceCount);
    }
    barrier();
    uint base=lane==0u?0u:laneEnds[lane-1u];
    for(uint s=start;s<stop;++s){
        SliceWork rect=work[s-start];
        rect.end+=base;
        slices[i*uint(p.grid.z)+s]=rect;
    }
}
)";
        constexpr const char* Schedule  = R"(
layout(set=0,binding=7,std430) buffer Particles { ParticleWork particles[]; };
layout(set=0,binding=9,std430) writeonly buffer Dispatch { uint x,y,z; } args;
shared uint laneEnds[64];

void main(){
    // One workgroup prefixes the per-particle group counts in contiguous ranges.
    uint lane=gl_LocalInvocationIndex;
    uint perLane=(p.info.x+gl_WorkGroupSize.x-1u)/gl_WorkGroupSize.x;
    uint start=lane*perLane,stop=min(start+perLane,p.info.x),localEnd=0u;
    for(uint i=start;i<stop;++i){
        localEnd+=particles[i].end;
        particles[i].end=localEnd;
    }
    laneEnds[lane]=localEnd;
    barrier();
    if(lane==0u){
        for(uint j=1u;j<gl_WorkGroupSize.x;++j)laneEnds[j]+=laneEnds[j-1u];
        uint total=laneEnds[gl_WorkGroupSize.x-1u];
        // A Vulkan storage binding is at most 2^32-1 bytes, hence fewer than
        // 2^28 froxels at 16 bytes each. With 4096 particles and at most 128
        // rounded-up chunks per particle, 512 cells per group fit in 65535^2.
        uint groupsX=min(total,max(64u,(total+65534u)/65535u));
        args.x=groupsX;
        args.y=total==0u?1u:(total+groupsX-1u)/groupsX;
        args.z=1u;
    }
    barrier();
    if(lane!=0u){
        uint base=laneEnds[lane-1u];
        for(uint i=start;i<stop;++i)particles[i].end+=base;
    }
}
)";
        constexpr const char* Splat     = R"(
layout(set=0,binding=7,std430) readonly buffer Particles { ParticleWork particles[]; };
layout(set=0,binding=8,std430) readonly buffer Slices { SliceWork slices[]; };
#ifdef FOG_SPLAT_COUNTERS
layout(set=0,binding=10,std430) buffer Diagnostics {
    uint candidates,contributions,maxCellsPerLane;
};
#endif
shared vec4 sphere,tint;
shared SliceWork rect;
shared uint firstCell;

void main(){
    uint group=gl_WorkGroupID.x+gl_WorkGroupID.y*gl_NumWorkGroups.x;
    if(group>=particles[p.info.x-1u].end)return;
    uint lane=gl_LocalInvocationIndex;
    if(lane==0u){
        // Prefix ends allow a group to find its particle and slice even when
        // many particle slots or slice rectangles have zero work.
        uint lo=0u,hi=p.info.x;
        while(lo<hi){
            uint mid=(lo+hi)/2u;
            if(particles[mid].end<=group)lo=mid+1u;else hi=mid;
        }
        ParticleWork particle=particles[lo];
        uint localGroup=group-(lo==0u?0u:particles[lo-1u].end);
        uint base=lo*uint(p.grid.z);
        lo=0u;hi=particle.sliceCount;
        while(lo<hi){
            uint mid=(lo+hi)/2u;
            if(slices[base+mid].end<=localGroup)lo=mid+1u;else hi=mid;
        }
        rect=slices[base+lo];
        firstCell=(localGroup-(lo==0u?0u:slices[base+lo-1u].end))*CellsPerGroup;
        sphere=particle.sphere;
        tint=particle.tint;
    }
    barrier();
    uint stop=min(firstCell+CellsPerGroup,rect.width*rect.height);
#ifdef FOG_SPLAT_COUNTERS
    uint visits=0u,writes=0u;
#endif
    for(uint i=firstCell+lane;i<stop;i+=gl_WorkGroupSize.x){
#ifdef FOG_SPLAT_COUNTERS
        ++visits;
#endif
        ivec3 cell=ivec3(rect.x+i%rect.width,rect.y+i/rect.width,rect.z);
        float d=length(sampleWorld(cell)-sphere.xyz)/sphere.w;
        float fade=1-smoothstep(0,1,d);
        if(fade==0)continue;
        float density=clamp(p.medium.x,0,100)*tint.a*fade;
        // 4096 candidates * 100 maximum density * 4096 precision stays below UINT32_MAX.
        uvec4 value=uvec4(vec4(clamp(p.albedo.rgb,0,1)*tint.rgb,1)*density*4096+.5);
        uint index=cellIndex(cell);
#ifdef FOG_SPLAT_COUNTERS
        ++writes;
#endif
        atomicAdd(sums[index].x,value.x);atomicAdd(sums[index].y,value.y);
        atomicAdd(sums[index].z,value.z);atomicAdd(sums[index].w,value.w);
    }
#ifdef FOG_SPLAT_COUNTERS
    if(visits!=0u){
        atomicAdd(candidates,visits);
        atomicMax(maxCellsPerLane,visits);
    }
    if(writes!=0u)atomicAdd(contributions,writes);
#endif
}
)";
        constexpr const char* Grid    = R"(
float density(ivec3 c){
    c=clamp(c,ivec3(0),ivec3(p.resolution.xyz)-1);
    uint cells=p.resolution.x*p.resolution.y*p.resolution.z;
    uint at=p.resolution.w*cells+uint((c.z*int(p.resolution.y)+c.y)*int(p.resolution.x)+c.x);
    float d=attributes[at].x;return isnan(d)||isinf(d)?0:clamp(d,0,100);
}
void main(){
    uint index=invocationIndex(),total=uint(p.grid.x*p.grid.y*p.grid.z);if(index>=total)return;
    ivec3 cell=ivec3(index%uint(p.grid.x),(index/uint(p.grid.x))%uint(p.grid.y),index/uint(p.grid.x*p.grid.y));
    vec3 q=(p.worldToGrid*vec4(sampleWorld(cell),1)).xyz/p.bounds.xyz+.5;
    if(any(lessThan(q,vec3(0)))||any(greaterThan(q,vec3(1))))return;
    vec3 at=q*vec3(p.resolution.xyz)-.5,w=fract(at);ivec3 base=ivec3(floor(at));float d=0;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        d+=density(base+ivec3(x,y,z))*(x==0?1-w.x:w.x)*(y==0?1-w.y:w.y)*(z==0?1-w.z:w.z);
    vec4 value=vec4(clamp(p.albedo.rgb,0,1),1)*d*clamp(p.medium.x,0,100);
    imageStore(injected,cell,min(imageLoad(injected,cell)+value,vec4(60000)));
    imageStore(media,cell,min(imageLoad(media,cell)+value,vec4(60000)));
}
)";
        constexpr const char* Resolve = R"(
void main(){
    uint i=invocationIndex(),total=uint(p.grid.x*p.grid.y*p.grid.z);if(i>=total)return;
    ivec3 cell=ivec3(i%uint(p.grid.x),(i/uint(p.grid.x))%uint(p.grid.y),i/uint(p.grid.x*p.grid.y));
    vec4 value=vec4(sums[i])/4096;
    imageStore(injected,cell,min(value,vec4(60000)));
    imageStore(media,cell,min(imageLoad(media,cell)+value,vec4(60000)));
}
)";
        Handle<ComputePipeline>
        MakePipeline(Device& device, const Handle<BindGroupLayout>& layout, const char* name, const std::string& body)
        {
            auto result = GetInternalComputeShaderData(name, std::string("#version 450\n") + Common + body);
            if (auto e = std::get_if<AssetError>(&result))
                throw std::runtime_error(e->Message);
            return device.CreateComputePipeline({device.CreateShader(std::get<ShaderData>(result)), {layout}, 0, name});
        }

        void Initialize(Device& device, FogInjectionState& state)
        {
            state.Layout = device.CreateBindGroupLayout({0,
                                                         {{0, BindingType::UniformBuffer},
                                                          {1, BindingType::ReadOnlyStorageBuffer},
                                                          {2, BindingType::ReadOnlyStorageBuffer},
                                                          {4, BindingType::StorageBuffer},
                                                          {5, BindingType::StorageTexture},
                                                          {6, BindingType::StorageTexture}}});
            const auto bindings = std::string(ParticleInputs) + SumsBinding + InjectionImages;
            state.Grid          = MakePipeline(device, state.Layout, "Fog density grid", bindings + Grid);
            state.Resolve       = MakePipeline(device, state.Layout, "Fog particle resolve", bindings + Resolve);
        }

        void InitializeParticles(Device& device, FogInjectionState& state, bool diagnostics)
        {
            if (!state.Splat)
            {
                state.BoundsLayout   = device.CreateBindGroupLayout({0,
                                                                     {{0, BindingType::UniformBuffer},
                                                                      {1, BindingType::ReadOnlyStorageBuffer},
                                                                      {2, BindingType::ReadOnlyStorageBuffer},
                                                                      {7, BindingType::StorageBuffer},
                                                                      {8, BindingType::StorageBuffer}}});
                state.ScheduleLayout = device.CreateBindGroupLayout({0,
                                                                     {{0, BindingType::UniformBuffer},
                                                                      {7, BindingType::StorageBuffer},
                                                                      {9, BindingType::StorageBuffer}}});
                state.SplatLayout    = device.CreateBindGroupLayout({0,
                                                                     {{0, BindingType::UniformBuffer},
                                                                      {4, BindingType::StorageBuffer},
                                                                      {7, BindingType::ReadOnlyStorageBuffer},
                                                                      {8, BindingType::ReadOnlyStorageBuffer}}});
                state.Bounds         = MakePipeline(device,
                                                    state.BoundsLayout,
                                                    "Fog particle bounds",
                                                    std::string(ParticleInputs) + WorkTypes + Bounds);
                state.Schedule       = MakePipeline(
                    device, state.ScheduleLayout, "Fog particle schedule", std::string(WorkTypes) + Schedule);
                state.Splat = MakePipeline(
                    device, state.SplatLayout, "Fog particle splat", std::string(SumsBinding) + WorkTypes + Splat);
            }
            if (diagnostics && !state.DiagnosticSplat)
            {
                auto layout = state.SplatLayout->GetDesc();
                layout.Entries.push_back({10, BindingType::StorageBuffer});
                state.DiagnosticLayout = device.CreateBindGroupLayout(layout);
                state.DiagnosticSplat =
                    MakePipeline(device,
                                 state.DiagnosticLayout,
                                 "Fog particle splat diagnostics",
                                 std::string(SumsBinding) + WorkTypes + "#define FOG_SPLAT_COUNTERS\n" + Splat);
            }
        }

        void AddParticleSplat(RenderGraph&                              graph,
                              const std::shared_ptr<FogInjectionState>& state,
                              const FogInjectionSource&                 source,
                              uint32_t                                  slicesPerParticle,
                              const Handle<RenderHardware::Buffer>&     uniform,
                              GraphBuffer                               sums,
                              const FogInjectionDiagnostics*            diagnostics)
        {
            const auto parameters = graph.ImportBuffer("Fog injection parameters", uniform);
            const auto attributes = graph.ImportBuffer("Fog injection attributes", source.Attributes);
            const auto flags      = source.Flags ? graph.ImportBuffer("Fog injection flags", source.Flags) : attributes;
            const auto particles = graph.CreateBuffer("Fog particle work",
                                                      {size_t(source.Count) * ParticleWorkBytes, BufferUsage::Storage});
            const auto slices    = graph.CreateBuffer(
                "Fog slice work", {size_t(source.Count) * slicesPerParticle * SliceWorkBytes, BufferUsage::Storage});
            const auto dispatch = graph.CreateBuffer(
                "Fog splat dispatch", {3 * sizeof(uint32_t), BufferUsage::Storage | BufferUsage::Indirect});
            std::vector<GraphUse> boundsUses {{parameters, GraphAccess::UniformRead},
                                              {attributes, GraphAccess::StorageRead},
                                              {particles, GraphAccess::StorageWrite},
                                              {slices, GraphAccess::StorageWrite}};
            if (flags != attributes)
                boundsUses.emplace_back(flags, GraphAccess::StorageRead);
            const auto boundsPass =
                graph.AddPass("Fog particle bounds", GraphPassKind::Compute, std::move(boundsUses), [=](auto& c) {
                    c.Commands.BindComputePipeline(state->Bounds);
                    c.Commands.BindGroup(c.Device.CreateBindGroup({state->BoundsLayout,
                                                                   {{0, uniform},
                                                                    {1, c.Buffer(attributes)},
                                                                    {2, c.Buffer(flags)},
                                                                    {7, c.Buffer(particles)},
                                                                    {8, c.Buffer(slices)}}}));
                    c.Commands.Dispatch(source.Count);
                });
            graph.OrderAfter(boundsPass, boundsPass - 1);
            graph.AddPass(
                "Fog particle schedule",
                GraphPassKind::Compute,
                {{parameters, GraphAccess::UniformRead},
                 {particles, GraphAccess::StorageReadWrite},
                 {dispatch, GraphAccess::StorageWrite}},
                [=](auto& c) {
                    c.Commands.BindComputePipeline(state->Schedule);
                    c.Commands.BindGroup(c.Device.CreateBindGroup(
                        {state->ScheduleLayout, {{0, uniform}, {7, c.Buffer(particles)}, {9, c.Buffer(dispatch)}}}));
                    c.Commands.Dispatch(1);
                });
            const GraphBuffer     counters = diagnostics ? diagnostics->Counters : GraphBuffer {};
            std::vector<GraphUse> uses {{parameters, GraphAccess::UniformRead},
                                        {particles, GraphAccess::StorageRead},
                                        {slices, GraphAccess::StorageRead},
                                        {dispatch, GraphAccess::IndirectRead},
                                        {sums, GraphAccess::StorageReadWrite}};
            const auto            pipeline = diagnostics ? state->DiagnosticSplat : state->Splat;
            const auto            layout   = diagnostics ? state->DiagnosticLayout : state->SplatLayout;
            if (diagnostics)
                uses.emplace_back(counters, GraphAccess::StorageReadWrite);
            graph.AddPass("Fog particle injection", GraphPassKind::Compute, std::move(uses), [=](auto& c) {
                BindGroupDesc bindings {
                    layout, {{0, uniform}, {4, c.Buffer(sums)}, {7, c.Buffer(particles)}, {8, c.Buffer(slices)}}};
                if (counters != GraphBuffer {})
                    bindings.Entries.push_back({10, c.Buffer(counters)});
                c.Commands.BindComputePipeline(pipeline);
                c.Commands.BindGroup(c.Device.CreateBindGroup(bindings));
                c.Commands.DispatchIndirect(c.Buffer(dispatch));
            });
        }
    } // namespace
    uint32_t AddFogInjection(RenderGraph&                           graph,
                             Device&                                device,
                             const FogFrameResources&               fog,
                             const std::vector<FogInjectionSource>& input,
                             std::shared_ptr<FogInjectionState>&    storage,
                             FogInjectionDiagnostics*               diagnostics)
    {
        auto sources = SelectFogInjectionSources(input);
        if (sources.empty())
            return uint32_t(-1);
        if (!storage)
        {
            storage = std::make_shared<FogInjectionState>();
            Initialize(device, *storage);
        }
        auto state = storage;
        if (std::any_of(sources.begin(), sources.end(), [](const auto& s) { return !s.Resolution.x; }))
            InitializeParticles(device, *state, diagnostics != nullptr);
        const uint32_t cells = fog.Size.x * fog.Size.y * fog.Size.z;
        auto           sumsUsage = BufferUsage::Storage | BufferUsage::CopyDestination;
        if (diagnostics)
            sumsUsage = sumsUsage | BufferUsage::CopySource;
        const auto sums = graph.CreateBuffer("Fog particle sums", {size_t(cells) * 16, sumsUsage});
        if (diagnostics)
        {
            diagnostics->Sums     = sums;
            diagnostics->Counters = graph.CreateBuffer(
                "Fog particle counters",
                {3 * sizeof(uint32_t), BufferUsage::Storage | BufferUsage::CopySource | BufferUsage::CopyDestination});
        }
        TextureDesc desc;
        desc.Dimension      = TextureDimension::Texture3D;
        desc.Width          = fog.Size.x;
        desc.Height         = fog.Size.y;
        desc.Depth          = fog.Size.z;
        desc.PixelFormat    = Format::RGBA16Float;
        desc.Usage          = RenderHardware::TextureUsage::Storage | RenderHardware::TextureUsage::Sampled |
                              RenderHardware::TextureUsage::CopySource;
        const auto injected = graph.CreateTexture("Fog injected density", desc);
        const GraphBuffer     counters = diagnostics ? diagnostics->Counters : GraphBuffer {};
        std::vector<GraphUse> clearUses {{sums, GraphAccess::TransferWrite}};
        if (diagnostics)
            clearUses.emplace_back(counters, GraphAccess::TransferWrite);
        auto clear = graph.AddPass("Fog injection clear", GraphPassKind::Transfer, std::move(clearUses), [=](auto& c) {
            c.Commands.FillBuffer(c.Buffer(sums), 0, size_t(cells) * 16, 0);
            if (counters != GraphBuffer {})
                c.Commands.FillBuffer(c.Buffer(counters), 0, 3 * sizeof(uint32_t), 0);
        });
        if (clear)
            graph.OrderAfter(clear, clear - 1);
        auto record = [&](const FogInjectionSource& s, bool resolve) {
            Parameters p {};
            p.InverseView       = fog.InverseView;
            p.InverseProjection = fog.InverseProjection;
            p.Projection        = glm::inverse(fog.InverseProjection);
            p.Owner             = s.Owner;
            p.WorldToGrid = glm::inverse(s.LocalSpace ? s.Owner : glm::translate(glm::mat4(1), glm::vec3(s.Owner[3])));
            p.Grid        = {glm::vec3(fog.Size), fog.Distance};
            p.Medium      = {s.Density, s.RadiusScale, 0, 0};
            p.Albedo      = {s.Albedo, 0};
            p.Bounds      = {s.WorldBounds, 0};
            p.Offsets     = {s.PositionOffset, s.SizeOffset, s.ColorOffset, s.Capacity};
            p.Info        = {s.Count, s.LocalSpace ? 1u : 0u, fog.JitterFrame, 0};
            p.Resolution  = {s.Resolution, s.DensityOffset};
            auto uniform  = device.CreateBuffer({sizeof(p), BufferUsage::Uniform, MemoryUsage::Upload});
            uniform->Write(0, &p, sizeof(p));
            if (!resolve && !s.Resolution.x)
            {
                AddParticleSplat(graph, state, s, fog.Size.z, uniform, sums, diagnostics);
                return;
            }
            // Unused descriptors still have valid resources on every backend.
            auto                  attributes = s.Attributes, flags = s.Flags ? s.Flags : s.Attributes;
            std::vector<GraphUse> uses {
                {graph.ImportBuffer("Fog injection parameters", uniform), GraphAccess::UniformRead},
                {graph.ImportBuffer("Fog injection attributes", attributes), GraphAccess::StorageRead},
                {sums, resolve ? GraphAccess::StorageRead : GraphAccess::StorageReadWrite}};
            // Resolve initializes every injected texel before grids read it.
            uses.push_back({fog.Media, GraphAccess::StorageReadWrite});
            uses.push_back({injected, resolve ? GraphAccess::StorageWrite : GraphAccess::StorageReadWrite});
            if (s.Flags)
                uses.push_back({graph.ImportBuffer("Fog injection flags", s.Flags), GraphAccess::StorageRead});
            auto pipeline = resolve ? state->Resolve : state->Grid;
            auto pass     = graph.AddPass(
                resolve ? "Fog injection resolve" : "Fog grid injection",
                GraphPassKind::Compute,
                std::move(uses),
                [=](auto& c) {
                    c.Commands.BindComputePipeline(pipeline);
                    c.Commands.BindGroup(c.Device.CreateBindGroup({state->Layout,
                                                                   {{0, uniform},
                                                                    {1, attributes},
                                                                    {2, flags},
                                                                    {4, c.Buffer(sums)},
                                                                    {5, {}, 0, 0, c.Texture(fog.Media)},
                                                                    {6, {}, 0, 0, c.Texture(injected)}}}));
                    const uint32_t groups = cells / 64 + uint32_t(cells % 64 != 0);
                    // Both axes stay within Vulkan's guaranteed 65535-group limit. Small views also
                    // use multiple rows, so ordinary injection tests exercise row addressing.
                    const uint32_t groupsX = std::min(groups, std::max(64u, (groups + 65534u) / 65535u));
                    c.Commands.Dispatch(groupsX, (groups + groupsX - 1) / groupsX);
                });
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        };
        for (const auto& s : sources)
            if (!s.Resolution.x)
                record(s, false);
        record(sources.front(), true);
        for (const auto& s : sources)
            if (s.Resolution.x)
                record(s, false);
        return injected;
    }
} // namespace Kans
