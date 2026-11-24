#ifndef KANS_TOON_SURFACE_GLSL
#define KANS_TOON_SURFACE_GLSL

#include <ToonLighting/AdditionalLights.glsl>

struct ToonSurfaceParams
{
    int Steps;             // Number of nonzero bands for the normal-based ramp, at least 1.
    float Threshold;       // N dot L at the first band, in [0,1].
    float Softness;        // Feather as a fraction of band spacing, or as a cosine width for faces.
    float AmbientStrength; // Linear multiplier of KansAmbientIrradiance on the shade tone.
    float RimStrength;     // Linear multiplier of the light's colour at backlit silhouettes.
    float RimWidth;        // Silhouette width in N dot V, in [0,1]; 0 disables the rim.
    float Saturation;      // 0 = grey, 1 = light colour, 2 = boosted colour.
    float MaxBrightness;   // Maximum RGB channel in linear HDR; 0 disables the cap.
    int MaxLights;         // 0 = all; otherwise directional, then points, spots and areas, in forward-list order.
};

ToonSurfaceParams ToonSurfaceDefaultParams()
{
    return ToonSurfaceParams(3, 0.15, 0.15, 1.0, 0.25, 0.25, 1.0, 0.0, 0);
}

float ToonSurfaceRamp(vec3 N, vec3 L, ToonLightParams ramp, bool useFace, mat3 head, vec2 thresholds)
{
    if (!useFace) return ToonDiffuseRamp(dot(N, L), ramp);

    // Head columns are the world-space forward, up and right axes.
    vec3 horizontal = L - head[1] * dot(head[1], L);
    float lengthSquared = dot(horizontal, horizontal);
    // Straight above/below has no azimuth. Use the frontal limit.
    float frontness = lengthSquared > 1e-8 ?
        clamp(dot(head[0], horizontal) * inversesqrt(lengthSquared), -1.0, 1.0) : 1.0;
    if (frontness < 0.0) return 0.0;
    float threshold = dot(head[2], horizontal) < 0.0 ? thresholds.y : thresholds.x;
    float width = 0.5 * clamp(ramp.Softness, 0.0, 1.0);
    return width > 0.0 ? smoothstep(-width, width, frontness - threshold) : step(threshold, frontness);
}

vec3 ToonSurfaceContribution(KansLightSample light, vec3 V, vec3 litColour, vec3 shadeColour,
                             float ramp, float silhouette, float saturation)
{
    // Use incident radiance here. KansSample* has already multiplied by visibility.
    vec3 radiance = light.Radiance;
    if (saturation != 1.0)
        radiance = max(mix(vec3(dot(radiance, vec3(0.2126, 0.7152, 0.0722))), radiance, saturation), vec3(0.0));
    float rim = silhouette * max(-dot(V, light.ToLight), 0.0);
    vec3 tone = mix(shadeColour, litColour, ramp * light.Visibility);
    return (tone + rim * light.Visibility) * radiance * light.DiffuseScale;
}

vec3 ToonSurfaceEvaluate(vec3 worldPos, vec3 normal, vec3 viewDir, vec3 litColour, vec3 shadeColour,
                         ToonSurfaceParams p, bool useFace, mat3 head, vec2 thresholds)
{
    vec3 N = dot(normal, normal) > 0.0 ? normalize(normal) : vec3(0, 0, 1);
    vec3 V = dot(viewDir, viewDir) > 0.0 ? normalize(viewDir) : N;
    float rimWidth = clamp(p.RimWidth, 0.0, 1.0);
    float silhouette = rimWidth > 0.0 ?
        max(p.RimStrength, 0.0) * (1.0 - smoothstep(0.0, rimWidth, max(dot(N, V), 0.0))) : 0.0;
    float saturation = clamp(p.Saturation, 0.0, 2.0);
    ToonLightParams ramp = ToonLightParams(p.Steps, p.Threshold, p.Softness, 0.0, 0.0, 1.0, 1.0, 0.0);
    vec3 sum = vec3(0.0);
    if (p.AmbientStrength > 0.0)
        sum = shadeColour * KansAmbientIrradiance(N) * p.AmbientStrength;

    int points = KansPointLightCount();
    int punctual = points + KansSpotLightCount();
    int areas = KansAreaLightCount();
    bool hasDirectional = u_Frame.DirectionalDirection.w != 0.0;
    int remaining = p.MaxLights > 0 ? p.MaxLights : int(hasDirectional) + punctual + areas;
    if (hasDirectional)
    {
        KansLightSample light = KansDirectionalIncident(worldPos, N);
        float band = ToonSurfaceRamp(N, light.ToLight, ramp, useFace, head, thresholds);
        sum += ToonSurfaceContribution(light, V, litColour, shadeColour, band, silhouette, saturation);
        --remaining;
    }
    int punctualCount = min(punctual, remaining);
    for (int i = 0; i < punctualCount; ++i)
    {
        KansLightSample light = i < points ? KansPointIncident(i, worldPos, N) :
            KansSpotIncident(i - points, worldPos, N);
        float band = ToonSurfaceRamp(N, light.ToLight, ramp, useFace, head, thresholds);
        sum += ToonSurfaceContribution(light, V, litColour, shadeColour, band, silhouette, saturation);
    }
    int areaCount = min(areas, remaining - punctualCount);
    for (int i = 0; i < areaCount; ++i)
    {
        vec3 L = KansAreaLightToLight(i, worldPos);
        if (L == vec3(0.0)) continue;
        // Evaluate the finite-emitter form factor facing its centre. The toon ramp
        // supplies the surface angular response. The integral includes diffuse scale.
        vec3 incident = KansAreaLightDiffuseIrradiance(i, worldPos, L);
        KansLightSample light = KansLightSample(L, 0.0, incident, 1.0, 1.0);
        float band = ToonSurfaceRamp(N, L, ramp, useFace, head, thresholds);
        sum += ToonSurfaceContribution(light, V, litColour, shadeColour, band, silhouette, saturation);
    }

    float peak = max(sum.r, max(sum.g, sum.b));
    return p.MaxBrightness > 0.0 && peak > p.MaxBrightness ? sum * (p.MaxBrightness / peak) : sum;
}

// World-space inputs, with viewDir pointing towards the camera. Colours are linear HDR.
vec3 ToonSurface(vec3 worldPos, vec3 normal, vec3 viewDir, vec3 litColour, vec3 shadeColour, ToonSurfaceParams p)
{
    return ToonSurfaceEvaluate(worldPos, normal, viewDir, litColour, shadeColour, p, false, mat3(1.0), vec2(0.0));
}

// Supply an orthonormal head frame in world space. Thresholds are the minimum
// horizontal light cosines for the right and left sides, respectively.
vec3 ToonSurfaceFace(vec3 worldPos, vec3 normal, vec3 viewDir, vec3 litColour, vec3 shadeColour, ToonSurfaceParams p,
                     vec3 headForward, vec3 headUp, vec3 headRight, vec2 thresholds)
{
    return ToonSurfaceEvaluate(worldPos, normal, viewDir, litColour, shadeColour, p, true,
        mat3(headForward, headUp, headRight), clamp(thresholds, 0.0, 1.0));
}

// Pass face UVs in [0,1] and a linear red-channel threshold map with horizontal symmetry
// provided by mirroring U. Read both sides once, outside the light loops.
vec3 ToonSurfaceFace(vec3 worldPos, vec3 normal, vec3 viewDir, vec3 litColour, vec3 shadeColour, ToonSurfaceParams p,
                     vec3 headForward, vec3 headUp, vec3 headRight, sampler2D thresholdMap, vec2 uv)
{
    vec2 thresholds = vec2(texture(thresholdMap, uv).r, texture(thresholdMap, vec2(1.0 - uv.x, uv.y)).r);
    return ToonSurfaceFace(worldPos, normal, viewDir, litColour, shadeColour, p,
        headForward, headUp, headRight, thresholds);
}

#endif
