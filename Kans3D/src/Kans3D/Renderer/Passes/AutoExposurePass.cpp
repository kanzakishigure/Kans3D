#include "kspch.h"
#include "AutoExposurePass.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    void AutoExposurePass::Prepare(RenderHardware::Device& device,
                                   uint64_t                world,
                                   bool                    reset,
                                   float                   initialEV,
                                   float                   compensation)
    {
        using namespace RenderHardware;
        if (!m_State[0])
        {
            TextureDesc d;
            d.PixelFormat = Format::RGBA32Float;
            d.Usage       = TextureUsage::Storage | TextureUsage::Sampled | TextureUsage::CopySource;
            for (auto& state : m_State)
                state = device.CreateTexture(d);
            m_Histogram = device.CreateBuffer(
                {256 * 4, BufferUsage::Storage | BufferUsage::CopyDestination, MemoryUsage::DeviceLocal});
            for (auto& slot : m_Readbacks)
                slot.Buffer = device.CreateBuffer({16, BufferUsage::CopyDestination, MemoryUsage::Readback});
            reset = true;
        }
        if (reset || world != m_World)
        {
            m_World = world;
            m_Reset = true;
            m_EV.reset();
            m_Published = m_Sequence;
            const std::array<float, 4> initial {
                std::clamp(std::exp2(compensation - initialEV) / 1.2f, 1e-10f, 1e8f), initialEV, 0, 0};
            auto commands = device.CreateCommandList();
            for (auto& state : m_State)
                commands->ClearStorageTexture(state, initial);
            device.Submit(commands);
        }
        for (auto& slot : m_Readbacks)
            if (slot.Pending && slot.Buffer->IsReadbackReady())
            {
                auto  bytes = slot.Buffer->Read(0, 16);
                float ev;
                float multiplier;
                std::memcpy(&multiplier, bytes.data(), 4);
                ev = -std::log2(1.2f * multiplier);
                if (std::isfinite(ev) && slot.Sequence > m_Published)
                {
                    m_EV        = ev;
                    m_Published = slot.Sequence;
                }
                slot.Pending = false;
            }
    }
    void AutoExposurePass::PatchUniforms(RenderHardware::Device&                                device,
                                         RenderHardware::CommandList&                           commands,
                                         const RenderHardware::Handle<RenderHardware::Texture>& exposure,
                                         const RenderHardware::Handle<RenderHardware::Buffer>&  destination,
                                         uint32_t                                               offset,
                                         uint32_t                                               stride,
                                         uint32_t                                               count)
    {
        using namespace RenderHardware;
        if (!exposure || !count)
            return;
        if (offset % 4 || stride % 4 || (count > 1 && !stride) ||
            uint64_t(offset) + uint64_t(stride) * (count - 1) + 4 > destination->GetDesc().Bytes)
            throw std::invalid_argument("Invalid exposure uniform range");
        if (!m_PatchPipeline)
        {
            const auto& built = GetInternalComputeShaderData("Exposure frame constants", R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std140) uniform Parameters { uvec4 Address; } p;
layout(set=0,binding=1) uniform sampler2D Exposure;
layout(set=0,binding=2,std430) buffer Destination { uint words[]; } d;
void main() {
    uint index=gl_GlobalInvocationID.x;
    if(index<p.Address.z) d.words[p.Address.x+index*p.Address.y]=floatBitsToUint(texelFetch(Exposure,ivec2(0),0).r);
})");
            if (auto* error = std::get_if<AssetError>(&built))
                throw std::runtime_error(error->Message);
            auto program    = device.CreateShader(std::get<ShaderData>(built));
            m_PatchLayout   = device.CreateBindGroupLayout({0,
                                                            {{0, BindingType::UniformBuffer},
                                                             {1, BindingType::CombinedTextureSampler},
                                                             {2, BindingType::StorageBuffer}}});
            m_PatchPipeline = device.CreateComputePipeline({program, {m_PatchLayout}, 0, "Exposure frame constants"});
            m_PatchSampler  = device.CreateSampler({});
        }
        const glm::uvec4 address(offset / 4, stride / 4, count, 0);
        auto             uniform = device.CreateBuffer({sizeof(address), BufferUsage::Uniform, MemoryUsage::Upload});
        uniform->Write(0, &address, sizeof(address));
        BindGroupDesc group;
        group.Layout               = m_PatchLayout;
        group.Entries              = {{0, uniform}, {1, {}, 0, 0, exposure, m_PatchSampler}, {2, destination}};
        group.Entries.back().Bytes = uint64_t(offset) + uint64_t(stride) * (count - 1) + 4;
        commands.BindComputePipeline(m_PatchPipeline);
        commands.BindGroup(device.CreateBindGroup(group));
        commands.Dispatch((count + 63) / 64);
    }
    void AutoExposurePass::Record(RenderHardware::Device&                                device,
                                  RenderHardware::CommandList&                           commands,
                                  const RenderHardware::Handle<RenderHardware::Texture>& hdr,
                                  const AutoExposureSettings&                            input,
                                  float                                                  compensation,
                                  float                                                  delta)
    {
        using namespace RenderHardware;
        const auto settings = input.Validated();
        if (!m_Layout)
        {
            m_Layout  = device.CreateBindGroupLayout({0,
                                                      {{0, BindingType::UniformBuffer},
                                                       {1, BindingType::StorageBuffer},
                                                       {2, BindingType::CombinedTextureSampler},
                                                       {3, BindingType::CombinedTextureSampler},
                                                       {4, BindingType::StorageTexture}}});
            m_Sampler = device.CreateSampler({});
            const std::string                common = R"(#version 450
layout(set=0,binding=0,std140) uniform Parameters { vec4 Range; vec4 Limits; vec4 Adaptation; } p;
layout(set=0,binding=1,std430) buffer Histogram { uint bins[256]; } h;
layout(set=0,binding=2) uniform sampler2D SceneHDR;
layout(set=0,binding=3) uniform sampler2D Previous;
layout(set=0,binding=4,rgba32f) uniform writeonly image2D Next;
)";
            const std::array<std::string, 2> bodies {R"(
layout(local_size_x=8,local_size_y=8) in;
void main() {
    ivec2 id=ivec2(gl_GlobalInvocationID.xy),size=textureSize(SceneHDR,0);
    if(any(greaterThanEqual(id,size))) return;
    float luminance=max(dot(max(texelFetch(SceneHDR,id,0).rgb,vec3(0)),vec3(.2126,.7152,.0722)),1e-10);
    // Reflected-light calibration K=12.5: EV100=log2(100*L/K).
    // https://google.github.io/filament/main/filament.html (Exposure value and luminance).
    float ev=log2(8.*luminance);
    uint bin=uint(clamp((ev-p.Range.x)/(p.Range.y-p.Range.x),0,.999999)*256.);
    vec2 xy=(vec2(id)+.5)/vec2(size)*2.-1.;
    uint weight=1u+uint(round(3.*p.Limits.z*exp(-dot(xy,xy)*4.)));
    atomicAdd(h.bins[bin],weight);
})",
                                                     R"(
layout(local_size_x=1) in;
void main() {
    float total=0.;for(int i=0;i<256;++i) total+=float(h.bins[i]);
    float low=total*p.Range.z,high=total*p.Range.w,position=0.,sum=0.,weight=0.;
    for(int i=0;i<256;++i) {
        float count=float(h.bins[i]);
        float selected=max(0.,min(position+count,high)-max(position,low));
        sum+=selected*mix(p.Range.x,p.Range.y,(float(i)+.5)/256.);weight+=selected;position+=count;
    }
    float target=clamp(sum/max(weight,1e-6),p.Limits.x,p.Limits.y);
    float previous=clamp(texelFetch(Previous,ivec2(0),0).g,p.Limits.x,p.Limits.y);
    float speed=target>previous?p.Adaptation.x:p.Adaptation.y;
    float ev=p.Limits.w!=0.?target:mix(previous,target,1.-exp(-speed*p.Adaptation.z));
    imageStore(Next,ivec2(0),vec4(clamp(exp2(p.Adaptation.w-ev)/1.2,1e-10,1e8),ev,0,0));
})"};
            for (int i = 0; i < 2; ++i)
            {
                const auto& built = GetInternalComputeShaderData(i ? "Histogram exposure reduce" : "Exposure histogram",
                                                                 common + bodies[i]);
                if (auto* e = std::get_if<AssetError>(&built))
                    throw std::runtime_error(e->Message);
                auto shader    = device.CreateShader(std::get<ShaderData>(built));
                m_Pipelines[i] = device.CreateComputePipeline(
                    {shader, {m_Layout}, 0, i ? "Histogram exposure reduce" : "Exposure histogram"});
            }
        }
        struct Parameters
        {
            glm::vec4 Range, Limits, Adaptation;
        } p {{settings.HistogramMinEV, settings.HistogramMaxEV, settings.LowPercentile, settings.HighPercentile},
             {settings.MinEV, settings.MaxEV, settings.CentreWeight, m_Reset ? 1.f : 0.f},
             {settings.SpeedUp, settings.SpeedDown, std::isfinite(delta) ? std::max(0.f, delta) : 0, compensation}};
        auto uniform = device.CreateBuffer({sizeof(p), BufferUsage::Uniform, MemoryUsage::Upload});
        uniform->Write(0, &p, sizeof(p));
        std::array<uint32_t, 256> zero {};
        commands.UpdateBuffer(m_Histogram, 0, zero.data(), sizeof(zero));
        const unsigned next = 1 - m_Current;
        BindGroupDesc  group;
        group.Layout                     = m_Layout;
        group.Entries                    = {{0, uniform},
                                            {1, m_Histogram},
                                            {2, {}, 0, 0, hdr, m_Sampler},
                                            {3, {}, 0, 0, m_State[m_Current], m_Sampler},
                                            {4, {}, 0, 0, m_State[next]}};
        group.Entries.back().Subresource = TextureSubresource {};
        auto bindings                    = device.CreateBindGroup(group);
        commands.BindComputePipeline(m_Pipelines[0]);
        commands.BindGroup(bindings);
        commands.Dispatch((hdr->GetDesc().Width + 7) / 8, (hdr->GetDesc().Height + 7) / 8);
        commands.BindComputePipeline(m_Pipelines[1]);
        commands.BindGroup(bindings);
        commands.Dispatch(1);
        for (auto& slot : m_Readbacks)
            if (!slot.Pending)
            {
                commands.CopyTextureToBuffer(m_State[next], {}, slot.Buffer);
                slot.Pending  = true;
                slot.Sequence = ++m_Sequence;
                break;
            }
        m_Current = next;
        m_Reset   = false;
    }
} // namespace Kans
