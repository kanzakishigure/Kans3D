#include "kspch.h"
#include "ColorGradingPass.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    RenderHardware::Handle<RenderHardware::Texture>
    ColorGradingPass::Record(RenderHardware::Device&                                device,
                             RenderHardware::CommandList&                           commands,
                             const RenderHardware::Handle<RenderHardware::Texture>& view,
                             const ColorGradingSettings&                            input,
                             const RenderHardware::Handle<RenderHardware::Texture>& user)
    {
        using namespace RenderHardware;
        const auto settings = input.Validated();
        if (!settings.Active())
            return {};
        const auto lut  = user && user->GetDesc().Dimension == TextureDimension::Texture3D ? user : Handle<Texture> {};
        const auto hash = settings.SettingsHash();
        m_Cache.erase(std::remove_if(m_Cache.begin(), m_Cache.end(), [](auto& c) { return c.View.expired(); }),
                      m_Cache.end());
        auto found = std::find_if(m_Cache.begin(), m_Cache.end(), [&](auto& c) { return c.View.lock() == view; });
        if (found != m_Cache.end() && found->Hash == hash && found->User.lock() == lut)
            return found->Lut;
        // A view's display/HDR compatibility boundaries have distinct attachments.
        // Share their immutable bake (also safe for views with identical settings).
        auto shared = std::find_if(
            m_Cache.begin(), m_Cache.end(), [&](auto& c) { return c.Hash == hash && c.User.lock() == lut; });
        if (shared != m_Cache.end())
        {
            auto   result = shared->Lut;
            Cached entry {view, hash, lut, result};
            if (found == m_Cache.end())
                m_Cache.push_back(entry);
            else
                *found = entry;
            return result;
        }
        if (!m_Shader)
        {
            const auto& built = GetInternalComputeShaderData("Color grading LUT bake", R"(#version 450
layout(local_size_x=4,local_size_y=4,local_size_z=4) in;
layout(set=0,binding=0,std140) uniform Parameters { vec4 Balance; vec4 Lift; vec4 Gamma; vec4 Gain; vec4 Options; } p;
layout(set=0,binding=1,rgba32f) uniform writeonly image3D OutputLut;
layout(set=0,binding=2) uniform sampler3D UserLut;
// Log2 exposure-scaled linear Rec.709, -12..+12 stops. Both user LUT input and output
// use this normalized log encoding, independently of the final view transform.
vec3 encode(vec3 c) { return clamp((log2(max(c,vec3(exp2(-12.))))+12.)/24.,0,1); }
void main() {
    ivec3 id=ivec3(gl_GlobalInvocationID);
    vec3 c=exp2(vec3(id)/31.*24.-12.);
    // Diagonal white-balance model, neutral at 6500 K; positive tint removes green.
    float warm=log2(p.Balance.x/6500.);
    c*=exp2(vec3(warm*.5,-p.Balance.y*.5,-warm*.5));
    c=max(c+p.Lift.rgb,vec3(0));
    c=pow(c,1./p.Gamma.rgb)*p.Gain.rgb;
    c=.18*pow(max(c/.18,vec3(1e-12)),vec3(p.Balance.z));
    c=mix(vec3(dot(c,vec3(.2126,.7152,.0722))),c,p.Balance.w);
    vec3 encoded=encode(c);
    if(p.Options.y!=0.) {
        vec3 size=vec3(textureSize(UserLut,0));
        vec3 graded=texture(UserLut,encoded*(size-1.)/size+.5/size).rgb;
        encoded=mix(encoded,graded,p.Options.x);
    }
    imageStore(OutputLut,id,vec4(clamp(encoded,0,1),1));
})");
            if (auto* error = std::get_if<AssetError>(&built))
                throw std::runtime_error(error->Message);
            m_Shader   = device.CreateShader(std::get<ShaderData>(built));
            m_Layout   = device.CreateBindGroupLayout({0,
                                                       {{0, BindingType::UniformBuffer},
                                                        {1, BindingType::StorageTexture},
                                                        {2, BindingType::CombinedTextureSampler}}});
            m_Pipeline = device.CreateComputePipeline({m_Shader, {m_Layout}, 0, "Color grading LUT bake"});
            SamplerDesc sampler;
            sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
            m_Sampler                         = device.CreateSampler(sampler);
        }
        TextureDesc desc;
        desc.Dimension = TextureDimension::Texture3D;
        desc.Width = desc.Height = desc.Depth = 32;
        desc.PixelFormat                      = Format::RGBA32Float;
        desc.Usage  = TextureUsage::Storage | TextureUsage::Sampled | TextureUsage::CopySource;
        auto output = device.CreateTexture(desc);
        struct Parameters
        {
            glm::vec4 Balance, Lift, Gamma, Gain, Options;
        } p {{settings.Temperature, settings.Tint, settings.Contrast, settings.Saturation},
             glm::vec4(settings.Lift, 0),
             glm::vec4(settings.Gamma, 0),
             glm::vec4(settings.Gain, 0),
             {settings.Contribution, lut ? 1.f : 0.f, 0, 0}};
        auto buffer = device.CreateBuffer({sizeof(p), BufferUsage::Uniform, MemoryUsage::Upload});
        buffer->Write(0, &p, sizeof(p));
        // A distinct unused 3D texture keeps descriptor dimensions valid without
        // sampling the storage output; the shader skips it when no user LUT exists.
        Handle<Texture> fallback;
        if (!lut)
        {
            auto d  = desc;
            d.Width = d.Height = d.Depth = 1;
            d.Usage                      = TextureUsage::Sampled;
            fallback                     = device.CreateTexture(d);
        }
        BindGroupDesc group;
        group.Layout  = m_Layout;
        group.Entries = {{0, buffer}, {1, {}, 0, 0, output}, {2, {}, 0, 0, lut ? lut : fallback, m_Sampler}};
        commands.BindComputePipeline(m_Pipeline);
        commands.BindGroup(device.CreateBindGroup(group));
        commands.Dispatch(8, 8, 8);
        Cached entry {view, hash, lut, output};
        if (found == m_Cache.end())
            m_Cache.push_back(entry);
        else
            *found = entry;
        return output;
    }
} // namespace Kans
