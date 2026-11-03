#include "kspch.h"
#include "StandardShader.h"
#include <stdexcept>
#include "Kans3D/Asset/Importer/MaterialSource.h"
#include "Kans3D/Asset/Importer/ShaderImporter.h"
#include "MaterialShaderInterface.h"

namespace Kans
{
    AssetID StandardShaderID() { return AssetID {SourceAssetID::FromString("ffffffffffffffff"), 1}; }
    AssetID SixWaySmokeShaderID() { return AssetID {SourceAssetID::FromString("ffffffffffffffff"), 2}; }
    AssetID SixWaySmokeMaterialID() { return AssetID {SourceAssetID::FromString("ffffffffffffffff"), 3}; }
    Ref<const MaterialDataAsset> GetSixWaySmokeMaterialAsset()
    {
        static const auto asset = [] {
            MaterialSource source;
            source.Shader           = SixWaySmokeShaderID();
            source.Render.AlphaMode = MaterialAlphaMode::Blend;
            auto result             = ResolveMaterialSource(source, &GetSixWaySmokeShader());
            if (auto error = std::get_if<AssetError>(&result))
                throw std::runtime_error(error->Message);
            auto value     = CreateRef<MaterialDataAsset>(std::get<MaterialData>(std::move(result)));
            value->assetID = SixWaySmokeMaterialID();
            return value;
        }();
        return asset;
    }
    bool IsStandardShader(AssetID id) { return !id.IsValid() || id == StandardShaderID(); }

    const ShaderData& GetStandardShader(bool particle)
    {
        static const auto build = [](bool particle) {
            const std::string                  manifest = R"(KansShaderProgram 1
vertex=Standard.vert
fragment=Standard.frag
output_space=linear_hdr
default BaseColor = 1 1 1 1
default Metallic = 0
default Roughness = 1
default EmissiveColor = 0 0 0
default EmissiveIntensity = 1
default NormalStrength = 1
default OcclusionStrength = 1
default UVTiling = 1 1
default UVOffset = 0 0
default VertexColorStrength = 0
hint BaseColor = color
hint Metallic = range 0 1
hint Roughness = range 0 1
hint EmissiveColor = color
hint OcclusionStrength = range 0 1
hint VertexColorStrength = range 0 1
texture BaseColorMap = white
texture NormalMap = normal
texture MetallicRoughnessMap = white
texture OcclusionMap = white
texture EmissiveMap = white
group Surface = BaseColor BaseColorMap Metallic Roughness MetallicRoughnessMap
group Normal = NormalMap NormalStrength
group Occlusion = OcclusionMap OcclusionStrength
group Emission = EmissiveColor EmissiveIntensity EmissiveMap
group UV = UVTiling UVOffset
group Advanced = VertexColorStrength
)";
            std::map<std::string, std::string> stages   = {{"Standard.vert", R"(#version 450
#include <Kans3D/Engine.glsl>
#include <Kans3D/VertexInput.glsl>
layout(location = 0) out vec2 v_UV;
layout(location = 1) out vec3 v_WorldPosition;
layout(location = 2) out vec3 v_Normal;
layout(location = 3) out vec4 v_Color;
layout(location = 4) out vec4 v_Tangent;
void main()
{
    v_UV = a_UV;
    v_WorldPosition = vec3(u_Object.Transform * vec4(a_Position, 1));
    v_Normal = mat3(u_Object.NormalMatrix) * a_Normal;
    v_Color = a_Color;
    // Tangents are directions, normals are covectors. A mirrored object also
    // reverses the basis handedness.
    v_Tangent = vec4(mat3(u_Object.Transform) * a_Tangent.xyz,
                     a_Tangent.w * sign(determinant(mat3(u_Object.Transform))));
    gl_Position = u_Frame.ViewProjection * vec4(v_WorldPosition, 1);
}
)"},
                                                           {"Standard.frag", R"(#version 450
#include <Kans3D/Lighting.glsl>
#include <Kans3D/Alpha.glsl>
layout(set = 1, binding = 0, std140) uniform StandardParameters
{
    vec4 BaseColor;
    float Metallic;
    float Roughness;
    vec3 EmissiveColor;
    float EmissiveIntensity;
    float NormalStrength;
    float OcclusionStrength;
    vec2 UVTiling;
    vec2 UVOffset;
    float VertexColorStrength;
} p;
layout(set = 1, binding = 1) uniform sampler2D BaseColorMap;
layout(set = 1, binding = 2) uniform sampler2D NormalMap;
layout(set = 1, binding = 3) uniform sampler2D MetallicRoughnessMap;
layout(set = 1, binding = 4) uniform sampler2D OcclusionMap;
layout(set = 1, binding = 5) uniform sampler2D EmissiveMap;
layout(location = 0) in vec2 v_UV;
layout(location = 1) in vec3 v_WorldPosition;
layout(location = 2) in vec3 v_Normal;
layout(location = 3) in vec4 v_Color;
layout(location = 4) in vec4 v_Tangent;
layout(location = 0) out vec4 o_Color;
vec3 StandardNormal(vec2 uv)
{
    vec3 N = normalize(v_Normal);
    vec3 T = v_Tangent.xyz - N * dot(N, v_Tangent.xyz);
    vec3 B;
    if (dot(T, T) > 1e-8 && abs(v_Tangent.w) > 1e-8)
    {
        T = normalize(T);
        B = cross(N, T) * sign(v_Tangent.w);
    }
    else
    {
        vec3 q1 = dFdx(v_WorldPosition), q2 = dFdy(v_WorldPosition);
        vec2 st1 = dFdx(uv), st2 = dFdy(uv);
        float determinantUV = st1.x * st2.y - st1.y * st2.x;
        T = (q1 * st2.y - q2 * st1.y) * sign(determinantUV);
        T -= N * dot(N, T);
        if (dot(T, T) < 1e-8 || abs(determinantUV) < 1e-8)
            return N;
        T = normalize(T);
        // Recover +V in world space. The sign of the screen-space UV
        // determinant alone changes when framebuffer Y changes direction.
        vec3 derivativeB = (q2 * st1.x - q1 * st2.x) * sign(determinantUV);
        B = cross(N, T) * sign(dot(cross(N, T), derivativeB));
    }
    vec3 mapped = texture(NormalMap, uv).xyz * 2.0 - 1.0;
    mapped.xy *= p.NormalStrength;
    return normalize(mat3(T, B, N) * mapped);
}
void main()
{
    vec2 uv = v_UV * p.UVTiling + p.UVOffset;
    vec4 baseColor = p.BaseColor * mix(vec4(1), v_Color, p.VertexColorStrength) * texture(BaseColorMap, uv);
    KansAlphaClip(baseColor.a);
    float alpha = KansOutputAlpha(baseColor.a);
    vec3 albedo = baseColor.rgb;
    vec3 N = StandardNormal(uv);
    if (u_Object.MaterialRender.z != 0.0 && !gl_FrontFacing)
        N = -N;
    if (u_Frame.DebugView == 1)
    {
        o_Color = vec4(pow(albedo, vec3(1.0 / 2.2)), alpha);
        if(u_Object.MaterialRender.x==7.0)o_Color.rgb*=alpha;
        return;
    }
    if (u_Frame.DebugView == 2)
    {
        o_Color = vec4(N * 0.5 + 0.5, alpha);
        if(u_Object.MaterialRender.x==7.0)o_Color.rgb*=alpha;
        return;
    }
    if (u_Frame.DebugView == 4)
    {
        int cascade = KansShadowCascade(v_WorldPosition);
        vec3 colors[4] = vec3[](vec3(1,.2,.2), vec3(.2,1,.2), vec3(.2,.4,1), vec3(1,1,.2));
        o_Color = vec4(cascade < 0 ? vec3(.2) : colors[cascade] * (.3 + .7 * KansShadow(v_WorldPosition,N)), alpha);
        if(u_Object.MaterialRender.x==7.0)o_Color.rgb*=alpha;
        return;
    }
    vec4 mr = texture(MetallicRoughnessMap, uv);
    float ao = mix(1.0, texture(OcclusionMap, uv).r, p.OcclusionStrength);
    vec3 color = KansShadePbr(albedo, p.Metallic * mr.b, p.Roughness * mr.g, ao, N, v_WorldPosition);
    color += p.EmissiveColor * p.EmissiveIntensity * texture(EmissiveMap, uv).rgb;
    o_Color = u_Frame.DebugView == 3 ? KansToneMap(color) : vec4(color, alpha);
    o_Color.a = alpha;
    if(u_Object.MaterialRender.x==7.0)o_Color.rgb*=alpha;
}
)"}};
            // Keep the ordinary mesh sources identical to Standard's original program.
            // Only procedural particle variants opt into vertex diffuse lighting.
            if (particle)
            {
                auto&             vertex   = stages.at("Standard.vert");
                const std::string lighting = "#define KANS_PARTICLE_DIFFUSE_ONLY\n"
                                             "#include <Kans3D/ParticleLighting.glsl>\n";
                vertex.insert(vertex.find("layout(location = 0)"), lighting);
                vertex.insert(vertex.find("    gl_Position"),
                              "    KansBuildParticleLighting(v_WorldPosition, v_Normal, v_Tangent.xyz);\n");
                auto& fragment = stages.at("Standard.frag");
                fragment.insert(fragment.find("#include"), lighting);
                const auto shade = fragment.find("    vec3 color = KansShadePbr");
                fragment.insert(shade,
                                "    vec3 color;\n    if (kans_ParticleLightingMode > 0.5)\n"
                                "        color = albedo * kans_ParticleDiffuse * ao;\n    else\n");
                fragment.replace(fragment.find("    vec3 color = KansShadePbr"), 17, "        color = ");
            }
            auto compiled = CompileEmbeddedShaderProgram(manifest, stages);
            if (auto* error = std::get_if<AssetError>(&compiled))
                throw std::runtime_error("Standard shader: " + error->Message);
            auto value    = std::get<ShaderData>(std::move(compiled));
            auto contract = ValidateMaterialShaderInterface(value.Reflection);
            if (auto* error = std::get_if<AssetError>(&contract))
                throw std::runtime_error("Standard shader: " + error->Message);
            return value;
        };
        static const ShaderData shader = build(false);
        if (!particle)
            return shader;
        static const ShaderData particleShader = build(true);
        return particleShader;
    }

    Ref<const ShaderDataAsset> GetStandardShaderAsset()
    {
        static const Ref<const ShaderDataAsset> asset = [] {
            auto value     = CreateRef<ShaderDataAsset>(GetStandardShader());
            value->assetID = StandardShaderID();
            return value;
        }();
        return asset;
    }

    const ShaderData& GetSixWaySmokeShader(bool particle)
    {
        static const auto build = [](bool particle) {
            const std::string                  manifest = R"(KansShaderProgram 1
vertex=SixWaySmoke.vert
fragment=SixWaySmoke.frag
output_space=linear_hdr
default AbsorptionTint = 1 1 1
default EmissiveColor = 0 0 0
default EmissiveIntensity = 1
default Alpha = 1
default UVTiling = 1 1
default UVOffset = 0 0
hint AbsorptionTint = color
hint EmissiveColor = color
hint Alpha = range 0 1
texture PositiveAxes = white
texture NegativeAxes = black
group Smoke = PositiveAxes NegativeAxes AbsorptionTint Alpha
group Emission = EmissiveColor EmissiveIntensity
group UV = UVTiling UVOffset
)";
            std::map<std::string, std::string> stages   = {{"SixWaySmoke.vert", R"(#version 450
#include <Kans3D/Engine.glsl>
#include <Kans3D/VertexInput.glsl>
#ifdef KANS_PARTICLE_VARIANT
#include <Kans3D/ParticleLighting.glsl>
#endif
layout(location = 0) out vec2 v_UV;
layout(location = 1) out vec3 v_WorldPosition;
layout(location = 2) out vec3 v_Normal;
layout(location = 4) out vec4 v_Tangent;
void main()
{
    v_UV = a_UV;
    v_WorldPosition = vec3(u_Object.Transform * vec4(a_Position, 1));
    v_Normal = mat3(u_Object.NormalMatrix) * a_Normal;
    v_Tangent = vec4(mat3(u_Object.Transform) * a_Tangent.xyz,
                     a_Tangent.w * sign(determinant(mat3(u_Object.Transform))));
#ifdef KANS_PARTICLE_VARIANT
    KansBuildParticleLighting(v_WorldPosition, v_Normal, v_Tangent.xyz);
#endif
    gl_Position = u_Frame.ViewProjection * vec4(v_WorldPosition, 1);
}
)"},
                                                           {"SixWaySmoke.frag", R"(#version 450
#ifndef KANS_PARTICLE_VARIANT
// Ordinary meshes are supported through per-pixel six-way evaluation. They
// neither produce nor consume particle varyings (including the mode selector).
#define KANS_PARTICLE_PIXEL_ONLY
#endif
#include <Kans3D/ParticleLighting.glsl>
#include <Kans3D/Alpha.glsl>
layout(set = 1, binding = 0, std140) uniform SixWaySmokeParameters
{
    vec3 AbsorptionTint;
    vec3 EmissiveColor;
    float EmissiveIntensity;
    float Alpha;
    vec2 UVTiling;
    vec2 UVOffset;
} p;
layout(set = 1, binding = 1) uniform sampler2D PositiveAxes;
layout(set = 1, binding = 2) uniform sampler2D NegativeAxes;
layout(location = 0) in vec2 v_UV;
layout(location = 1) in vec3 v_WorldPosition;
layout(location = 2) in vec3 v_Normal;
layout(location = 4) in vec4 v_Tangent;
layout(location = 0) out vec4 o_Color;
void main()
{
    // The particle wrapper invokes the whole material for both atlas frames,
    // blending both axes textures, opacity and emission together.
    vec2 uv = v_UV * p.UVTiling + p.UVOffset;
    vec4 positiveAxes = texture(PositiveAxes, uv);
    vec4 negativeAxes = texture(NegativeAxes, uv);
    float opacity = positiveAxes.a * p.Alpha;
    KansAlphaClip(opacity);
    float alpha = KansOutputAlpha(opacity);
    vec3 normal = normalize(v_Normal);
    if (u_Object.MaterialRender.z != 0.0 && !gl_FrontFacing)
        normal = -normal;
    vec3 color = KansSixWayLighting(positiveAxes, negativeAxes, p.AbsorptionTint,
                                    normal, v_Tangent.xyz, v_WorldPosition)
               + negativeAxes.a * p.EmissiveColor * p.EmissiveIntensity;
    o_Color = u_Frame.DebugView == 3 ? KansToneMap(color) : vec4(color, alpha);
    o_Color.a = alpha;
    if(u_Object.MaterialRender.x==7.0)o_Color.rgb*=alpha;
}
)"}};
            if (particle)
                for (auto& [name, source] : stages)
                    source.insert(source.find('\n') + 1, "#define KANS_PARTICLE_VARIANT\n");
            auto compiled = CompileEmbeddedShaderProgram(manifest, stages);
            if (auto* error = std::get_if<AssetError>(&compiled))
                throw std::runtime_error("Kans3D/SixWaySmoke shader: " + error->Message);
            auto value    = std::get<ShaderData>(std::move(compiled));
            auto contract = ValidateMaterialShaderInterface(value.Reflection);
            if (auto* error = std::get_if<AssetError>(&contract))
                throw std::runtime_error("Kans3D/SixWaySmoke shader: " + error->Message);
            return value;
        };
        static const ShaderData shader = build(false);
        if (!particle)
            return shader;
        static const ShaderData particleShader = build(true);
        return particleShader;
    }

    Ref<const ShaderDataAsset> GetSixWaySmokeShaderAsset()
    {
        static const Ref<const ShaderDataAsset> asset = [] {
            auto value     = CreateRef<ShaderDataAsset>(GetSixWaySmokeShader());
            value->assetID = SixWaySmokeShaderID();
            return value;
        }();
        return asset;
    }

    std::variant<MaterialData, AssetError> ResolveStandardMaterial(const MaterialData&                 legacy,
                                                                   std::optional<std::array<float, 3>> emissive)
    {
        if (legacy.Bindings && IsStandardShader(legacy.Shader))
            return legacy;
        MaterialSource source;
        source.Name       = legacy.Name;
        source.Render     = legacy.Render;
        source.Parameters = {{"BaseColor", {legacy.BaseColor.begin(), legacy.BaseColor.end()}},
                             {"Metallic", {legacy.Metallic}},
                             {"Roughness", {legacy.Roughness}}};
        using S           = MaterialTextureSemantic;
        const auto pick   = [&](const char* name, std::initializer_list<S> semantics) {
            for (S semantic : semantics)
                for (const auto& texture : legacy.Textures)
                    if (texture.Semantic == uint32_t(semantic) && texture.UVChannel == 0 && texture.Texture.IsValid())
                    {
                        source.Textures[name] = texture.Texture;
                        return;
                    }
        };
        pick("BaseColorMap", {S::BaseColor, S::Diffuse});
        pick("NormalMap", {S::Normal, S::NormalCamera, S::Height});
        pick("MetallicRoughnessMap", {S::MetallicRoughness, S::Unknown, S::DiffuseRoughness});
        pick("OcclusionMap", {S::AmbientOcclusion, S::Lightmap});
        pick("EmissiveMap", {S::EmissionColor, S::Emissive});
        // Legacy materials emitted the texture directly, with no factor.
        if (source.Textures.count("EmissiveMap"))
            source.Parameters["EmissiveColor"] = {1, 1, 1};
        if (emissive && (!source.Textures.count("EmissiveMap") || *emissive != std::array<float, 3> {0, 0, 0}))
            source.Parameters["EmissiveColor"] = {emissive->begin(), emissive->end()};
        return ResolveMaterialSource(source, nullptr);
    }
} // namespace Kans
