#ifndef KANS_TOON_ADDITIONAL_LIGHTS_GLSL
#define KANS_TOON_ADDITIONAL_LIGHTS_GLSL

// Enable ToonLighting in the project and include <ToonLighting/AdditionalLights.glsl>
// in the generated or custom material fragment shader. After your key light and
// base shading, add the point, spot and area contributions in linear HDR:
//     ToonLightParams p = ToonDefaultParams();
//     color += baseColor * ToonAdditionalLights(worldPos, normal, viewDir, p);
// For the directional key, multiply its ramp by ToonKeyShadow(worldPos, normal,
// p.Softness). Use world-space inputs and point viewDir from the surface to the camera.
#include <Kans3D/LightAccess.glsl>

struct ToonLightParams
{
    int Steps;           // Number of nonzero diffuse bands, at least 1.
    float Threshold;     // N dot L at the first band, in [0,1].
    float Softness;      // Transition width as a fraction of band spacing, in [0,1].
    float RimStrength;   // Linear multiplier for backlighting at the silhouette.
    float RimWidth;      // Silhouette width measured in N dot V, in [0,1]; 0 disables it.
    float Saturation;    // 0 = luminance only, 1 = full light colour.
    float Intensity;     // Linear multiplier applied to the sum.
    float MaxBrightness; // Maximum RGB channel after intensity, before exposure.
};

ToonLightParams ToonDefaultParams()
{
    return ToonLightParams(3, 0.15, 0.15, 0.25, 0.25, 1.0, 1.0, 2.0);
}

float ToonDiffuseRamp(float cosine, ToonLightParams p)
{
    if (cosine <= 0.0) return 0.0;
    float steps = float(max(1, p.Steps));
    float threshold = clamp(p.Threshold, 0.0, 1.0);
    if (threshold == 1.0) return step(1.0, cosine);

    float band = (min(cosine, 1.0) - threshold) * steps / (1.0 - threshold);
    if (p.Softness <= 0.0) return clamp(floor(band) + 1.0, 0.0, steps) / steps;

    float width = 0.5 * min(p.Softness, 1.0);
    float boundary = floor(band + 0.5);
    float blend = smoothstep(-width, width, band - boundary);
    return clamp(boundary + blend, 0.0, steps) / steps;
}

float ToonKeyShadow(vec3 worldPos, vec3 normal, float softness)
{
    float visibility = KansSampleDirectionalLight(worldPos, normalize(normal)).Visibility;
    if (softness <= 0.0) return step(0.5, visibility);
    return clamp((visibility - 0.5) / min(softness, 1.0) + 0.5, 0.0, 1.0);
}

vec3 ToonAdditionalLights(vec3 worldPos, vec3 normal, vec3 viewDir, ToonLightParams p)
{
    vec3 N = normalize(normal);
    vec3 V = normalize(viewDir);
    float rimWidth = clamp(p.RimWidth, 0.0, 1.0);
    float rim = rimWidth > 0.0 ?
        max(p.RimStrength, 0.0) * (1.0 - smoothstep(0.0, rimWidth, max(dot(N, V), 0.0))) : 0.0;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < KansPunctualLightCount(); ++i)
    {
        KansLightSample light = KansSamplePunctualLight(i, worldPos, N);
        float diffuse = ToonDiffuseRamp(dot(N, light.ToLight), p);
        float backlight = rim * max(-dot(V, light.ToLight), 0.0);
        sum += light.Radiance * light.DiffuseScale * (diffuse + backlight);
    }
    for (int i = 0; i < KansAreaLightCount(); ++i)
    {
        vec3 L = KansAreaLightToLight(i, worldPos);
        float diffuse = ToonDiffuseRamp(dot(N, L), p);
        float backlight = rim * max(-dot(V, L), 0.0);
        if (diffuse > 0.0)
            sum += KansAreaLightDiffuseIrradiance(i, worldPos, N) * diffuse;
        // Measure the emitter's contribution facing its centre for a backlit rim.
        if (backlight > 0.0)
            sum += KansAreaLightDiffuseIrradiance(i, worldPos, L) * backlight;
    }
    float luminance = dot(sum, vec3(0.2126, 0.7152, 0.0722));
    vec3 color = mix(vec3(luminance), sum, clamp(p.Saturation, 0.0, 1.0)) * max(p.Intensity, 0.0);
    float peak = max(color.r, max(color.g, color.b));
    float limit = max(p.MaxBrightness, 0.0);
    return peak > limit ? color * (limit / peak) : color;
}

#endif
