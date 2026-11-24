#ifndef KANS_TOON_SURFACE_GLSL
#define KANS_TOON_SURFACE_GLSL

#include <ToonLighting/AdditionalLights.glsl>

struct ToonSurfaceParams
{
    int Steps;             // Number of nonzero bands for the normal-based ramp, at least 1.
    float Threshold;       // N dot L at the first band, in [0,1].
    float Softness;        // Fraction of band spacing, or cosine width for face thresholds.
    float AmbientStrength;
    float ShadeFloor;      // Minimum ambient shade multiplier, in [0,1].
    float LightTint;       // 0 = luminance only, 1 = full RGB multiply.
    float MainBlendBand;   // Relative score gap over which to blend the two strongest lights.
    float MaxElevation;    // Main-light elevation in degrees relative to world XZ; 0 disables.
    float FillWrap;         // Wrapped diffuse offset, in [0,1].
    float FillStrength;
    float RimStrength;
    float RimWidth;        // Silhouette width in N dot V, in [0,1]; 0 disables.
    float MaxBrightness;   // Maximum linear HDR channel; 0 disables.
    int MaxLights;         // 0 = all; otherwise directional, then points, spots and areas.
};

/*
Imported lighting graphs can keep their own ramps, light maps and highlights.

    ToonMainLight m = ToonSelectMainLight(u_Object.Transform[3].xyz, p);
    vec3 lit = MyGraph(m.Direction);
    vec3 shade = MyGraph(-m.Direction);
    float vis = ToonMainLightVisibility(m, worldPos, N);
    vec3 base = mix(shade, lit, vis) * m.Radiance + shade * ambient;
    vec3 colour = base + ToonFillLift(worldPos, N, V, base, lit, m, p);

Radiance is tinted incident RGB at the reference point before shadowing.
The graph can use it as an RGB multiplier or use just its luminance.
Use the same reference point and parameters across the object.
*/
struct ToonMainLight
{
    vec3 Direction;
    vec3 Radiance;
    bool Found;
    // Forward light indices and runner-up weight for subsequent fragment queries.
    int First;
    int Second;
    float SecondWeight;
};

ToonSurfaceParams ToonSurfaceDefaultParams()
{
    return ToonSurfaceParams(3, 0.15, 0.15, 1.0, 0.15, 0.3, 0.15, 0.0, 0.25, 0.35, 0.0, 0.25, 0.0, 0);
}

const vec3 TOON_SURFACE_LUMA = vec3(0.2126, 0.7152, 0.0722);
const mat3 TOON_SURFACE_WORLD_FRAME = mat3(vec3(0, 0, 1), vec3(0, 1, 0), vec3(1, 0, 0));

vec3 ToonSurfaceUnit(vec3 direction, vec3 fallback)
{
    float lengthSquared = dot(direction, direction);
    return lengthSquared > 1e-12 ? direction * inversesqrt(lengthSquared) : fallback;
}

float ToonSurfaceRamp(vec3 N, vec3 L, ToonLightParams ramp, bool useThresholds, mat3 head, vec2 thresholds)
{
    if (!useThresholds) return ToonDiffuseRamp(dot(N, L), ramp);

    vec3 horizontal = L - head[1] * dot(head[1], L);
    // Straight above/below has no azimuth. Use the frontal limit.
    float frontness = dot(head[0], ToonSurfaceUnit(horizontal, head[0]));
    if (frontness < 0.0) return 0.0;
    float threshold = dot(head[2], horizontal) < 0.0 ? thresholds.y : thresholds.x;
    float width = 0.5 * clamp(ramp.Softness, 0.0, 1.0);
    return width > 0.0 ? smoothstep(-width, width, frontness - threshold) : step(threshold, frontness);
}

vec3 ToonSurfaceClampElevation(vec3 L, float maxElevation, vec3 forward)
{
    if (maxElevation <= 0.0) return L;
    float angle = radians(min(maxElevation, 90.0));
    float limit = sin(angle);
    if (abs(L.y) <= limit) return L;

    vec3 horizontal = ToonSurfaceUnit(vec3(L.x, 0.0, L.z),
        ToonSurfaceUnit(vec3(forward.x, 0.0, forward.z), TOON_SURFACE_WORLD_FRAME[0]));
    return horizontal * cos(angle) + vec3(0.0, sign(L.y) * limit, 0.0);
}

struct ToonSurfaceLights
{
    int Points;
    int Punctual;
    bool HasDirectional;
    int Count;
};

ToonSurfaceLights ToonSurfaceForwardLights(int maxLights)
{
    int points = KansPointLightCount();
    int punctual = points + KansSpotLightCount();
    bool hasDirectional = u_Frame.DirectionalDirection.w != 0.0;
    int count = int(hasDirectional) + punctual + KansAreaLightCount();
    if (maxLights > 0) count = min(count, maxLights);
    return ToonSurfaceLights(points, punctual, hasDirectional, count);
}

KansLightSample ToonSurfaceIncident(int index, ToonSurfaceLights lights,
                                   vec3 position, vec3 normal, bool referenceSample)
{
    if (lights.HasDirectional && index == 0) return KansDirectionalIncident(position, normal, referenceSample);
    int i = index - int(lights.HasDirectional);
    if (i < lights.Points) return KansPointIncident(i, position, normal, referenceSample);
    if (i < lights.Punctual) return KansSpotIncident(i - lights.Points, position, normal, referenceSample);

    int area = i - lights.Punctual;
    vec3 L = KansAreaLightToLight(area, position);
    // The centre-facing finite-emitter integral includes diffuse scale.
    vec3 incident = L == vec3(0.0) ? vec3(0.0) : KansAreaLightDiffuseIrradiance(area, position, L);
    return KansLightSample(L, 0.0, incident, 1.0, 1.0);
}

vec3 ToonSurfaceDiffuseRadiance(KansLightSample light)
{
    return max(light.Radiance * light.DiffuseScale, vec3(0.0));
}

vec3 ToonSurfaceTint(vec3 radiance, float luminance, float tint)
{
    return mix(vec3(luminance), radiance, tint);
}

vec3 ToonSurfaceBlendDirections(vec3 first, vec3 second, float weight)
{
    vec3 blended = mix(first, second, weight);
    vec3 midpoint = first + second;
    if (dot(midpoint, midpoint) <= 1e-12)
    {
        // Take a perpendicular detour for opposing directions. Reversing the chord
        // leaves the projected axis unchanged, so the detour survives a rank swap.
        vec3 chord = normalize(first - second);
        vec3 axis = abs(chord.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
        vec3 perpendicular = normalize(axis - chord * dot(axis, chord));
        blended += (4.0 * weight * (1.0 - weight)) * perpendicular;
    }
    return ToonSurfaceUnit(blended, TOON_SURFACE_WORLD_FRAME[0]);
}

// With no incident main light, Found is false, Radiance is zero and Direction is +Z.
ToonMainLight ToonSelectMainLight(vec3 referencePoint, ToonSurfaceParams p)
{
    ToonSurfaceLights lights = ToonSurfaceForwardLights(p.MaxLights);
    ToonMainLight mainLights = ToonMainLight(TOON_SURFACE_WORLD_FRAME[0], vec3(0.0), false, -1, -1, 0.0);
    vec2 scores = vec2(0.0);
    int fallback = -1;
    float fallbackScore = 0.0;
    KansLightSample firstSample, secondSample, fallbackSample;
    for (int i = 0; i < lights.Count; ++i)
    {
        // The reference uses a plane facing the light for stable shadow-map visibility.
        KansLightSample light = ToonSurfaceIncident(i, lights, referencePoint, vec3(0, 1, 0), true);
        float intensity = dot(ToonSurfaceDiffuseRadiance(light), TOON_SURFACE_LUMA);
        float score = intensity * light.Visibility;
        if (score > scores.x)
        {
            if (mainLights.First >= 0) secondSample = firstSample;
            mainLights.Second = mainLights.First;
            scores.y = scores.x;
            mainLights.First = i;
            firstSample = light;
            scores.x = score;
        }
        else if (score > scores.y)
        {
            mainLights.Second = i;
            secondSample = light;
            scores.y = score;
        }
        if (intensity > fallbackScore)
        {
            fallback = i;
            fallbackSample = light;
            fallbackScore = intensity;
        }
    }
    // If every score is zero, an incident light can still shade exposed fragments.
    if (mainLights.First < 0)
    {
        if (fallback < 0) return mainLights;
        mainLights.First = fallback;
        firstSample = fallbackSample;
    }
    float band = clamp(p.MainBlendBand, 0.0, 1.0);
    if (mainLights.Second >= 0 && band > 0.0)
        mainLights.SecondWeight = 0.5 * (1.0 - smoothstep(0.0, band, 1.0 - scores.y / scores.x));

    mainLights.Found = true;
    vec3 direction = ToonSurfaceUnit(firstSample.ToLight, TOON_SURFACE_WORLD_FRAME[0]);
    float tint = clamp(p.LightTint, 0.0, 1.0);
    vec3 radiance = ToonSurfaceDiffuseRadiance(firstSample);
    mainLights.Radiance = ToonSurfaceTint(radiance, dot(radiance, TOON_SURFACE_LUMA), tint);
    if (mainLights.SecondWeight > 0.0)
    {
        direction = ToonSurfaceBlendDirections(direction, secondSample.ToLight, mainLights.SecondWeight);
        vec3 secondRadiance = ToonSurfaceDiffuseRadiance(secondSample);
        mainLights.Radiance = mix(mainLights.Radiance,
            ToonSurfaceTint(secondRadiance, dot(secondRadiance, TOON_SURFACE_LUMA), tint), mainLights.SecondWeight);
    }
    mainLights.Direction = ToonSurfaceClampElevation(direction, p.MaxElevation, TOON_SURFACE_WORLD_FRAME[0]);
    return mainLights;
}

// Returns filtered visibility, with 1 for an absent or unshadowed main light.
float ToonMainLightVisibility(ToonMainLight m, vec3 worldPos, vec3 normal)
{
    if (!m.Found) return 1.0;
    ToonSurfaceLights lights = ToonSurfaceForwardLights(0);
    vec3 N = ToonSurfaceUnit(normal, TOON_SURFACE_WORLD_FRAME[0]);
    float first = ToonSurfaceIncident(m.First, lights, worldPos, N, false).Visibility;
    if (m.SecondWeight <= 0.0) return first;
    float second = ToonSurfaceIncident(m.Second, lights, worldPos, N, false).Visibility;
    return mix(first, second, m.SecondWeight);
}

vec3 ToonSurfaceWrappedFill(KansLightSample light, vec3 N, vec3 tintedRadiance, float wrap, float weight)
{
    float wrappedDiffuse = clamp((dot(N, light.ToLight) + wrap) / (1.0 + wrap), 0.0, 1.0);
    return weight * wrappedDiffuse * light.Visibility * tintedRadiance;
}

vec3 ToonSurfaceBoundedLift(vec3 baseColour, vec3 litColour, vec3 fill, float strength)
{
    return max(litColour - baseColour, vec3(0.0)) * clamp(strength * fill, 0.0, 1.0);
}

// A positive blend weight gives both main lights to the caller's graph.
// The separate TL2 rim is evaluated by the surface wrappers.
vec3 ToonFillLift(vec3 worldPos, vec3 normal, vec3 viewDir, vec3 baseColour, vec3 litColour,
                  ToonMainLight m, ToonSurfaceParams p)
{
    float strength = max(p.FillStrength, 0.0);
    if (strength == 0.0) return vec3(0.0);
    ToonSurfaceLights lights = ToonSurfaceForwardLights(p.MaxLights);
    vec3 N = ToonSurfaceUnit(normal, TOON_SURFACE_WORLD_FRAME[0]);
    float tint = clamp(p.LightTint, 0.0, 1.0);
    float wrap = clamp(p.FillWrap, 0.0, 1.0);
    vec3 fill = vec3(0.0);
    for (int i = 0; i < lights.Count; ++i)
    {
        if (i == m.First || (m.SecondWeight > 0.0 && i == m.Second)) continue;
        KansLightSample light = ToonSurfaceIncident(i, lights, worldPos, N, false);
        vec3 radiance = ToonSurfaceDiffuseRadiance(light);
        float intensity = dot(radiance, TOON_SURFACE_LUMA);
        if (intensity <= 0.0) continue;
        fill += ToonSurfaceWrappedFill(light, N, ToonSurfaceTint(radiance, intensity, tint), wrap, 1.0);
    }
    return ToonSurfaceBoundedLift(baseColour, litColour, fill, strength);
}

vec3 ToonSurfaceCapBrightness(vec3 colour, float maxBrightness)
{
    float peak = max(colour.r, max(colour.g, colour.b));
    return maxBrightness > 0.0 && peak > maxBrightness ? colour * (maxBrightness / peak) : colour;
}

vec3 ToonSurfaceEvaluate(vec3 worldPos, vec3 referencePoint, vec3 normal, vec3 viewDir,
                         vec3 litColour, vec3 shadeColour, ToonSurfaceParams p,
                         mat3 head, float flatStrength, bool useThresholds, vec2 thresholds)
{
    vec3 geometricNormal = ToonSurfaceUnit(normal, TOON_SURFACE_WORLD_FRAME[0]);
    vec3 N = ToonSurfaceUnit(mix(geometricNormal, head[0], clamp(flatStrength, 0.0, 1.0)), head[0]);
    vec3 V = ToonSurfaceUnit(viewDir, geometricNormal);
    float rimWidth = clamp(p.RimWidth, 0.0, 1.0);
    float silhouette = rimWidth > 0.0 ?
        max(p.RimStrength, 0.0) * (1.0 - smoothstep(0.0, rimWidth, max(dot(geometricNormal, V), 0.0))) : 0.0;
    vec3 ambient = vec3(0.0);
    if (p.AmbientStrength > 0.0)
        ambient = shadeColour * KansAmbientIrradiance(N) * p.AmbientStrength;
    vec3 base = max(ambient, shadeColour * clamp(p.ShadeFloor, 0.0, 1.0));

    ToonSurfaceLights lights = ToonSurfaceForwardLights(p.MaxLights);
    ToonMainLight mainLights = ToonSelectMainLight(referencePoint, p);

    float tint = clamp(p.LightTint, 0.0, 1.0);
    float wrap = clamp(p.FillWrap, 0.0, 1.0);
    float fillStrength = max(p.FillStrength, 0.0);
    ToonLightParams ramp = ToonLightParams(p.Steps, p.Threshold, p.Softness, 0.0, 0.0, 1.0, 1.0, 0.0);
    vec3 fill = vec3(0.0);
    vec3 rim = vec3(0.0);
    float strongestBacklight = 0.0;
    for (int i = 0; i < lights.Count; ++i)
    {
        KansLightSample light = ToonSurfaceIncident(i, lights, worldPos, geometricNormal, false);
        vec3 radiance = ToonSurfaceDiffuseRadiance(light);
        float intensity = dot(radiance, TOON_SURFACE_LUMA);
        if (intensity <= 0.0) continue;

        vec3 tintedRadiance = ToonSurfaceTint(radiance, intensity, tint);
        float mainWeight = i == mainLights.First ? 1.0 - mainLights.SecondWeight :
            (i == mainLights.Second ? mainLights.SecondWeight : 0.0);
        if (mainWeight > 0.0)
        {
            vec3 mainDirection = ToonSurfaceClampElevation(light.ToLight, p.MaxElevation, head[0]);
            float band = ToonSurfaceRamp(N, mainDirection, ramp, useThresholds, head, thresholds);
            float litWeight = band * step(0.5, light.Visibility);
            base += mainWeight * mix(shadeColour * intensity, litColour * tintedRadiance, litWeight);
        }
        if (mainWeight < 1.0 && fillStrength > 0.0)
            fill += ToonSurfaceWrappedFill(light, N, tintedRadiance, wrap, 1.0 - mainWeight);
        if (silhouette > 0.0)
        {
            float backlight = max(-dot(V, light.ToLight), 0.0);
            float score = intensity * backlight;
            if (score > strongestBacklight)
            {
                strongestBacklight = score;
                rim = radiance * backlight * light.Visibility;
            }
        }
    }

    vec3 colour = base + ToonSurfaceBoundedLift(base, litColour, fill, fillStrength) + silhouette * rim;
    return ToonSurfaceCapBrightness(colour, p.MaxBrightness);
}

// Use the same world-space reference point for every fragment of an object.
// Colours are linear HDR. viewDir points towards the camera.
vec3 ToonSurface(vec3 worldPos, vec3 referencePoint, vec3 normal, vec3 viewDir,
                 vec3 litColour, vec3 shadeColour, ToonSurfaceParams p)
{
    return ToonSurfaceEvaluate(worldPos, referencePoint, normal, viewDir, litColour, shadeColour, p,
        TOON_SURFACE_WORLD_FRAME, 0.0, false, vec2(0.0));
}

// The orthonormal world-space head frame has forward, up and right columns.
// flatStrength can include a caller-sampled strength texture channel.
vec3 ToonSurfaceFace(vec3 worldPos, vec3 referencePoint, vec3 normal, vec3 viewDir,
                     vec3 litColour, vec3 shadeColour, ToonSurfaceParams p, mat3 head, float flatStrength)
{
    return ToonSurfaceEvaluate(worldPos, referencePoint, normal, viewDir, litColour, shadeColour, p,
        head, flatStrength, false, vec2(0.0));
}

// Thresholds are minimum horizontal light cosines for the right and left sides.
vec3 ToonSurfaceFace(vec3 worldPos, vec3 referencePoint, vec3 normal, vec3 viewDir,
                     vec3 litColour, vec3 shadeColour, ToonSurfaceParams p,
                     mat3 head, float flatStrength, vec2 thresholds)
{
    return ToonSurfaceEvaluate(worldPos, referencePoint, normal, viewDir, litColour, shadeColour, p,
        head, flatStrength, true, clamp(thresholds, 0.0, 1.0));
}

// Pass face UVs in [0,1] and a linear red-channel threshold map. Mirroring U
// supplies the second side. Read the two thresholds outside the light loops.
vec3 ToonSurfaceFace(vec3 worldPos, vec3 referencePoint, vec3 normal, vec3 viewDir,
                     vec3 litColour, vec3 shadeColour, ToonSurfaceParams p,
                     mat3 head, float flatStrength, sampler2D thresholdMap, vec2 uv)
{
    vec2 thresholds = vec2(texture(thresholdMap, uv).r, texture(thresholdMap, vec2(1.0 - uv.x, uv.y)).r);
    return ToonSurfaceFace(worldPos, referencePoint, normal, viewDir, litColour, shadeColour, p,
        head, flatStrength, thresholds);
}

#endif
