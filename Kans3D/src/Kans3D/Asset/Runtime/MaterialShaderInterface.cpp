#include "kspch.h"
#include "MaterialShaderInterface.h"
#include <map>
#include <set>

namespace Kans
{
    namespace
    {
        const std::map<std::string, std::string>& EngineIncludes()
        {
            static const std::map<std::string, std::string> includes = {
                {
                    "Kans3D/MSDF.glsl",
#include "KansMSDFInclude.h"
                },
                {"Kans3D/Engine.glsl", R"(#ifndef KANS3D_ENGINE_GLSL
#define KANS3D_ENGINE_GLSL
#define KANS_LIGHT_COUNT 4
#define KANS_POINT_LIGHT_LIMIT 16
// Engine data (descriptor set 0). Lighting positions and directions are in world space.
layout(set = 0, binding = 0, std140) uniform KansFrame
{
    mat4  ViewProjection;
    vec4  CameraPosition;
    vec4  LightPositions[KANS_LIGHT_COUNT];
    vec4  LightColors[KANS_LIGHT_COUNT];  // PBR radiance
    vec4  LightDiffuse[KANS_LIGHT_COUNT]; // diffuse intensity for non-PBR shading
    float Time;
    float DebugView;
    vec2 FramePadding;
    vec4 PointPositions[KANS_POINT_LIGHT_LIMIT];
    vec4 PointRadiance[KANS_POINT_LIGHT_LIMIT];
    vec4 PointSpecular[KANS_POINT_LIGHT_LIMIT];
    vec4 DirectionalDirection;
    vec4 DirectionalRadiance;
    vec4 DirectionalSpecular;
    vec4 DirectionalAmbient;
    vec4 LightingInfo;
    mat4 CascadeViewProjection[4];
    vec4 CascadeSplits;
    vec4 CascadeBlendStarts;
    vec4 CascadeTexel[4];
    vec4 ShadowInfo;
    vec4 ShadowCameraDepth;
    vec4 PointShadowIndices[4];
    vec4 PointShadowPositionRange[4];
    vec4 PointShadowParams[4];
    vec4 ShadowPass;
    vec4 ShadowPassPosition;
    vec4 PointShadowSourceRadius;
    vec4 EnvironmentInfo; // diffuse, specular, cos(rotation), sin(rotation).
    mat4 View; // World to view: right-handed, -Z forward, +Y up.
    mat4 Projection; // View to device clip (same correction as ViewProjection).
    mat4 InverseView;
    mat4 InverseProjection;

} u_Frame;
int KansPointLightCount() { return clamp(int(u_Frame.LightingInfo.x), 0, KANS_POINT_LIGHT_LIMIT); }
float KansExposure() { return u_Frame.LightingInfo.y; }
vec3 KansPointLightPosition(int i) { return u_Frame.PointPositions[i].xyz; }
vec3 KansPointLightRadiance(int i) { return u_Frame.PointRadiance[i].rgb; }
vec3 KansPointLightSpecular(int i) { return u_Frame.PointSpecular[i].rgb; }
vec3 KansDirectionalToLight() { return -u_Frame.DirectionalDirection.xyz; }
vec3 KansDirectionalRadiance() { return u_Frame.DirectionalRadiance.rgb; }
vec3 KansDirectionalAmbient() { return u_Frame.DirectionalAmbient.rgb; }

layout(set = 0, binding = 1, std140) uniform KansObject
{
    mat4 Transform;
    mat4 NormalMatrix;
    vec4 MaterialRender; // x: alpha mode (0 opaque / 1 mask / 2 blend), y: cutoff, z: double-sided
} u_Object;
layout(set = 0, binding = 2) uniform samplerCube u_IrradianceMap;
layout(set = 0, binding = 3) uniform samplerCube u_PrefilterMap;
layout(set = 0, binding = 4) uniform sampler2D   u_BrdfLUT;

#endif
)"},
                {"Kans3D/PostProcess.glsl", R"(#ifndef KANS3D_POST_PROCESS_GLSL
#define KANS3D_POST_PROCESS_GLSL
#ifndef KANS_FRAGMENT_STAGE
#error PostProcess.glsl is fragment only
#endif
#include <Kans3D/Frame.glsl>
// Set 0: shared KansFrame=0, linear-clamp colour=11, point-clamp device depth=12,
// custom device depth=13, unsigned custom stencil=14. Set 1 is material-owned.
layout(set=0, binding=11) uniform sampler2D kans_PostColor;
layout(set=0, binding=12) uniform sampler2D kans_PostDepth;
layout(set=0, binding=13) uniform sampler2D kans_CustomDepth;
layout(set=0, binding=14) uniform usampler2D kans_CustomStencil;
vec2 KansViewportSize() { return vec2(textureSize(kans_PostColor, 0)); }
vec2 KansTexelSize() { return 1.0 / KansViewportSize(); }
vec2 KansPostUV() { return gl_FragCoord.xy * KansTexelSize(); }
vec4 KansSceneColor(vec2 uv) { return texture(kans_PostColor, uv); }
float KansSceneDepth(vec2 uv) { return texture(kans_PostDepth, uv).r; }
vec3 KansViewPosition(vec2 uv)
{
    vec4 p = u_Frame.InverseProjection * vec4(uv * 2.0 - 1.0, KansSceneDepth(uv), 1.0);
    return p.xyz / p.w;
}
float KansLinearDepth(vec2 uv) { return -KansViewPosition(uv).z; }
vec3 KansWorldPosition(vec2 uv) { return (u_Frame.InverseView * vec4(KansViewPosition(uv), 1)).xyz; }
vec3 KansSceneNormal(vec2 uv)
{
    // Geometric world normal reconstructed from depth; normal maps are not included.
    vec2 t = KansTexelSize();
    vec2 lo = t * 0.5, hi = vec2(1) - lo;
    vec3 p = KansViewPosition(uv);
    vec3 l = KansViewPosition(clamp(uv - vec2(t.x,0), lo, hi));
    vec3 r = KansViewPosition(clamp(uv + vec2(t.x,0), lo, hi));
    vec3 u = KansViewPosition(clamp(uv - vec2(0,t.y), lo, hi));
    vec3 d = KansViewPosition(clamp(uv + vec2(0,t.y), lo, hi));
    vec3 dx = abs(p.z-l.z) < abs(r.z-p.z) ? p-l : r-p;
    vec3 dy = abs(p.z-u.z) < abs(d.z-p.z) ? p-u : d-p;
    vec3 n = cross(dy, dx);
    if (dot(n,n) < 1e-20) n = vec3(0,0,1);
    if (dot(n,-p) < 0.0) n = -n;
    return normalize(transpose(mat3(u_Frame.View)) * normalize(n));
}
float KansCustomDepth(vec2 uv) { return texture(kans_CustomDepth, uv).r; }
uint KansCustomStencil(ivec2 pixel)
{
    return texelFetch(kans_CustomStencil, clamp(pixel, ivec2(0), textureSize(kans_CustomStencil,0)-1), 0).r;
}
#endif
)"},
                {"Kans3D/Alpha.glsl", R"(#ifndef KANS3D_ALPHA_GLSL
#define KANS3D_ALPHA_GLSL
#include <Kans3D/Engine.glsl>
// Fragment-stage helpers. Call with the final base colour alpha, before any debug-view return.
void KansAlphaClip(float alpha)
{
    if (u_Object.MaterialRender.x == 1.0 && alpha < u_Object.MaterialRender.y)
        discard;
    if (u_Object.MaterialRender.x == 3.0)
    {
        // Fixed 4x4 Bayer pattern at pixel centres, independent of submission and time.
        const int bayer[16] = int[](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);
        ivec2 p = ivec2(floor(gl_FragCoord.xy)) & 3;
        if (alpha <= (float(bayer[p.y*4+p.x])+0.5)/16.0) discard;
    }
}
float KansOutputAlpha(float alpha)
{
    if (u_Object.MaterialRender.x == 4.0)
        return clamp((alpha-u_Object.MaterialRender.y)/max(fwidth(alpha), 1e-4)+0.5, 0.0, 1.0);
    return (u_Object.MaterialRender.x == 2.0 || u_Object.MaterialRender.x == 5.0 || u_Object.MaterialRender.x == 6.0 || u_Object.MaterialRender.x == 7.0) ? alpha : 1.0;
}
#endif
)"},
                {"Kans3D/ShadowCaster.glsl", R"(#ifndef KANS3D_SHADOW_CASTER_GLSL
#define KANS3D_SHADOW_CASTER_GLSL
#include <Kans3D/Alpha.glsl>
// ShadowCaster fragments call this after KansAlphaClip, including for point cubes.
void KansWriteShadowDepth(vec3 worldPos)
{
    if (u_Frame.ShadowPass.x == 2.0)
    {
        float depth = length(worldPos - u_Frame.ShadowPassPosition.xyz) / u_Frame.ShadowPassPosition.w;
        float slope = max(abs(dFdx(depth)), abs(dFdy(depth)));
        gl_FragDepth = clamp(depth + u_Frame.ShadowPass.y * 1e-6 + u_Frame.ShadowPass.z * slope, 0.0, 1.0);
    }
    else gl_FragDepth = gl_FragCoord.z;
}
#endif
)"},
                {"Kans3D/SmoothNormal.glsl", R"(#ifndef KANS3D_SMOOTH_NORMAL_GLSL
#define KANS3D_SMOOTH_NORMAL_GLSL
layout(location = 7) in vec3 a_SmoothNormal;
// Mesh-space normal, rotated by the engine skinning prologue; morphs leave it unchanged.
vec3 KansSmoothNormal() { return a_SmoothNormal; }
#endif
)"},
                {"Kans3D/VertexSource.glsl", R"(#ifndef KANS3D_VERTEX_SOURCE_GLSL
#define KANS3D_VERTEX_SOURCE_GLSL
// Generic values supplied by a procedural vertex source; defaults support ordinary meshes.
vec4 KansVertexColor = vec4(1);
vec4 KansVertexDynamicMaterialParameter = vec4(0);
vec3 KansVertexSubUV = vec3(0);
vec2 KansVertexUV1 = vec2(0);
#endif
)"},
                {"Kans3D/VertexInput.glsl", R"(#ifndef KANS3D_VERTEX_INPUT_GLSL
#define KANS3D_VERTEX_INPUT_GLSL
// Engine mesh vertex layout.
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV;
layout(location = 3) in vec4 a_Color;
layout(location = 4) in vec4 a_Tangent;
// A private copy makes this independent of the deformation prologue's position alias.
vec3 kans_RestPosition = a_Position;
vec3 KansRestPosition() { return kans_RestPosition; }
#endif
)"},
                {"Kans3D/ParticleLighting.glsl", R"(#ifndef KANS3D_PARTICLE_LIGHTING_GLSL
#define KANS3D_PARTICLE_LIGHTING_GLSL
// Shader programs have no shading-model enum. This opt-in extends their lit path;
// ordinary meshes keep PBR, and procedural particle sources select diffuse vertex evaluation.
#ifndef KANS_PARTICLE_PIXEL_ONLY
#ifdef KANS_FRAGMENT_STAGE
layout(location = 15) in vec3 kans_ParticleDiffuse;
#ifndef KANS_PARTICLE_DIFFUSE_ONLY
layout(location = 16) in vec3 kans_ParticleAmbient;
layout(location = 17) in vec3 kans_ParticlePositiveX;
layout(location = 18) in vec3 kans_ParticlePositiveY;
layout(location = 19) in vec3 kans_ParticlePositiveZ;
layout(location = 20) in vec3 kans_ParticleNegativeX;
layout(location = 21) in vec3 kans_ParticleNegativeY;
layout(location = 22) in vec3 kans_ParticleNegativeZ;
#endif
layout(location = 23) flat in float kans_ParticleLightingMode;
layout(location = 24) flat in float kans_DirectionalTransmittance;
#else
float KansParticleEvaluation = 0.0;
float KansParticleTransmittance = 1.0;
layout(location = 15) out vec3 kans_ParticleDiffuse;
#ifndef KANS_PARTICLE_DIFFUSE_ONLY
layout(location = 16) out vec3 kans_ParticleAmbient;
layout(location = 17) out vec3 kans_ParticlePositiveX;
layout(location = 18) out vec3 kans_ParticlePositiveY;
layout(location = 19) out vec3 kans_ParticlePositiveZ;
layout(location = 20) out vec3 kans_ParticleNegativeX;
layout(location = 21) out vec3 kans_ParticleNegativeY;
layout(location = 22) out vec3 kans_ParticleNegativeZ;
#endif
layout(location = 23) flat out float kans_ParticleLightingMode;
layout(location = 24) flat out float kans_DirectionalTransmittance;
#endif
#endif
#ifndef KANS_PARTICLE_PIXEL_ONLY
#ifdef KANS_FRAGMENT_STAGE
#define KANS_DIRECTIONAL_TRANSMITTANCE clamp(kans_DirectionalTransmittance, 0.0, 1.0)
#else
#define KANS_DIRECTIONAL_TRANSMITTANCE clamp(KansParticleTransmittance, 0.0, 1.0)
#endif
#endif
#include <Kans3D/Lighting.glsl>
mat3 KansParticleFrame(vec3 normal, vec3 tangent)
{
    vec3 N = normalize(normal);
    vec3 T = tangent - N * dot(N, tangent);
    if (dot(T, T) < 1e-10)
        T = cross(abs(N.y) < .99 ? vec3(0, 1, 0) : vec3(1, 0, 0), N);
    T = normalize(T);
    return mat3(T, cross(N, T), N);
}
void KansParticleAddLight(vec3 L, vec3 radiance, mat3 frame, inout vec3 diffuse, inout vec3 axes[6])
{
    diffuse += radiance * max(dot(frame[2], L), 0.0);
#ifndef KANS_PARTICLE_DIFFUSE_ONLY
    vec3 local = transpose(frame) * L;
    for (int i = 0; i < 3; ++i)
    {
        axes[i] += radiance * max(local[i], 0.0);
        axes[i + 3] += radiance * max(-local[i], 0.0);
    }
#endif
}
void KansEvaluateParticleLighting(vec3     worldPos,
                                  vec3     normal,
                                  vec3     tangent,
                                  out vec3 diffuse,
                                  out vec3 ambient,
                                  out vec3 axes[6])
{
    mat3 frame = KansParticleFrame(normal, tangent);
    ambient    = KansEnvironmentDiffuse(frame[2]) + KansDirectionalAmbient();
    diffuse    = ambient;
    for (int i = 0; i < 6; ++i)
        axes[i] = vec3(0);
    if (u_Frame.DirectionalDirection.w != 0.0)
        KansParticleAddLight(
            KansDirectionalToLight(), KansDirectionalRadiance() * KansShadow(worldPos, frame[2])
#ifdef KANS_DIRECTIONAL_TRANSMITTANCE
            * KANS_DIRECTIONAL_TRANSMITTANCE
#endif
            , frame, diffuse, axes);
    for (int i = 0; i < KansPointLightCount(); ++i)
    {
        vec3  delta = KansPointLightPosition(i) - worldPos;
        float d2    = dot(delta, delta);
        if (d2 <= 1e-8)
            continue;
        float shadow = 1.0;
#ifdef KANS_FRAGMENT_STAGE
        shadow = KansPointShadow(i, worldPos, frame[2]);
#endif
        // Vertex mode omits point PCF: each cube receiver requires 9 or 25 taps.
        KansParticleAddLight(
            delta * inversesqrt(d2), KansPointLightRadiance(i) * shadow / max(d2, 1e-4), frame, diffuse, axes);
    }
}
#if !defined(KANS_FRAGMENT_STAGE) && !defined(KANS_PARTICLE_PIXEL_ONLY)
void KansBuildParticleLighting(vec3 worldPos, vec3 normal, vec3 tangent)
{
    kans_ParticleLightingMode = KansParticleEvaluation;
    kans_DirectionalTransmittance = KansParticleTransmittance;
    kans_ParticleDiffuse      = vec3(0);
#ifndef KANS_PARTICLE_DIFFUSE_ONLY
    kans_ParticleAmbient      = vec3(0);
    kans_ParticlePositiveX = kans_ParticlePositiveY = kans_ParticlePositiveZ = kans_ParticleNegativeX =
        kans_ParticleNegativeY = kans_ParticleNegativeZ = vec3(0);
#endif
    // Sample directional shadows at each actual vertex, with the geometric normal
    // as the receiver plane. Interpolation smooths the result over the two triangles.
    if (KansParticleEvaluation > 0.5)
    {
        vec3 axes[6];
#ifdef KANS_PARTICLE_DIFFUSE_ONLY
        vec3 ambient;
        KansEvaluateParticleLighting(worldPos, normal, tangent, kans_ParticleDiffuse, ambient, axes);
#else
        KansEvaluateParticleLighting(worldPos, normal, tangent, kans_ParticleDiffuse, kans_ParticleAmbient, axes);
        kans_ParticlePositiveX = axes[0];
        kans_ParticlePositiveY = axes[1];
        kans_ParticlePositiveZ = axes[2];
        kans_ParticleNegativeX = axes[3];
        kans_ParticleNegativeY = axes[4];
        kans_ParticleNegativeZ = axes[5];
#endif
    }
}
#endif
#if defined(KANS_FRAGMENT_STAGE) && !defined(KANS_PARTICLE_DIFFUSE_ONLY)
vec3 KansSixWayLighting(vec4 positiveAxes,
                        vec4 negativeAxes,
                        vec3 absorptionTint,
                        vec3 normal,
                        vec3 tangent,
                        vec3 worldPos)
{
    vec3 diffuse, ambient, axes[6];
#ifndef KANS_PARTICLE_PIXEL_ONLY
    if (kans_ParticleLightingMode > 0.5)
    {
        ambient = kans_ParticleAmbient;
        axes[0] = kans_ParticlePositiveX;
        axes[1] = kans_ParticlePositiveY;
        axes[2] = kans_ParticlePositiveZ;
        axes[3] = kans_ParticleNegativeX;
        axes[4] = kans_ParticleNegativeY;
        axes[5] = kans_ParticleNegativeZ;
    }
    else
#endif
        KansEvaluateParticleLighting(worldPos, normal, tangent, diffuse, ambient, axes);
    vec3 result =
        ambient *
        (positiveAxes.r + positiveAxes.g + positiveAxes.b + negativeAxes.r + negativeAxes.g + negativeAxes.b) / 6.0;
    for (int i = 0; i < 3; ++i)
        result += axes[i] * positiveAxes[i] + axes[i + 3] * negativeAxes[i];
    return result * absorptionTint;
}
#endif
#endif
)"},
                {"Kans3D/Lighting.glsl", R"(#ifndef KANS3D_LIGHTING_GLSL
#define KANS3D_LIGHTING_GLSL
#include <Kans3D/Engine.glsl>
layout(set = 0, binding = 5) uniform sampler2DShadow u_DirectionalShadow;
layout(set = 0, binding = 6) uniform sampler2D u_DirectionalShadowRaw;
layout(set = 0, binding = 7) uniform samplerCubeShadow u_PointShadow0;
layout(set = 0, binding = 8) uniform samplerCubeShadow u_PointShadow1;
layout(set = 0, binding = 9) uniform samplerCubeShadow u_PointShadow2;
layout(set = 0, binding = 10) uniform samplerCubeShadow u_PointShadow3;

// Inverse world rotation: the same lookup transform is used by the skybox.
vec3 KansEnvironmentDirection(vec3 direction)
{
    vec2 cs = u_Frame.EnvironmentInfo.zw;
    if (cs == vec2(1.0, 0.0)) return direction;
    return vec3(cs.x * direction.x - cs.y * direction.z, direction.y,
                cs.y * direction.x + cs.x * direction.z);
}
vec3 KansEnvironmentDiffuse(vec3 normal)
{
    vec3 value = textureLod(u_IrradianceMap, KansEnvironmentDirection(normal), 0.0).rgb;
    return u_Frame.EnvironmentInfo.x == 1.0 ? value : value * u_Frame.EnvironmentInfo.x;
}
vec3 KansEnvironmentSpecular(vec3 reflection, float roughness)
{
    vec3 value = textureLod(u_PrefilterMap, KansEnvironmentDirection(reflection), roughness * 4.0).rgb;
    return u_Frame.EnvironmentInfo.y == 1.0 ? value : value * u_Frame.EnvironmentInfo.y;
}


// Engine PBR lighting (point lights + image-based lighting), in linear HDR.
const float KANS_PI = 3.14159265359;

// The atlas has four independent tiles, including the half-texel bilinear footprint.
vec2 KansShadowUV(int cascade, vec2 localUV)
{
    vec2 tile = vec2(cascade & 1, cascade >> 1) * 0.5;
    float h = 0.25 / u_Frame.CascadeTexel[cascade].z;
    return clamp(tile + localUV * 0.5, tile + vec2(h), tile + vec2(0.5 - h));
}
// Use the geometric receiver plane, including on meshes with interpolated shading normals.
vec3 KansReceiverNormal(vec3 worldPos, vec3 normal)
{
    #ifndef KANS_FRAGMENT_STAGE
    return normalize(normal); // Vertex receivers have no derivatives.
    #else
    vec3 n = cross(dFdx(worldPos), dFdy(worldPos));
    return dot(n,n) > 1e-16 ? normalize(n) : normalize(normal);
    #endif
}
vec2 KansReceiverGradient(int cascade, vec3 normal)
{
    // Orthographic cascade rows are perpendicular. Transform the plane covector
    // into local shadow UV/depth coordinates; dz/duv = -plane.xy / plane.z.
    mat3 rows = transpose(mat3(u_Frame.CascadeViewProjection[cascade]));
    vec3 u = rows[0] * 0.5, v = rows[1] * 0.5, z = rows[2];
    vec3 plane = vec3(dot(normal,u)/max(dot(u,u),1e-20),
                      dot(normal,v)/max(dot(v,v),1e-20),
                      dot(normal,z)/max(dot(z,z),1e-20));
    return abs(plane.z) > 1e-8 ? -plane.xy / plane.z : vec2(0);
}
vec2 KansShadowLocalUV(int cascade, vec2 uv)
{
    return (uv - vec2(cascade & 1, cascade >> 1) * 0.5) * 2.0;
}
float KansCascadeCompare(int cascade, vec3 p, vec2 offset, vec2 gradient)
{
    float resolution = u_Frame.CascadeTexel[cascade].z;
    vec2 uv = KansShadowUV(cascade, p.xy + offset / resolution);
    // Correct each tap at its clamped location. The extra half-texel term bounds
    // all four bilinear comparison texels, rather than displacing the whole kernel.
    float receiver = p.z + dot(gradient, KansShadowLocalUV(cascade, uv) - p.xy);
    float footprint = 0.5 * dot(abs(gradient), vec2(1.0 / resolution));
    // Wide grazing kernels can extrapolate beyond the cascade depth interval.
    // Clear depth (1) remains lit there, rather than becoming a false occluder.
    return texture(u_DirectionalShadow, vec3(uv, clamp(receiver - footprint - 2e-7, 0.0, 1.0)));
}
const vec2 KANS_POISSON[32] = vec2[](
    vec2(-.613,.617),vec2(.170,-.040),vec2(-.299,-.791),vec2(.645,.493),
    vec2(-.652,-.302),vec2(.421,-.712),vec2(-.817,.090),vec2(.329,.875),
    vec2(.849,-.222),vec2(-.112,.329),vec2(.015,-.956),vec2(-.456,.004),
    vec2(.259,.303),vec2(-.274,-.374),vec2(.741,.072),vec2(-.084,.740),
    vec2(-.900,-.400),vec2(.528,.714),vec2(.540,-.390),vec2(-.460,.846),
    vec2(.903,.405),vec2(-.091,-.182),vec2(-.501,-.587),vec2(.282,-.453),
    vec2(-.737,.411),vec2(.002,.012),vec2(.655,-.718),vec2(-.350,.499),
    vec2(.131,.606),vec2(-.236,-.967),vec2(.974,-.030),vec2(-.018,-.570));
float KansCascadeShadow(int cascade, vec3 worldPos, vec3 normal)
{
    vec2 gradient = KansReceiverGradient(cascade, KansReceiverNormal(worldPos, normal));
    vec3 q = worldPos + normal * u_Frame.ShadowInfo.z *
        (1.0 - clamp(dot(normal, KansDirectionalToLight()), 0.0, 1.0));
    vec4 clip = u_Frame.CascadeViewProjection[cascade] * vec4(q, 1);
    vec3 p = clip.xyz / clip.w;
    p.xy = p.xy * 0.5 + 0.5;
    if (any(lessThan(p, vec3(0))) || any(greaterThan(p, vec3(1)))) return 1.0;
    int shadowFilter = int(u_Frame.ShadowInfo.y);
    float result = 0.0;
    if (shadowFilter < 2)
    {
        int r = shadowFilter == 0 ? 1 : 2;
        float total = 0.0;
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
            {
                float weight = shadowFilter == 0 ? 1.0 : float((3 - abs(x)) * (3 - abs(y)));
                result += weight * KansCascadeCompare(cascade, p, vec2(x,y), gradient);
                total += weight;
            }
        return result / total;
    }
    #ifndef KANS_FRAGMENT_STAGE
    // Stable world-space rotation; no screen-space noise on translucent vertex receivers.
    float angle = 0.0;
    #else
    float angle = 6.28318530718 * fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(.06711056,.00583715))));
    #endif
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    vec4 texel = u_Frame.CascadeTexel[cascade];
    float searchRadius = clamp(texel.y * u_Frame.ShadowInfo.w / texel.x, 1.0, 64.0);
    float blockers = 0.0, depthSum = 0.0;
    // A dense inner disk prevents thin casters from disappearing between wide search samples.
    // Both radii are independent of the camera-relative light depth, preserving the stabilized grid.
    for (int disk = 0; disk < 2; ++disk)
        for (int i = 0; i < 32; ++i)
        {
            float radius = disk == 0 ? searchRadius : max(1.0, searchRadius * 0.125);
            vec2 uv = KansShadowUV(cascade, p.xy + rotation * KANS_POISSON[i] * radius / texel.z);
            // Nearest raw depth belongs to the texel centre, not to the search UV.
            vec2 size = vec2(textureSize(u_DirectionalShadowRaw, 0));
            uv = (floor(uv * size) + 0.5) / size;
            float depth = textureLod(u_DirectionalShadowRaw, uv, 0.0).r;
            float receiver = p.z + dot(gradient, KansShadowLocalUV(cascade, uv) - p.xy);
            float separation = receiver - depth;
            if (depth < 1.0 && separation > 0.00001) { depthSum += p.z - separation; blockers += 1.0; }
        }
    if (blockers == 0.0) return 1.0;
    float penumbra = clamp((p.z - depthSum / blockers) * texel.y * u_Frame.ShadowInfo.w / texel.x, 0.0, 64.0);
    for (int i = 0; i < 32; ++i)
        result += KansCascadeCompare(cascade, p, rotation * KANS_POISSON[i] * penumbra, gradient);
    return result / 32.0;
}
int KansShadowCascade(vec3 worldPos)
{
    float depth = dot(u_Frame.ShadowCameraDepth, vec4(worldPos, 1));
    for (int i = 0; i < int(u_Frame.ShadowInfo.x); ++i)
        if (depth <= u_Frame.CascadeSplits[i]) return i;
    return -1;
}
float KansShadow(vec3 worldPos, vec3 normal)
{
    if (u_Frame.ShadowInfo.x == 0.0 || u_Object.MaterialRender.w != 0.0) return 1.0;
    int cascade = KansShadowCascade(worldPos);
    if (cascade < 0) return 1.0;
    float result = KansCascadeShadow(cascade, worldPos, normal);
    float depth = dot(u_Frame.ShadowCameraDepth, vec4(worldPos, 1));
    float start = u_Frame.CascadeBlendStarts[cascade], end = u_Frame.CascadeSplits[cascade];
    if (end > start && depth > start)
    {
        float next = cascade + 1 < int(u_Frame.ShadowInfo.x) ? KansCascadeShadow(cascade + 1, worldPos, normal) : 1.0;
        result = mix(result, next, smoothstep(start, end, depth));
    }
    return result;
}
float KansCubeCompare(int shadow, vec3 direction, float depth)
{
    if (shadow == 0) return texture(u_PointShadow0, vec4(direction, depth));
    if (shadow == 1) return texture(u_PointShadow1, vec4(direction, depth));
    if (shadow == 2) return texture(u_PointShadow2, vec4(direction, depth));
    return texture(u_PointShadow3, vec4(direction, depth));
}
float KansPointShadow(int light, vec3 worldPos, vec3 normal)
{
    if (light < 0 || light >= KANS_POINT_LIGHT_LIMIT || u_Object.MaterialRender.w != 0.0) return 1.0;
    int shadow = int(u_Frame.PointShadowIndices[light / 4][light % 4]);
    if (shadow < 0) return 1.0;
    vec4 positionRange = u_Frame.PointShadowPositionRange[shadow];
    vec4 params = u_Frame.PointShadowParams[shadow];
    vec3 delta = worldPos - positionRange.xyz;
    float distanceToLight = length(delta);
    if (distanceToLight >= positionRange.w || distanceToLight < 0.001) return 1.0;
    vec3 L = -delta / distanceToLight;
    delta += normal * params.y * (1.0 - clamp(dot(normal,L),0.0,1.0));
    vec3 receiverNormal = KansReceiverNormal(worldPos, normal);
    float planeDistance = abs(dot(receiverNormal, delta));
    vec3 direction = normalize(delta);
    vec3 T = normalize(cross(abs(direction.y) < .99 ? vec3(0,1,0) : vec3(1,0,0), direction));
    vec3 B = cross(direction,T);
    float sum = 0.0, total = 0.0;
    int r = params.w == 1.0 ? 2 : 1;
    float angularStep = max(params.x * 2.0, u_Frame.PointShadowSourceRadius[shadow] / (distanceToLight * float(r)));
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x)
        {
            float weight = r == 1 ? 1.0 : float((3-abs(x))*(3-abs(y)));
            vec3 tap = normalize(direction + (T * x + B * y) * angularStep);
            // Intersect each PCF ray with the receiver plane. This radial correction
            // scales with distance * angularStep * kernel radius (world footprint).
            float cosine = abs(dot(receiverNormal, tap));
            // A cube texel spans <=2/res radians. Bound the half-texel bilinear
            // footprint on both axes, including the radial slope at grazing angles.
            float halfFootprint = 1.414214 * params.x;
            float closest = planeDistance / max(cosine + halfFootprint * sqrt(max(0.0,1.0-cosine*cosine)), 1e-6);
            float depth = clamp(closest / positionRange.w - params.z - 2e-7, 0.0, 1.0);
            sum += weight * KansCubeCompare(shadow, tap, depth);
            total += weight;
        }
    return sum / total;
}

float KansDistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a     = roughness * roughness;
    float a2    = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (KANS_PI * denom * denom);
}

float KansGeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

vec3 KansDirectPbr(vec3 albedo, float metallic, float roughness, vec3 N, vec3 V,
                   vec3 L, vec3 radiance, vec3 specularScale)
{
    if (dot(N, L) <= 0.0) return vec3(0);
    vec3 H = normalize(V + L);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NdotV = max(dot(N, V), 0.0), NdotL = max(dot(N, L), 0.0);
    float G = KansGeometrySchlickGGX(NdotV, roughness) * KansGeometrySchlickGGX(NdotL, roughness);
    vec3 F = F0 + (1.0 - F0) * pow(clamp(1.0 - max(dot(H, V), 0.0), 0.0, 1.0), 5.0);
    vec3 specular = KansDistributionGGX(N, H, roughness) * G * F / (4.0 * NdotV * NdotL + 0.001);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    return (kD * albedo / KANS_PI + specular * specularScale) * radiance * NdotL;
}
vec3 KansShadePbr(vec3 albedo, float metallic, float roughness, float ao, vec3 N, vec3 worldPos)
{
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    vec3 V = normalize(u_Frame.CameraPosition.xyz - worldPos);
    vec3 R = reflect(-V, N);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NdotV = max(dot(N, V), 0.0);
    vec3 Lo = vec3(0.0);
    for (int i = 0; i < KansPointLightCount(); ++i)
    {
        if (all(lessThanEqual(KansPointLightRadiance(i), vec3(0)))) continue;
        vec3 toLight = KansPointLightPosition(i) - worldPos;
        if (dot(toLight, toLight) <= 1e-8) continue;
        float distanceSquared = max(dot(toLight, toLight), 1e-4);
        vec3 L = normalize(toLight);
        Lo += KansDirectPbr(albedo, metallic, roughness, N, V, L,
                            KansPointLightRadiance(i) / distanceSquared, KansPointLightSpecular(i)) * KansPointShadow(i, worldPos, N);
    }
    if (u_Frame.DirectionalDirection.w != 0.0)
        Lo += KansDirectPbr(albedo, metallic, roughness, N, V, KansDirectionalToLight(),
                            KansDirectionalRadiance(), u_Frame.DirectionalSpecular.rgb) * KansShadow(worldPos, N)
#ifdef KANS_DIRECTIONAL_TRANSMITTANCE
            * KANS_DIRECTIONAL_TRANSMITTANCE
#endif
            ;
    vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = KansEnvironmentDiffuse(N) * albedo;
    vec3 prefiltered = KansEnvironmentSpecular(R, roughness);
    vec2 brdf = texture(u_BrdfLUT, vec2(NdotV, roughness)).rg;
    return (kD * diffuse + prefiltered * (F * brdf.x + brdf.y)
            + KansDirectionalAmbient() * albedo) * ao + Lo;
}

// Legacy display-referred helper. Retained for published/custom sources; HDR shaders return linear colour.
vec4 KansToneMap(vec3 color)
{
    color = color / (color + vec3(1.0));
    return vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
#endif
)"},
            };
            return includes;
        }

        struct ExpectedMember
        {
            const char* Name;
            uint64_t    Offset;
            uint32_t    Components, Columns, ArrayCount;
        };
        struct ExpectedBinding
        {
            ShaderBindingKind           Kind;
            ShaderImageDimension        Dimension;
            uint64_t                    BlockBytes;
            std::vector<ExpectedMember> Members;
        };

        const ExpectedBinding& Expected(EngineBinding binding)
        {
            static const ExpectedBinding table[EngineBindingCount] = {
                {ShaderBindingKind::UniformBuffer,
                 ShaderImageDimension::Texture2D,
                 sizeof(EngineFrameUniforms),
                 {{"ViewProjection", 0, 4, 4, 1},
                  {"CameraPosition", 64, 4, 1, 1},
                  {"LightPositions", 80, 4, 1, EngineLightCount},
                  {"LightColors", 144, 4, 1, EngineLightCount},
                  {"LightDiffuse", 208, 4, 1, EngineLightCount},
                  {"Time", 272, 1, 1, 1},
                  {"DebugView", 276, 1, 1, 1},
                  {"FramePadding", 280, 2, 1, 1},
                  {"PointPositions", 288, 4, 1, EnginePointLightLimit},
                  {"PointRadiance", 544, 4, 1, EnginePointLightLimit},
                  {"PointSpecular", 800, 4, 1, EnginePointLightLimit},
                  {"DirectionalDirection", 1056, 4, 1, 1},
                  {"DirectionalRadiance", 1072, 4, 1, 1},
                  {"DirectionalSpecular", 1088, 4, 1, 1},
                  {"DirectionalAmbient", 1104, 4, 1, 1},
                  {"LightingInfo", 1120, 4, 1, 1},
                  {"CascadeViewProjection", 1136, 4, 4, 4},
                  {"CascadeSplits", 1392, 4, 1, 1},
                  {"CascadeBlendStarts", 1408, 4, 1, 1},
                  {"CascadeTexel", 1424, 4, 1, 4},
                  {"ShadowInfo", 1488, 4, 1, 1},
                  {"ShadowCameraDepth", 1504, 4, 1, 1},
                  {"PointShadowIndices", 1520, 4, 1, 4},
                  {"PointShadowPositionRange", 1584, 4, 1, 4},
                  {"PointShadowParams", 1648, 4, 1, 4},
                  {"ShadowPass", 1712, 4, 1, 1},
                  {"ShadowPassPosition", 1728, 4, 1, 1},
                  {"PointShadowSourceRadius", 1744, 4, 1, 1},
                  {"EnvironmentInfo", 1760, 4, 1, 1},
                  {"View", 1776, 4, 4, 1},
                  {"Projection", 1840, 4, 4, 1},
                  {"InverseView", 1904, 4, 4, 1},
                  {"InverseProjection", 1968, 4, 4, 1}}},
                {ShaderBindingKind::UniformBuffer,
                 ShaderImageDimension::Texture2D,
                 sizeof(EngineObjectUniforms),
                 {{"Transform", 0, 4, 4, 1}, {"NormalMatrix", 64, 4, 4, 1}, {"MaterialRender", 128, 4, 1, 1}}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Cube, 0, {}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Cube, 0, {}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Texture2D, 0, {}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Texture2D, 0, {}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Texture2D, 0, {}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Cube, 0, {}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Cube, 0, {}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Cube, 0, {}},
                {ShaderBindingKind::CombinedImageSampler, ShaderImageDimension::Cube, 0, {}},
            };
            return table[static_cast<uint32_t>(binding)];
        }

        AssetError Contract(const std::string& message)
        {
            return AssetError {AssetErrorCode::InvalidData, "Material Shader contract: " + message};
        }

        // Published shaders before DebugView contain only the original KansFrame members.
        constexpr size_t LegacyFrameMemberCount  = 6;
        constexpr size_t LegacyObjectMemberCount = 2;

        std::optional<AssetError> CheckEngineBinding(const ShaderResourceBinding& binding)
        {
            if (binding.Binding >= EngineBindingCount)
                return Contract("set 0 binding " + std::to_string(binding.Binding) +
                                " is not engine data; declare engine data with #include <Kans3D/Engine.glsl>");
            const auto& expected = Expected(static_cast<EngineBinding>(binding.Binding));
            // Block sizes are reflected without tail padding; members pin the layout.
            bool matches =
                binding.Kind == expected.Kind && binding.Count == 1 && binding.BlockBytes <= expected.BlockBytes &&
                (binding.Kind != ShaderBindingKind::CombinedImageSampler || binding.Dimension == expected.Dimension) &&
                (binding.Members.size() == expected.Members.size() ||
                 (binding.Binding == uint32_t(EngineBinding::Frame) &&
                  (binding.Members.size() == LegacyFrameMemberCount || binding.Members.size() == 7 ||
                   binding.Members.size() == 17 || binding.Members.size() == 28 || binding.Members.size() == 29)) ||
                 (binding.Binding == uint32_t(EngineBinding::Object) &&
                  binding.Members.size() == LegacyObjectMemberCount));
            for (size_t i = 0; matches && i < binding.Members.size(); ++i)
            {
                const auto& member = binding.Members[i];
                const auto& want   = expected.Members[i];
                matches            = member.Name == want.Name && member.Offset == want.Offset &&
                                     member.Type == ShaderUniformType::Float && member.Components == want.Components &&
                                     member.Columns == want.Columns && member.ArrayCount == want.ArrayCount;
            }
            if (!matches)
                return Contract("engine binding '" + binding.Name +
                                "' does not match the engine layout; use #include <Kans3D/Engine.glsl>");
            return std::nullopt;
        }
    } // namespace

    const std::string* FindEngineShaderInclude(const std::string& name)
    {
        if (name == "Kans3D/Frame.glsl")
        {
            // Keep the published Engine.glsl text inline, and derive the exact same
            // guarded frame block for fragment-only post shaders without surface bindings.
            static const std::string frame = [] {
                const auto& engine = EngineIncludes().at("Kans3D/Engine.glsl");
                const auto  begin  = engine.find("#define KANS_LIGHT_COUNT");
                const auto  end    = engine.find("layout(set = 0, binding = 1, std140)");
                return "#ifndef KANS3D_FRAME_GLSL\n#define KANS3D_FRAME_GLSL\n" + engine.substr(begin, end - begin) +
                       "#endif\n";
            }();
            return &frame;
        }
        const auto found = EngineIncludes().find(name);
        return found == EngineIncludes().end() ? nullptr : &found->second;
    }

    static std::variant<std::monostate, AssetError>
    ValidateMaterialInterface(const std::vector<ShaderStageReflection>& reflection,
                              bool                                      depthOnly,
                              ShaderDomain                              domain = ShaderDomain::Surface)
    {
        const ShaderStageReflection* vertex   = nullptr;
        const ShaderStageReflection* fragment = nullptr;
        for (const auto& stage : reflection)
        {
            if (stage.Stage == ShaderProductStage::Vertex)
                vertex = &stage;
            else if (stage.Stage == ShaderProductStage::Fragment)
                fragment = &stage;
        }
        if (!vertex || !fragment || reflection.size() != 2)
            return Contract("a vertex and a fragment stage are required");

        if (domain == ShaderDomain::PostProcess && !vertex->Inputs.empty())
            return Contract("post-process vertex stage must have no inputs");
        // Engine mesh layout: position, normal, uv, color, tangent.
        constexpr uint32_t vertexComponents[] = {3, 3, 2, 4, 4};
        for (const auto& input : vertex->Inputs)
            if ((input.Location >= 5 && input.Location != 7) || input.Columns != 1 ||
                input.Scalar != ShaderInterfaceScalar::Float ||
                input.Components != (input.Location == 7 ? 3 : vertexComponents[input.Location]))
                return Contract("vertex input '" + input.Name + "' does not match the engine vertex layout");
        if (!(depthOnly && fragment->Outputs.empty()) &&
            (fragment->Outputs.size() != 1 || fragment->Outputs[0].Location != 0 ||
             fragment->Outputs[0].Components != 4 || fragment->Outputs[0].Columns != 1 ||
             fragment->Outputs[0].Scalar != ShaderInterfaceScalar::Float))
            return Contract("the fragment stage must write one vec4 color at location 0");

        for (const auto& stage : reflection)
            for (const auto& binding : stage.Bindings)
            {
                if (binding.Set == EngineDescriptorSet)
                {
                    if (domain == ShaderDomain::PostProcess && binding.Binding != 0)
                    {
                        if (stage.Stage != ShaderProductStage::Fragment || binding.Binding < 11 ||
                            binding.Binding > 14 || binding.Kind != ShaderBindingKind::CombinedImageSampler ||
                            binding.Count != 1 || binding.Dimension != ShaderImageDimension::Texture2D)
                            return Contract("invalid post-process engine binding");
                    }
                    else if (auto error = CheckEngineBinding(binding))
                        return *error;
                }
                else if (binding.Set == MaterialDescriptorSet)
                {
                    if (binding.Kind == ShaderBindingKind::CombinedImageSampler &&
                        binding.Dimension != ShaderImageDimension::Texture2D)
                        return Contract("material sampler '" + binding.Name + "' must be a sampler2D");
                }
                else
                    return Contract("'" + binding.Name + "' uses descriptor set " + std::to_string(binding.Set) +
                                    "; only set 0 (engine) and set 1 (material) are available");
            }
        return std::monostate {};
    }

    std::variant<std::monostate, AssetError>
    ValidateMaterialShaderInterface(const std::vector<ShaderStageReflection>& reflection, ShaderDomain domain)
    {
        return ValidateMaterialInterface(reflection, false, domain);
    }

    namespace
    {
        bool MemberSubset(const ShaderUniformMember& a, const ShaderUniformMember& b)
        {
            if (a.Name != b.Name || a.Offset != b.Offset || a.Type != b.Type || a.Components != b.Components ||
                a.Columns != b.Columns || a.ArrayCount != b.ArrayCount || a.ArrayStride != b.ArrayStride ||
                a.MatrixStride != b.MatrixStride || a.RowMajor != b.RowMajor || a.Bytes > b.Bytes)
                return false;
            if (a.Type != ShaderUniformType::Struct && a.Bytes != b.Bytes)
                return false;
            for (const auto& member : a.Members)
            {
                auto found = std::find_if(
                    b.Members.begin(), b.Members.end(), [&](const auto& want) { return want.Name == member.Name; });
                if (found == b.Members.end() || !MemberSubset(member, *found))
                    return false;
            }
            return true;
        }
        bool BindingSubset(const ShaderResourceBinding& a, const ShaderResourceBinding& b)
        {
            if (a.Kind != b.Kind || a.Set != b.Set || a.Binding != b.Binding || a.Count != b.Count ||
                a.Dimension != b.Dimension || a.Name != b.Name || a.BlockBytes > b.BlockBytes)
                return false;
            for (const auto& member : a.Members)
            {
                auto found = std::find_if(
                    b.Members.begin(), b.Members.end(), [&](const auto& want) { return want.Name == member.Name; });
                if (found == b.Members.end() || !MemberSubset(member, *found))
                    return false;
            }
            return true;
        }
    } // namespace
    std::variant<std::monostate, AssetError> ValidateMaterialShaderPasses(const ShaderProgramCode&       main,
                                                                          const std::vector<ShaderPass>& passes)
    {
        if (passes.empty())
            return std::monostate {}; // Generic single-stage products retain their existing contract.
        auto inspected = InspectShaderProgram(main);
        if (auto* error = std::get_if<AssetError>(&inspected))
            return *error;
        const auto& reflection = std::get<std::vector<ShaderStageReflection>>(inspected);
        auto        valid      = ValidateMaterialShaderInterface(reflection);
        if (auto* error = std::get_if<AssetError>(&valid))
            return *error;
        std::map<std::pair<uint32_t, uint32_t>, ShaderResourceBinding> bindings;
        for (const auto& stage : reflection)
            for (const auto& binding : stage.Bindings)
            {
                auto [found, inserted] = bindings.emplace(std::make_pair(binding.Set, binding.Binding), binding);
                if (!inserted && (!BindingSubset(binding, found->second) || !BindingSubset(found->second, binding)))
                    return Contract("main pass has inconsistent resource names/layouts across stages");
            }
        std::set<std::string> names;
        if (passes.size() > 32)
            return Contract("too many additional passes");
        for (const auto& pass : passes)
        {
            const auto fail = [&](const std::string& message) {
                return Contract("pass '" + pass.Name + "': " + message);
            };
            const auto token = [](const std::string& value) {
                return !value.empty() && value.size() <= 256 &&
                       std::all_of(
                           value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
            };
            const auto& state = pass.State;
            if (!token(pass.Name) || pass.Name == "Main" || !token(pass.Tag) || !names.insert(pass.Name).second)
                return fail("invalid/duplicate name or tag");
            if (uint32_t(state.Cull) > 2 || uint32_t(state.DepthCompare) > 7 || state.ColorWriteMask > 15 ||
                uint32_t(state.SourceColor) > 9 || uint32_t(state.DestinationColor) > 9 ||
                uint32_t(state.SourceAlpha) > 9 || uint32_t(state.DestinationAlpha) > 9)
                return fail("invalid render state");
            auto extra = InspectShaderProgram(pass.Code);
            if (auto* error = std::get_if<AssetError>(&extra))
                return fail(error->Message);
            const auto& stages   = std::get<std::vector<ShaderStageReflection>>(extra);
            auto contract = ValidateMaterialInterface(stages, pass.Tag == "ShadowCaster" || pass.Tag == "DepthOnly");
            if (auto* error = std::get_if<AssetError>(&contract))
                return fail(error->Message);
            std::set<uint32_t> engine;
            for (const auto& stage : stages)
                for (const auto& binding : stage.Bindings)
                {
                    auto found = bindings.find({binding.Set, binding.Binding});
                    if (found == bindings.end() || !BindingSubset(binding, found->second))
                        return fail("set " + std::to_string(binding.Set) + " binding " +
                                    std::to_string(binding.Binding) + " ('" + binding.Name +
                                    "') is not a subset of the main pass layout");
                    if (binding.Set == 0)
                    {
                        if (!BindingSubset(found->second, binding))
                            return fail("set 0 contract differs from main pass");
                        engine.insert(binding.Binding);
                    }
                }
            for (const auto& [key, binding] : bindings)
                if (key.first == 0 && key.second < uint32_t(EngineBinding::DirectionalShadow) &&
                    !engine.count(key.second))
                    return fail("set 0 contract omits main pass binding " + std::to_string(key.second));
        }
        return std::monostate {};
    }

    std::vector<ShaderResourceBinding> MaterialShaderResources(const std::vector<ShaderStageReflection>& reflection)
    {
        std::map<uint32_t, ShaderResourceBinding> merged;
        for (const auto& stage : reflection)
            for (const auto& binding : stage.Bindings)
                if (binding.Set == MaterialDescriptorSet)
                    merged.emplace(binding.Binding, binding);
        std::vector<ShaderResourceBinding> result;
        for (auto& entry : merged)
            result.push_back(std::move(entry.second));
        return result;
    }
    std::variant<std::monostate, AssetError>
    ValidateMaterialParameterGroups(const std::vector<MaterialParameterGroup>& groups,
                                    const std::vector<ShaderStageReflection>&  reflection)
    {
        std::set<std::string> names, used, groupNames;
        for (const auto& resource : MaterialShaderResources(reflection))
            if (resource.Kind == ShaderBindingKind::CombinedImageSampler)
                names.insert(resource.Name);
            else
                for (const auto& member : resource.Members)
                    if (member.Type != ShaderUniformType::Struct)
                        names.insert(member.Name);
        if (groups.size() > 4096)
            return AssetError {AssetErrorCode::InvalidData, "Too many Shader groups"};
        for (const auto& group : groups)
        {
            if (group.Name.empty() || group.Name.size() > 256 || group.Members.empty() || group.Members.size() > 4096 ||
                !groupNames.insert(group.Name).second || group.Name.front() == ' ' || group.Name.back() == ' ' ||
                std::any_of(group.Name.begin(), group.Name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
                return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate Shader group name"};
            for (const auto& member : group.Members)
                if (!names.count(member) || !used.insert(member).second)
                    return AssetError {AssetErrorCode::InvalidData,
                                       "Unknown or repeated Shader group member '" + member + "'"};
        }
        return std::monostate {};
    }
} // namespace Kans
