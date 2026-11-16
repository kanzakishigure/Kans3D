#include "kspch.h"
#include "DepthOfFieldFeature.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    void DepthOfFieldFeature::Prepare(RenderView& view, RenderFrame& frame)
    {
        if (!view.GetPostProcessSettings().DepthOfField.Active())
            return;
        frame.DeferToneMapping = true;
        frame.Output.EnsureSampledDepth();
    }
    void DepthOfFieldFeature::AddPasses(RenderExtensionPoint, const RenderFeatureContext& context)
    {
        auto*      frame    = context.Frame;
        const auto settings = context.View.GetPostProcessSettings();
        if (!frame || !settings.DepthOfField.Active())
            return;
        auto&      graph = context.Graph;
        const auto hdr   = graph.ImportTexture("SceneHDR", frame->Output.GetHDR());
        const auto depth = graph.ImportTexture("DoF scene depth", frame->Output.GetDepth());
        if (!frame->PostProcessHDR)
        {
            // Immediate compatibility draws may have replaced the display target since the last HDR draw.
            const auto display = graph.ImportTexture("DoF display input", frame->Output.GetColors()[0]);
            graph.AddPass("DoF HDR prefill",
                          {{display, GraphAccess::SampledRead}, {hdr, GraphAccess::ColorWrite}},
                          [frame, settings](auto& c) {
                              frame->Services.GetToneMapping().Record(
                                  c.Device,
                                  c.Commands,
                                  frame->Output.GetColors()[0],
                                  {frame->Output.GetHDR()},
                                  settings.Exposure,
                                  true,
                                  settings.ToneMapping,
                                  settings.ColorGrading,
                                  frame->View.Assets && settings.ColorGrading.LUT.IsValid() ?
                                      frame->View.Assets->GetTextureResource(settings.ColorGrading.LUT) :
                                      RenderHardware::Handle<RenderHardware::Texture> {},
                                  frame->View.GPUExposure);
                          });
        }

        AddDepthOfFieldPasses(graph,
                              hdr,
                              depth,
                              context.View.Viewport,
                              settings,
                              DeviceProjection(context.View.Scene.sceneCamera.camera.GetProjectionMatrix(),
                                               frame->GetDevice()->GetCoordinateConventions()),
                              frame->RenderSource ? glm::vec2(0) : frame->Output.GetJitter());
        frame->PostProcessHDR = true;
    }
    GraphTexture DepthOfFieldFeature::AddDepthOfFieldPasses(RenderGraph&               graph,
                                                            GraphTexture               hdr,
                                                            GraphTexture               depth,
                                                            glm::uvec2                 display,
                                                            const PostProcessSettings& input,
                                                            const glm::mat4&           projection,
                                                            glm::vec2                  jitter)
    {
        using namespace RenderHardware;
        const auto settings = input.Validated();
        const auto d        = settings.DepthOfField;
        if (!d.Active())
            return hdr;
        const auto  camera = settings.Exposure.Camera;
        const float f      = camera.FocalLengthMm * .001f;
        const float lens   = .5f * f * f / (camera.Aperture * std::max(d.FocusDistance - f, .001f)) /
                             (camera.SensorHeightMm * .001f) * display.y;
        const auto  inv    = glm::inverse(projection);
        Parameters  p {};
        p.Lens       = {lens, d.FocusDistance, display.y * d.MaxCoCPercent * .01f, float(d.Mode)};
        p.Ranges     = {d.NearFocus, d.FarFocus, d.NearBlend, d.FarBlend};
        p.Options    = {0, float(d.Samples), d.BokehStrength, 0};
        p.Projection = {inv[2][2], inv[3][2], inv[2][3], inv[3][3]};
        p.Jitter     = {jitter, display.x, display.y};
        auto create  = [&](const char* name, glm::uvec2 size) {
            TextureDesc desc;
            desc.Width       = size.x;
            desc.Height      = size.y;
            desc.PixelFormat = Format::RGBA16Float;
            desc.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::RenderTarget |
                               RenderHardware::TextureUsage::CopySource;
            return graph.CreateTexture(name, desc);
        };
        auto add = [&](const char* name, int stage, glm::uvec2 size, std::array<GraphTexture, 4> sources) {
            auto target      = create(name, size);
            auto params      = p;
            params.Size      = {size.x, size.y, 0, 0};
            params.Options.x = float(stage);
            std::vector<GraphUse> uses;
            for (auto source : sources)
                if (std::none_of(uses.begin(), uses.end(), [&](const auto& u) { return u.Texture == source; }))
                    uses.push_back({source, GraphAccess::SampledRead});
            uses.push_back({target, GraphAccess::ColorWrite});
            graph.AddPass(name, std::move(uses), [this, target, sources, params](auto& c) {
                Record(c, target, sources, params);
            });
            return target;
        };
        // Stable resolved display colour, followed later by motion blur, bloom and tone mapping.
        const auto half       = glm::max((display + 1u) / 2u, glm::uvec2(1));
        const auto tiles      = (display + 15u) / 16u;
        const auto coc        = add("DoF CoC", 0, display, {depth, depth, depth, depth});
        const auto tile       = add("DoF Tile max", 1, tiles, {coc, coc, coc, coc});
        const auto dilation   = add("DoF Dilate tiles", 2, tiles, {tile, tile, tile, tile});
        const auto farField   = add("DoF Prefilter far", 3, half, {hdr, coc, coc, coc});
        const auto nearField  = add("DoF Prefilter near", 4, half, {hdr, coc, coc, coc});
        const auto farGather  = add("DoF Gather far", 5, half, {farField, coc, dilation, farField});
        const auto nearGather = add("DoF Gather near", 6, half, {nearField, coc, dilation, nearField});
        const auto farFill    = add("DoF Fill far", 7, half, {farGather, coc, coc, coc});
        const auto nearFill   = add("DoF Fill near", 8, half, {nearGather, coc, coc, coc});
        const auto output     = add("DoF Bilateral composite", 9, display, {hdr, coc, farFill, nearFill});
        graph.AddPass("DoF Resolve",
                      GraphPassKind::Transfer,
                      {{output, GraphAccess::TransferRead}, {hdr, GraphAccess::TransferWrite}},
                      [output, hdr](auto& c) { c.Commands.CopyTexture(c.Texture(output), {}, c.Texture(hdr)); });
        return hdr;
    }
    void DepthOfFieldFeature::Record(RenderGraphContext&         c,
                                     GraphTexture                target,
                                     std::array<GraphTexture, 4> inputs,
                                     const Parameters&           p)
    {
        using namespace RenderHardware;
        auto& device = c.Device;
        if (!m_Shader)
        {
            auto built = CreateInternalShader(device, "Gather depth of field", R"(#version 450
void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,0,1);}
)",
#include "DepthOfFieldShader.h"
            );
            if (auto* error = std::get_if<AssetError>(&built))
                throw std::runtime_error(error->Message);
            m_Shader = std::get<Handle<Shader>>(std::move(built));
            m_Layout = device.CreateBindGroupLayout({0,
                                                     {{0, BindingType::UniformBuffer},
                                                      {1, BindingType::CombinedTextureSampler},
                                                      {2, BindingType::CombinedTextureSampler},
                                                      {3, BindingType::CombinedTextureSampler},
                                                      {4, BindingType::CombinedTextureSampler}}});
            SamplerDesc sampler;
            sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
            m_Sampler                         = device.CreateSampler(sampler);
            sampler.Min = sampler.Mag = sampler.Mip = Filter::Nearest;
            m_PointSampler                          = device.CreateSampler(sampler);
            PipelineDesc desc;
            desc.Program         = m_Shader;
            desc.DebugName       = "DoF";
            desc.GroupLayouts    = {m_Layout};
            desc.ColorFormats    = {Format::RGBA16Float};
            desc.Rasterizer.Cull = CullMode::None;
            desc.Blends.resize(1);
            m_Pipeline = device.CreatePipeline(desc);
        }
        auto buffer = device.CreateBuffer({sizeof(p), BufferUsage::Uniform, MemoryUsage::Upload});
        buffer->Write(0, &p, sizeof(p));
        BindGroupDesc group;
        group.Layout  = m_Layout;
        group.Entries = {{0, buffer}};
        for (unsigned i = 0; i < 4; ++i)
            group.Entries.push_back(
                {i + 1,
                 {},
                 0,
                 0,
                 c.Texture(inputs[i]),
                 p.Options.x == 0 || (i == 2 && (p.Options.x == 5 || p.Options.x == 6)) ? m_PointSampler : m_Sampler});
        RenderingDesc render;
        render.Colors.push_back({c.Texture(target), {}, LoadOp::Discard});
        c.Commands.BeginRendering(render);
        c.Commands.SetViewport({0, 0, p.Size.x, p.Size.y});
        c.Commands.SetScissor({0, 0, uint32_t(p.Size.x), uint32_t(p.Size.y)});
        c.Commands.BindPipeline(m_Pipeline);
        c.Commands.BindGroup(device.CreateBindGroup(group));
        c.Commands.Draw(3);
        c.Commands.EndRendering();
    }
} // namespace Kans
