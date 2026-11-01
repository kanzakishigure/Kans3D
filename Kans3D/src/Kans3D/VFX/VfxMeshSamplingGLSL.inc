R"glsl(
mat4 meshBindMatrix(int g,int o){return mat4(meshStatic(g,o),meshStatic(g,o+1),meshStatic(g,o+2),meshStatic(g,o+3));}
mat4 meshMatrix(int o){return mat4(diValues[o],diValues[o+1],diValues[o+2],diValues[o+3]);}
vec3 meshUnit(vec3 n){return dot(n,n)>1e-20?normalize(n):vec3(0);}
int meshRandom(int base,int geometry,int kind,float u){
vec4 h=diValues[base],a=diValues[base+1],b=diValues[base+2],c=diValues[base+3];
if(b.w==0)return -1;
if(kind==0){if(b.z<=0)return -1;int lo=0,hi=int(h.y);while(lo<hi){int mid=(lo+hi)/2;if(u<meshStatic(geometry,int(a.y)+mid).w)hi=mid;else lo=mid+1;}return min(lo,int(h.y)-1);}
int count=int(kind==1?c.w:h.w);if(count==0)return -1;
return int(meshStatic(geometry,int(kind==1?c.z:a.z)+min(int(u*float(count)),count-1)).x);}
vec4 meshSample(int base,int geometry,int index,vec3 bary,int mode,int field){
vec4 h=diValues[base],a=diValues[base+1],b=diValues[base+2],c=diValues[base+3];
if(b.w==0||index<0||index>=int(mode==2?h.z:mode==1?h.x:h.y))return field==7?vec4(0,0,0,1):vec4(0);
if(field==8)return vec4(1,0,0,0);
mat4 source=meshMatrix(base+4),oldSource=meshMatrix(base+8);
vec3 pos=vec3(0),old=vec3(0),normal=vec3(0);vec4 tangent=vec4(0),uv=vec4(0),color=vec4(0);
if(mode==2){mat4 bind=meshBindMatrix(geometry,int(b.y)+index*4);
mat4 current=source*meshMatrix(base+int(a.w)+index*4)*bind;
mat4 before=oldSource*meshMatrix(base+int(b.x)+index*4)*bind;
pos=current[3].xyz;old=before[3].xyz;
if(field==7){mat3 m=mat3(meshUnit(current[0].xyz),meshUnit(current[1].xyz),meshUnit(current[2].xyz));
// Stable matrix-to-quaternion conversion, including rotations near pi.
float tr=m[0][0]+m[1][1]+m[2][2];vec4 q;
if(tr>0){float s=sqrt(tr+1)*2;q=vec4((m[1][2]-m[2][1])/s,(m[2][0]-m[0][2])/s,(m[0][1]-m[1][0])/s,s*.25);}
else if(m[0][0]>m[1][1]&&m[0][0]>m[2][2]){float s=sqrt(1+m[0][0]-m[1][1]-m[2][2])*2;q=vec4(s*.25,(m[1][0]+m[0][1])/s,(m[2][0]+m[0][2])/s,(m[1][2]-m[2][1])/s);}
else if(m[1][1]>m[2][2]){float s=sqrt(1+m[1][1]-m[0][0]-m[2][2])*2;q=vec4((m[1][0]+m[0][1])/s,s*.25,(m[2][1]+m[1][2])/s,(m[2][0]-m[0][2])/s);}
else{float s=sqrt(1+m[2][2]-m[0][0]-m[1][1])*2;q=vec4((m[2][0]+m[0][2])/s,(m[2][1]+m[1][2])/s,s*.25,(m[0][1]-m[1][0])/s);}return normalize(q);}}
else{vec3 indices=vec3(index);if(mode==0)indices=meshStatic(geometry,int(a.y)+index).xyz;else bary=vec3(1,0,0);
for(int v=0;v<(mode==1?1:3);++v){int o=int(a.x)+int(indices[v])*7;
vec4 p=meshStatic(geometry,o),t=meshStatic(geometry,o+2);vec3 n=meshStatic(geometry,o+1).xyz;mat4 skin=mat4(1),oldSkin=mat4(1);
if(c.y!=0){skin=mat4(0);oldSkin=mat4(0);for(int k=0;k<4;++k){float w=meshStatic(geometry,o+6)[k];if(w>0){int joint=int(meshStatic(geometry,o+5)[k]);skin+=meshMatrix(base+int(a.w)+joint*4)*w;oldSkin+=meshMatrix(base+int(b.x)+joint*4)*w;}}}
pos+=(source*skin*p).xyz*bary[v];old+=(oldSource*oldSkin*p).xyz*bary[v];
normal+=meshUnit(transpose(inverse(mat3(source*skin)))*n)*bary[v];tangent+=vec4(meshUnit(mat3(source*skin)*t.xyz),t.w)*bary[v];uv+=meshStatic(geometry,o+3)*bary[v];color+=meshStatic(geometry,o+4)*bary[v];}}
if(field==0)return vec4(pos,0);if(field==1)return vec4(meshUnit(normal),0);if(field==2)return vec4(meshUnit(tangent.xyz),tangent.w);
if(field==3)return uv;if(field==4)return color;return vec4((pos-old)*c.x,0);}
)glsl"
