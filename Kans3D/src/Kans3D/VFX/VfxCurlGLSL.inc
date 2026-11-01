R"glsl(
vec3 seedOffsetV(){uint h=hashV(pc.seed^hashV(pc.emitter));return vec3(h&255u,(h>>8)&255u,(h>>16)&255u)/17.0;}
vec3 potentialV(vec3 p){return vec3(noiseV(p),noiseV(p+vec3(19,37,11)),noiseV(p+vec3(47,7,29)));}
vec3 curlV(vec3 position,float frequency,vec3 offset,int octaves){
 vec3 p=position*frequency+offset,result=vec3(0);float amplitude=1;
 for(int octave=0;octave<octaves;++octave){
 vec3 dx=(potentialV(p+vec3(.01,0,0))-potentialV(p-vec3(.01,0,0)))/.02;
 vec3 dy=(potentialV(p+vec3(0,.01,0))-potentialV(p-vec3(0,.01,0)))/.02;
 vec3 dz=(potentialV(p+vec3(0,0,.01))-potentialV(p-vec3(0,0,.01)))/.02;
 result+=amplitude*vec3(dy.z-dz.y,dz.x-dx.z,dx.y-dy.x);p*=2;amplitude*=.5;
 }return result;
}
)glsl"
