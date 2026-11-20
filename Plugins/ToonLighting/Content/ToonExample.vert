#version 450
#include <Kans3D/Engine.glsl>
#include <Kans3D/VertexInput.glsl>

layout(location = 0) out vec3 WorldPosition;
layout(location = 1) out vec3 WorldNormal;

void main()
{
    vec4 world = u_Object.Transform * vec4(a_Position, 1.0);
    WorldPosition = world.xyz;
    WorldNormal = mat3(u_Object.NormalMatrix) * a_Normal;
    gl_Position = u_Frame.ViewProjection * world;
}
