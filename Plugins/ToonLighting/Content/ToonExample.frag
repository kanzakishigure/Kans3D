#version 450
#include <ToonLighting/AdditionalLights.glsl>

layout(location = 0) in vec3 WorldPosition;
layout(location = 1) in vec3 WorldNormal;
layout(location = 0) out vec4 Color;

void main()
{
    vec3 baseColor = vec3(0.55, 0.65, 0.8);
    vec3 N = normalize(WorldNormal);
    vec3 V = normalize(u_Frame.CameraPosition.xyz - WorldPosition);
    ToonLightParams p = ToonDefaultParams();

    vec3 key = KansDirectionalToLight();
    float keyRamp = step(0.35, dot(N, key)) * ToonKeyShadow(WorldPosition, N, p.Softness);
    vec3 color = baseColor * (0.15 + 0.45 * keyRamp);
    color += baseColor * ToonAdditionalLights(WorldPosition, N, V, p);
    Color = vec4(color, 1.0);
}
