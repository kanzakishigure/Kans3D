#include "kspch.h"
#include "Kans3D/Renderer/Passes/ToneMappingPass.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    float ToneMappingPass::EffectiveExposure(float exposure)
    {
        // Physical camera exposure can be much smaller than the legacy scalar range.
        return std::isfinite(exposure) ? std::clamp(exposure, 1e-10f, 1e8f) : 0.8f;
    }
    void ToneMappingPass::Record(RenderHardware::Device&                                             device,
                                 RenderHardware::CommandList&                                        commands,
                                 const RenderHardware::Handle<RenderHardware::Texture>&              source,
                                 const std::vector<RenderHardware::Handle<RenderHardware::Texture>>& targets,
                                 float                                                               exposure,
                                 bool                                                                inverse,
                                 ToneMappingMode                                                     mode,
                                 const ColorGradingSettings&                                         grading,
                                 const RenderHardware::Handle<RenderHardware::Texture>&              userLut,
                                 const RenderHardware::Handle<RenderHardware::Texture>&              autoExposure)
    {
        using namespace RenderHardware;
        const auto samples = source->GetDesc().Samples;
        const auto lut     = m_Grading.Record(device, commands, targets.at(0), grading, userLut);
        const bool graded  = bool(lut);
        auto&      layout  = m_Layouts[graded][bool(autoExposure)];
        auto&      program = m_Programs[samples > 1][graded][bool(autoExposure)];
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
// KhronosGroup/ToneMapping, PBR_Neutral/pbrNeutral.glsl (linear Rec.709).
vec3 neutral(vec3 v) {
    float m = min(v.r,min(v.g,v.b));
    v -= m < .08 ? m-6.25*m*m : .04;
    float peak = max(v.r,max(v.g,v.b));
    if (peak < .76) return v;
    float compressed = 1.-.0576/(peak-.52);
    return mix(v*(compressed/peak),vec3(compressed),1.-1./(1.+.15*(peak-compressed)));
}
// AgX base polynomial approximation: https://iolite-engine.com/blog_posts/minimal_agx_implementation
// Rec.709 inset/outset; log2 bounds -12.47393 to 4.026069; output is linearized before sRGB.
vec3 agx(vec3 v) {
    const mat3 inset = mat3(.842479062253094,.0423282422610123,.0423756549057051,
                            .0784335999999992,.878468636469772,.0784336,
                            .0792237451477643,.0791661274605434,.879142973793104);
    v = clamp((log2(max(inset*v,vec3(1e-10)))+12.47393)/16.5,0,1);
    vec3 v2=v*v, v4=v2*v2;
    v = 15.5*v4*v2-40.14*v4*v+31.96*v4-6.868*v2*v+.4298*v2+.1191*v-.00232;
    // ASC CDL looks used by Minimal AgX and Filament's AgxToneMapper.
    if (p.Settings.z >= 4.) {
        float luma=dot(v,vec3(.2126,.7152,.0722));
        vec3 slope=p.Settings.z==5. ? vec3(1.,.9,.5) : vec3(1.);
        float power=p.Settings.z==5. ? .8 : 1.35;
        float saturation=p.Settings.z==5. ? 1.3 : 1.4;
        v=luma+saturation*(pow(max(v*slope,vec3(0)),vec3(power))-luma);
    }
    v = inverse(inset)*v;
    return clamp(pow(max(v,vec3(0)),vec3(2.2)),0,1);
}
vec3 tonemap(vec3 x) {
    if (p.Settings.z == 1.) return clamp(x,0,1);
    if (p.Settings.z == 2. || p.Settings.z >= 4.) return agx(x);
    if (p.Settings.z == 3.) return neutral(x);
    // Narkowicz ACES fitted curve: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
    return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0,1);
}
// New view transforms are coupled RGB functions. Solve their inverse at display/HDR
// compatibility boundaries; clamped highlights have no unique inverse.
vec3 inverseView(vec3 y) {
    vec3 x=max(y,vec3(1e-5));
    for(int i=0;i<24;++i) {
        vec3 f=tonemap(x), h=max(x*.002,vec3(.0001));
        mat3 jac=mat3((tonemap(x+vec3(h.x,0,0))-f)/h.x,
                      (tonemap(x+vec3(0,h.y,0))-f)/h.y,
                      (tonemap(x+vec3(0,0,h.z))-f)/h.z);
        if(abs(determinant(jac))<1e-10) break;
        vec3 step=inverse(jac)*(f-y);
        x=max(x-clamp(step,-x*.5,x*.5),vec3(0));
    }
    return x;
}
void main() {
    vec4 sampleColor = texelFetch(InputColor, ivec2(gl_FragCoord.xy), 0);
    if (p.Settings.y != 0) {
        vec3 y = decodeSRGB(sampleColor.rgb);
        if (p.Settings.z == 1.0) { Color=vec4(y/p.Settings.x,sampleColor.a); return; }
        if (p.Settings.z >= 2.) { Color=vec4(inverseView(y)/p.Settings.x,sampleColor.a); return; }
        vec3 b = 0.59*y-0.03;
        vec3 a = 2.43*y-2.51;
        vec3 c = 0.14*y;
        vec3 x = 2*c / max(sqrt(b*b-4*a*c)-b, vec3(1e-8));
        Color = vec4(x/p.Settings.x, sampleColor.a);
    } else {
        vec3 x = max(sampleColor.rgb,vec3(0))*p.Settings.x;
        // Divide first for large values: no half-float intermediate clamping.
        vec3 mapped = tonemap(x);
        Color = vec4(encodeSRGB(mapped),sampleColor.a);
    }
})";
                    if (graded)
                    {
                        auto at = fragment.find("vec3 encodeSRGB");
                        fragment.insert(at, R"(layout(set=0,binding=2) uniform sampler3D GradingLut;
vec3 grade(vec3 c) {
    vec3 uv=clamp((log2(max(c,vec3(exp2(-12.))))+12.)/24.,0,1);
    return exp2(texture(GradingLut,uv*(31./32.)+.5/32.).rgb*24.-12.);
}
)");
                        at = fragment.find("vec3 tonemap(vec3 x) {");
                        fragment.insert(at + std::string("vec3 tonemap(vec3 x) {").size(), "x=grade(x);\n");
                        at = fragment.find("if (p.Settings.z == 1.0)");
                        fragment.replace(at, std::string("if (p.Settings.z == 1.0)").size(), "if (false)");
                        at = fragment.find("if (p.Settings.z >= 2.)");
                        fragment.replace(at, std::string("if (p.Settings.z >= 2.)").size(), "if (true)");
                    }
                    if (autoExposure)
                    {
                        auto at = fragment.find("vec3 encodeSRGB");
                        fragment.insert(at,
                                        "layout(set=0,binding=3) uniform sampler2D AutoExposure;\nfloat "
                                        "exposure(){return texelFetch(AutoExposure,ivec2(0),0).r;}\n");
                        for (size_t index = 0; (index = fragment.find("p.Settings.x", index)) != std::string::npos;
                             index += 10)
                            fragment.replace(index, 12, "exposure()");
                    }
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
            BindGroupLayoutDesc bindings {0,
                                          {{0, BindingType::UniformBuffer}, {1, BindingType::CombinedTextureSampler}}};
            if (graded)
                bindings.Entries.push_back({2, BindingType::CombinedTextureSampler});
            if (autoExposure)
                bindings.Entries.push_back({3, BindingType::CombinedTextureSampler});
            layout    = device.CreateBindGroupLayout(bindings);
            m_Sampler = device.CreateSampler({});
        }
        std::vector<Format> formats;
        RenderingDesc       render;
        for (const auto& target : targets)
        {
            formats.push_back(target->GetDesc().PixelFormat);
            render.Colors.push_back({target, {}, LoadOp::Load});
        }
        auto& pipeline =
            m_Pipelines[{formats, samples | (graded ? 0x80000000u : 0u) | (autoExposure ? 0x40000000u : 0u)}];
        if (!pipeline)
        {
            PipelineDesc desc;
            desc.Program         = program;
            desc.Samples         = samples;
            desc.GroupLayouts    = {layout};
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
        group.Layout  = layout;
        group.Entries = {{0, buffer}, {1, {}, 0, 0, source, m_Sampler}};
        if (graded)
            group.Entries.push_back({2, {}, 0, 0, lut, m_Sampler});
        if (autoExposure)
            group.Entries.push_back({3, {}, 0, 0, autoExposure, m_Sampler});
        commands.BeginRendering(render);
        commands.SetViewport({0, 0, float(source->GetDesc().Width), float(source->GetDesc().Height)});
        commands.SetScissor({0, 0, source->GetDesc().Width, source->GetDesc().Height});
        commands.BindPipeline(pipeline);
        commands.BindGroup(device.CreateBindGroup(group));
        commands.Draw(3);
        commands.EndRendering();
    }
} // namespace Kans
