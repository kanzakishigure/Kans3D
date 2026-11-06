#pragma once
// Engine-owned compute GLSL 450; no filesystem or editor resource dependency.
namespace Kans::ComputeShaders
{
    inline constexpr const char* CompactAlive = R"glsl(#version 450
// Stable per-workgroup prefix scan, then atomic reservation of a compact block.
// Fill count, draw args and dispatch args with zero before scan. Dispatch at
// least one group even for empty input, to initialise the constant argument fields.
// Group reservations and atomic maxima produce final arguments without spin waits.
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Flags { uint alive[]; };
layout(set=0,binding=1,std430) buffer Indices { uint indices[]; };
layout(set=0,binding=2,std430) buffer Count { uint count; };
layout(set=0,binding=3,std430) buffer DrawArgs { uint vertexCount; uint instanceCount; uint firstVertex; uint firstInstance; } drawArgs;
layout(set=0,binding=4,std430) buffer DispatchArgs { uint x; uint y; uint z; } dispatchArgs;
layout(push_constant) uniform Parameters { uint elements; } p;
shared uint prefix[64];
shared uint base;
void main() {
    uint lane = gl_LocalInvocationIndex;
    uint i = gl_GlobalInvocationID.x;
    uint flag = i < p.elements ? uint(alive[i] != 0) : 0;
    prefix[lane] = flag;
    barrier();
    for (uint step=1; step<64; step*=2) {
        uint previous = lane >= step ? prefix[lane-step] : 0;
        barrier();
        prefix[lane] += previous;
        barrier();
    }
    if (lane==0) {
        base=atomicAdd(count,prefix[63]);
        uint end=base+prefix[63];
        atomicMax(drawArgs.instanceCount,end);
        atomicMax(dispatchArgs.x,end/64+uint(end%64!=0));
    }
    if (i==0) {
        drawArgs.vertexCount=6; drawArgs.firstVertex=0; drawArgs.firstInstance=0;
        dispatchArgs.y=1; dispatchArgs.z=1;
    }
    barrier();
    if (flag!=0) indices[base+prefix[lane]-1]=i;
}
)glsl";

    inline constexpr const char* CompactAliveFinalize = R"glsl(#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) readonly buffer Count { uint count; };
layout(set=0,binding=1,std430) buffer DrawArgs { uint vertexCount; uint instanceCount; uint firstVertex; uint firstInstance; } drawArgs;
layout(set=0,binding=2,std430) buffer DispatchArgs { uint x; uint y; uint z; } dispatchArgs;
void main() {
    drawArgs.vertexCount=6; drawArgs.instanceCount=count; drawArgs.firstVertex=0; drawArgs.firstInstance=0;
    dispatchArgs.x=count/64+uint(count%64!=0); dispatchArgs.y=1; dispatchArgs.z=1;
}
)glsl";
} // namespace Kans::ComputeShaders
