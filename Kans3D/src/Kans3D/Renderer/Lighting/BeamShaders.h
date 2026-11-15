#pragma once
namespace Kans::BeamShaders
{
    inline constexpr const char* Vertex            = R"(#version 450
void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,0,1);}
)";
    inline constexpr const char* March             = R"(
struct Beam { vec4 indexLengthScaleSamples; vec4 fadesFlags; };
layout(set=1,binding=4,std140) uniform Beams { Beam beams[16]; vec4 renderSize; };
#ifdef MULTISAMPLE_DEPTH
layout(set=1,binding=5) uniform sampler2DMS sceneDepth;
float readDepth(ivec2 p){return texelFetch(sceneDepth,p,0).r;}
#else
layout(set=1,binding=5) uniform sampler2D sceneDepth;
float readDepth(ivec2 p){return texelFetch(sceneDepth,p,0).r;}
#endif
layout(set=1,binding=6) uniform sampler3D integratedFog;
layout(push_constant) uniform DrawBeam { int beamIndex; };
layout(location=0) out vec4 color;
// Intersect the axial slab, then the cone quadratic. Testing intervals also handles
// parallel rays, the camera inside the cone, tangencies and the negative quadratic.
bool coneInterval(vec3 o,vec3 r,vec3 axis,float cosine,float len,inout float lo,inout float hi) {
    float h=dot(o,axis),v=dot(r,axis);
    if(abs(v)<1e-7){if(h<0||h>len)return false;}
    else {float a=-h/v,b=(len-h)/v;lo=max(lo,min(a,b));hi=min(hi,max(a,b));}
    if(hi<=lo)return false;
    float c2=cosine*cosine,A=c2-dot(r,axis)*dot(r,axis);
    float B=2*(c2*dot(o,r)-h*v),C=c2*dot(o,o)-h*h;
    float cuts[4];cuts[0]=lo;cuts[1]=hi;int n=2;
    if(abs(A)<1e-7){if(abs(B)>1e-7){float t=-C/B;if(t>lo&&t<hi)cuts[n++]=t;}}
    else {float d=B*B-4*A*C;if(d>=0){float q=sqrt(d);float a=(-B-q)/(2*A),b=(-B+q)/(2*A);
        if(a>lo&&a<hi)cuts[n++]=a;if(b>lo&&b<hi)cuts[n++]=b;}}
    for(int i=0;i<n;++i)for(int j=i+1;j<n;++j)if(cuts[j]<cuts[i]){float t=cuts[i];cuts[i]=cuts[j];cuts[j]=t;}
    for(int i=0;i<n-1;++i){float t=(cuts[i]+cuts[i+1])*.5;if((A*t+B)*t+C<=0){lo=cuts[i];hi=cuts[i+1];return hi>lo;}}
    return false;
}
void main() {
    vec2 uv=gl_FragCoord.xy/renderSize.xy;
    ivec2 pixel=clamp(ivec2(uv*renderSize.zw),ivec2(0),ivec2(renderSize.zw)-1);
    vec4 p=f.inverseProjection*vec4(uv*2-1,readDepth(pixel),1);
    float sceneZ=min(f.grid.w,max(0,-p.z/p.w));
    color=vec4(0,0,0,sceneZ);
    vec3 origin=worldAt(uv,0),delta=worldAt(uv,1)-origin;
    float rayScale=length(delta);vec3 ray=delta/rayScale;
    Beam b=beams[beamIndex];int i=int(b.indexLengthScaleSamples.x);
    if(b.indexLengthScaleSamples.z<=0)return;
    float lo=0,hi=sceneZ*rayScale;
    vec3 axis=u_Frame.SpotDirectionOuter[i].xyz;
    if(!coneInterval(origin-u_Frame.SpotPositionRange[i].xyz,ray,axis,u_Frame.SpotDirectionOuter[i].w,b.indexLengthScaleSamples.y,lo,hi))return;
    int count=int(b.indexLengthScaleSamples.w);float ds=(hi-lo)/float(count);
    // Interleaved-gradient noise, shifted each frame. FSR TAA or our stationary-view history resolves it.
    float j=fract(52.9829189*fract(dot(gl_FragCoord.xy+f.temporal.w*vec2(5.588238,5.588238),vec2(.06711056,.00583715))));
    vec3 sum=vec3(0);
    for(int s=0;s<count;++s){
        float t=lo+(float(s)+j)*ds;vec3 world=origin+ray*t;
        vec3 d=u_Frame.SpotPositionRange[i].xyz-world;float attenuation=KansSpotAttenuation(i,d);
        if(attenuation<=0)continue;
        vec4 medium=mediumAt(world);if(medium.a<=0)continue;
        float h=dot(-d,axis),fade=1;
        if(b.fadesFlags.x>0)fade*=clamp(h/b.fadesFlags.x,0,1);
        if(b.fadesFlags.y>0)fade*=clamp((b.indexLengthScaleSamples.y-h)/b.fadesFlags.y,0,1);
        vec3 cookie=b.fadesFlags.w>0 ? KansSpotCookie(i,world) : vec3(1);
        float visibility=b.fadesFlags.z>0 ? spotShadow(i,world) : 1;
        float z=t/rayScale,slice=log(1+z/.1)/log(1+f.grid.w/.1)*f.grid.z;
        float tr=mix(1.0,textureLod(integratedFog,vec3(uv-f.previousDepth.yz,(slice-.5)/f.grid.z),0).a,clamp(slice,0,1));
        sum+=tr*medium.rgb*u_Frame.SpotRadianceInner[i].rgb*attenuation*cookie*visibility*
             phase(dot(-normalize(d),-ray))*fade*ds;
    }
    color.rgb=min(sum*b.indexLengthScaleSamples.z,vec3(60000));
}
)";
    inline constexpr const char* ResolveParameters = R"(
layout(set=1,binding=0,std140) uniform Resolve { mat4 inverseProjection; vec4 sizes; vec4 temporal; } b;
layout(set=1,binding=1) uniform sampler2D currentBeam;
layout(set=1,binding=2) uniform sampler2D previousBeam;
layout(location=0) out vec4 color;
)";
    inline constexpr const char* Temporal          = R"(
void main(){
    ivec2 p=ivec2(gl_FragCoord.xy),size=textureSize(currentBeam,0);
    vec4 v=texelFetch(currentBeam,p,0);color=v;
    if(b.temporal.x==0)return;
    vec4 old=texelFetch(previousBeam,p,0);
    if(abs(old.a-v.a)>max(.01,v.a*.005))return;
    vec3 lo=v.rgb,hi=v.rgb;
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){vec3 n=texelFetch(currentBeam,clamp(p+ivec2(x,y),ivec2(0),size-1),0).rgb;lo=min(lo,n);hi=max(hi,n);}
    color.rgb=mix(v.rgb,clamp(old.rgb,max(lo,v.rgb*.8),min(hi,v.rgb*1.2)),b.temporal.y);
}
)";
    inline constexpr const char* Composite         = R"(
#ifdef MULTISAMPLE_DEPTH
layout(set=1,binding=3) uniform sampler2DMS sceneDepth;
float readDepth(ivec2 p){return texelFetch(sceneDepth,p,0).r;}
#else
layout(set=1,binding=3) uniform sampler2D sceneDepth;
float readDepth(ivec2 p){return texelFetch(sceneDepth,p,0).r;}
#endif
void main(){
    vec2 uv=gl_FragCoord.xy/b.sizes.zw;
    if(b.sizes.xy==b.sizes.zw){color=vec4(texelFetch(currentBeam,ivec2(gl_FragCoord.xy),0).rgb,0);return;}
    vec4 p=b.inverseProjection*vec4(uv*2-1,readDepth(ivec2(gl_FragCoord.xy)),1);
    float depth=min(b.temporal.z,max(0,-p.z/p.w));
    vec2 q=uv*b.sizes.xy-.5,frac=fract(q);ivec2 base=ivec2(floor(q));
    vec3 sum=vec3(0);float total=0;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){
        vec4 v=texelFetch(currentBeam,clamp(base+ivec2(x,y),ivec2(0),ivec2(b.sizes.xy)-1),0);
        float w=(x==0 ? 1-frac.x : frac.x)*(y==0 ? 1-frac.y : frac.y);
        w*=exp(-abs(v.a-depth)/max(.01,depth*.01));sum+=v.rgb*w;total+=w;
    }
    // No compatible low-resolution neighbour: leave the foreground clear instead of leaking a background beam.
    color=vec4(total>1e-6 ? sum/total : vec3(0),0);
}
)";
} // namespace Kans::BeamShaders
