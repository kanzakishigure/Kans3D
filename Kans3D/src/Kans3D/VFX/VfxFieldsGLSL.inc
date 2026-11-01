R"glsl(
vec3 fieldUnit(vec3 v){float n=length(v);return n>1e-8?v/n:vec3(0);}
vec3 fieldRotate(vec4 q,vec3 v){return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
float fieldNoise(vec3 p){
 ivec3 cell=ivec3(floor(p));vec3 f=fract(p);f=f*f*(3-2*f);float result=0;
 for(uint i=0u;i<8u;++i){
 uint h=uint(cell.x+int(i&1u))*0x8da6b343u^uint(cell.y+int((i>>1)&1u))*0xd8163841u^uint(cell.z+int(i>>2))*0xcb1ab31fu;
 h=hashV(h);float v=float(h&0xffffffu)/float(0xffffffu)*2-1;
 result+=v*((i&1u)!=0u?f.x:1-f.x)*((i&2u)!=0u?f.y:1-f.y)*((i&4u)!=0u?f.z:1-f.z);
 }return result;
}
vec3 fieldNoise3(vec3 p){return vec3(fieldNoise(p),fieldNoise(p+vec3(19,37,11)),fieldNoise(p+vec3(47,7,29)));}
float fieldAttenuation(uint mode,float d,vec4 a,vec4 b){
 if(mode==1u)return 1-d;
 if(mode==2u)return 1/(1+99*d*d);
 if(mode==3u){float x=clamp(d,0,1)*7;int i=min(6,int(x));float c[8]=float[8](a.x,a.y,a.z,a.w,b.x,b.y,b.z,b.w);return mix(c[i],c[i+1],x-i);}
 return 1;
}
vec3 sampleFieldsV(vec3 position,vec3 velocity,int resultKind,uvec4 config,uvec4 first){
 uint layer=config.x;float time=uintBitsToFloat(config.y);if(layer>=32u)return vec3(0);
 uvec4 header=floatBitsToUint(fieldWords[0]);vec3 result=vec3(0);
 if(resultKind==3){
 for(uint i=0u;i<header.y;++i){uint o=1u+header.x*10u+i*4u;
 uvec4 fieldFilter=floatBitsToUint(fieldWords[o]),meta=floatBitsToUint(fieldWords[o+3]);
 if((fieldFilter.x&(1u<<layer))==0u||(fieldFilter.y&8u)==0u||meta.y<first.y||(meta.y==first.y&&meta.x<first.x))continue;
 vec4 p=fieldWords[o+1],impulse=fieldWords[o+2];float d=distance(position,p.xyz);if(p.w<=0||d>p.w)continue;
 vec3 v=meta.w!=0u?fieldUnit(position-(p.xyz-vec3(0,impulse.w,0)))*impulse.x:impulse.xyz;
 result+=v*fieldAttenuation(meta.z,d/p.w,vec4(0),vec4(0));
 }return result;
 }
 for(uint i=0u;i<header.x;++i){uint o=1u+i*10u;
 uvec4 kind=floatBitsToUint(fieldWords[o]),fieldFilter=floatBitsToUint(fieldWords[o+1]);
 if((fieldFilter.x&(1u<<layer))==0u||(fieldFilter.y&8u)==0u)continue;
 vec4 pr=fieldWords[o+2],q=fieldWords[o+3],ext=fieldWords[o+4],ds=fieldWords[o+5],np=fieldWords[o+6],si=fieldWords[o+7];
 vec3 delta=position-pr.xyz,local=fieldRotate(vec4(-q.xyz,q.w),delta);float d=0;
 if(kind.y==1u)d=length(delta)/max(1e-8,pr.w);
 if(kind.y==2u){vec3 v=abs(local)/max(ext.xyz,vec3(1e-8));d=max(v.x,max(v.y,v.z));}
 if(d>1)continue;
 float attenuation=kind.x==0u?(ext.w>0?clamp((1-d)/ext.w,0,1):1):fieldAttenuation(kind.z,d,fieldWords[o+8],fieldWords[o+9]);
 vec3 axis=fieldRotate(q,fieldUnit(ds.xyz)),v=vec3(0),p=position*np.y+si.xyz*time+vec3(float(fieldFilter.z%65536u));
 if(kind.x==0u){if(resultKind==0){p=position*np.y+vec3(time*np.y);result+=attenuation*(axis*(ds.w+np.z*sin(time*np.w*6.283185307))+np.x*fieldNoise3(p));}continue;}
 if((kind.w!=0u?2:1)!=resultKind)continue;
 if(kind.x==1u)v=fieldUnit(delta);
 if(kind.x==2u)v=axis;
 if(kind.x==3u){vec3 radial=delta-axis*dot(delta,axis);v=fieldUnit(cross(axis,radial))-si.w*fieldUnit(radial);}
 if(kind.x==5u)v=-velocity;
 if(kind.x==4u){if(fieldFilter.w!=0u){
 vec3 dx=(fieldNoise3(p+vec3(.01,0,0))-fieldNoise3(p-vec3(.01,0,0)))/.02;
 vec3 dy=(fieldNoise3(p+vec3(0,.01,0))-fieldNoise3(p-vec3(0,.01,0)))/.02;
 vec3 dz=(fieldNoise3(p+vec3(0,0,.01))-fieldNoise3(p-vec3(0,0,.01)))/.02;
 v=vec3(dy.z-dz.y,dz.x-dx.z,dx.y-dy.x);
 }else v=fieldNoise3(p);}
 result+=v*ds.w*attenuation;
 }return result;
}
)glsl"
