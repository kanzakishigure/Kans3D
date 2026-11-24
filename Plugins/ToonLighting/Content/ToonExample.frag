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

void main()
{
    vec3 viewDir = u_Frame.CameraPosition.xyz - WorldPosition;
    ToonSurfaceParams p = ToonSurfaceDefaultParams();
    Color = vec4(ToonSurface(WorldPosition, WorldNormal, viewDir, u_Toon.LitColour, u_Toon.ShadeColour, p), 1.0);
}
