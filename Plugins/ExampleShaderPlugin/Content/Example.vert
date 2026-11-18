#version 450
#include <Kans3D/Engine.glsl>
#include <Kans3D/VertexInput.glsl>
void main() { gl_Position = u_Frame.ViewProjection * u_Object.Transform * vec4(a_Position, 1); }
