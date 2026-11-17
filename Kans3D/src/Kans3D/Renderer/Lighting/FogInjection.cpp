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
            glm::mat4  InverseView, InverseProjection, ViewProjection, Owner, WorldToGrid;
            glm::vec4  Grid, Medium, Albedo, Bounds;
            glm::uvec4 Offsets, Info, Resolution;
        };
        constexpr const char* Common  = R"(
layout(local_size_x=64) in;
layout(set=0,binding=0,std140) uniform Parameters {
    mat4 inverseView,inverseProjection,viewProjection,owner,worldToGrid;
    vec4 grid,medium,albedo,bounds; uvec4 offsets,info,resolution;
} p;
layout(set=0,binding=1,std430) readonly buffer Attributes { vec4 attributes[]; };
layout(set=0,binding=2,std430) readonly buffer Flags { uint flags[]; };
layout(set=0,binding=4,std430) buffer Sums { uvec4 sums[]; };
layout(set=0,binding=5,rgba16f) uniform image3D media;
layout(set=0,binding=6,rgba16f) uniform image3D injected;
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
        constexpr const char* Splat   = R"(
void main(){
    uint i=invocationIndex();if(i>=p.info.x)return;
    if(flags[i]==0u)return;
    uint slot=i,cap=p.offsets.w;
    vec3 center=attributes[p.offsets.x*cap+slot].xyz;
    vec2 size=attributes[p.offsets.y*cap+slot].xy;
    vec4 tint=clamp(attributes[p.offsets.z*cap+slot],0,1);
    if(p.info.y!=0u)center=(p.owner*vec4(center,1)).xyz;
    float radius=max(abs(size.x),abs(size.y))*.5*p.medium.y;
    if(p.info.y!=0u)radius*=max(length(p.owner[0].xyz),max(length(p.owner[1].xyz),length(p.owner[2].xyz)));
    if(!(radius>0)||isnan(radius)||isinf(radius)||any(isnan(center))||any(isinf(center))||any(isnan(tint)))return;
    // Conservative projection of the world-space sphere's AABB. Near-plane crossing covers all XY.
    vec2 lo=vec2(1),hi=vec2(-1);bool crossing=false;
    for(int c=0;c<8;++c){
        vec3 corner=center+radius*(vec3(c&1,(c>>1)&1,(c>>2)&1)*2-1);
        vec4 clip=p.viewProjection*vec4(corner,1);
        if(clip.w<=0)crossing=true;
        else{vec2 q=clip.xy/clip.w;lo=min(lo,q);hi=max(hi,q);}
    }
    if(crossing){lo=vec2(-1);hi=vec2(1);}
    vec3 viewCenter=(inverse(p.inverseView)*vec4(center,1)).xyz;
    if(-viewCenter.z+radius<=0||-viewCenter.z-radius>=p.grid.w)return;
    ivec3 first=ivec3(floor((lo*.5+.5)*p.grid.xy),floor(depthSlice(-viewCenter.z-radius)));
    ivec3 last=ivec3(floor((hi*.5+.5)*p.grid.xy),floor(depthSlice(-viewCenter.z+radius)));
    first=clamp(first,ivec3(0),ivec3(p.grid.xyz)-1);last=clamp(last,ivec3(0),ivec3(p.grid.xyz)-1);
    for(int z=first.z;z<=last.z;++z)for(int y=first.y;y<=last.y;++y)for(int x=first.x;x<=last.x;++x){
        ivec3 cell=ivec3(x,y,z);float d=length(sampleWorld(cell)-center)/radius;
        float fade=1-smoothstep(0,1,d);
        float density=clamp(p.medium.x,0,100)*tint.a*fade;
        // 4096 candidates * 100 maximum density * 4096 precision stays below UINT32_MAX.
        uvec4 value=uvec4(vec4(clamp(p.albedo.rgb,0,1)*tint.rgb,1)*density*4096+.5);
        uint index=cellIndex(cell);
        atomicAdd(sums[index].x,value.x);atomicAdd(sums[index].y,value.y);
        atomicAdd(sums[index].z,value.z);atomicAdd(sums[index].w,value.w);
    }
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
        void                  Initialize(Device& device, FogInjectionState& state)
        {
            state.Layout = device.CreateBindGroupLayout({0,
                                                         {{0, BindingType::UniformBuffer},
                                                          {1, BindingType::ReadOnlyStorageBuffer},
                                                          {2, BindingType::ReadOnlyStorageBuffer},
                                                          {4, BindingType::StorageBuffer},
                                                          {5, BindingType::StorageTexture},
                                                          {6, BindingType::StorageTexture}}});
            auto make    = [&](const char* name, const char* body) {
                auto result = GetInternalComputeShaderData(name, std::string("#version 450\n") + Common + body);
                if (auto e = std::get_if<AssetError>(&result))
                    throw std::runtime_error(e->Message);
                return device.CreateComputePipeline(
                    {device.CreateShader(std::get<ShaderData>(result)), {state.Layout}, 0, name});
            };
            state.Splat   = make("Fog particle splat", Splat);
            state.Grid    = make("Fog density grid", Grid);
            state.Resolve = make("Fog particle resolve", Resolve);
        }
    } // namespace
    uint32_t AddFogInjection(RenderGraph&                           graph,
                             Device&                                device,
                             const FogFrameResources&               fog,
                             const std::vector<FogInjectionSource>& input,
                             std::shared_ptr<FogInjectionState>&    storage)
    {
        auto sources = SelectFogInjectionSources(input);
        if (sources.empty())
            return uint32_t(-1);
        if (!storage)
        {
            storage = std::make_shared<FogInjectionState>();
            Initialize(device, *storage);
        }
        auto           state = storage;
        const uint32_t cells = fog.Size.x * fog.Size.y * fog.Size.z;
        const auto     sums  = graph.CreateBuffer(
            "Fog particle sums",
            {size_t(cells) * 16, BufferUsage::Storage | BufferUsage::CopyDestination, MemoryUsage::DeviceLocal});
        TextureDesc desc;
        desc.Dimension      = TextureDimension::Texture3D;
        desc.Width          = fog.Size.x;
        desc.Height         = fog.Size.y;
        desc.Depth          = fog.Size.z;
        desc.PixelFormat    = Format::RGBA16Float;
        desc.Usage          = RenderHardware::TextureUsage::Storage | RenderHardware::TextureUsage::Sampled |
                              RenderHardware::TextureUsage::CopySource;
        const auto injected = graph.CreateTexture("Fog injected density", desc);
        auto       clear =
            graph.AddPass("Fog injection clear",
                          GraphPassKind::Transfer,
                          {{sums, GraphAccess::TransferWrite}},
                          [sums, cells](auto& c) { c.Commands.FillBuffer(c.Buffer(sums), 0, size_t(cells) * 16, 0); });
        if (clear)
            graph.OrderAfter(clear, clear - 1);
        auto record = [&](const FogInjectionSource& s, bool resolve) {
            Parameters p {};
            p.InverseView       = fog.InverseView;
            p.InverseProjection = fog.InverseProjection;
            p.ViewProjection    = glm::inverse(fog.InverseProjection) * glm::inverse(fog.InverseView);
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
            // Unused descriptors still have valid resources on every backend.
            auto                  attributes = s.Attributes, flags = s.Flags ? s.Flags : s.Attributes;
            std::vector<GraphUse> uses {
                {graph.ImportBuffer("Fog injection parameters", uniform), GraphAccess::UniformRead},
                {graph.ImportBuffer("Fog injection attributes", attributes), GraphAccess::StorageRead},
                {sums, resolve ? GraphAccess::StorageRead : GraphAccess::StorageReadWrite}};
            // Splats touch only the atomic buffer. Resolve initializes every injected texel before grids read it.
            if (resolve || s.Resolution.x)
            {
                uses.push_back({fog.Media, GraphAccess::StorageReadWrite});
                uses.push_back({injected, resolve ? GraphAccess::StorageWrite : GraphAccess::StorageReadWrite});
            }
            if (s.Flags)
                uses.push_back({graph.ImportBuffer("Fog injection flags", s.Flags), GraphAccess::StorageRead});
            auto pipeline = resolve ? state->Resolve : s.Resolution.x ? state->Grid : state->Splat;
            auto pass     = graph.AddPass(
                resolve        ? "Fog injection resolve" :
                s.Resolution.x ? "Fog grid injection" :
                                 "Fog particle injection",
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
                    const uint32_t count  = (resolve || s.Resolution.x) ? cells : s.Count;
                    const uint32_t groups = count / 64 + uint32_t(count % 64 != 0);
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
