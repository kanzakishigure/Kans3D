// Shared shader-facing literals. Generators use these constants; the registry hashes their exact bytes.
// Add emitted GLSL here. Bump EngineShaderContractVersion for non-text compiler/contract changes.
KANS_SHADER_TEXT(StandardManifest, "Standard/Manifest", R"(KansShaderProgram 1
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
)")
KANS_SHADER_TEXT(StandardVertex, "Standard/Vertex", R"(#version 450
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
)")
KANS_SHADER_TEXT(StandardFragment, "Standard/Fragment", R"(#version 450
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
)")
KANS_SHADER_TEXT(StandardParticleLightingInclude,
                 "Standard/define_KANS_PARTICLE_DIFFUSE_ONLY_include_Kans3D_ParticleLig",
                 "#define KANS_PARTICLE_DIFFUSE_ONLY\n"
                 "#include <Kans3D/ParticleLighting.glsl>\n")
KANS_SHADER_TEXT(StandardParticleVertexLighting,
                 "Standard/KansBuildParticleLighting_v_WorldPosition_v_Normal_v_Tangent",
                 "    KansBuildParticleLighting(v_WorldPosition, v_Normal, v_Tangent.xyz);\n")
KANS_SHADER_TEXT(StandardParticleFragmentLighting,
                 "Standard/vec3_color_if_kans_ParticleLightingMode_color",
                 "    vec3 color;\n    if (kans_ParticleLightingMode > 0.5)\n"
                 "        color = albedo * kans_ParticleDiffuse * ao;\n    else\n")
KANS_SHADER_TEXT(StandardParticleFallbackColor, "Standard/color", "        color = ")
KANS_SHADER_TEXT(StandardSixWayManifest, "Standard/SixWayManifest", R"(KansShaderProgram 1
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
)")
KANS_SHADER_TEXT(StandardSixWayVertex, "Standard/SixWayVertex", R"(#version 450
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
)")
KANS_SHADER_TEXT(StandardSixWayFragment, "Standard/SixWayFragment", R"(#version 450
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
)")
KANS_SHADER_TEXT(StandardParticleVariantDefine,
                 "Standard/define_KANS_PARTICLE_VARIANT",
                 "#define KANS_PARTICLE_VARIANT\n")
KANS_SHADER_TEXT(Skinning_mat4_header_kans_Palette, "Skinning/mat4_header_kans_Palette_kans_Palette_length", R"(
    mat4 header=kans_Palette[kans_Palette.length()-1];
    if(header[3][3]==-2718.0) {
        uint first=uint(header[0][0]), count=uint(header[0][1]);
 )")
KANS_SHADER_TEXT(Skinning_uint_vertex_kans_Deltas,
                 "Skinning/uint_vertex_kans_Deltas_kans_Deltas_uint",
                 "uint "
                 "vertex=kans_Deltas[kans_Deltas[0]+uint(gl_VertexIndex)/2];\n")
KANS_SHADER_TEXT(Skinning_uint_vertex_uint_gl,
                 "Skinning/uint_vertex_uint_gl_VertexIndex",
                 "uint vertex=uint(gl_VertexIndex);\n")
KANS_SHADER_TEXT(Skinning_for_uint_i_i, "Skinning/for_uint_i_i_count", R"(
        for(uint i=0; i<count; ++i) {
            vec4 kans_Active=kans_Palette[first+i][0];
            uint key=1+uint(kans_Active.y)*5, size=kans_Deltas[key], indices=kans_Deltas[key+1];
            uint lo=0, hi=size;
            while(lo<hi) {
                uint mid=lo+(hi-lo)/2;
                if(kans_Deltas[indices+mid]<vertex) lo=mid+1;
                else hi=mid;
            }
            if(lo==size || kans_Deltas[indices+lo]!=vertex) continue;
            uint base=lo*3;
            p+=kans_Active.x*kans_MorphVector(kans_Deltas[key+2]+base);
)")
KANS_SHADER_TEXT(Skinning_if_kans_Deltas_key,
                 "Skinning/if_kans_Deltas_key_sn_kans_Active",
                 "if(kans_Deltas[key+3]!=0) "
                 "sn+=kans_Active.x*kans_MorphVector(kans_Deltas[key+3]+base);\n")
KANS_SHADER_TEXT(Skinning_if_kans_Deltas_key2,
                 "Skinning/if_kans_Deltas_key_kans_Active_x",
                 "if(kans_Deltas[key+3]!=0) "
                 "n+=kans_Active.x*kans_MorphVector(kans_Deltas[key+3]+base);\n")
KANS_SHADER_TEXT(Skinning_if_kans_Deltas_key3,
                 "Skinning/if_kans_Deltas_key_t_kans_Active",
                 "if(kans_Deltas[key+4]!=0) "
                 "t+=kans_Active.x*kans_MorphVector(kans_Deltas[key+4]+base);\n")
KANS_SHADER_TEXT(Skinning_mat4_skin_mat4_if, "Skinning/mat4_skin_mat4_if_dot", R"(
        }
    }
    mat4 skin=mat4(1);
    if(dot(kans_Weights,vec4(1))>0)
        skin=kans_Palette[kans_Joints.x]*kans_Weights.x+kans_Palette[kans_Joints.y]*kans_Weights.y+
             kans_Palette[kans_Joints.z]*kans_Weights.z+kans_Palette[kans_Joints.w]*kans_Weights.w;
    vec3 kans_DeformedPosition=(skin*vec4(p,1)).xyz;
)")
KANS_SHADER_TEXT(Skinning_kans_Position, "Skinning/kans_Position", "kans_Position")
KANS_SHADER_TEXT(Skinning_kans_Normal, "Skinning/kans_Normal", "kans_Normal")
KANS_SHADER_TEXT(Skinning_kans_SmoothNormal, "Skinning/kans_SmoothNormal", "kans_SmoothNormal")
KANS_SHADER_TEXT(Skinning_kans_Tangent, "Skinning/kans_Tangent", "kans_Tangent")
KANS_SHADER_TEXT(Skinning_in, "Skinning/in", "in ")
KANS_SHADER_TEXT(Space, "Common/Space", " ")
KANS_SHADER_TEXT(Semicolon, "Common/Semicolon", ";")
KANS_SHADER_TEXT(EndStatement, "Common/EndStatement", ";\n")
KANS_SHADER_TEXT(Vec3, "Common/Vec3", "vec3")
KANS_SHADER_TEXT(Skinning_kans_RawPosition, "Skinning/kans_RawPosition", "kans_RawPosition")
KANS_SHADER_TEXT(Skinning_kans_RawNormal, "Skinning/kans_RawNormal", "kans_RawNormal")
KANS_SHADER_TEXT(Skinning_kans_RawSmoothNormal, "Skinning/kans_RawSmoothNormal", "kans_RawSmoothNormal")
KANS_SHADER_TEXT(Vec4, "Common/Vec4", "vec4")
KANS_SHADER_TEXT(Skinning_kans_RawTangent, "Skinning/kans_RawTangent", "kans_RawTangent")
KANS_SHADER_TEXT(Skinning_kans_RestPosition_kans_Position,
                 "Skinning/kans_RestPosition_kans_Position",
                 "kans_RestPosition = kans_Position;")
KANS_SHADER_TEXT(Skinning_kans_RestPosition_kans_RawPosition,
                 "Skinning/kans_RestPosition_kans_RawPosition",
                 "kans_RestPosition = kans_RawPosition;")
KANS_SHADER_TEXT(Skinning_void_kans_MaterialMain, "Skinning/void_kans_MaterialMain", "void kans_MaterialMain()")
KANS_SHADER_TEXT(Skinning_layout_location_in_uvec4, "Skinning/layout_location_in_uvec4_kans_Joints", R"(
layout(location=5) in uvec4 kans_Joints;
layout(location=6) in vec4 kans_Weights;
layout(set=2,binding=0,std430) readonly buffer KansSkinningPalette { mat4 kans_Palette[]; };
layout(set=2,binding=1,std430) readonly buffer KansMorphDeltas { uint kans_Deltas[]; };
)")
KANS_SHADER_TEXT(Skinning_void_main_vec3_p, "Skinning/void_main_vec3_p_kans_RawPosition", R"(
void main() {
    vec3 p=kans_RawPosition;
)")
KANS_SHADER_TEXT(Skinning_vec3_sn_kans_RawSmoothNormal,
                 "Skinning/vec3_sn_kans_RawSmoothNormal",
                 "vec3 sn=kans_RawSmoothNormal;\n")
KANS_SHADER_TEXT(Skinning_vec3_kans_RawNormal, "Skinning/vec3_kans_RawNormal", "vec3 n=kans_RawNormal;\n")
KANS_SHADER_TEXT(Skinning_vec3_t_kans_RawTangent,
                 "Skinning/vec3_t_kans_RawTangent_xyz",
                 "vec3 t=kans_RawTangent.xyz;\n")
KANS_SHADER_TEXT(Skinning_kans_Position_kans_DeformedPosition,
                 "Skinning/kans_Position_kans_DeformedPosition",
                 "kans_Position=kans_DeformedPosition;\n")
KANS_SHADER_TEXT(Skinning_kans_SmoothNormal_normalize_transpose,
                 "Skinning/kans_SmoothNormal_normalize_transpose_inverse_mat3",
                 "kans_SmoothNormal=normalize(transpose(inverse(mat3(skin)))*sn);\n")
KANS_SHADER_TEXT(Skinning_kans_Normal_normalize_transpose,
                 "Skinning/kans_Normal_normalize_transpose_inverse_mat3",
                 "kans_Normal=normalize(transpose(inverse(mat3(skin)))*n);\n")
KANS_SHADER_TEXT(Skinning_kans_Tangent_vec4_normalize,
                 "Skinning/kans_Tangent_vec4_normalize_mat3_skin",
                 "kans_Tangent=vec4(normalize(mat3(skin)*t),kans_RawTangent.w*(determinant(mat3("
                 "skin))<0 ? -1 : 1));\n")
KANS_SHADER_TEXT(Skinning_kans_MaterialMain, "Skinning/kans_MaterialMain", "kans_MaterialMain();\n}\n")
KANS_SHADER_TEXT(Main, "Common/Main", "main")
KANS_SHADER_TEXT(Depth_include_Kans3D_DepthNormals_void, "Depth/include_Kans3D_DepthNormals_void_main", R"(
#include <Kans3D/DepthNormals.glsl>
void main()
{
    vec2 uv = v_UV * p.UVTiling + p.UVOffset;
    vec4 baseColor = p.BaseColor * mix(vec4(1), v_Color, p.VertexColorStrength) * texture(BaseColorMap, uv);
    KansAlphaClip(baseColor.a);
    vec3 N = StandardNormal(uv);
    if (u_Object.MaterialRender.z != 0.0 && !gl_FrontFacing)
        N = -N;
    o_Color = KansNormalRoughness(N, p.Roughness * texture(MetallicRoughnessMap, uv).g);
}
)")
KANS_SHADER_TEXT(Depth_KANS_FRAGMENT_STAGE, "Depth/KANS_FRAGMENT_STAGE", "KANS_FRAGMENT_STAGE")
KANS_SHADER_TEXT(Depth_kans_PrepassFlag, "Depth/kans_PrepassFlag", "kans_PrepassFlag")
KANS_SHADER_TEXT(Depth_void_kans_DepthMain, "Depth/void_kans_DepthMain", "void kans_DepthMain()")
KANS_SHADER_TEXT(Depth_nlayout_location, "Depth/nlayout_location", "\nlayout(location=")
KANS_SHADER_TEXT(Depth_out_vec4_kans_PrepassFlag, "Depth/out_vec4_kans_PrepassFlag", ") out vec4 kans_PrepassFlag;\n")
KANS_SHADER_TEXT(Depth_nvoid_main_kans_DepthMain,
                 "Depth/nvoid_main_kans_DepthMain_kans_PrepassFlag_vec4",
                 "\nvoid main(){ kans_DepthMain(); kans_PrepassFlag=vec4(0,0,0,2); }\n")
KANS_SHADER_TEXT(Depth_nvoid_main_kans_DepthMain2,
                 "Depth/nvoid_main_kans_DepthMain_kans_PrepassFlag_vec4_2",
                 "\nvoid main(){ kans_DepthMain(); kans_PrepassFlag=vec4(0); }\n")
KANS_SHADER_TEXT(Particle_layout_set_binding_std140, "Particle/layout_set_binding_std140_uniform", R"(
layout(set=2,binding=2,std140) uniform KansSprite {
mat4 owner;mat4 view;mat4 projection;mat4 inverseView;mat4 inverseProjection;
uvec4 offsets0;uvec4 offsets1;vec4 pivotGrid;vec4 facing;vec4 alignment;vec4 meshScale;vec4 meshPivot;vec4 lockedAxis;vec4 lighting;vec4 smokeShadow;vec4 options;
} sprite;
)")
KANS_SHADER_TEXT(Particle_kans_SpriteInput, "Particle/kans_SpriteInput", "kans_SpriteInput")
KANS_SHADER_TEXT(Particle_kans_MaterialUV, "Particle/kans_MaterialUV", "kans_MaterialUV")
KANS_SHADER_TEXT(Particle_kans_SpriteOutput, "Particle/kans_SpriteOutput", "kans_SpriteOutput")
KANS_SHADER_TEXT(Particle_void_kans_SpriteMaterialMain,
                 "Particle/void_kans_SpriteMaterialMain",
                 "void kans_SpriteMaterialMain()")
KANS_SHADER_TEXT(Particle_layout_set_binding_std430, "Particle/layout_set_binding_std430_readonly", R"(
layout(set=2,binding=0,std430) readonly buffer KansParticleAttributes{uvec4 attributes[];};
layout(set=2,binding=1,std430) readonly buffer KansParticleAlive{uint alive[];};
vec4 particle(uint attributeIndex,uint slot){return uintBitsToFloat(attributes[attributeIndex*sprite.offsets1.w+slot]);}
)")
KANS_SHADER_TEXT(Particle_KansParticleTransmittance,
                 "Particle/KansParticleTransmittance",
                 "KansParticleTransmittance = 1.0;")
KANS_SHADER_TEXT(Particle_KansParticleTransmittance_sprite_smokeShadow_x,
                 "Particle/KansParticleTransmittance_sprite_smokeShadow_x_particle",
                 "KansParticleTransmittance = "
                 "sprite.smokeShadow.x<0.0?1.0:particle(uint(sprite.smokeShadow.x),alive[gl_"
                 "InstanceIndex]).x;")
KANS_SHADER_TEXT(Particle_KansParticleEvaluation, "Particle/KansParticleEvaluation", "KansParticleEvaluation = 0.0;")
KANS_SHADER_TEXT(Particle_KansParticleEvaluation_sprite_lighting_z,
                 "Particle/KansParticleEvaluation_sprite_lighting_z",
                 "KansParticleEvaluation = sprite.lighting.z;")
KANS_SHADER_TEXT(Particle_Kans_particle_geometry, "Particle/Kans_particle_geometry", "\n// Kans particle geometry\n")
KANS_SHADER_TEXT(Vec2, "Common/Vec2", "vec2")
KANS_SHADER_TEXT(Particle_layout_location_out_vec4, "Particle/layout_location_out_vec4_kans_ParticleColor", R"(
layout(location=11) out vec4 kans_ParticleColor;
layout(location=12) out vec4 kans_ParticleDynamic;
layout(location=13) out vec3 kans_ParticleSubUV;
void main(){uint slot=alive[gl_InstanceIndex];vec3 position=particle(sprite.offsets0.x,slot).xyz;
if(sprite.options.w>0.5)position=(sprite.owner*vec4(position,1)).xyz;
vec3 right=sprite.inverseView[0].xyz,up=sprite.inverseView[1].xyz,normal=normalize(cross(right,up));
int mode=int(sprite.options.x);
if(mode==1){normal=normalize(sprite.inverseView[3].xyz-position);right=normalize(cross(up,normal));up=normalize(cross(normal,right));}
if(mode==2){vec3 velocity=particle(sprite.offsets1.x,slot).xyz;if(sprite.options.w>0.5)velocity=mat3(sprite.owner)*velocity;
vec3 projected=velocity-normal*dot(normal,velocity);if(dot(projected,projected)>1e-12){up=normalize(projected);right=normalize(cross(up,normal));}}
if(mode==3){normal=normalize(sprite.facing.xyz);up=normalize(sprite.alignment.xyz);right=normalize(cross(up,normal));up=normalize(cross(normal,right));}
vec2 corners[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));vec2 uv=corners[gl_VertexIndex%6];
vec2 corner=(uv-sprite.pivotGrid.xy)*particle(sprite.offsets0.z,slot).xy;float rotation=particle(sprite.offsets0.w,slot).x;
vec3 rotatedRight=right*cos(rotation)+up*sin(rotation);
up=up*cos(rotation)-right*sin(rotation);right=rotatedRight;
position+=right*corner.x+up*corner.y;
// Curvature is bend strength: 0 is flat, 1 is normalize(right*u + up*v + toCamera).
// Leave this unnormalized for exact per-pixel normalization after interpolation.
if(sprite.lighting.x==1.0)normal+=sprite.lighting.y*(right*(uv.x*2-1)+up*(uv.y*2-1));
if(sprite.lighting.x==2.0){normal=particle(uint(sprite.lighting.w),slot).xyz;
if(sprite.options.w>0.5)normal=transpose(inverse(mat3(sprite.owner)))*normal;
normal=dot(normal,normal)>1e-12?normalize(normal):normalize(cross(right,up));}
vec3 lightingTangent=right-normal*dot(right,normal)/max(dot(normal,normal),1e-12);
if(dot(lightingTangent,lightingTangent)<1e-12)lightingTangent=cross(up,normal);
right=normalize(lightingTangent);
float frame=particle(sprite.offsets1.y,slot).x;float total=sprite.pivotGrid.z*sprite.pivotGrid.w;
float current=mod(floor(frame),total),next=mod(current+1,total);
vec2 cell=vec2(mod(current,sprite.pivotGrid.z),floor(current/sprite.pivotGrid.z));
vec2 cellNext=vec2(mod(next,sprite.pivotGrid.z),floor(next/sprite.pivotGrid.z));
vec2 atlasUV=(cell+uv)/sprite.pivotGrid.zw;
kans_ParticleColor=particle(sprite.offsets0.y,slot);
kans_ParticleDynamic=particle(sprite.offsets1.z,slot);
kans_ParticleSubUV=vec3((cellNext+uv)/sprite.pivotGrid.zw,fract(frame)*sprite.options.z);
)")
KANS_SHADER_TEXT(Particle_layout_set_binding_std4302, "Particle/layout_set_binding_std430_readonly_2", R"(
layout(set=2,binding=0,std430) readonly buffer KansRibbonVertices{vec4 vertices[];};
layout(location=11) out vec4 kans_ParticleColor;
layout(location=12) out vec4 kans_ParticleDynamic;
layout(location=13) out vec3 kans_ParticleSubUV;
layout(location=14) out vec2 kans_RibbonUV1;
void main(){uint at=uint(gl_VertexIndex)*6;
vec3 position=vertices[at].xyz,normal=vertices[at+1].xyz,right=vertices[at+5].xyz;
vec2 atlasUV=vertices[at+2].xy;kans_RibbonUV1=vertices[at+2].zw;
kans_ParticleColor=vertices[at+3];kans_ParticleDynamic=vertices[at+4];kans_ParticleSubUV=vec3(0);
)")
KANS_SHADER_TEXT(Particle_layout_location_out_vec42, "Particle/layout_location_out_vec4_kans_ParticleColor_2", R"(
layout(location=11) out vec4 kans_ParticleColor;
layout(location=12) out vec4 kans_ParticleDynamic;
layout(location=13) out vec3 kans_ParticleSubUV;
vec3 rotateQ(vec4 q,vec3 v){return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
void main(){uint slot=alive[gl_InstanceIndex];vec3 position=particle(sprite.offsets0.x,slot).xyz;
vec4 q=particle(sprite.offsets0.w,slot);float qlen=length(q);q=qlen>1e-8?q/qlen:vec4(0,0,0,1);
vec3 scale=particle(sprite.offsets0.z,slot).xyz*sprite.meshScale.xyz;
vec3 x=vec3(1,0,0),y=vec3(0,1,0),z=vec3(0,0,1);int mode=int(sprite.options.x);
vec3 direction=vec3(0);
if(mode==2)direction=particle(sprite.offsets1.x,slot).xyz;
if(mode==1||mode==5){vec3 world=position;if(sprite.options.w>0.5)world=(sprite.owner*vec4(position,1)).xyz;
direction=mode==1?sprite.inverseView[3].xyz-world:sprite.inverseView[2].xyz;
if(sprite.options.w>0.5)direction=mat3(inverse(sprite.owner))*direction;}
if(dot(direction,direction)>1e-12){x=normalize(direction);vec3 up=sprite.lockedAxis.w>0.5?normalize(sprite.lockedAxis.xyz):vec3(0,0,1);
if(abs(dot(x,up))>.999)up=vec3(0,1,0);
y=normalize(cross(up,x));z=normalize(cross(x,y));
if(sprite.lockedAxis.w>0.5){z=normalize(sprite.lockedAxis.xyz);vec3 projected=x-z*dot(x,z);if(dot(projected,projected)>1e-12)x=normalize(projected);else x=normalize(cross(y,z));y=normalize(cross(z,x));}}
mat3 facing=mat3(x,y,z);
mat3 rotation=mat3(rotateQ(q,vec3(1,0,0)),rotateQ(q,vec3(0,1,0)),rotateQ(q,vec3(0,0,1)));
mat3 transform=facing*rotation;mat3 normalTransform=transform*mat3(vec3(1.0/max(abs(scale.x),1e-8)*sign(scale.x),0,0),vec3(0,1.0/max(abs(scale.y),1e-8)*sign(scale.y),0),vec3(0,0,1.0/max(abs(scale.z),1e-8)*sign(scale.z)));
vec3 localVertex=transform*(KANS_POSITION*scale+sprite.meshPivot.xyz*particle(sprite.offsets0.z,slot).xyz);
position+=localVertex;
transform=transform*mat3(vec3(scale.x,0,0),vec3(0,scale.y,0),vec3(0,0,scale.z));
if(sprite.options.w>0.5){position=(sprite.owner*vec4(position,1)).xyz;normalTransform=transpose(inverse(mat3(sprite.owner)))*normalTransform;transform=mat3(sprite.owner)*transform;}
kans_ParticleColor=particle(sprite.offsets0.y,slot);kans_ParticleDynamic=particle(sprite.offsets1.z,slot);kans_ParticleSubUV=vec3(0);
)")
KANS_SHADER_TEXT(Particle_vec3_2, "Particle/vec3_2", "vec3(0)")
KANS_SHADER_TEXT(Particle_Value, "Particle/Value", "Value")
KANS_SHADER_TEXT(Particle_position, "Particle/position", "=position;\n")
KANS_SHADER_TEXT(Particle_normalize_normalTransform,
                 "Particle/normalize_normalTransform",
                 "=normalize(normalTransform*")
KANS_SHADER_TEXT(Particle_Punctuation_4, "Particle/Punctuation_4", ");\n")
KANS_SHADER_TEXT(Assign, "Common/Assign", "=")
KANS_SHADER_TEXT(Particle_kans_ParticleColor, "Particle/kans_ParticleColor", "*kans_ParticleColor;\n")
KANS_SHADER_TEXT(Particle_vec4_normalize_transform, "Particle/vec4_normalize_transform", "=vec4(normalize(transform*")
KANS_SHADER_TEXT(Particle_xyz, "Particle/xyz", ".xyz),")
KANS_SHADER_TEXT(Particle_w_sign_determinant_transform,
                 "Particle/w_sign_determinant_transform",
                 ".w*sign(determinant(transform)));\n")
KANS_SHADER_TEXT(Particle_normal, "Particle/normal", "=normal;\n")
KANS_SHADER_TEXT(Particle_atlasUV, "Particle/atlasUV", "=atlasUV;\n")
KANS_SHADER_TEXT(Particle_kans_ParticleColor_2, "Particle/kans_ParticleColor_2", "=kans_ParticleColor;\n")
KANS_SHADER_TEXT(Particle_particle_sprite_offsets0_y,
                 "Particle/particle_sprite_offsets0_y_slot",
                 "=particle(sprite.offsets0.y,slot);\n")
KANS_SHADER_TEXT(Particle_vec4_right, "Particle/vec4_right", "=vec4(right,1);\n")
KANS_SHADER_TEXT(Particle_kans_SpriteMaterialMain, "Particle/kans_SpriteMaterialMain", "kans_SpriteMaterialMain();}\n")
KANS_SHADER_TEXT(Particle_layout_location_in_vec4,
                 "Particle/layout_location_in_vec4_kans_ParticleColor",
                 "layout(location=11) in vec4 kans_ParticleColor;\nlayout(location=12) in vec4 "
                 "kans_ParticleDynamic;\nlayout(location=13) in vec3 kans_ParticleSubUV;\n")
KANS_SHADER_TEXT(Particle_layout_location_in_vec2,
                 "Particle/layout_location_in_vec2_kans_RibbonUV1",
                 "layout(location=14) in vec2 kans_RibbonUV1;\n")
KANS_SHADER_TEXT(Particle_in_vec2_kans_MaterialUV, "Particle/in_vec2_kans_MaterialUV", "in vec2 kans_MaterialUV;")
KANS_SHADER_TEXT(Particle_in_vec2_kans_RawMaterialUV,
                 "Particle/in_vec2_kans_RawMaterialUV_nvec2_kans_MaterialUV",
                 "in vec2 kans_RawMaterialUV;\nvec2 kans_MaterialUV;")
KANS_SHADER_TEXT(Particle_KansVertexColor, "Particle/KansVertexColor", "KansVertexColor")
KANS_SHADER_TEXT(Particle_kans_ParticleColor_3, "Particle/kans_ParticleColor_3", "kans_ParticleColor")
KANS_SHADER_TEXT(Particle_KansVertexDynamicMaterialParameter,
                 "Particle/KansVertexDynamicMaterialParameter",
                 "KansVertexDynamicMaterialParameter")
KANS_SHADER_TEXT(Particle_kans_ParticleDynamic, "Particle/kans_ParticleDynamic", "kans_ParticleDynamic")
KANS_SHADER_TEXT(Particle_KansVertexSubUV, "Particle/KansVertexSubUV", "KansVertexSubUV")
KANS_SHADER_TEXT(Particle_kans_ParticleSubUV, "Particle/kans_ParticleSubUV", "kans_ParticleSubUV")
KANS_SHADER_TEXT(Particle_Punctuation_6, "Particle/Punctuation_6", " = ")
KANS_SHADER_TEXT(Particle_layout_set_binding_uniform, "Particle/layout_set_binding_uniform_sampler2D", R"(
layout(set=2,binding=3) uniform sampler2D kans_SpriteDepth;
void main(){
)")
KANS_SHADER_TEXT(Particle_kans_MaterialUV_kans_RawMaterialUV,
                 "Particle/kans_MaterialUV_kans_RawMaterialUV",
                 "kans_MaterialUV=kans_RawMaterialUV;\n")
KANS_SHADER_TEXT(Particle_KansVertexUV1_kans_RibbonUV1,
                 "Particle/KansVertexUV1_kans_RibbonUV1",
                 "KansVertexUV1 = kans_RibbonUV1")
KANS_SHADER_TEXT(Particle_kans_SpriteMaterialMain_2,
                 "Particle/kans_SpriteMaterialMain_2",
                 "kans_SpriteMaterialMain();\n")
KANS_SHADER_TEXT(Particle_if_kans_ParticleSubUV_z,
                 "Particle/if_kans_ParticleSubUV_z_vec4_first",
                 "if(kans_ParticleSubUV.z>0.0){vec4 "
                 "first=kans_SpriteOutput;kans_MaterialUV=kans_ParticleSubUV.xy;kans_SpriteMaterialMain("
                 ");kans_SpriteOutput=mix(first,kans_SpriteOutput,kans_ParticleSubUV.z); }\n")
KANS_SHADER_TEXT(Particle_if_sprite_options_y, "Particle/if_sprite_options_y_vec2", R"(
if(sprite.options.y>0){vec2 uv=gl_FragCoord.xy/vec2(textureSize(kans_SpriteDepth,0));float opaque=texture(kans_SpriteDepth,uv).r;
vec4 a=sprite.inverseProjection*vec4(uv*2-1,opaque,1);vec4 b=sprite.inverseProjection*vec4(uv*2-1,gl_FragCoord.z,1);
float fade=clamp((abs(a.z/a.w)-abs(b.z/b.w))/sprite.options.y,0,1);
kans_SpriteOutput.a*=fade;if(sprite.facing.w>0.5)kans_SpriteOutput.rgb*=fade;}
)")
KANS_SHADER_TEXT(Particle_Punctuation_7, "Particle/Punctuation_7", "}\n")
KANS_SHADER_TEXT(Particle_ParticleSprite, "Particle/ParticleSprite", "ParticleSprite")
KANS_SHADER_TEXT(ImporterPostVertex, "Importer/PostVertex", R"(#version 450
void main() { vec2 p=vec2((gl_VertexIndex<<1)&2, gl_VertexIndex&2); gl_Position=vec4(p*2.0-1.0,0,1); }
)")
KANS_SHADER_TEXT(Vfx_Punctuation_3, "Vfx/Punctuation_3", "(0);\n")
KANS_SHADER_TEXT(Vfx_version_nlayout_local_size,
                 "Vfx/version_nlayout_local_size_x_in_nvoid",
                 "#version 450\nlayout(local_size_x=1) in;\nvoid main(){\n")
KANS_SHADER_TEXT(Vfx_input, "Vfx/input", "input")
KANS_SHADER_TEXT(Newline, "Common/Newline", "\n")
KANS_SHADER_TEXT(Vfx_result, "Vfx/result", " result=")
KANS_SHADER_TEXT(Vfx_Punctuation_5, "Vfx/Punctuation_5", ";\n}")
KANS_SHADER_TEXT(Vfx_Punctuation_6, "Vfx/Punctuation_6", ".0")
KANS_SHADER_TEXT(Vfx_Punctuation_7, "Vfx/Punctuation_7", "(-2147483647-1)")
KANS_SHADER_TEXT(True, "Common/True", "true")
KANS_SHADER_TEXT(False, "Common/False", "false")
KANS_SHADER_TEXT(OpenParen, "Common/OpenParen", "(")
KANS_SHADER_TEXT(Comma, "Common/Comma", ",")
KANS_SHADER_TEXT(CloseParen, "Common/CloseParen", ")")
KANS_SHADER_TEXT(Vfx_ivec3, "Vfx/ivec3", "ivec3(")
KANS_SHADER_TEXT(Vfx_xyz, "Vfx/xyz", ".xyz)")
KANS_SHADER_TEXT(Vfx_int, "Vfx/int", "int(")
KANS_SHADER_TEXT(Vfx_x, "Vfx/x", ".x)")
KANS_SHADER_TEXT(Vfx_x_u, "Vfx/x_u", ".x!=0u)")
KANS_SHADER_TEXT(Vfx_uintBitsToFloat, "Vfx/uintBitsToFloat", "uintBitsToFloat(")
KANS_SHADER_TEXT(Vfx_x_2, "Vfx/x_2", ".x")
KANS_SHADER_TEXT(Vfx_xy, "Vfx/xy", ".xy")
KANS_SHADER_TEXT(Vfx_xyz_2, "Vfx/xyz_2", ".xyz")
KANS_SHADER_TEXT(Empty, "Common/Empty", "")
KANS_SHADER_TEXT(Vfx_uvec4_uvec3, "Vfx/uvec4_uvec3", "uvec4(uvec3(")
KANS_SHADER_TEXT(Vfx_u, "Vfx/u", "),0u)")
KANS_SHADER_TEXT(Vfx_uvec4_uint, "Vfx/uvec4_uint", "uvec4(uint(")
KANS_SHADER_TEXT(Vfx_u_u_u, "Vfx/u_u_u", "),0u,0u,0u)")
KANS_SHADER_TEXT(Vfx_uvec4, "Vfx/uvec4", "uvec4(")
KANS_SHADER_TEXT(Vfx_u_u_u_u, "Vfx/u_u_u_u_u", "?1u:0u,0u,0u,0u)")
KANS_SHADER_TEXT(Vfx_floatBitsToUint_vec4, "Vfx/floatBitsToUint_vec4", "floatBitsToUint(vec4(")
KANS_SHADER_TEXT(Vfx_Punctuation_12, "Vfx/Punctuation_12", ",0.0,0.0")
KANS_SHADER_TEXT(Vfx_Punctuation_13, "Vfx/Punctuation_13", ",0.0")
KANS_SHADER_TEXT(Vfx_Punctuation_14, "Vfx/Punctuation_14", "))")
KANS_SHADER_TEXT(Vfx_version_layout_local_size, "Vfx/version_layout_local_size_x_in_layout", R"glsl(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Attributes { uvec4 attributes[]; };
layout(set=0,binding=1,std430) readonly buffer Parameters { uvec4 parameters[]; };
layout(set=0,binding=2,std430) buffer Flags { uint flags[]; };
layout(set=0,binding=3,std430) buffer FreeList { uint freeList[]; };
layout(set=0,binding=4,std430) buffer Counters { uint counters[]; };
layout(set=0,binding=5,std430) readonly buffer AliveList { uint aliveList[]; };
layout(set=0,binding=6,std430) readonly buffer DataInterfaces {vec4 diValues[];};
layout(push_constant) uniform Push {uint capacity;uint seed;uint emitter;uint requested;uint worldSpace;uint kill;} pc;
uint eventKey=0u;
uint hashV(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float randomV(uint id,uint key,uint lane){return float(hashV(pc.seed^hashV(pc.emitter)^hashV(id)^key^hashV(lane)^hashV(eventKey))>>8)*(1.0/16777216.0);}
float noiseV(vec3 x){ivec3 b=ivec3(floor(x));vec3 f=fract(x);f=f*f*(3.0-2.0*f);float v=0;
for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int a=0;a<2;++a){ivec3 p=b+ivec3(a,y,z);float n=float(hashV(uint(p.x)^hashV(uint(p.y))^hashV(uint(p.z)))>>8)*(1.0/16777216.0);v+=n*(a!=0?f.x:1-f.x)*(y!=0?f.y:1-f.y)*(z!=0?f.z:1-f.z);}return v;}
void main(){uint invocation=gl_GlobalInvocationID.x;uint slot;uint uniqueID;
)glsl")
KANS_SHADER_TEXT(Vfx_vec3_fieldRotate_vec3_v, "Vfx/vec3_fieldRotate_vec3_v_vec4", R"glsl(
vec3 fieldRotate(vec3 v,vec4 q){float n=dot(q,q);q=n>1e-20?q*inversesqrt(n):vec4(0,0,0,1);return v+2.0*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
vec3 fieldPosition(vec3 p,vec3 t,vec4 q,vec3 scale){vec3 d;for(int a=0;a<3;++a)d[a]=abs(scale[a])>=1e-6?scale[a]:(scale[a]<0?-1e-6:1e-6);return fieldRotate(p-t,vec4(-q.xyz,q.w))/d;}
)glsl")
KANS_SHADER_TEXT(Vfx_layout_set_binding, "Vfx/layout_set_binding", "layout(set=0,binding=")
KANS_SHADER_TEXT(Vfx_std430_readonly_buffer_SceneColliders,
                 "Vfx/std430_readonly_buffer_SceneColliders_vec4",
                 ",std430) readonly buffer SceneColliders {vec4 collisionWords[];};\n")
KANS_SHADER_TEXT(Vfx_uniform_sampler2D_collisionDepth,
                 "Vfx/uniform_sampler2D_collisionDepth",
                 ") uniform sampler2D collisionDepth;\n")
KANS_SHADER_TEXT(Vfx_std430_readonly_buffer_SceneFields,
                 "Vfx/std430_readonly_buffer_SceneFields_vec4",
                 ",std430) readonly buffer SceneFields {vec4 fieldWords[];};\n")
KANS_SHADER_TEXT(Vfx_vec4_meshStatic_int_g, "Vfx/vec4_meshStatic_int_g_int", "vec4 meshStatic(int g,int o){switch(g){")
KANS_SHADER_TEXT(Vfx_std430_readonly_buffer_MeshGeometry,
                 "Vfx/std430_readonly_buffer_MeshGeometry",
                 ",std430) readonly buffer MeshGeometry")
KANS_SHADER_TEXT(Vfx_vec4_meshValues, "Vfx/vec4_meshValues", "{vec4 meshValues")
KANS_SHADER_TEXT(Vfx_Punctuation_15, "Vfx/Punctuation_15", "[];};\n")
KANS_SHADER_TEXT(Vfx_case, "Vfx/case", "case ")
KANS_SHADER_TEXT(Vfx_return_meshValues, "Vfx/return_meshValues", ":return meshValues")
KANS_SHADER_TEXT(Vfx_o, "Vfx/o", "[o];")
KANS_SHADER_TEXT(Vfx_return_vec4, "Vfx/return_vec4", "}return vec4(0); }\n")
KANS_SHADER_TEXT(Vfx_std430_readonly_buffer_GridRead,
                 "Vfx/std430_readonly_buffer_GridRead",
                 ",std430) readonly buffer GridRead")
KANS_SHADER_TEXT(Vfx_vec4_gridRead, "Vfx/vec4_gridRead", "{vec4 gridRead")
KANS_SHADER_TEXT(Vfx_std430_buffer_GridWrite, "Vfx/std430_buffer_GridWrite", ",std430) buffer GridWrite")
KANS_SHADER_TEXT(Vfx_vec4_gridWrite, "Vfx/vec4_gridWrite", "{vec4 gridWrite")
KANS_SHADER_TEXT(Vfx_std430_buffer_GridAtomic, "Vfx/std430_buffer_GridAtomic", ",std430) buffer GridAtomic")
KANS_SHADER_TEXT(Vfx_int_gridAtomic, "Vfx/int_gridAtomic", "{int gridAtomic")
KANS_SHADER_TEXT(Vfx_int_gridOffset, "Vfx/int_gridOffset", "int gridOffset")
KANS_SHADER_TEXT(Vfx_int_a_ivec3_p, "Vfx/int_a_ivec3_p_p", "(int a,ivec3 p){p=clamp(p,ivec3(0),")
KANS_SHADER_TEXT(Vfx_return_a, "Vfx/return_a", "-1);return a*")
KANS_SHADER_TEXT(Vfx_p_x, "Vfx/p_x", "+p.x+")
KANS_SHADER_TEXT(Vfx_p_y, "Vfx/p_y", "*(p.y+")
KANS_SHADER_TEXT(Vfx_p_z, "Vfx/p_z", "*p.z); }\n")
KANS_SHADER_TEXT(Vfx_vec4_gridSample, "Vfx/vec4_gridSample", "vec4 gridSample")
KANS_SHADER_TEXT(Vfx_int_a_vec3_p,
                 "Vfx/int_a_vec3_p_ivec3",
                 "(int a,vec3 p){ivec3 lo=ivec3(floor(p));vec3 f=fract(p);vec4 v=vec4(0);for(int "
                 "z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x)v+=gridRead")
KANS_SHADER_TEXT(Vfx_gridOffset, "Vfx/gridOffset", "[gridOffset")
KANS_SHADER_TEXT(Vfx_a_lo_ivec3_x,
                 "Vfx/a_lo_ivec3_x_y",
                 "(a,lo+ivec3(x,y,z))]*(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y)*(z==0?1-f.z:f.z);return v;}\n")
KANS_SHADER_TEXT(Vfx_std430_readonly_buffer_ReaderAttributes,
                 "Vfx/std430_readonly_buffer_ReaderAttributes",
                 ",std430) readonly buffer ReaderAttributes")
KANS_SHADER_TEXT(Vfx_uvec4_readerAttributes, "Vfx/uvec4_readerAttributes", "{uvec4 readerAttributes")
KANS_SHADER_TEXT(Vfx_std430_readonly_buffer_ReaderAlive,
                 "Vfx/std430_readonly_buffer_ReaderAlive",
                 ",std430) readonly buffer ReaderAlive")
KANS_SHADER_TEXT(Vfx_uint_readerAlive, "Vfx/uint_readerAlive", "{uint readerAlive")
KANS_SHADER_TEXT(Vfx_std430_readonly_buffer_ReaderCounters,
                 "Vfx/std430_readonly_buffer_ReaderCounters",
                 ",std430) readonly buffer ReaderCounters")
KANS_SHADER_TEXT(Vfx_uint_readerCounters, "Vfx/uint_readerCounters", "{uint readerCounters")
KANS_SHADER_TEXT(Vfx_std430_buffer_EmitterState_uvec4,
                 "Vfx/std430_buffer_EmitterState_uvec4_emitterState",
                 ",std430) buffer EmitterState{uvec4 emitterState[];};\n")
KANS_SHADER_TEXT(Vfx_std430, "Vfx/std430", ",std430) ")
KANS_SHADER_TEXT(Vfx_readonly, "Vfx/readonly", "readonly ")
KANS_SHADER_TEXT(Vfx_buffer_Events, "Vfx/buffer_Events", "buffer Events")
KANS_SHADER_TEXT(Vfx_uvec4_events, "Vfx/uvec4_events", "{uvec4 events")
KANS_SHADER_TEXT(Vfx_layout_local_size_x, "Vfx/layout_local_size_x_in", "layout(local_size_x=64) in;")
KANS_SHADER_TEXT(Vfx_layout_local_size_x2,
                 "Vfx/layout_local_size_x_local_size_y_local_size_z_in",
                 "layout(local_size_x=4,local_size_y=4,local_size_z=4) in;")
KANS_SHADER_TEXT(Vfx_ivec3_gridExecutionIndex_ivec3_gl,
                 "Vfx/ivec3_gridExecutionIndex_ivec3_gl_GlobalInvocationID_if",
                 "ivec3 gridExecutionIndex=ivec3(gl_GlobalInvocationID);if(any(greaterThanEqual(gridExecutionIndex,")
KANS_SHADER_TEXT(Vfx_return_slot_u_uniqueID, "Vfx/return_slot_u_uniqueID_u", ")))return;slot=0u;uniqueID=0u;\n")
KANS_SHADER_TEXT(Vfx_gridWrite, "Vfx/gridWrite", "gridWrite")
KANS_SHADER_TEXT(Vfx_gridExecutionIndex_gridRead, "Vfx/gridExecutionIndex_gridRead", ",gridExecutionIndex)]=gridRead")
KANS_SHADER_TEXT(Vfx_gridExecutionIndex, "Vfx/gridExecutionIndex", ",gridExecutionIndex)];\n")
KANS_SHADER_TEXT(Vfx_if_invocation_u_return,
                 "Vfx/if_invocation_u_return_slot",
                 "if(invocation!=0u)return;slot=0u;uniqueID=0u;\n")
KANS_SHADER_TEXT(Vfx_if_invocation_counters_return,
                 "Vfx/if_invocation_counters_return_slot",
                 "if(invocation>=counters[2])return;slot=freeList[counters[4]+invocation];uniqueID=counters[5]+"
                 "invocation;flags[slot]=1u;\n")
KANS_SHADER_TEXT(Vfx_if_invocation_counters_return2,
                 "Vfx/if_invocation_counters_return_slot_2",
                 "if(invocation>=counters[1])return;slot=aliveList[invocation];uniqueID=uint(")
KANS_SHADER_TEXT(Vfx_attributes, "Vfx/attributes", "attributes[")
KANS_SHADER_TEXT(Vfx_u_pc_capacity_slot, "Vfx/u_pc_capacity_slot", "u*pc.capacity+slot]")
KANS_SHADER_TEXT(Vfx_mat4_owner_mat4, "Vfx/mat4_owner_mat4", "mat4 owner=mat4(")
KANS_SHADER_TEXT(Vfx_uintBitsToFloat_parameters, "Vfx/uintBitsToFloat_parameters", "uintBitsToFloat(parameters[")
KANS_SHADER_TEXT(Vfx_Punctuation_17, "Vfx/Punctuation_17", "])")
KANS_SHADER_TEXT(Vfx_nmat4_inverseOwner_inverse_owner,
                 "Vfx/nmat4_inverseOwner_inverse_owner",
                 ");\nmat4 inverseOwner=inverse(owner);\n")
KANS_SHADER_TEXT(Vfx_a, "Vfx/a", " a")
KANS_SHADER_TEXT(Vfx_if_pc_worldSpace_u, "Vfx/if_pc_worldSpace_u_a", "if(pc.worldSpace!=0u)a")
KANS_SHADER_TEXT(Vfx_owner_vec4_a, "Vfx/owner_vec4_a", "=(owner*vec4(a")
KANS_SHADER_TEXT(Vfx_xyz_3, "Vfx/xyz_3", ",1)).xyz;\n")
KANS_SHADER_TEXT(AttributePrefix, "Common/AttributePrefix", "a")
KANS_SHADER_TEXT(Vfx_int_uniqueID, "Vfx/int_uniqueID", "=int(uniqueID);\n")
KANS_SHADER_TEXT(Vfx_true_2, "Vfx/true_2", "=true;\n")
KANS_SHADER_TEXT(Vfx_uint_eventIndex_invocation, "Vfx/uint_eventIndex_invocation", "uint eventIndex=invocation/")
KANS_SHADER_TEXT(Vfx_u_2, "Vfx/u_2", "u;\n")
KANS_SHADER_TEXT(Vfx_uint_eventCount_min_events0,
                 "Vfx/uint_eventCount_min_events0_x",
                 "uint eventCount=min(events0[0].x,")
KANS_SHADER_TEXT(Vfx_u_for_uint_eventIndex,
                 "Vfx/u_for_uint_eventIndex_u",
                 "u);for(uint eventIndex=0u;eventIndex<eventCount;++eventIndex){\n")
KANS_SHADER_TEXT(Vfx_eventKey_events0_u_eventIndex,
                 "Vfx/eventKey_events0_u_eventIndex",
                 "eventKey=events0[1u+eventIndex*")
KANS_SHADER_TEXT(Vfx_u_3, "Vfx/u_3", "u+")
KANS_SHADER_TEXT(Vfx_u_x, "Vfx/u_x", "u].x;\n")
KANS_SHADER_TEXT(Vfx_parameters, "Vfx/parameters", "parameters[")
KANS_SHADER_TEXT(Vfx_Punctuation_19, "Vfx/Punctuation_19", "]")
KANS_SHADER_TEXT(Vfx_int_invocation, "Vfx/int_invocation", "int(invocation)")
KANS_SHADER_TEXT(Vfx_emitterState, "Vfx/emitterState", "emitterState[")
KANS_SHADER_TEXT(Vfx_events0_u_eventIndex, "Vfx/events0_u_eventIndex", "events0[1u+eventIndex*")
KANS_SHADER_TEXT(Vfx_u_4, "Vfx/u_4", "u]")
KANS_SHADER_TEXT(Vfx_pc_worldSpace_u_inverseOwner,
                 "Vfx/pc_worldSpace_u_inverseOwner_vec4",
                 "(pc.worldSpace==0u?(inverseOwner*vec4(")
KANS_SHADER_TEXT(Vfx_xyz_4, "Vfx/xyz_4", ",1)).xyz:")
KANS_SHADER_TEXT(Vfx_pc_worldSpace_u_owner, "Vfx/pc_worldSpace_u_owner_vec4", "(pc.worldSpace!=0u?(owner*vec4(")
KANS_SHADER_TEXT(Vfx_p, "Vfx/p", " p")
KANS_SHADER_TEXT(RegisterPrefix, "Common/RegisterPrefix", "r")
KANS_SHADER_TEXT(Vfx_Punctuation_20, "Vfx/Punctuation_20", "0.0")
KANS_SHADER_TEXT(Vfx_gridExecutionIndex_2, "Vfx/gridExecutionIndex_2", "gridExecutionIndex")
KANS_SHADER_TEXT(Vfx_xyz_5, "Vfx/xyz_5", ",1)).xyz:(")
KANS_SHADER_TEXT(Vfx_owner_xyz, "Vfx/owner_xyz", "-owner[3].xyz))")
KANS_SHADER_TEXT(Vfx_pc_worldSpace_u_owner2, "Vfx/pc_worldSpace_u_owner_vec4_2", "(pc.worldSpace==0u?(owner*vec4(")
KANS_SHADER_TEXT(Vfx_owner_xyz_2, "Vfx/owner_xyz_2", "+owner[3].xyz))")
KANS_SHADER_TEXT(Vfx_Punctuation_21, "Vfx/Punctuation_21", "((")
KANS_SHADER_TEXT(Divide, "Common/Divide", "/")
KANS_SHADER_TEXT(Vfx_vec3_2, "Vfx/vec3_2", "+0.5)*vec3(")
KANS_SHADER_TEXT(Vfx_gridOffset_2, "Vfx/gridOffset_2", "gridOffset")
KANS_SHADER_TEXT(Vfx_vec3_3, "Vfx/vec3_3", "/vec3(")
KANS_SHADER_TEXT(Vfx_vec3_4, "Vfx/vec3_4", "((vec3(")
KANS_SHADER_TEXT(Vfx_vec3_5, "Vfx/vec3_5", ")+0.5)/vec3(")
KANS_SHADER_TEXT(Vfx_Punctuation_23, "Vfx/Punctuation_23", ")-0.5)*")
KANS_SHADER_TEXT(Vfx_Punctuation_24, "Vfx/Punctuation_24", "-0.5")
KANS_SHADER_TEXT(Vfx_ivec3_floor, "Vfx/ivec3_floor", "ivec3(floor(")
KANS_SHADER_TEXT(Vfx_gridRead, "Vfx/gridRead", "gridRead")
KANS_SHADER_TEXT(Vfx_Punctuation_25, "Vfx/Punctuation_25", "[")
KANS_SHADER_TEXT(Vfx_gridSample, "Vfx/gridSample", "gridSample")
KANS_SHADER_TEXT(Vfx_Punctuation_26, "Vfx/Punctuation_26", "-0.5)")
KANS_SHADER_TEXT(Vfx_cell, "Vfx/cell", "cell")
KANS_SHADER_TEXT(Vfx_tau, "Vfx/tau", "tau")
KANS_SHADER_TEXT(Vfx_step, "Vfx/step", "step")
KANS_SHADER_TEXT(Vfx_vec3_6, "Vfx/vec3_6", "vec3 ")
KANS_SHADER_TEXT(Vfx_vec3_cs, "Vfx/vec3_cs", "vec3 cs")
KANS_SHADER_TEXT(Vfx_if_pc_worldSpace_u2, "Vfx/if_pc_worldSpace_u_cs", "if(pc.worldSpace==0u)cs")
KANS_SHADER_TEXT(Vfx_length_owner, "Vfx/length_owner", "]*=length(owner[")
KANS_SHADER_TEXT(Vfx_xyz_6, "Vfx/xyz_6", "].xyz);\n")
KANS_SHADER_TEXT(Vfx_cs, "Vfx/cs", "cs")
KANS_SHADER_TEXT(Vfx_float, "Vfx/float", "float ")
KANS_SHADER_TEXT(Vfx_Punctuation_27, "Vfx/Punctuation_27", ">0.0?")
KANS_SHADER_TEXT(Vfx_min, "Vfx/min", ":min(")
KANS_SHADER_TEXT(Vfx_x_min, "Vfx/x_min", ".x,min(")
KANS_SHADER_TEXT(Vfx_y, "Vfx/y", ".y,")
KANS_SHADER_TEXT(Vfx_z, "Vfx/z", ".z));\n")
KANS_SHADER_TEXT(Vfx_if, "Vfx/if", "=0.0;if(")
KANS_SHADER_TEXT(Vfx_w_for_int_stepIndex,
                 "Vfx/w_for_int_stepIndex_stepIndex",
                 ".w!=0.0){for(int stepIndex=1;stepIndex<=clamp(")
KANS_SHADER_TEXT(Vfx_stepIndex_vec3_samplePos, "Vfx/stepIndex_vec3_samplePos", ",1,64);++stepIndex){vec3 samplePos=")
KANS_SHADER_TEXT(Add, "Common/Add", "+")
KANS_SHADER_TEXT(Vfx_xyz_float_stepIndex, "Vfx/xyz_float_stepIndex", ".xyz*(float(stepIndex)*")
KANS_SHADER_TEXT(Vfx_vec3_q, "Vfx/vec3_q", ");vec3 q=")
KANS_SHADER_TEXT(Vfx_samplePos, "Vfx/samplePos", "samplePos")
KANS_SHADER_TEXT(Vfx_if_any_lessThan_q,
                 "Vfx/if_any_lessThan_q_vec3",
                 ";if(any(lessThan(q,vec3(0)))||any(greaterThanEqual(q,vec3(")
KANS_SHADER_TEXT(Vfx_break, "Vfx/break", "))))break;")
KANS_SHADER_TEXT(Vfx_gridSample_2, "Vfx/gridSample_2", "+=gridSample")
KANS_SHADER_TEXT(Vfx_q_x_max, "Vfx/q_x_max", ",q-0.5).x*max(0.0,")
KANS_SHADER_TEXT(Vfx_Punctuation_29, "Vfx/Punctuation_29", ")*")
KANS_SHADER_TEXT(Vfx_Punctuation_30, "Vfx/Punctuation_30", ";}}\n")
KANS_SHADER_TEXT(Vfx_exp, "Vfx/exp", "exp(-")
KANS_SHADER_TEXT(Vfx_atomicAdd_gridAtomic, "Vfx/atomicAdd_gridAtomic", "atomicAdd(gridAtomic")
KANS_SHADER_TEXT(Vfx_Punctuation_31, "Vfx/Punctuation_31", "[4*(")
KANS_SHADER_TEXT(Vfx_Punctuation_32, "Vfx/Punctuation_32", ")+")
KANS_SHADER_TEXT(Vfx_int_round, "Vfx/int_round", "],int(round(")
KANS_SHADER_TEXT(Vfx_Punctuation_33, "Vfx/Punctuation_33", "*65536.0)));\n")
KANS_SHADER_TEXT(ParameterPrefix, "Common/ParameterPrefix", "p")
KANS_SHADER_TEXT(Vfx_mat3_owner_p, "Vfx/mat3_owner_p", "=mat3(owner)*p")
KANS_SHADER_TEXT(Vfx_colliderVelocityV, "Vfx/colliderVelocityV", "colliderVelocityV(")
KANS_SHADER_TEXT(Vfx_parameters_2, "Vfx/parameters_2", ",parameters[")
KANS_SHADER_TEXT(Vfx_z_2, "Vfx/z_2", "].z)")
KANS_SHADER_TEXT(Vfx_collision, "Vfx/collision", "collision")
KANS_SHADER_TEXT(Vfx_mat4, "Vfx/mat4", "mat4(")
KANS_SHADER_TEXT(Vfx_CollisionResultV, "Vfx/CollisionResultV", "CollisionResultV ")
KANS_SHADER_TEXT(Vfx_collideParticleV, "Vfx/collideParticleV", "=collideParticleV(")
KANS_SHADER_TEXT(Vfx_owner, "Vfx/owner", ",owner,")
KANS_SHADER_TEXT(Vfx_z_3, "Vfx/z_3", "].z,")
KANS_SHADER_TEXT(Vfx_x_u_2, "Vfx/x_u_2", "].x!=0u);\n")
KANS_SHADER_TEXT(Dot, "Common/Dot", ".")
KANS_SHADER_TEXT(Vfx_events, "Vfx/events", "events")
KANS_SHADER_TEXT(UnsignedSuffix, "Common/UnsignedSuffix", "u")
KANS_SHADER_TEXT(Vfx_uint_max, "Vfx/uint_max", "uint(max(0,")
KANS_SHADER_TEXT(Vfx_xffffffffu, "Vfx/xffffffffu", "0xffffffffu")
KANS_SHADER_TEXT(Vfx_uint_wanted, "Vfx/uint_wanted", "uint wanted")
KANS_SHADER_TEXT(Vfx_uint_max_2, "Vfx/uint_max_2", "=uint(max(0,")
KANS_SHADER_TEXT(Vfx_uint_generated, "Vfx/uint_generated", "));uint generated")
KANS_SHADER_TEXT(Vfx_z_uint_allowed, "Vfx/z_uint_allowed", "[0].z;uint allowed")
KANS_SHADER_TEXT(Vfx_u_6, "Vfx/u_6", "=0u;\n")
KANS_SHADER_TEXT(Vfx_while_generated, "Vfx/while_generated", "while(generated")
KANS_SHADER_TEXT(Vfx_Punctuation_35, "Vfx/Punctuation_35", "<")
KANS_SHADER_TEXT(Vfx_uint_candidate_min_wanted, "Vfx/uint_candidate_min_wanted", "){uint candidate=min(wanted")
KANS_SHADER_TEXT(Vfx_generated, "Vfx/generated", "-generated")
KANS_SHADER_TEXT(Vfx_uint_previous_atomicCompSwap,
                 "Vfx/uint_previous_atomicCompSwap",
                 ");uint previous=atomicCompSwap(")
KANS_SHADER_TEXT(Vfx_z_generated, "Vfx/z_generated", "[0].z,generated")
KANS_SHADER_TEXT(Vfx_generated_2, "Vfx/generated_2", ",generated")
KANS_SHADER_TEXT(Vfx_candidate_if_previous_generated,
                 "Vfx/candidate_if_previous_generated",
                 "+candidate);if(previous==generated")
KANS_SHADER_TEXT(Vfx_allowed, "Vfx/allowed", "){allowed")
KANS_SHADER_TEXT(Vfx_candidate_break_generated, "Vfx/candidate_break_generated", "=candidate;break;}generated")
KANS_SHADER_TEXT(Vfx_previous, "Vfx/previous", "=previous;}\n")
KANS_SHADER_TEXT(Vfx_uint_attempts, "Vfx/uint_attempts", "uint attempts")
KANS_SHADER_TEXT(Vfx_min_allowed, "Vfx/min_allowed", "=min(allowed")
KANS_SHADER_TEXT(Vfx_atomicAdd, "Vfx/atomicAdd", ");atomicAdd(")
KANS_SHADER_TEXT(Vfx_y_allowed, "Vfx/y_allowed", "[0].y,allowed")
KANS_SHADER_TEXT(Vfx_attempts, "Vfx/attempts", "-attempts")
KANS_SHADER_TEXT(Vfx_for_uint_ordinal_u, "Vfx/for_uint_ordinal_u_ordinal", "for(uint ordinal=0u;ordinal<attempts")
KANS_SHADER_TEXT(Vfx_ordinal_uint_at, "Vfx/ordinal_uint_at", ";++ordinal){uint at=")
KANS_SHADER_TEXT(Vfx_x_bool_reserved_false, "Vfx/x_bool_reserved_false_while", "[0].x;bool reserved=false;while(at<")
KANS_SHADER_TEXT(Vfx_uint_previous_atomicCompSwap_2,
                 "Vfx/uint_previous_atomicCompSwap_2",
                 "){uint previous=atomicCompSwap(")
KANS_SHADER_TEXT(Vfx_x_at_at_u,
                 "Vfx/x_at_at_u_if",
                 "[0].x,at,at+1u);if(previous==at){reserved=true;break;}at=previous;}if(!reserved){atomicAdd(")
KANS_SHADER_TEXT(Vfx_y_u_continue, "Vfx/y_u_continue", "[0].y,1u);continue;}\n")
KANS_SHADER_TEXT(Vfx_u_at, "Vfx/u_at", "[1u+at*")
KANS_SHADER_TEXT(Vfx_u_7, "Vfx/u_7", "u]=")
KANS_SHADER_TEXT(Vfx_u_uvec4_hashV_uniqueID,
                 "Vfx/u_uvec4_hashV_uniqueID_hashV",
                 "u]=uvec4(hashV(uniqueID^hashV(ordinal^")
KANS_SHADER_TEXT(Vfx_u_8, "Vfx/u_8", "u)),0,0,0); }\n")
KANS_SHADER_TEXT(Vfx_Punctuation_36, "Vfx/Punctuation_36", "0")
KANS_SHADER_TEXT(Vfx_readerCounters, "Vfx/readerCounters", "readerCounters")
KANS_SHADER_TEXT(Vfx_Punctuation_37, "Vfx/Punctuation_37", "[1]")
KANS_SHADER_TEXT(Vfx_found, "Vfx/found", "found")
KANS_SHADER_TEXT(Vfx_sourceSlot, "Vfx/sourceSlot", "sourceSlot")
KANS_SHADER_TEXT(Vfx_bool, "Vfx/bool", "bool ")
KANS_SHADER_TEXT(Vfx_false_uint, "Vfx/false_uint", "=false;uint ")
KANS_SHADER_TEXT(Vfx_if_2, "Vfx/if_2", "if(")
KANS_SHADER_TEXT(Vfx_uint, "Vfx/uint", ">=0&&uint(")
KANS_SHADER_TEXT(Vfx_Punctuation_38, "Vfx/Punctuation_38", ")<")
KANS_SHADER_TEXT(Vfx_Punctuation_39, "Vfx/Punctuation_39", "){")
KANS_SHADER_TEXT(Vfx_true_3, "Vfx/true_3", "=true;")
KANS_SHADER_TEXT(Vfx_readerAlive, "Vfx/readerAlive", "=readerAlive")
KANS_SHADER_TEXT(Vfx_uint_2, "Vfx/uint_2", "[uint(")
KANS_SHADER_TEXT(Vfx_Punctuation_40, "Vfx/Punctuation_40", ")];}\n")
KANS_SHADER_TEXT(Vfx_for_uint_j_u, "Vfx/for_uint_j_u_j", "for(uint j=0u;j<")
KANS_SHADER_TEXT(Vfx_j_uint_candidate_readerAlive,
                 "Vfx/j_uint_candidate_readerAlive",
                 ";++j){uint candidate=readerAlive")
KANS_SHADER_TEXT(Vfx_j_if_readerAttributes, "Vfx/j_if_readerAttributes", "[j];if(readerAttributes")
KANS_SHADER_TEXT(Vfx_u_9, "Vfx/u_9", "u*")
KANS_SHADER_TEXT(Vfx_u_candidate_x_uint, "Vfx/u_candidate_x_uint", "u+candidate].x==uint(")
KANS_SHADER_TEXT(Vfx_Punctuation_41, "Vfx/Punctuation_41", ")){")
KANS_SHADER_TEXT(Vfx_candidate_break, "Vfx/candidate_break", "=candidate;break;}}\n")
KANS_SHADER_TEXT(Vfx_readerAttributes, "Vfx/readerAttributes", "readerAttributes")
KANS_SHADER_TEXT(Question, "Common/Question", "?")
KANS_SHADER_TEXT(Colon, "Common/Colon", ":")
KANS_SHADER_TEXT(Vfx_Punctuation_44, "Vfx/Punctuation_44", "(0))")
KANS_SHADER_TEXT(Vfx_Punctuation_45, "Vfx/Punctuation_45", "==0?0:")
KANS_SHADER_TEXT(Vfx_Punctuation_46, "Vfx/Punctuation_46", "%")
KANS_SHADER_TEXT(Vfx_owner_2, "Vfx/owner_2", "owner[")
KANS_SHADER_TEXT(Vfx_p_3, "Vfx/p_3", "(p")
KANS_SHADER_TEXT(Subtract, "Common/Subtract", "-")
KANS_SHADER_TEXT(Multiply, "Common/Multiply", "*")
KANS_SHADER_TEXT(Vfx_Punctuation_49, "Vfx/Punctuation_49", "==")
KANS_SHADER_TEXT(Vfx_xyzw, "Vfx/xyzw", "xyzw")
KANS_SHADER_TEXT(Vfx_Punctuation_50, "Vfx/Punctuation_50", "==0.0?0.0:")
KANS_SHADER_TEXT(Vfx_Punctuation_51, "Vfx/Punctuation_51", "<=")
KANS_SHADER_TEXT(Vfx_Punctuation_52, "Vfx/Punctuation_52", ">")
KANS_SHADER_TEXT(Vfx_Punctuation_53, "Vfx/Punctuation_53", "&&")
KANS_SHADER_TEXT(Vfx_Punctuation_54, "Vfx/Punctuation_54", "||")
KANS_SHADER_TEXT(Vfx_Punctuation_55, "Vfx/Punctuation_55", "(!")
KANS_SHADER_TEXT(Vfx_isnan, "Vfx/isnan", "(isnan(")
KANS_SHADER_TEXT(Vfx_Punctuation_56, "Vfx/Punctuation_56", ")?0:")
KANS_SHADER_TEXT(Vfx_Punctuation_57, "Vfx/Punctuation_57", ">=2147483648.0?2147483647:")
KANS_SHADER_TEXT(Vfx_int_floor, "Vfx/int_floor", "<=-2147483648.0?(-2147483647-1):int(floor(")
KANS_SHADER_TEXT(Vfx_Punctuation_58, "Vfx/Punctuation_58", ")))")
KANS_SHADER_TEXT(Vfx_float_2, "Vfx/float_2", "float(")
KANS_SHADER_TEXT(Vfx_y_2, "Vfx/y_2", "y")
KANS_SHADER_TEXT(Vfx_z_4, "Vfx/z_4", "z")
KANS_SHADER_TEXT(Vfx_w, "Vfx/w", "w")
KANS_SHADER_TEXT(Vfx_x_3, "Vfx/x_3", "x")
KANS_SHADER_TEXT(Vfx_clamp, "Vfx/clamp", "clamp(")
KANS_SHADER_TEXT(Vfx_Punctuation_59, "Vfx/Punctuation_59", "(0),")
KANS_SHADER_TEXT(Vfx_Punctuation_60, "Vfx/Punctuation_60", "(1))")
KANS_SHADER_TEXT(Vfx_dot, "Vfx/dot", "(dot(")
KANS_SHADER_TEXT(Vfx_e_normalize, "Vfx/e_normalize", ")>1e-20?normalize(")
KANS_SHADER_TEXT(Vfx_Punctuation_61, "Vfx/Punctuation_61", "):")
KANS_SHADER_TEXT(Vfx_randomV, "Vfx/randomV", "randomV(")
KANS_SHADER_TEXT(Vfx_invocation, "Vfx/invocation", "(invocation%")
KANS_SHADER_TEXT(Vfx_u_10, "Vfx/u_10", "u)")
KANS_SHADER_TEXT(Vfx_uniqueID, "Vfx/uniqueID", "uniqueID")
KANS_SHADER_TEXT(Vfx_u_11, "Vfx/u_11", "u,")
KANS_SHADER_TEXT(Vfx_Punctuation_62, "Vfx/Punctuation_62", "*6.28318530718")
KANS_SHADER_TEXT(Vfx_Punctuation_63, "Vfx/Punctuation_63", "*2.0-1.0)")
KANS_SHADER_TEXT(Vfx_sqrt_max, "Vfx/sqrt_max", "sqrt(max(0.0,1.0-")
KANS_SHADER_TEXT(Vfx_vec3_7, "Vfx/vec3_7", "vec3(")
KANS_SHADER_TEXT(Vfx_cos, "Vfx/cos", "*cos(")
KANS_SHADER_TEXT(Vfx_Punctuation_64, "Vfx/Punctuation_64", "),")
KANS_SHADER_TEXT(Vfx_sin, "Vfx/sin", "*sin(")
KANS_SHADER_TEXT(Vfx_Punctuation_65, "Vfx/Punctuation_65", "+(")
KANS_SHADER_TEXT(Vfx_pc_worldSpace_u, "Vfx/pc_worldSpace_u", "(pc.worldSpace!=0u?")
KANS_SHADER_TEXT(Vfx_inverse_mat3_owner, "Vfx/inverse_mat3_owner", ":inverse(mat3(owner))*")
KANS_SHADER_TEXT(Vfx_mat3_owner, "Vfx/mat3_owner", ":mat3(owner)*")
KANS_SHADER_TEXT(Vfx_seedOffsetV, "Vfx/seedOffsetV", "seedOffsetV()")
KANS_SHADER_TEXT(Vfx_fieldPosition, "Vfx/fieldPosition", "fieldPosition(")
KANS_SHADER_TEXT(Vfx_fieldRotate, "Vfx/fieldRotate", "fieldRotate(")
KANS_SHADER_TEXT(Vfx_sampleVectorField, "Vfx/sampleVectorField", "sampleVectorField(")
KANS_SHADER_TEXT(Vfx_bvec3, "Vfx/bvec3", ",bvec3(")
KANS_SHADER_TEXT(Vfx_Punctuation_66, "Vfx/Punctuation_66", "))*")
KANS_SHADER_TEXT(Vfx_meshStatic, "Vfx/meshStatic", "meshStatic(")
KANS_SHADER_TEXT(Vfx_Punctuation_67, "Vfx/Punctuation_67", "1")
KANS_SHADER_TEXT(Vfx_xyz_7, "Vfx/xyz_7", ").xyz")
KANS_SHADER_TEXT(Vfx_int_meshStatic, "Vfx/int_meshStatic", "int(meshStatic(")
KANS_SHADER_TEXT(Vfx_Punctuation_68, "Vfx/Punctuation_68", ",2).")
KANS_SHADER_TEXT(Vfx_curlV, "Vfx/curlV", "curlV(")
KANS_SHADER_TEXT(Vfx_owner_vec4, "Vfx/owner_vec4", "(owner*vec4(")
KANS_SHADER_TEXT(Vfx_xyz_8, "Vfx/xyz_8", ",1)).xyz")
KANS_SHADER_TEXT(Vfx_mat3_owner_2, "Vfx/mat3_owner_2", "(mat3(owner)*")
KANS_SHADER_TEXT(Vfx_sampleFieldsV, "Vfx/sampleFieldsV", "sampleFieldsV(")
KANS_SHADER_TEXT(Vfx_parameters_3, "Vfx/parameters_3", "],parameters[")
KANS_SHADER_TEXT(Vfx_Punctuation_69, "Vfx/Punctuation_69", "] )")
KANS_SHADER_TEXT(Vfx_inverse_mat3_owner_2, "Vfx/inverse_mat3_owner_2", "(inverse(mat3(owner))*")
KANS_SHADER_TEXT(Vfx_noiseV, "Vfx/noiseV", "noiseV(")
KANS_SHADER_TEXT(Vfx_inverseOwner, "Vfx/inverseOwner", "inverseOwner")
KANS_SHADER_TEXT(Vfx_owner_3, "Vfx/owner_3", "owner")
KANS_SHADER_TEXT(Vfx_vec4_2, "Vfx/vec4_2", "*vec4(")
KANS_SHADER_TEXT(Vfx_xyz_9, "Vfx/xyz_9", ")).xyz")
KANS_SHADER_TEXT(Vfx_custom, "Vfx/custom", "custom")
KANS_SHADER_TEXT(Vfx_Punctuation_70, "Vfx/Punctuation_70", "){\n")
KANS_SHADER_TEXT(Vfx_nreturn, "Vfx/nreturn", "\nreturn ")
KANS_SHADER_TEXT(Vfx_Punctuation_71, "Vfx/Punctuation_71", ";\n}\n")
KANS_SHADER_TEXT(Vfx_int_diValues, "Vfx/int_diValues", "int(diValues[")
KANS_SHADER_TEXT(Vfx_x_4, "Vfx/x_4", "].x)")
KANS_SHADER_TEXT(Vfx_invocation_2, "Vfx/invocation_2", "invocation%")
KANS_SHADER_TEXT(Vfx_meshRandom, "Vfx/meshRandom", "meshRandom(")
KANS_SHADER_TEXT(Vfx_Punctuation_72, "Vfx/Punctuation_72", "2")
KANS_SHADER_TEXT(Vfx_randomV_2, "Vfx/randomV_2", ",randomV(")
KANS_SHADER_TEXT(Vfx_u_hashV, "Vfx/u_hashV", "u^hashV(")
KANS_SHADER_TEXT(Vfx_invocation_3, "Vfx/invocation_3", "invocation")
KANS_SHADER_TEXT(Vfx_u_12, "Vfx/u_12", "0u")
KANS_SHADER_TEXT(Vfx_x9e3779b9u_u, "Vfx/x9e3779b9u_u", "+0x9e3779b9u),0u))")
KANS_SHADER_TEXT(Vfx_diValues, "Vfx/diValues", "(diValues[")
KANS_SHADER_TEXT(Vfx_w_int_diValues, "Vfx/w_int_diValues", "+2].w==0?0:int(diValues[")
KANS_SHADER_TEXT(Vfx_w_2, "Vfx/w_2", "+3].w")
KANS_SHADER_TEXT(Vfx_w_3, "Vfx/w_3", "].w")
KANS_SHADER_TEXT(Vfx_y_3, "Vfx/y_3", "].y")
KANS_SHADER_TEXT(Vfx_meshSample, "Vfx/meshSample", "meshSample(")
KANS_SHADER_TEXT(Vfx_vec3_8, "Vfx/vec3_8", "vec3(1,0,0)")
KANS_SHADER_TEXT(Vfx_x_5, "Vfx/x_5", ".x!=0)")
KANS_SHADER_TEXT(Vfx_diValues_2, "Vfx/diValues_2", "diValues[")
KANS_SHADER_TEXT(Vfx_min_2, "Vfx/min_2", "min(")
KANS_SHADER_TEXT(Vfx_int_clamp, "Vfx/int_clamp", ",int(clamp(")
KANS_SHADER_TEXT(Vfx_y_4, "Vfx/y_4", ".y,0.0,1.0)*")
KANS_SHADER_TEXT(Vfx_min_3, "Vfx/min_3", "+min(")
KANS_SHADER_TEXT(Vfx_x_6, "Vfx/x_6", ".x,0.0,1.0)*")
KANS_SHADER_TEXT(Vfx_mix, "Vfx/mix", "?mix(")
KANS_SHADER_TEXT(Vfx_clamp_2, "Vfx/clamp_2", ",clamp((")
KANS_SHADER_TEXT(Vfx_Punctuation_73, "Vfx/Punctuation_73", ")/")
KANS_SHADER_TEXT(Vfx_Punctuation_74, "Vfx/Punctuation_74", ",0.0,1.0)):")
KANS_SHADER_TEXT(Vfx_mix_2, "Vfx/mix_2", "mix")
KANS_SHADER_TEXT(Vfx_fract, "Vfx/fract", "fract")
KANS_SHADER_TEXT(Vfx_r_2, "Vfx/r_2", " r")
KANS_SHADER_TEXT(Vfx_p_4, "Vfx/p_4", "=p")
KANS_SHADER_TEXT(Vfx_if_a, "Vfx/if_a", "if(!a")
KANS_SHADER_TEXT(Vfx_break_2, "Vfx/break_2", ")break;}\n")
KANS_SHADER_TEXT(Vfx_Punctuation_75, "Vfx/Punctuation_75", "]=")
KANS_SHADER_TEXT(Vfx_counters_uint_max_p, "Vfx/counters_uint_max_p", "counters[15]=uint(max(0,p")
KANS_SHADER_TEXT(Vfx_Punctuation_76, "Vfx/Punctuation_76", "));\n")
KANS_SHADER_TEXT(Vfx_return, "Vfx/return", "return;\n")
KANS_SHADER_TEXT(Vfx_mat3_owner_a, "Vfx/mat3_owner_a", "=mat3(owner)*a")
KANS_SHADER_TEXT(Vfx_if_pc_kill_u, "Vfx/if_pc_kill_u_a", "if(pc.kill!=0u)a")
KANS_SHADER_TEXT(Vfx_false_2, "Vfx/false_2", "=false;\n")
KANS_SHADER_TEXT(Vfx_u_pc_capacity_slot2, "Vfx/u_pc_capacity_slot_2", "u*pc.capacity+slot]=")
KANS_SHADER_TEXT(Vfx_if_3, "Vfx/if_3", "if(!")
KANS_SHADER_TEXT(Vfx_flags_slot_u_uint,
                 "Vfx/flags_slot_u_uint_at",
                 "){flags[slot]=0u;uint at=atomicAdd(counters[0],1u);freeList[at]=slot;}\n}\n")
