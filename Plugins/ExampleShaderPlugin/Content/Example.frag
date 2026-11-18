#version 450
#include <ExampleShaderPlugin/ExampleColor.glsl>
layout(location=0) out vec4 Color;
void main() { Color = ExamplePluginColor(); }
