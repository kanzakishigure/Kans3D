#include "kspch.h"
#include "Kans3D/Renderer/Passes/ToneMappingPass.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    float ToneMappingPass::EffectiveExposure(float exposure)
    {
        // The inverse must fit RGBA16F even for white display pixels.
        return std::isfinite(exposure) ? std::clamp(exposure, 0.001f, 128.0f) : 0.8f;
    }
    void ToneMappingPass::Record(RenderHardware::Device&                                             device,
                                 RenderHardware::CommandList&                                        commands,
                                 const RenderHardware::Handle<RenderHardware::Texture>&              source,
                                 const std::vector<RenderHardware::Handle<RenderHardware::Texture>>& targets,
                                 float                                                               exposure,
                                 bool                                                                inverse,
                                 ToneMappingMode                                                     mode)
    {
        using namespace RenderHardware;
        const auto samples = source->GetDesc().Samples;
        auto&      program = m_Programs[samples > 1];
        if (!program)
        {
            auto built = CreateInternalShader(
                device,
                "ACES fitted + sRGB",
                R"(#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2 - 1, 0, 1);
})",
                [&] {
                    std::string fragment = R"(#version 450
layout(set=0,binding=0,std140) uniform Parameters { vec4 Settings; } p;
layout(set=0,binding=1) uniform sampler2D InputColor;
layout(location=0) out vec4 Color;
vec3 encodeSRGB(vec3 c) {
    return mix(12.92*c, 1.055*pow(c, vec3(1.0/2.4))-0.055, greaterThan(c,vec3(0.0031308)));
}
vec3 decodeSRGB(vec3 c) {
    return mix(c/12.92, pow((c+0.055)/1.055,vec3(2.4)), greaterThan(c,vec3(0.04045)));
}
void main() {
    vec4 sampleColor = texelFetch(InputColor, ivec2(gl_FragCoord.xy), 0);
    if (p.Settings.y != 0) {
        vec3 y = decodeSRGB(sampleColor.rgb);
        if (p.Settings.z == 1.0) { Color=vec4(y/p.Settings.x,sampleColor.a); return; }
        vec3 b = 0.59*y-0.03;
        vec3 a = 2.43*y-2.51;
        vec3 c = 0.14*y;
        vec3 x = 2*c / max(sqrt(b*b-4*a*c)-b, vec3(1e-8));
        Color = vec4(x/p.Settings.x, sampleColor.a);
    } else {
        vec3 x = max(sampleColor.rgb,vec3(0))*p.Settings.x;
        // Divide first for large values: no half-float intermediate clamping.
        vec3 mapped = p.Settings.z == 1.0 ? clamp(x,0,1) : clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0,1);
        Color = vec4(encodeSRGB(mapped),sampleColor.a);
    }
})";
                    if (samples > 1)
                    {
                        auto at = fragment.find("uniform sampler2D InputColor;");
                        fragment.replace(
                            at, std::string("uniform sampler2D InputColor;").size(), "uniform sampler2DMS InputColor;");
                        at = fragment.find("ivec2(gl_FragCoord.xy), 0");
                        fragment.replace(
                            at, std::string("ivec2(gl_FragCoord.xy), 0").size(), "ivec2(gl_FragCoord.xy), gl_SampleID");
                    }
                    return fragment;
                }());
            if (auto* error = std::get_if<AssetError>(&built))
                throw std::runtime_error(error->Message);
            program  = std::get<Handle<RenderHardware::Shader>>(std::move(built));
            m_Layout = device.CreateBindGroupLayout(
                {0, {{0, BindingType::UniformBuffer}, {1, BindingType::CombinedTextureSampler}}});
            m_Sampler = device.CreateSampler({});
        }
        std::vector<Format> formats;
        RenderingDesc       render;
        for (const auto& target : targets)
        {
            formats.push_back(target->GetDesc().PixelFormat);
            render.Colors.push_back({target, {}, LoadOp::Load});
        }
        auto& pipeline = m_Pipelines[{formats, samples}];
        if (!pipeline)
        {
            PipelineDesc desc;
            desc.Program         = program;
            desc.Samples         = samples;
            desc.GroupLayouts    = {m_Layout};
            desc.ColorFormats    = formats;
            desc.Rasterizer.Cull = CullMode::None;
            desc.Blends.resize(formats.size());
            // Secondary compatibility target is preserved, as with all material draws.
            for (size_t i = 1; i < formats.size(); ++i)
                desc.Blends[i].WriteMask = 0;
            pipeline = device.CreatePipeline(desc);
        }
        glm::vec4      settings {EffectiveExposure(exposure), inverse ? 1.0f : 0.0f, float(mode), 0};
        Handle<Buffer> buffer;
        for (const auto& entry : m_Uniforms)
            if (entry.first == settings)
            {
                buffer = entry.second;
                break;
            }
        if (!buffer)
        {
            // Recycle only a snapshot no command list can still reference. This also
            // avoids unbounded caching while an exposure slider changes every frame.
            auto reusable = std::find_if(
                m_Uniforms.begin(), m_Uniforms.end(), [](const auto& entry) { return entry.second.use_count() == 1; });
            if (reusable == m_Uniforms.end())
            {
                m_Uniforms.emplace_back(
                    settings, device.CreateBuffer({sizeof(settings), BufferUsage::Uniform, MemoryUsage::Upload}));
                reusable = std::prev(m_Uniforms.end());
            }
            reusable->first = settings;
            buffer          = reusable->second;
            buffer->Write(0, &settings, sizeof(settings));
        }
        BindGroupDesc group;
        group.Layout  = m_Layout;
        group.Entries = {{0, buffer}, {1, {}, 0, 0, source, m_Sampler}};
        commands.BeginRendering(render);
        commands.SetViewport({0, 0, float(source->GetDesc().Width), float(source->GetDesc().Height)});
        commands.SetScissor({0, 0, source->GetDesc().Width, source->GetDesc().Height});
        commands.BindPipeline(pipeline);
        commands.BindGroup(device.CreateBindGroup(group));
        commands.Draw(3);
        commands.EndRendering();
    }
} // namespace Kans
