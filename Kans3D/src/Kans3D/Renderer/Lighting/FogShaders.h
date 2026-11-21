#pragma once
namespace Kans::FogShaders
{
    inline constexpr const char* Parameters     = R"(
layout(set=1,binding=0,std140) uniform FogParameters {
    mat4 inverseView; mat4 inverseProjection; mat4 previousVP; mat4 previousView;
    vec4 grid; vec4 medium; vec4 albedoG;
    vec4 temporal; // weight, history mode (0 reset, 1 reproject, 2 stationary), changed-light count, jitter frame
    vec4 noise; vec4 scroll; vec4 previousDepth;
} f;
float sliceDepth(float z) { return .1*(exp(log(1+f.grid.w/.1)*z/f.grid.z)-1); }
vec3 viewAt(vec2 uv,float depth,out float scale) {
    vec4 a=f.inverseProjection*vec4(uv*2-1,0,1), b=f.inverseProjection*vec4(uv*2-1,1,1);
    vec3 lo=a.xyz/a.w, hi=b.xyz/b.w;
    float t=(-depth-lo.z)/(hi.z-lo.z);
    scale=mix(1/a.w,1/b.w,t);
    return mix(lo,hi,t);
}
vec3 viewAt(vec2 uv,float depth) { float scale; return viewAt(uv,depth,scale); }
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
    inline constexpr const char* FroxelBounds   = R"(
struct FroxelSphere { vec3 center; float radius; };
vec2 cellDepths(ivec3 cell) { return vec2(sliceDepth(float(cell.z)),sliceDepth(float(cell.z+1))); }
FroxelSphere froxelSphere(ivec3 cell,vec2 depths) {
    vec2 uv=(vec2(cell.xy)+.5)/f.grid.xy;
    float nearScale,farScale;
    vec3 front=(f.inverseView*vec4(viewAt(uv,depths.x,nearScale),1)).xyz;
    vec3 back=(f.inverseView*vec4(viewAt(uv,depths.y,farScale),1)).xyz;
    vec3 halfDepth=.5*(back-front),center=front+halfDepth;
    float radius2=0;
    if(f.inverseProjection[0].zw==vec2(0)&&f.inverseProjection[1].zw==vec2(0)) {
        // Perspective and orthographic caps have the same axes. Use the larger cap for both ends
        // and test its opposite corner pairs, including any skew from the inverse view transform.
        float scale=max(abs(nearScale),abs(farScale));
        vec3 x=mat3(f.inverseView)*f.inverseProjection[0].xyz*(scale/f.grid.x);
        vec3 y=mat3(f.inverseView)*f.inverseProjection[1].xyz*(scale/f.grid.y);
        vec3 a=x+y,b=x-y;
        radius2=dot(halfDepth,halfDepth)+max(dot(a,a)+2*abs(dot(halfDepth,a)),
                                             dot(b,b)+2*abs(dot(halfDepth,b)));
    } else {
        // With a general projection the caps can be asymmetric. Enclose all eight corners.
        for(int i=0;i<8;++i) {
            vec2 corner=(vec2(cell.xy)+vec2(i&1,(i>>1)&1))/f.grid.xy;
            vec3 d=worldAt(corner,depths[i>>2])-center;
            radius2=max(radius2,dot(d,d));
        }
    }
    return FroxelSphere(center,sqrt(radius2));
}
bool outsideRange(float distance2,float radius,float range) {
    return range>0&&distance2>(range+radius)*(range+radius);
}
// Signed side distance is radial*cos(angle) - axial*sin(angle). Compare the squared
// radial term against the two sphere tangencies, keeping the sign of the axial terms.
int classifySpot(FroxelSphere sphere,vec4 positionRange,vec4 directionOuter) {
    vec3 d=sphere.center-positionRange.xyz;
    float distance2=dot(d,d),r=sphere.radius;
    if(outsideRange(distance2,r,positionRange.w))return -1;
    float axial=dot(d,directionOuter.xyz);
    if(axial<-r)return -1;
    float cosine2=directionOuter.w*directionOuter.w,sine2=max(0,1-cosine2);
    vec3 perpendicular=d-axial*directionOuter.xyz;
    float radial2=dot(perpendicular,perpendicular);
    // Behind the side ray's closest point, the nearest point on the cone is the apex.
    if(axial<0&&distance2>r*r&&axial*axial*cosine2>radial2*sine2)return -1;
    radial2*=cosine2;
    float cone=axial*sqrt(sine2);
    float outer=cone+r;
    if(outer<0||radial2>outer*outer)return -1;
    float inner=cone-r;
    return inner>=0&&radial2<=inner*inner ? 1 : 0;
}
)";
    inline constexpr const char* MediumSampling = R"(
layout(set=1,binding=2) uniform sampler3D densityNoise;
struct Volume { mat4 worldToLocal; vec4 albedoDensity; vec4 shapeFalloff; vec4 noise; vec4 scroll; };
layout(set=1,binding=3,std140) uniform FogVolumes { Volume volumes[32]; };
vec4 mediumAt(vec3 p) {
    float density=f.medium.x*exp(clamp(-f.medium.y*(p.y-f.medium.z),-40,12));
    float n=f.noise.z>0 ? mix(1.0,textureLod(densityNoise,p*f.noise.x+f.scroll.xyz,0).r*2,f.noise.y) : 1;
    density*=n;
    vec3 scattering=f.albedoG.rgb*density;
    for(int i=0;i<int(f.noise.w);++i) {
        vec3 q=(volumes[i].worldToLocal*vec4(p,1)).xyz*2;
        float edge=volumes[i].shapeFalloff.x>0.5 ? length(q) : max(abs(q.x),max(abs(q.y),abs(q.z)));
        float fade=volumes[i].shapeFalloff.y>0 ? clamp((1-edge)/volumes[i].shapeFalloff.y,0,1) : float(edge<=1);
        vec4 vn=volumes[i].noise;
        float volumeNoise=vn.z<0 ? n : vn.z>0 ? mix(1.0,textureLod(densityNoise,p*vn.x+volumes[i].scroll.xyz,0).r*2,vn.y) : 1;
        float d=volumes[i].albedoDensity.w*fade*volumeNoise;
        density+=d; scattering+=d*volumes[i].albedoDensity.rgb;
    }
    return min(vec4(scattering,density)*f.medium.w,vec4(60000));
}
)";
    inline constexpr const char* Media          = R"(
layout(local_size_x=4,local_size_y=4,local_size_z=4) in;
layout(set=1,binding=1,rgba16f) writeonly uniform image3D media;
void main() {
    ivec3 cell=ivec3(gl_GlobalInvocationID); if(any(greaterThanEqual(cell,ivec3(f.grid.xyz))))return;
    imageStore(media,cell,mediumAt(sampleWorld(cell)));
}
)";
    inline constexpr const char* LightSampling  = R"(
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
)";
    inline constexpr const char* Lighting       = R"(
layout(local_size_x=4,local_size_y=4,local_size_z=4) in;
layout(set=1,binding=1) uniform sampler3D media;
layout(set=1,binding=2,rgba16f) writeonly uniform image3D scattering;
layout(set=1,binding=3,std140) uniform FogLights { vec4 pointScale[4]; vec4 spotScale[16]; vec4 areaScale[4]; vec4 directionalScale; };
vec3 spotContribution(int i,vec3 p,vec3 toCamera,vec3 radiance) {
    vec3 d=u_Frame.SpotPositionRange[i].xyz-p;
    float a=KansSpotAttenuation(i,d);
    if(!(a>0))return vec3(0);
    return radiance*a*KansSpotCookie(i,p)*phase(dot(-normalize(d),toCamera))*spotShadow(i,p);
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
    int spotCount=KansSpotLightCount();
    if(spotCount>0) {
        vec2 depths=cellDepths(cell);
        FroxelSphere sphere=froxelSphere(cell,depths);
        for(int i=0;i<spotCount;++i) {
            float scale=spotScale[i/4][i%4];
            if(scale<=0)continue;
            int coverage=classifySpot(sphere,u_Frame.SpotPositionRange[i],u_Frame.SpotDirectionOuter[i]);
            if(coverage<0)continue;
            vec3 radiance=u_Frame.SpotRadianceInner[i].rgb*scale;
            vec3 contribution=vec3(0);
            if(coverage>0) {
                contribution=spotContribution(i,p,toCamera,radiance);
            } else {
                // Eight samples split all three axes into halves. Decorrelate each octant's
                // offset so a narrow cone can accumulate coverage across frames.
                vec3 j=jitter(cell);
                for(int s=0;s<8;++s) {
                    vec3 octant=vec3(s&1,(s>>1)&1,s>>2);
                    vec3 offset=.5*(octant+fract(j+float(s)*vec3(.754877666,.569840296,.438447187)));
                    vec3 q=worldAt((vec2(cell.xy)+offset.xy)/f.grid.xy,mix(depths.x,depths.y,offset.z));
                    vec3 v=f.inverseView[3].xyz-q;
                    contribution+=spotContribution(i,q,v*inversesqrt(max(dot(v,v),1e-12)),radiance);
                }
                contribution*=.125;
            }
            light+=contribution;
        }
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
    inline constexpr const char* Temporal       = R"(
layout(local_size_x=4,local_size_y=4,local_size_z=4) in;
layout(set=1,binding=1) uniform sampler3D currentScattering;
layout(set=1,binding=2) uniform sampler3D history;
layout(set=1,binding=3,rgba16f) writeonly uniform image3D result;
struct Influence { vec4 positionRange; vec4 directionOuter; };
layout(set=1,binding=4,std430) readonly buffer ChangedLights { Influence changedLights[]; };
bool lightChanged(ivec3 cell) {
    int count=int(f.temporal.z);
    if(count==0)return false;
    FroxelSphere sphere=froxelSphere(cell,cellDepths(cell));
    for(int i=0;i<count;++i) {
        Influence light=changedLights[i];
        if(light.directionOuter.w>0) {
            if(classifySpot(sphere,light.positionRange,light.directionOuter)>=0)return true;
        } else {
            vec3 d=sphere.center-light.positionRange.xyz;
            if(!outsideRange(dot(d,d),sphere.radius,light.positionRange.w))return true;
        }
    }
    return false;
}
vec4 filterHistory(ivec3 cell,ivec3 size,vec4 current) {
    if(f.temporal.y==0||lightChanged(cell))return current;
    vec4 previous;
    if(f.temporal.y==2) {
        // An unchanged camera maps to the same froxel. Avoid resampling across logarithmic slices.
        previous=texelFetch(history,cell,0);
    } else {
        vec2 depths=cellDepths(cell);
        vec3 p=worldAt((vec2(cell.xy)+.5)/f.grid.xy,.5*(depths.x+depths.y));
        vec4 old=f.previousVP*vec4(p,1);
        float depth=-(f.previousView*vec4(p,1)).z;
        float slice=log(1+max(0,depth)/.1)/log(1+f.previousDepth.x/.1);
        vec3 uvw=vec3(old.xy/old.w*.5+.5,slice);
        if(!(old.w>0&&depth>0&&all(greaterThanEqual(uvw,vec3(0)))&&all(lessThanEqual(uvw,vec3(1)))))return current;
        previous=textureLod(history,uvw,0);
    }
    if(previous==current)return current;
    vec4 lo=current,hi=current;
    for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
        vec4 v=texelFetch(currentScattering,clamp(cell+ivec3(x,y,z),ivec3(0),size-1),0); lo=min(lo,v); hi=max(hi,v);
    }
    return mix(current,clamp(previous,lo,hi),f.temporal.x);
}
void main() {
    ivec3 cell=ivec3(gl_GlobalInvocationID),size=ivec3(f.grid.xyz); if(any(greaterThanEqual(cell,size)))return;
    vec4 current=texelFetch(currentScattering,cell,0);
    imageStore(result,cell,filterHistory(cell,size,current));
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
