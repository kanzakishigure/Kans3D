R"glsl(
// Seven words per shape, matching CollisionShape. Mesh/convex geometry is queried
// on the CPU; GPU emitters use the visible scene's depth snapshot for it.
float collisionDistanceV(uint at,vec3 p,out vec3 normal){
 vec4 cr=collisionWords[at],ax=collisionWords[at+1u],ay=collisionWords[at+2u],az=collisionWords[at+3u];
 uint kind=floatBitsToUint(collisionWords[at+6u]).x;p-=cr.xyz;
 if(kind==0u){normal=ay.xyz;return dot(p,normal);}
 if(kind==1u||kind==3u){if(kind==3u)p-=ay.xyz*clamp(dot(p,ay.xyz),-ay.w,ay.w);float len=length(p);normal=len>1e-8?p/len:vec3(0,1,0);return len-cr.w;}
 mat3 axes=mat3(ax.xyz,ay.xyz,az.xyz);vec3 local=transpose(axes)*p;
 vec3 q=abs(local)-vec3(ax.w,ay.w,az.w),outside=max(q,vec3(0)),n=vec3(0);float len=length(outside);
 if(len>1e-8)n=outside/len;else{int axis=q.x>q.y?(q.x>q.z?0:2):(q.y>q.z?1:2);n[axis]=1;}
 for(int axis=0;axis<3;++axis)if(local[axis]<0)n[axis]=-n[axis];normal=axes*n;return len+min(max(q.x,max(q.y,q.z)),0.0);
}
struct CollisionResultV{vec3 Position;vec3 Velocity;vec3 Normal;vec3 Contact;bool Hit;};
CollisionResultV collideParticleV(vec3 previous,vec3 position,vec3 velocity,vec2 size,vec3 scale,int mode,float restitution,float friction,float radiusScale,float radiusOverride,float resting,float dt,float mesh,mat4 owner,bool local,uint mask,mat4 depthVP,mat4 depthInverse,bool depthValid){
 mat4 inv=local?inverse(owner):mat4(1);
 if(local){previous=(owner*vec4(previous,1)).xyz;position=(owner*vec4(position,1)).xyz;velocity=mat3(owner)*velocity;}
 float radius=radiusOverride>=0?radiusOverride:(mesh>=0?mesh*max(abs(scale.x),max(abs(scale.y),abs(scale.z))):.5*max(abs(size.x),abs(size.y)))*max(0.0,radiusScale);
 if(local)radius*=max(length(owner[0].xyz),max(length(owner[1].xyz),length(owner[2].xyz)));
 float best=2;vec3 normal=vec3(0),bodyVelocity=vec3(0),center=vec3(0);
 if(mode==1){
  if(depthValid&&mask!=0u){
   vec4 clip=depthVP*vec4(position,1);vec3 ndc=clip.xyz/clip.w;
   if(clip.w>0&&all(lessThan(abs(ndc.xy),vec2(1)))&&ndc.z>=0&&ndc.z<=1){
    ivec2 extent=textureSize(collisionDepth,0),pixel=ivec2((ndc.xy*.5+.5)*vec2(extent));
    float z=texelFetch(collisionDepth,pixel,0).r;
    if(z>0&&z<1&&all(greaterThan(pixel,ivec2(0)))&&all(lessThan(pixel,extent-1))){
     vec2 uv=(vec2(pixel)+.5)/vec2(extent);vec4 h=depthInverse*vec4(uv*2-1,z,1);vec3 surface=h.xyz/h.w;
     // Choose the closest-depth neighbour on each axis to avoid silhouettes
     // producing a normal across unrelated surfaces.
     ivec2 px=pixel+ivec2(1,0),py=pixel+ivec2(0,1);
     if(abs(texelFetch(collisionDepth,pixel-ivec2(1,0),0).r-z)<abs(texelFetch(collisionDepth,px,0).r-z))px=pixel-ivec2(1,0);
     if(abs(texelFetch(collisionDepth,pixel-ivec2(0,1),0).r-z)<abs(texelFetch(collisionDepth,py,0).r-z))py=pixel-ivec2(0,1);
     vec4 hx=depthInverse*vec4((vec2(px)+.5)/vec2(extent)*2-1,texelFetch(collisionDepth,px,0).r,1);
     vec4 hy=depthInverse*vec4((vec2(py)+.5)/vec2(extent)*2-1,texelFetch(collisionDepth,py,0).r,1);
     vec3 n=cross(hx.xyz/hx.w-surface,hy.xyz/hy.w-surface);
     if(dot(n,n)>1e-16){
      n=normalize(n);vec4 eyeH=depthInverse*vec4(ndc.xy,0,1);vec3 eye=eyeH.xyz/eyeH.w;if(dot(n,eye-surface)<0)n=-n;
      float before=dot(previous-surface,n)-radius,after=dot(position-surface,n)-radius;
      // World-space thickness bounds the accepted back side of the depth shell.
      float thickness=max(.1,2*radius);
      if(after<=1e-5&&after>=-thickness&&before>=-1e-4&&dot(position-previous,n)<0){
       best=clamp(before/max(before-after,1e-8),0.0,1.0);normal=n;center=mix(previous,position,best);center+=n*(radius-dot(center-surface,n));
      }
     }
    }
   }
  }
 }else{
  uint count=min(floatBitsToUint(collisionWords[0]).x,256u);
  for(uint shape=0u;shape<count;++shape){uint at=1u+shape*7u;uvec4 meta=floatBitsToUint(collisionWords[at+6u]);if(meta.y>31u||(mask&(1u<<meta.y))==0u)continue;
   vec3 bv=collisionWords[at+4u].xyz,motion=position-previous-bv*dt,start=previous+bv*dt;float speed=length(motion),t=0;
   for(int step=0;step<96&&t<=1&&t<best;++step){vec3 n,p=start+motion*t;float distance=collisionDistanceV(at,p,n)-radius;
    if(distance<=1e-5){if(distance< -1e-5||dot(motion,n)< -1e-8){best=t;normal=n;center=p-n*distance;bodyVelocity=bv+cross(collisionWords[at+5u].xyz,center-n*radius-collisionWords[at].xyz);}break;}
    if(speed<1e-8)break;t+=distance/speed;
   }
  }
 }
 bool hit=best<=1;
 if(hit){vec3 relative=velocity-bodyVelocity;float vn=dot(relative,normal);vec3 tangent=relative-vn*normal;float bounce=max(0.0,-vn)*clamp(restitution,0.0,1.0);if(bounce<max(0.0,resting))bounce=0;
  velocity=bodyVelocity+tangent*(1-clamp(friction,0.0,1.0))+normal*(vn>0?vn:bounce);
  position=center+(velocity-bodyVelocity)*dt*(1-best);
 }
 vec3 contact=hit?center-normal*radius:position;
 if(local){position=(inv*vec4(position,1)).xyz;contact=(inv*vec4(contact,1)).xyz;velocity=mat3(inv)*velocity;normal=hit?normalize(transpose(mat3(owner))*normal):normal;}
 return CollisionResultV(position,velocity,normal,contact,hit);
}
)glsl"
