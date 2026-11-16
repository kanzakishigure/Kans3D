R"(#version 450
layout(set=0,binding=0,std140) uniform Parameters {
    vec4 Size; vec4 Lens; vec4 Ranges; vec4 Options; vec4 Projection; vec4 Jitter;
} p;
layout(set=0,binding=1) uniform sampler2D A;
layout(set=0,binding=2) uniform sampler2D B;
layout(set=0,binding=3) uniform sampler2D C;
layout(set=0,binding=4) uniform sampler2D D;
layout(location=0) out vec4 Color;
vec4 at(sampler2D t, ivec2 xy) {return texelFetch(t,clamp(xy,ivec2(0),textureSize(t,0)-1),0);}
float luma(vec3 c) {return dot(c,vec3(.2126,.7152,.0722));}
void main() {
    vec2 uv=gl_FragCoord.xy/p.Size.xy;
    ivec2 xy=ivec2(gl_FragCoord.xy);
    int stage=int(p.Options.x);
    if(stage==0) {
        ivec2 coord=ivec2(floor(uv*vec2(textureSize(A,0))+p.Jitter.xy));
        float d=at(A,coord).r;
        float z=max(.001,-(d*p.Projection.x+p.Projection.y)/(d*p.Projection.z+p.Projection.w));
        float coc=p.Lens.x*(z-p.Lens.y)/z;
        if(p.Lens.w==2.) coc=p.Lens.z*(clamp((z-p.Ranges.y)/p.Ranges.w,0,1)-clamp((p.Ranges.x-z)/p.Ranges.z,0,1));
        Color=vec4(clamp(coc,-p.Lens.z,p.Lens.z),min(z,65000),0,1); return;
    }
    if(stage==1) {
        vec2 maximum=vec2(0);
        for(int y=0;y<16;++y) for(int x=0;x<16;++x) {
            float coc=at(A,xy*16+ivec2(x,y)).r;
            maximum=max(maximum,vec2(-coc,coc));
        }
        Color=vec4(maximum,0,1); return;
    }
    if(stage==2) {
        vec2 maximum=vec2(0);
        int reach=int(ceil(p.Lens.z/16.))+1;
        for(int y=-reach;y<=reach;++y) for(int x=-reach;x<=reach;++x)
            maximum=max(maximum,at(A,xy+ivec2(x,y)).rg);
        Color=vec4(maximum,0,1); return;
    }
    if(stage==3 || stage==4) {
        vec3 sum=vec3(0); float weight=0, radius=0;
        for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            ivec2 coord=xy*2+ivec2(x,y);
            float r=at(B,coord).r*(stage==4 ? -1.:1.);
            // Separate signed fields before filtering: a focused foreground never acquires far radiance.
            float w=clamp(r,0,1);
            sum+=at(A,coord).rgb*w; weight+=w; radius=max(radius,r);
        }
        Color=vec4(sum/max(weight,1e-6),radius); return;
    }
    if(stage==5 || stage==6) {
        bool nearField=stage==6;
        vec2 tile=texture(C,uv).rg;
        float radius=nearField ? tile.r : max(texture(A,uv).a,0.);
        if(radius<.5) {Color=nearField ? vec4(0):texture(A,uv); return;}
        vec3 sum=vec3(0); float weight=0, coverage=0;
        int count=int(p.Options.y);
        // Fixed golden-angle disc in display pixels. No frame/jitter-dependent kernel rotation.
        for(int i=0;i<count;++i) {
            float r=sqrt((float(i)+.5)/float(count));
            float a=float(i)*2.39996323;
            vec2 offset=vec2(cos(a),sin(a))*r*radius;
            vec2 q=uv+offset/p.Jitter.zw;
            vec4 s=texture(A,q);
            float w=clamp(s.a-length(offset)+1.,0.,1.);
            // Far samples must also cover the receiving lens footprint, excluding in-focus samples.
            if(!nearField) w*=clamp(s.a,0.,1.);
            float bokeh=1.+p.Options.z*clamp(luma(s.rgb),0.,32.);
            sum+=s.rgb*w*bokeh; weight+=w*bokeh; coverage+=w;
        }
        vec3 color=weight>1e-6 ? sum/weight : texture(A,uv).rgb;
        float alpha=nearField ? coverage/float(count) : texture(A,uv).a;
        Color=nearField ? vec4(color*alpha,alpha) : vec4(color,alpha); return;
    }
    if(stage==7 || stage==8) {
        vec4 sum=vec4(0); float weight=0;
        float center=texture(B,uv).r;
        // Small tent fill removes holes in the finite disc gather; retain far depth boundaries.
        for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
            vec2 q=uv+vec2(x,y)/vec2(textureSize(A,0));
            float w=float((2-abs(x))*(2-abs(y)));
            if(stage==7) w*=1./(1.+abs(texture(B,q).r-center)*4.);
            sum+=texture(A,q)*w; weight+=w;
        }
        Color=sum/max(weight,1e-6); return;
    }
    vec4 sharp=texture(A,uv);
    float coc=at(B,xy).r;
    // Bilateral upsample of far colour. Near is premultiplied coverage and deliberately crosses depth edges.
    vec2 position=uv*vec2(textureSize(C,0))-.5, base=floor(position), f=fract(position);
    vec3 farColor=vec3(0); float total=0;
    vec4 nearColor=vec4(0);
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        vec2 q=(base+vec2(x,y)+.5)/vec2(textureSize(C,0));
        float w=(x==0?1.-f.x:f.x)*(y==0?1.-f.y:f.y);
        float bilateral=w/(1.+abs(texture(B,q).r-coc)*4.);
        farColor+=texture(C,q).rgb*bilateral; total+=bilateral;
        nearColor+=texture(D,q)*w;
    }
    vec3 result=mix(sharp.rgb,farColor/max(total,1e-6),smoothstep(.5,2.,coc));
    result=result*(1.-nearColor.a)+nearColor.rgb;
    Color=vec4(result,sharp.a);
})"
