#include "kspch.h"
#include "GraphCapture.h"
#include <cstring>
#include <glm/gtc/packing.hpp>
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    namespace
    {
        using namespace RenderHardware;
        bool Same(CaptureSelection a, CaptureSelection b)
        {
            return a.Pass == b.Pass && a.Resource == b.Resource && a.Subresource.Mip == b.Subresource.Mip &&
                   a.Subresource.Face == b.Subresource.Face;
        }
        bool   Depth(Format format) { return format == Format::Depth32Float || format == Format::Depth24Stencil8; }
        size_t PixelBytes(Format format)
        {
            switch (format)
            {
                case Format::RGBA32Float:
                    return 16;
                case Format::RGBA16Float:
                    return 8;
                case Format::RG8Unorm:
                case Format::R16Float:
                    return 2;
                case Format::R8Unorm:
                    return 1;
                default:
                    return 4;
            }
        }
        uint32_t Layers(const TextureDesc& desc)
        {
            return desc.Dimension == TextureDimension::Cube ? 6 : desc.ArrayLayers;
        }
        bool Compatible(const TextureDesc& a, const TextureDesc& b)
        {
            return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels &&
                   a.Dimension == b.Dimension && a.ArrayLayers == b.ArrayLayers && a.PixelFormat == b.PixelFormat &&
                   a.Usage == b.Usage && a.Samples == b.Samples && a.Depth == b.Depth;
        }
    } // namespace
    const CapturedImage* CapturedGraphFrame::Find(CaptureSelection selection, bool fullResolution) const
    {
        for (auto& image : Images)
            if (Same(selection, image.Selection) && image.FullResolution == fullResolution)
                return &image;
        return nullptr;
    }
    GraphCapture::GraphCapture(std::shared_ptr<RenderHardware::Device> device) : m_Device(std::move(device))
    {
        if (!m_Device)
            throw std::invalid_argument("GraphCapture needs a device");
    }
    void GraphCapture::BeginFrame(const GraphCaptureSettings& settings, uint64_t frame, const glm::mat4& projection)
    {
        if (!settings.ThumbnailMaxEdge)
            throw std::invalid_argument("Capture thumbnail edge must be positive");
        m_Settings          = settings;
        m_InverseProjection = glm::inverse(projection);
        m_Current           = std::make_shared<CapturedGraphFrame>();
        m_Current->Frame    = frame;
        // Reclaim unused sizes/selections, retaining a small reuse window for viewport changes.
        m_Images.erase(std::remove_if(m_Images.begin(),
                                      m_Images.end(),
                                      [&](const auto& slot) {
                                          return slot.Image.use_count() == 1 &&
                                                 slot.LastFrame + m_Device->GetDesc().FramesInFlight + 2 < frame;
                                      }),
                       m_Images.end());
    }
    RenderHardware::Handle<RenderHardware::Texture> GraphCapture::Acquire(RenderHardware::TextureDesc desc)
    {
        for (auto& slot : m_Images)
            if (slot.Image.use_count() == 1 && Compatible(slot.Image->GetDesc(), desc))
            {
                slot.LastFrame = m_Current ? m_Current->Frame : 0;
                return slot.Image;
            }
        auto image = m_Device->CreateTexture(desc);
        m_Images.push_back({image, m_Current ? m_Current->Frame : 0});
        return image;
    }
    void GraphCapture::AfterPass(const RenderGraph& graph, GraphPass pass)
    {
        if (!m_Current || !m_Settings.CaptureGraph)
            return;
        auto       commands = m_Device->CreateCommandList();
        bool       recorded = false;
        m_Current->Resources = graph.Resources();
        m_Current->Passes    = graph.Passes();
        const auto passID   = graph.Passes().at(pass).ID;
        for (auto use : graph.Passes()[pass].Uses)
        {
            if (use.IsBuffer || use.Access == GraphAccess::SampledRead || use.Access == GraphAccess::TransferRead ||
                use.Access == GraphAccess::StorageRead)
                continue;
            const auto&                   resource = graph.Resources().at(use.Texture);
            const auto&                   desc     = resource.Description;
            auto                          source   = graph.Texture(use.Texture);
            if (desc.Samples > 1)
            {
                auto resolved    = desc;
                resolved.Samples = 1;
                resolved.Usage   = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopySource |
                                   RenderHardware::TextureUsage::CopyDestination |
                                   (Depth(desc.PixelFormat) ? RenderHardware::TextureUsage::DepthStencil :
                                                              RenderHardware::TextureUsage::RenderTarget);
                auto image       = Acquire(resolved);
                if (!Depth(desc.PixelFormat) && desc.PixelFormat != Format::R32Uint)
                    m_AntiAliasing.Resolve(
                        *m_Device, *commands, source, image, desc.PixelFormat == Format::RGBA16Float);
                else
                {
                    RenderingDesc render;
                    if (Depth(desc.PixelFormat))
                    {
                        render.Depth.Target        = source;
                        render.Depth.Load          = LoadOp::Load;
                        render.Depth.ResolveTarget = image;
                    }
                    else
                    {
                        render.Colors.push_back({source, {}, LoadOp::Load});
                        render.Colors[0].ResolveTarget = image;
                    }
                    commands->BeginRendering(render);
                    commands->EndRendering();
                }
                source   = image;
                recorded = true;
            }
            std::vector<CaptureSelection> selections {{passID, resource.ID, {}}};
            for (auto& request : m_Settings.Requests)
                if (request.Pass == passID && request.Resource == resource.ID)
                {
                    if (request.AllLayers)
                        for (uint32_t layer = 0; layer < Layers(desc); ++layer)
                            selections.push_back({passID, resource.ID, {request.Subresource.Mip, layer}});
                    else
                        selections.push_back(request);
                }
            if (m_Settings.Inspector && m_Settings.Inspector->Pass == passID &&
                m_Settings.Inspector->Resource == resource.ID)
                selections.push_back(*m_Settings.Inspector);
            std::vector<CaptureSelection> seen;
            for (auto selection : selections)
            {
                if (std::any_of(seen.begin(), seen.end(), [&](auto previous) { return Same(previous, selection); }))
                    continue;
                seen.push_back(selection);
                const bool full = m_Settings.Inspector && Same(*m_Settings.Inspector, selection);
                for (unsigned kind = 0; kind < (full ? 2u : 1u); ++kind)
                {
                    CapturedImage image;
                    image.Selection         = selection;
                    image.Frame             = m_Current->Frame;
                    image.SourceDescription = desc;
                    image.Label             = resource.Name;
                    if (desc.Samples > 1)
                        image.Label += " (" + std::to_string(desc.Samples) + "x resolved: " +
                                       (Depth(desc.PixelFormat) || desc.PixelFormat == Format::R32Uint ? "sample 0" :
                                        desc.PixelFormat == Format::RGBA16Float                        ? "Karis HDR" :
                                                                                                         "average") +
                                       ")";
                    image.FullResolution    = kind == 1;
                    image.InverseProjection = m_InverseProjection;
                    const auto sub          = selection.Subresource;
                    if (desc.Dimension == TextureDimension::Texture3D)
                        image.Unavailable = "Volume preview requires a slice selection";
                    else if (sub.Mip >= desc.MipLevels || sub.Face >= Layers(desc))
                        image.Unavailable = "Invalid mip/layer";
                    else
                    {
                        const bool canCopy   = image.FullResolution &&
                                               (uint32_t(source->GetDesc().Usage) & uint32_t(TextureUsage::CopySource));
                        const bool canSample = uint32_t(source->GetDesc().Usage) & uint32_t(TextureUsage::Sampled);
                        if (!canCopy && !canSample)
                            image.Unavailable = "Source requires sampled or copy-source usage";
                        else
                        {
                            TextureDesc output;
                            output.Width  = std::max(1u, desc.Width >> sub.Mip);
                            output.Height = std::max(1u, desc.Height >> sub.Mip);
                            if (!image.FullResolution)
                            {
                                const float scale = std::min(
                                    1.0f, float(m_Settings.ThumbnailMaxEdge) / std::max(output.Width, output.Height));
                                output.Width  = std::max(1u, uint32_t(output.Width * scale));
                                output.Height = std::max(1u, uint32_t(output.Height * scale));
                            }
                            image.DepthInColor = Depth(desc.PixelFormat) && !canCopy;
                            output.PixelFormat = image.DepthInColor ? Format::RGBA32Float : desc.PixelFormat;
                            output.Usage  = TextureUsage::Sampled | RenderHardware::TextureUsage::CopySource |
                                            RenderHardware::TextureUsage::CopyDestination |
                                            (Depth(output.PixelFormat) ? RenderHardware::TextureUsage::DepthStencil :
                                                                         RenderHardware::TextureUsage::RenderTarget);
                            image.Texture = Acquire(output);
                            if (canCopy)
                                commands->CopyTexture(source, sub, image.Texture);
                            else
                                Convert(*commands, source, sub, image.Texture, false, true, m_InverseProjection);
                            recorded = true;
                        }
                    }
                    m_Current->Images.push_back(std::move(image));
                }
            }
        }
        if (recorded)
            m_Device->Submit(commands);
    }
    void GraphCapture::Convert(RenderHardware::CommandList&                           commands,
                               const RenderHardware::Handle<RenderHardware::Texture>& source,
                               RenderHardware::TextureSubresource                     sub,
                               const RenderHardware::Handle<RenderHardware::Texture>& target,
                               bool                                                   depthInColor,
                               bool                                                   snapshot,
                               const glm::mat4&                                       inverseProjection,
                               const CaptureDisplaySettings&                          settings)
    {
        using namespace RenderHardware;
        const auto format = source->GetDesc().PixelFormat;
        // 0=color float/unorm, 1=uint, 2=depth, 3=depth+stencil, 4=depth stored in color.
        const unsigned kind       = depthInColor                      ? 4 :
                                    format == Format::R32Uint         ? 1 :
                                    format == Format::Depth24Stencil8 ? 3 :
                                    Depth(format)                     ? 2 :
                                                                        0;
        const bool     outputUint = target->GetDesc().PixelFormat == Format::R32Uint;
        if (!m_Sampler)
        {
            SamplerDesc sampler;
            sampler.Min = sampler.Mag = sampler.Mip = Filter::Nearest;
            sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
            m_Sampler                         = m_Device->CreateSampler(sampler);
            m_Layout                          = m_Device->CreateBindGroupLayout(
                {0, {{0, BindingType::UniformBuffer}, {1, BindingType::CombinedTextureSampler}}});
            m_StencilLayout = m_Device->CreateBindGroupLayout({0,
                                                               {{0, BindingType::UniformBuffer},
                                                                {1, BindingType::CombinedTextureSampler},
                                                                {2, BindingType::CombinedTextureSampler}}});
        }
        auto& pipeline = m_Pipelines[{kind, target->GetDesc().PixelFormat}];
        if (!pipeline)
        {
            std::string fragment = R"(#version 450
layout(set=0,binding=0,std140) uniform Parameters { mat4 InverseProjection; vec4 Size; vec4 Adjust; ivec4 Options; } p;
)";
            fragment += kind == 1 ? "layout(set=0,binding=1) uniform usampler2D InputColor;\n" :
                                    "layout(set=0,binding=1) uniform sampler2D InputColor;\n";
            if (kind == 3)
                fragment += "layout(set=0,binding=2) uniform usampler2D Stencil;\n";
            fragment += outputUint ? "layout(location=0) out uint Color;\n" : "layout(location=0) out vec4 Color;\n";
            fragment += R"(
vec3 encodeSRGB(vec3 c) {
    return mix(12.92*c,1.055*pow(max(c,vec3(0)),vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308)));
}
void main() {
    ivec2 extent=textureSize(InputColor,0);
    ivec2 xy=clamp(ivec2(gl_FragCoord.xy/p.Size.xy*vec2(extent)),ivec2(0),extent-1);
    vec4 raw=vec4(texelFetch(InputColor,xy,0));
)";
            if (kind == 2 || kind == 3)
                fragment += "raw=vec4(raw.r,0,0,1);\n";
            if (kind == 3)
                fragment += "raw.g=float(texelFetch(Stencil,xy,0).r);\n";
            if (outputUint)
                fragment += "Color=texelFetch(InputColor,xy,0).r;\n}";
            else
            {
                fragment += "if (p.Options.x!=0) { Color=raw; return; }\n";
                if (kind >= 2)
                    fragment += R"(
    float depth=raw.r;
    if (p.Options.w!=0) {
        vec4 eye=p.InverseProjection*vec4((vec2(xy)+0.5)/vec2(extent)*2-1,depth,1);
        depth=abs(eye.z/eye.w);
    }
    raw=p.Options.y<0 ? vec4(depth,depth,depth,1) : vec4(depth,raw.g,0,1);
)";
                fragment += R"(
    vec4 adjusted=vec4(raw.rgb*exp2(p.Adjust.x),raw.a);
    vec4 mapped=(adjusted-p.Adjust.y)/(p.Adjust.z-p.Adjust.y);
    if (p.Options.y>=0) mapped=vec4(vec3(mapped[p.Options.y]),1);
    else {
        mapped.rgb*=vec3((p.Options.z&1)!=0,(p.Options.z&2)!=0,(p.Options.z&4)!=0);
        mapped.a=(p.Options.z&8)!=0 ? mapped.a : 1;
    }
    if (p.Adjust.w!=0) mapped.rgb=encodeSRGB(mapped.rgb);
    Color=clamp(mapped,0,1);
})";
            }
            auto shader =
                CreateInternalShader(*m_Device,
                                     "Graph capture " + std::to_string(kind) + (outputUint ? " uint" : " float"),
                                     R"(#version 450
void main() { vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(p*2-1,0,1); }
)",
                                     fragment);
            if (auto* error = std::get_if<AssetError>(&shader))
                throw std::runtime_error(error->Message);
            PipelineDesc desc;
            desc.Program      = std::get<Handle<Shader>>(shader);
            desc.GroupLayouts = {kind == 3 ? m_StencilLayout : m_Layout};
            desc.ColorFormats = {target->GetDesc().PixelFormat};
            desc.Blends.resize(1);
            desc.Rasterizer.Cull = CullMode::None;
            pipeline             = m_Device->CreatePipeline(desc);
        }
        struct Parameters
        {
            glm::mat4  Inverse;
            glm::vec4  Size, Adjust;
            glm::ivec4 Options;
        };
        Parameters parameters {inverseProjection,
                               {target->GetDesc().Width, target->GetDesc().Height, 0, 0},
                               {settings.Exposure, settings.RangeMin, settings.RangeMax, settings.SRGB ? 1 : 0},
                               {snapshot ? 1 : 0, settings.Solo, settings.ChannelMask, settings.LinearDepth ? 1 : 0}};
        auto       buffer = m_Device->CreateBuffer({sizeof(parameters), BufferUsage::Uniform, MemoryUsage::Upload});
        buffer->Write(0, &parameters, sizeof(parameters));
        BindGroupDesc group;
        group.Layout  = kind == 3 ? m_StencilLayout : m_Layout;
        group.Entries = {{0, buffer}, {1, {}, 0, 0, source, m_Sampler, sub}};
        if (kind == 3)
            group.Entries.push_back({2, {}, 0, 0, source, m_Sampler, sub, TextureAspect::Stencil});
        RenderingDesc render;
        render.Colors.push_back({target, {}, LoadOp::Discard});
        commands.BeginRendering(render);
        commands.SetViewport({0, 0, float(target->GetDesc().Width), float(target->GetDesc().Height)});
        commands.SetScissor({0, 0, target->GetDesc().Width, target->GetDesc().Height});
        commands.BindPipeline(pipeline);
        commands.BindGroup(m_Device->CreateBindGroup(group));
        commands.Draw(3);
        commands.EndRendering();
    }
    RenderHardware::Handle<RenderHardware::Texture> GraphCapture::Display(const CapturedImage&          image,
                                                                          const CaptureDisplaySettings& settings)
    {
        if (!image.Texture)
            return {};
        if (settings.Solo < -1 || settings.Solo > 3 || settings.ChannelMask > 15 || !std::isfinite(settings.Exposure) ||
            !std::isfinite(settings.RangeMin) || !std::isfinite(settings.RangeMax) ||
            settings.RangeMax <= settings.RangeMin)
            throw std::invalid_argument("Invalid capture display settings");
        auto desc        = image.Texture->GetDesc();
        desc.PixelFormat = RenderHardware::Format::RGBA8Unorm;
        desc.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::RenderTarget |
                           RenderHardware::TextureUsage::CopySource;
        auto display     = Acquire(desc);
        auto commands    = m_Device->CreateCommandList();
        Convert(*commands, image.Texture, {}, display, image.DepthInColor, false, image.InverseProjection, settings);
        m_Device->Submit(commands);
        return display;
    }
    std::optional<uint64_t> GraphCapture::RequestPixel(const CapturedImage& image, glm::uvec2 pixel)
    {
        using namespace RenderHardware;
        if (!image.Texture || m_Probes.size() >= 64)
            return {};
        const auto& desc = image.Texture->GetDesc();
        if (pixel.x >= desc.Width || pixel.y >= desc.Height)
            throw std::invalid_argument("Probe pixel outside capture");
        const size_t bytes    = desc.PixelFormat == Format::Depth24Stencil8 ? 8 : PixelBytes(desc.PixelFormat);
        auto         buffer   = m_Device->CreateBuffer({bytes, BufferUsage::CopyDestination, MemoryUsage::Readback});
        auto         commands = m_Device->CreateCommandList();
        commands->CopyTextureTexelToBuffer(image.Texture, {}, pixel.x, pixel.y, buffer);
        m_Device->Submit(commands);
        PixelProbeResult result;
        result.Request      = m_NextProbe++;
        result.Frame        = image.Frame;
        result.Selection    = image.Selection;
        result.Pixel        = pixel;
        result.StoredFormat = desc.PixelFormat;
        m_Probes.push_back({result, buffer});
        return result.Request;
    }
    std::optional<PixelProbeResult> GraphCapture::PollPixel(uint64_t request)
    {
        using namespace RenderHardware;
        for (auto it = m_Probes.begin(); it != m_Probes.end(); ++it)
            if (it->Result.Request == request)
            {
                if (!it->Buffer->IsReadbackReady())
                    return {};
                auto result  = std::move(it->Result);
                result.Bytes = it->Buffer->Read(0, it->Buffer->GetDesc().Bytes);
                auto read    = [&](size_t offset, auto& value) {
                    std::memcpy(&value, result.Bytes.data() + offset, sizeof(value));
                };
                if (result.StoredFormat == Format::RGBA16Float || result.StoredFormat == Format::RG16Float ||
                    result.StoredFormat == Format::R16Float)
                    for (size_t i = 0; i < (result.StoredFormat == Format::R16Float  ? 1u :
                                            result.StoredFormat == Format::RG16Float ? 2u :
                                                                                       4u);
                         ++i)
                    {
                        uint16_t value;
                        read(i * 2, value);
                        result.Value[i] = glm::unpackHalf1x16(value);
                    }
                else if (result.StoredFormat == Format::RGBA32Float || result.StoredFormat == Format::Depth32Float ||
                         result.StoredFormat == Format::R32Float)
                    for (size_t i = 0; i < (result.StoredFormat == Format::RGBA32Float ? 4u : 1u); ++i)
                    {
                        float value;
                        read(i * 4, value);
                        result.Value[i] = value;
                    }
                else if (result.StoredFormat == Format::R32Uint)
                {
                    uint32_t value;
                    read(0, value);
                    result.Value[0] = value;
                }
                else if (result.StoredFormat == Format::Depth24Stencil8)
                {
                    uint32_t value;
                    read(0, value);
                    value = ((value & 0xffffff) << 8) | result.Bytes[4];
                    result.Bytes.resize(4);
                    std::memcpy(result.Bytes.data(), &value, 4);
                    result.Value[0] = double(value >> 8) / 16777215.0;
                    result.Value[1] = value & 255;
                }
                else
                    for (size_t i = 0; i < result.Bytes.size(); ++i)
                        result.Value[i] = result.Bytes[i] / 255.0;
                m_Probes.erase(it);
                return result;
            }
        return {};
    }
} // namespace Kans
