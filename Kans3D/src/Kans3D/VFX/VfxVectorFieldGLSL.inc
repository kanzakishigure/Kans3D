R"glsl(
vec3 sampleVectorField(int g,vec3 p,bvec3 repeatAxis){
vec3 loBounds=meshStatic(g,0).xyz,hiBounds=meshStatic(g,1).xyz;
ivec3 res=ivec3(meshStatic(g,2).xyz);
vec3 uv=(p-loBounds)/(hiBounds-loBounds);
for(int a=0;a<3;++a)uv[a]=repeatAxis[a]?fract(uv[a]):clamp(uv[a],0.0,1.0);
vec3 grid=uv*vec3(res-1),f=fract(grid);
ivec3 lo=ivec3(floor(grid)),hi=min(lo+1,res-1);vec3 value=vec3(0);
for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x){
ivec3 cell=ivec3(x!=0?hi.x:lo.x,y!=0?hi.y:lo.y,z!=0?hi.z:lo.z);
value+=meshStatic(g,3+cell.x+res.x*(cell.y+res.y*cell.z)).xyz*
(x!=0?f.x:1-f.x)*(y!=0?f.y:1-f.y)*(z!=0?f.z:1-f.z);}
return value;}
)glsl"
