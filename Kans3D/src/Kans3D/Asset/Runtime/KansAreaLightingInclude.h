R"(#ifndef KANS3D_AREA_LIGHTING_GLSL
#define KANS3D_AREA_LIGHTING_GLSL
// Heitz et al., Real-Time Polygonal-Light Shading with Linearly Transformed Cosines,
// SIGGRAPH 2016. GGX tables: Three.js MIT mirror of selfshadow/ltc_code; see LtcTables.h.
layout(set=0,binding=18) uniform sampler2D u_LtcMatrix;
layout(set=0,binding=19) uniform sampler2D u_LtcAmplitude;
layout(set=0,binding=20) uniform sampler2D u_AreaSource0;
layout(set=0,binding=21) uniform sampler2D u_AreaSource1;
layout(set=0,binding=22) uniform sampler2D u_AreaSource2;
layout(set=0,binding=23) uniform sampler2D u_AreaSource3;
layout(set=0,binding=24) uniform sampler2D u_AreaSource4;
layout(set=0,binding=25) uniform sampler2D u_AreaSource5;
layout(set=0,binding=26) uniform sampler2D u_AreaSource6;
layout(set=0,binding=27) uniform sampler2D u_AreaSource7;
layout(set=0,binding=28) uniform sampler2D u_AreaSource8;
layout(set=0,binding=29) uniform sampler2D u_AreaSource9;
layout(set=0,binding=30) uniform sampler2D u_AreaSource10;
layout(set=0,binding=31) uniform sampler2D u_AreaSource11;
layout(set=0,binding=32) uniform sampler2D u_AreaSource12;
layout(set=0,binding=33) uniform sampler2D u_AreaSource13;
layout(set=0,binding=34) uniform sampler2D u_AreaSource14;
layout(set=0,binding=35) uniform sampler2D u_AreaSource15;
int KansAreaLightCount() { return clamp(int(u_Frame.LightingInfo.w),0,KANS_AREA_LIGHT_LIMIT); }
vec3 KansAreaTexture(int i, vec2 uv, float lod)
{
    // Individual bindings avoid descriptor indexing/non-uniform indexing requirements.
    switch(i) {
    case 0: return textureLod(u_AreaSource0,uv,lod).rgb;
    case 1: return textureLod(u_AreaSource1,uv,lod).rgb;
    case 2: return textureLod(u_AreaSource2,uv,lod).rgb;
    case 3: return textureLod(u_AreaSource3,uv,lod).rgb;
    case 4: return textureLod(u_AreaSource4,uv,lod).rgb;
    case 5: return textureLod(u_AreaSource5,uv,lod).rgb;
    case 6: return textureLod(u_AreaSource6,uv,lod).rgb;
    case 7: return textureLod(u_AreaSource7,uv,lod).rgb;
    case 8: return textureLod(u_AreaSource8,uv,lod).rgb;
    case 9: return textureLod(u_AreaSource9,uv,lod).rgb;
    case 10: return textureLod(u_AreaSource10,uv,lod).rgb;
    case 11: return textureLod(u_AreaSource11,uv,lod).rgb;
    case 12: return textureLod(u_AreaSource12,uv,lod).rgb;
    case 13: return textureLod(u_AreaSource13,uv,lod).rgb;
    case 14: return textureLod(u_AreaSource14,uv,lod).rgb;
    case 15: return textureLod(u_AreaSource15,uv,lod).rgb;
    }
    return vec3(1);
}
// Untextured finite-emitter Lambert approximation used by particles and VFX.
vec3 KansAreaApproxRadiance(int i, vec3 P)
{
    vec3 delta = u_Frame.AreaPositionRange[i].xyz-P;
    float d2 = dot(delta,delta);
    if (d2 <= 1e-8) return vec3(0);
    float facing = dot(cross(u_Frame.AreaRightWidth[i].xyz,u_Frame.AreaUpHeight[i].xyz),delta)*inversesqrt(d2);
    if (u_Frame.AreaScales[i].z > 0.5) facing = abs(facing);
    float area = u_Frame.AreaRightWidth[i].w*u_Frame.AreaUpHeight[i].w;
    if (u_Frame.AreaRadiance[i].w > 0.5) area *= 0.78539816339;
    float form = area / (3.14159265359*d2+area);
    return u_Frame.AreaRadiance[i].rgb*u_Frame.AreaScales[i].x*max(0.0,facing)*form*
        KansRangeWindow(d2,u_Frame.AreaPositionRange[i].w);
}
mat3 KansAreaFrame(vec3 N, vec3 V)
{
    vec3 T = V - N * dot(N,V);
    if (dot(T,T) < 1e-8)
        T = cross(abs(N.z) < 0.99 ? vec3(0,0,1) : vec3(0,1,0),N);
    T = normalize(T);
    return transpose(mat3(T,cross(N,T),N));
}
vec3 KansAreaVertex(int i, int vertex, vec3 P)
{
    vec4 x = u_Frame.AreaRightWidth[i], y = u_Frame.AreaUpHeight[i];
    vec2 corner;
    if (u_Frame.AreaRadiance[i].w > 0.5) {
        float angle = float(vertex) * (6.28318530718/32.0);
        corner = vec2(cos(angle),sin(angle));
    } else {
        // CCW in local XY. Winding is handled after clipping; local -Z is the emitter front.
        corner = vertex == 0 ? vec2(-1,-1) : vertex == 1 ? vec2(1,-1) :
                 vertex == 2 ? vec2(1,1) : vec2(-1,1);
    }
    return u_Frame.AreaPositionRange[i].xyz - P + 0.5 * (corner.x*x.xyz*x.w + corner.y*y.xyz*y.w);
}
float KansAreaEdge(vec3 a, vec3 b)
{
    float aa=dot(a,a), bb=dot(b,b);
    if (min(aa,bb) < 1e-20) return 0.0;
    a *= inversesqrt(aa); b *= inversesqrt(bb);
    vec3 c = cross(a,b);
    float len = length(c);
    return len > 1e-7 ? c.z * atan(len,clamp(dot(a,b),-1.0,1.0)) / len : c.z;
}
float KansAreaIntegral(int i, vec3 P, mat3 transform)
{
    // Sutherland-Hodgman against z=0 before normalization, equivalent to the
    // reference horizon clipping. A convex polygon acquires at most one extra vertex.
    int count = u_Frame.AreaRadiance[i].w > 0.5 ? 32 : 4;
    vec3 clipped[33];
    int n=0;
    vec3 a = transform * KansAreaVertex(i,count-1,P);
    for (int j=0; j<count; ++j) {
        vec3 b = transform * KansAreaVertex(i,j,P);
        if ((a.z > 0.0) != (b.z > 0.0))
            clipped[n++] = mix(a,b,a.z/(a.z-b.z));
        if (b.z > 0.0) clipped[n++] = b;
        a=b;
    }
    if (n < 3) return 0.0;
    float sum=0.0;
    for (int j=0; j<n; ++j) sum += KansAreaEdge(clipped[j],clipped[(j+1)%n]);
    // Integral of normalized cosine, i.e. irradiance/pi. Check physical sidedness
    // in world space, independently of the LTC transform's horizon.
    return clamp(abs(sum) / 6.28318530718,0.0,1.0);
}
vec3 KansAreaFilteredSource(int i, vec3 P, mat3 transform)
{
    if (u_Frame.AreaScales[i].w < 0.5 || u_Frame.AreaRadiance[i].w > 0.5) return vec3(1);
    // Textured-polygon LTC approximation: project the origin onto the transformed
    // emitter plane, solve its (possibly sheared) UV basis, and filter by projected
    // footprint area. Apply independently in diffuse and GGX LTC space.
    vec3 p = transform * KansAreaVertex(i,0,P);
    vec3 ex = transform * (u_Frame.AreaRightWidth[i].xyz*u_Frame.AreaRightWidth[i].w);
    vec3 ey = transform * (u_Frame.AreaUpHeight[i].xyz*u_Frame.AreaUpHeight[i].w);
    vec3 normal = cross(ex,ey);
    float area = length(normal);
    if (area < 1e-10) return vec3(1);
    normal /= area;
    float distance = dot(p,normal);
    vec3 q = normal*distance - p;
    float xx=dot(ex,ex), yy=dot(ey,ey), xy=dot(ex,ey);
    vec2 uv = vec2(dot(q,ex)*yy-dot(q,ey)*xy, dot(q,ey)*xx-dot(q,ex)*xy) / max(xx*yy-xy*xy,1e-20);
    vec3 size = u_Frame.AreaSourceSize[i].xyz;
    // The cosine kernel subtends a footprint proportional to distance squared.
    // Mips are pre-filtered linear source colour; clamp to the supplied mip chain.
    float lod = 0.5 * log2(max(1.0,size.x*size.y*distance*distance/area));
    return KansAreaTexture(i,clamp(uv,vec2(0),vec2(1)),clamp(lod,0.0,max(0.0,size.z-1.0)));
}
vec3 KansAreaPbr(int i, vec3 albedo, float metallic, float roughness, vec3 N, vec3 V, vec3 P)
{
    vec3 delta = u_Frame.AreaPositionRange[i].xyz-P;
    vec3 normal = -cross(u_Frame.AreaRightWidth[i].xyz,u_Frame.AreaUpHeight[i].xyz);
    if (dot(normal,-delta) <= 0.0 && u_Frame.AreaScales[i].z < 0.5) return vec3(0);
    float window = KansRangeWindow(dot(delta,delta),u_Frame.AreaPositionRange[i].w);
    if (window <= 0.0) return vec3(0);
    mat3 frame = KansAreaFrame(N,V);
    vec2 uv = vec2(roughness,sqrt(1.0-clamp(dot(N,V),0.0,1.0))) * (63.0/64.0) + 0.5/64.0;
    vec4 t = textureLod(u_LtcMatrix,uv,0);
    vec2 amplitude = textureLod(u_LtcAmplitude,uv,0).xy;
    mat3 invM = mat3(vec3(t.x,0,t.y),vec3(0,1,0),vec3(t.z,0,t.w));
    mat3 specFrame = invM*frame;
    vec3 F0 = mix(vec3(0.04),albedo,metallic);
    vec3 diffuse = (1.0-F0)*(1.0-metallic)*albedo * u_Frame.AreaScales[i].x *
        KansAreaIntegral(i,P,frame) * KansAreaFilteredSource(i,P,frame);
    vec3 specular = (F0*amplitude.x+(1.0-F0)*amplitude.y) * u_Frame.AreaScales[i].y *
        KansAreaIntegral(i,P,specFrame) * KansAreaFilteredSource(i,P,specFrame);
    // Area emitters are unshadowed in R3b.
    return (diffuse+specular)*u_Frame.AreaRadiance[i].rgb*window;
}
#endif
)"
