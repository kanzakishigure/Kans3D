#version 450
#include <ToonLighting/ToonSurface.glsl>

layout(location = 0) in vec3 WorldPosition;
layout(location = 1) in vec3 WorldNormal;
layout(location = 0) out vec4 Color;

layout(set = 1, binding = 0, std140) uniform ToonMaterial
{
    vec3 LitColour;
    vec3 ShadeColour;
} u_Toon;

// A material can replace this graph with its own ramps and light maps.
vec3 MyGraph(vec3 N, vec3 lightDirection)
{
    ToonLightParams ramp = ToonDefaultParams();
    ramp.Steps = 2;
    return mix(u_Toon.ShadeColour, u_Toon.LitColour, ToonDiffuseRamp(dot(N, lightDirection), ramp));
}

void main()
{
    vec3 N = ToonSurfaceUnit(WorldNormal, vec3(0, 0, 1));
    vec3 viewDir = u_Frame.CameraPosition.xyz - WorldPosition;
    vec3 referencePoint = u_Object.Transform[3].xyz;
    ToonSurfaceParams p = ToonSurfaceDefaultParams();
    p.MaxElevation = 40.0;
    p.MaxBrightness = 2.0;
    ToonMainLight m = ToonSelectMainLight(referencePoint, p);
    vec3 lit = MyGraph(N, m.Direction);
    vec3 shade = m.Found ? MyGraph(N, -m.Direction) : u_Toon.ShadeColour;
    float vis = ToonMainLightVisibility(m, WorldPosition, N);
    vec3 ambient = max(shade * p.AmbientStrength * KansAmbientIrradiance(N), shade * p.ShadeFloor);
    vec3 base = ambient + mix(shade * dot(m.Radiance, TOON_SURFACE_LUMA), lit * m.Radiance, vis);
    vec3 colour = base + ToonFillLift(WorldPosition, N, viewDir, base, lit, m, p);
    Color = vec4(ToonSurfaceCapBrightness(colour, p.MaxBrightness), 1.0);
}
