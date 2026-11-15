#pragma once
namespace Kans::FogShaders
{
    inline constexpr const char* Parameters = R"(
layout(set=1,binding=0,std140) uniform FogParameters {
    mat4 inverseView; mat4 inverseProjection; mat4 previousVP; mat4 previousView;
    vec4 grid; vec4 medium; vec4 albedoG; vec4 temporal; vec4 noise; vec4 scroll; vec4 previousDepth;
} f;
float sliceDepth(float z) { return .1*(exp(log(1+f.grid.w/.1)*z/f.grid.z)-1); }
vec3 viewAt(vec2 uv,float depth) {
    vec4 a=f.inverseProjection*vec4(uv*2-1,0,1), b=f.inverseProjection*vec4(uv*2-1,1,1);
    vec3 lo=a.xyz/a.w, hi=b.xyz/b.w;
    return mix(lo,hi,(-depth-lo.z)/(hi.z-lo.z));
}
vec3 worldAt(vec2 uv,float depth) { return (f.inverseView*vec4(viewAt(uv,depth),1)).xyz; }
vec3 jitter(ivec3 cell) {
    uvec3 v=uvec3(cell)*uvec3(1597334677u,3812015801u,2798796415u)+uint(f.temporal.w)*747796405u;
    uint h=(v.x^v.y^v.z)*2891336453u; h=(h^(h>>16))*2246822519u;
    // Midpoints exclude the camera boundary, where a scattering direction would be undefined.
    return (vec3(h&1023u,(h>>10)&1023u,(h>>20)&1023u)+.5)/1024.0;
}
vec3 sampleWorld(ivec3 cell) {
    vec3 j=jitter(cell);
    float a=sliceDepth(float(cell.z)),b=sliceDepth(float(cell.z+1));
    return worldAt((vec2(cell.xy)+j.xy)/f.grid.xy,mix(a,b,j.z));
}
)";
    inline constexpr const char* Media      = R"(
layout(local_size_x=4,local_size_y=4,local_size_z=4) in;
layout(set=1,binding=1,rgba16f) writeonly uniform image3D media;
layout(set=1,binding=2) uniform sampler3D densityNoise;
struct Volume { mat4 worldToLocal; vec4 albedoDensity; vec4 shapeFalloff; };
layout(set=1,binding=3,std140) uniform FogVolumes { Volume volumes[32]; };
void main() {
    ivec3 cell=ivec3(gl_GlobalInvocationID); if(any(greaterThanEqual(cell,ivec3(f.grid.xyz))))return;
    vec3 p=sampleWorld(cell);
    float density=f.medium.x*exp(clamp(-f.medium.y*(p.y-f.medium.z),-40,12));
    vec3 scattering=f.albedoG.rgb*density;
    for(int i=0;i<int(f.noise.w);++i) {
        vec3 q=(volumes[i].worldToLocal*vec4(p,1)).xyz*2;
        float edge=volumes[i].shapeFalloff.x>0.5 ? length(q) : max(abs(q.x),max(abs(q.y),abs(q.z)));
        float fade=volumes[i].shapeFalloff.y>0 ? clamp((1-edge)/volumes[i].shapeFalloff.y,0,1) : float(edge<=1);
        float d=volumes[i].albedoDensity.w*fade;
        density+=d; scattering+=d*volumes[i].albedoDensity.rgb;
    }
    float n=f.noise.z>0 ? mix(1.0,textureLod(densityNoise,p*f.noise.x+f.scroll.xyz*f.temporal.z,0).r*2,f.noise.y) : 1;
    imageStore(media,cell,min(vec4(scattering,density)*f.medium.w*n,vec4(60000)));
}
)";
    inline constexpr const char* Lighting   = R"(
layout(local_size_x=4,local_size_y=4,local_size_z=4) in;
layout(set=1,binding=1) uniform sampler3D media;
layout(set=1,binding=2,rgba16f) writeonly uniform image3D scattering;
layout(set=1,binding=3,std140) uniform FogLights { vec4 pointScale[4]; vec4 spotScale[16]; vec4 areaScale[4]; vec4 directionalScale; };
float phase(float c) { float g=f.albedoG.w,d=1+g*g-2*g*c; return (1-g*g)/(12.566370614359*d*sqrt(d)); }
float directionalShadow(vec3 p) {
    int c=KansShadowCascade(p); if(c<0)return 1;
    vec4 clip=u_Frame.CascadeViewProjection[c]*vec4(p,1); vec3 q=clip.xyz/clip.w; q.xy=q.xy*.5+.5;
    if(any(lessThan(q,vec3(0)))||any(greaterThan(q,vec3(1))))return 1;
    float a=textureLod(u_DirectionalShadow,vec3(KansShadowUV(c,q.xy),q.z-0.00001),0);
    float depth=dot(u_Frame.ShadowCameraDepth,vec4(p,1));
    float start=u_Frame.CascadeBlendStarts[c],end=u_Frame.CascadeSplits[c];
    if(end>start&&depth>start) {
        float b=1;
        if(c+1<int(u_Frame.ShadowInfo.x)) {
            clip=u_Frame.CascadeViewProjection[c+1]*vec4(p,1); q=clip.xyz/clip.w; q.xy=q.xy*.5+.5;
            b=textureLod(u_DirectionalShadow,vec3(KansShadowUV(c+1,q.xy),q.z-0.00001),0);
        }
        a=mix(a,b,smoothstep(start,end,depth));
    }
    return a;
}
float pointShadow(int i,vec3 p) {
    int s=int(u_Frame.PointShadowIndices[i/4][i%4]); if(s<0)return 1;
    vec3 d=p-u_Frame.PointShadowPositionRange[s].xyz;
    float ref=length(d)/u_Frame.PointShadowPositionRange[s].w-u_Frame.PointShadowParams[s].z;
    if(ref>=1)return 1;
    if(s==0)return texture(u_PointShadow0,vec4(d,ref));
    if(s==1)return texture(u_PointShadow1,vec4(d,ref));
    if(s==2)return texture(u_PointShadow2,vec4(d,ref));
    return texture(u_PointShadow3,vec4(d,ref));
}
float spotShadow(int i,vec3 p) {
    vec4 r=u_Frame.SpotShadowRect[i]; if(r.z==0)return 1;
    vec4 clip=u_Frame.SpotViewProjection[i]*vec4(p,1); if(clip.w<=0)return 1;
    vec3 q=clip.xyz/clip.w; q.xy=q.xy*.5+.5;
    if(any(lessThan(q,vec3(0)))||any(greaterThan(q,vec3(1))))return 1;
    vec2 h=.5/vec2(textureSize(u_SpotShadowAtlas,0));
    return textureLod(u_SpotShadowAtlas,vec3(clamp(r.xy+q.xy*r.zw,r.xy+h,r.xy+r.zw-h),q.z-0.00001),0);
}
void main() {
    ivec3 cell=ivec3(gl_GlobalInvocationID); if(any(greaterThanEqual(cell,ivec3(f.grid.xyz))))return;
    vec4 m=texelFetch(media,cell,0); if(m.a<=0){imageStore(scattering,cell,vec4(0));return;}
    vec3 p=sampleWorld(cell),cameraDelta=f.inverseView[3].xyz-p;
    vec3 toCamera=cameraDelta*inversesqrt(max(dot(cameraDelta,cameraDelta),1e-12));
    // A six-axis mean is the l=0 environment term. Irradiance is pi times radiance.
    vec3 ambient=(KansEnvironmentDiffuse(vec3(1,0,0))+KansEnvironmentDiffuse(vec3(-1,0,0))+
                  KansEnvironmentDiffuse(vec3(0,1,0))+KansEnvironmentDiffuse(vec3(0,-1,0))+
                  KansEnvironmentDiffuse(vec3(0,0,1))+KansEnvironmentDiffuse(vec3(0,0,-1)))/(6*3.14159265359);
    vec3 light=ambient;
    if(u_Frame.DirectionalDirection.w>0&&directionalScale.x>0)
        light+=u_Frame.DirectionalRadiance.rgb*directionalScale.x*phase(dot(u_Frame.DirectionalDirection.xyz,toCamera))*directionalShadow(p);
    for(int i=0;i<KansPointLightCount();++i) {
        if(pointScale[i/4][i%4]<=0)continue;
        vec3 d=KansPointLightPosition(i)-p; float d2=max(dot(d,d),.01);
        light+=u_Frame.PointRadiance[i].rgb*pointScale[i/4][i%4]*KansPointAttenuation(i,d2)*phase(dot(-d*inversesqrt(d2),toCamera))*pointShadow(i,p);
    }
    for(int i=0;i<KansSpotLightCount();++i) {
        if(spotScale[i/4][i%4]<=0)continue;
        vec3 d=u_Frame.SpotPositionRange[i].xyz-p; float a=KansSpotAttenuation(i,d);
        if(a>0)light+=u_Frame.SpotRadianceInner[i].rgb*spotScale[i/4][i%4]*a*KansSpotCookie(i,p)*phase(dot(-normalize(d),toCamera))*spotShadow(i,p);
    }
    for(int i=0;i<int(u_Frame.LightingInfo.w);++i) {
        if(areaScale[i/4][i%4]<=0)continue;
        vec3 d=u_Frame.AreaPositionRange[i].xyz-p; float d2=max(dot(d,d),.01); vec3 l=d*inversesqrt(d2);
        vec3 n=-normalize(cross(u_Frame.AreaRightWidth[i].xyz,u_Frame.AreaUpHeight[i].xyz));
        float cosine=dot(n,-l); cosine=u_Frame.AreaScales[i].z>0 ? abs(cosine) : max(0,cosine);
        float area=u_Frame.AreaRightWidth[i].w*u_Frame.AreaUpHeight[i].w*(u_Frame.AreaRadiance[i].w>0 ? .78539816339 : 1);
        // Finite solid angle approximation avoids an inverse-square singularity within the emitter.
        float omega=area/max(d2,area);
        light+=u_Frame.AreaRadiance[i].rgb*areaScale[i/4][i%4]*cosine*omega*KansRangeWindow(d2,u_Frame.AreaPositionRange[i].w)*phase(dot(-l,toCamera));
    }
    imageStore(scattering,cell,vec4(min(light*m.rgb,vec3(60000)),m.a));
}
)";
    inline constexpr const char* Temporal   = R"(
layout(local_size_x=4,local_size_y=4,local_size_z=4) in;
layout(set=1,binding=1) uniform sampler3D currentScattering;
layout(set=1,binding=2) uniform sampler3D history;
layout(set=1,binding=3,rgba16f) writeonly uniform image3D result;
void main() {
    ivec3 cell=ivec3(gl_GlobalInvocationID),size=ivec3(f.grid.xyz); if(any(greaterThanEqual(cell,size)))return;
    vec4 current=texelFetch(currentScattering,cell,0),value=current;
    if(f.temporal.y>0) {
        float z=.5*(sliceDepth(float(cell.z))+sliceDepth(float(cell.z+1)));
        vec3 p=worldAt((vec2(cell.xy)+.5)/f.grid.xy,z);
        vec4 old=f.previousVP*vec4(p,1);
        float depth=-(f.previousView*vec4(p,1)).z;
        float slice=log(1+max(0,depth)/.1)/log(1+f.previousDepth.x/.1);
        vec3 uvw=vec3(old.xy/old.w*.5+.5,slice);
        if(old.w>0&&depth>0&&all(greaterThanEqual(uvw,vec3(0)))&&all(lessThanEqual(uvw,vec3(1)))) {
            vec4 lo=current,hi=current;
            for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
                vec4 v=texelFetch(currentScattering,clamp(cell+ivec3(x,y,z),ivec3(0),size-1),0); lo=min(lo,v); hi=max(hi,v);
            }
            // Current-cell bounds also reject a moving light's history inside a broad neighbourhood.
            lo=max(lo,current*.8); hi=min(hi,current*1.2);
            value=mix(current,clamp(textureLod(history,uvw,0),lo,hi),f.temporal.x);
        }
    }
    imageStore(result,cell,value);
}
)";
    inline constexpr const char* Integrate  = R"(
layout(local_size_x=8,local_size_y=8) in;
layout(set=1,binding=1) uniform sampler3D scattering;
layout(set=1,binding=2,rgba16f) writeonly uniform image3D integrated;
void main() {
    ivec2 xy=ivec2(gl_GlobalInvocationID.xy); if(any(greaterThanEqual(xy,ivec2(f.grid.xy))))return;
    vec3 sum=vec3(0); float transmittance=1;
    // Axial depth cells must be converted to physical length along this pixel's ray.
    vec2 uv=(vec2(xy)+.5)/f.grid.xy; float rayScale=length(viewAt(uv,1)-viewAt(uv,0));
    for(int z=0;z<int(f.grid.z);++z) {
        vec4 s=texelFetch(scattering,ivec3(xy,z),0);
        float ds=(sliceDepth(float(z+1))-sliceDepth(float(z)))*rayScale;
        float t=exp(-s.a*ds),weight=s.a>.000001 ? (1-t)/s.a : ds;
        sum+=transmittance*s.rgb*weight; transmittance*=t;
        imageStore(integrated,ivec3(xy,z),vec4(min(sum,vec3(60000)),transmittance));
    }
}
)";
} // namespace Kans::FogShaders
