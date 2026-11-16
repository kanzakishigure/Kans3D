#include "kspch.h"
#include "HiZ.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    std::vector<glm::uvec2> HiZMipSizes(glm::uvec2 size)
    {
        if (!size.x || !size.y)
            throw std::invalid_argument("Hi-Z dimensions must be positive");
        std::vector<glm::uvec2> sizes {size};
        while (size.x > 1 || size.y > 1)
        {
            size = glm::max(size / 2u, glm::uvec2(1));
            sizes.push_back(size);
        }
        return sizes;
    }
    GraphTexture BuildHiZ(RenderGraph&                                           graph,
                          RenderHardware::Device&                                device,
                          const RenderHardware::Handle<RenderHardware::Texture>& depth,
                          HiZState&                                              s)
    {
        using namespace RenderHardware;
        const auto sizes = HiZMipSizes({depth->GetDesc().Width, depth->GetDesc().Height});
        if (!s.Pyramid || s.Pyramid->GetDesc().Width != sizes[0].x || s.Pyramid->GetDesc().Height != sizes[0].y)
        {
            TextureDesc d;
            d.Width       = sizes[0].x;
            d.Height      = sizes[0].y;
            d.MipLevels   = uint32_t(sizes.size());
            d.PixelFormat = Format::R32Float;
            d.Usage       = RenderHardware::TextureUsage::Storage | RenderHardware::TextureUsage::Sampled |
                            RenderHardware::TextureUsage::CopySource;
            s.Pyramid     = device.CreateTexture(d);
        }
        if (!s.Copy)
        {
            s.CopyLayout = device.CreateBindGroupLayout(
                {0, {{0, BindingType::CombinedTextureSampler}, {1, BindingType::StorageTexture}}});
            s.ReduceLayout =
                device.CreateBindGroupLayout({0, {{0, BindingType::StorageTexture}, {1, BindingType::StorageTexture}}});
            SamplerDesc sampler;
            sampler.U = sampler.V = AddressMode::ClampToEdge;
            sampler.Min = sampler.Mag = sampler.Mip = Filter::Nearest;
            s.Point                                 = device.CreateSampler(sampler);
            const std::string common                = R"(#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=1,r32f) uniform writeonly image2D Result;
)";
            const std::string copy                  = common + R"(
layout(set=0,binding=0) uniform sampler2D Source;
void main() {
    ivec2 id=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(id,imageSize(Result)))) return;
    imageStore(Result,id,vec4(texelFetch(Source,id,0).r));
})";
            const std::string reduce                = common + R"(
layout(set=0,binding=0,r32f) uniform readonly image2D Source;
void main() {
    ivec2 id=ivec2(gl_GlobalInvocationID.xy), size=imageSize(Result), srcSize=imageSize(Source);
    if(any(greaterThanEqual(id,size))) return;
    ivec2 begin=id*2, end=min(begin+2,srcSize);
    if(id.x==size.x-1) end.x=srcSize.x;
    if(id.y==size.y-1) end.y=srcSize.y;
    float z=1;
    for(int y=begin.y;y<end.y;++y) for(int x=begin.x;x<end.x;++x) z=min(z,imageLoad(Source,ivec2(x,y)).r);
    imageStore(Result,id,vec4(z));
})";
            auto              pipeline              = [&](const char* name, const std::string& source, auto layout) {
                const auto& built = GetInternalComputeShaderData(name, source);
                if (auto* error = std::get_if<AssetError>(&built))
                    throw std::runtime_error(error->Message);
                return device.CreateComputePipeline(
                    {device.CreateShader(std::get<ShaderData>(built)), {layout}, 0, name});
            };
            s.Copy   = pipeline("Hi-Z copy", copy, s.CopyLayout);
            s.Reduce = pipeline("Hi-Z reduce", reduce, s.ReduceLayout);
        }
        const auto input   = graph.ImportTexture("SceneDepthStencil", depth);
        const auto pyramid = graph.ImportTexture("Hi-Z min depth", s.Pyramid);
        for (uint32_t mip = 0; mip < sizes.size(); ++mip)
        {
            std::vector<GraphUse> uses {{pyramid, mip ? GraphAccess::StorageReadWrite : GraphAccess::StorageWrite}};
            if (!mip)
                uses.push_back({input, GraphAccess::SampledRead});
            const auto pass = graph.AddPass(
                "Hi-Z mip " + std::to_string(mip),
                GraphPassKind::Compute,
                uses,
                [depth,
                 image    = s.Pyramid,
                 layout   = mip ? s.ReduceLayout : s.CopyLayout,
                 pipeline = mip ? s.Reduce : s.Copy,
                 sampler  = s.Point,
                 mip,
                 size = sizes[mip]](auto& c) {
                    BindGroupDesc group;
                    group.Layout = layout;
                    group.Entries.push_back({0, {}, 0, 0, mip ? image : depth, mip ? Handle<Sampler> {} : sampler});
                    if (mip)
                        group.Entries.back().Subresource = TextureSubresource {mip - 1, 0};
                    group.Entries.push_back({1, {}, 0, 0, image});
                    group.Entries.back().Subresource = TextureSubresource {mip, 0};
                    c.Commands.BindComputePipeline(pipeline);
                    c.Commands.BindGroup(c.Device.CreateBindGroup(group));
                    c.Commands.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
                });
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        }
        graph.ExportTexture(pyramid);
        return pyramid;
    }
} // namespace Kans
