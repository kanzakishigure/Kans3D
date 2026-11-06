#pragma once
// Engine-owned GLSL 450; no filesystem or editor resource dependency.
namespace Kans::SceneShaders
{

    inline constexpr const char* HdrVertex = R"glsl(#version 450
layout (location = 0) in vec3 aPos;
layout(location = 0) out vec3 WorldPos;
layout(set = 0, binding = 0, std140) uniform Capture { mat4 projection; mat4 view; vec4 Parameters; };

void main()
{
    WorldPos = aPos;
    gl_Position =  projection * view * vec4(WorldPos, 1.0);
}
)glsl";

    inline constexpr const char* HdrFragment = R"glsl(#version 450
layout(location = 0) in vec3 WorldPos;
layout(set = 0, binding = 1) uniform sampler2D equirectangularMap;
const vec2 invAtan = vec2(0.1591, 0.3183);
layout(location = 0) out vec4 FragColor;
vec2 SampleSphericalMap(vec3 v)
{
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}
void main()
{
    vec2 uv = SampleSphericalMap(normalize(WorldPos));
    vec3 color = texture(equirectangularMap, uv).rgb;
    FragColor = vec4(color, 1.0);
}
)glsl";

    inline constexpr const char* IrradianceVertex = R"glsl(#version 450
layout (location = 0) in vec3 aPos;
layout(location = 0) out vec3 WorldPos;
layout(set = 0, binding = 0, std140) uniform Capture { mat4 projection; mat4 view; vec4 Parameters; };

void main()
{
    WorldPos = aPos;
    gl_Position =  projection * view * vec4(WorldPos, 1.0);
}
)glsl";

    inline constexpr const char* IrradianceFragment = R"glsl(#version 450
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec3 WorldPos;
layout(set = 0, binding = 1) uniform samplerCube environmentMap;
const float PI = 3.14159265359;
void main()
{
    vec3 N = normalize(WorldPos);
    vec3 irradiance = vec3(0.0);
    vec3 up    = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));
    float sampleDelta = 0.025;
    float nrSamples = 0.0;
    for(float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for(float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            vec3 tangentSample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            irradiance += texture(environmentMap, sampleVec).rgb * cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    irradiance = PI * irradiance * (1.0 / float(nrSamples));
    FragColor = vec4(irradiance, 1.0);
    }
)glsl";

    inline constexpr const char* PrefilterVertex = R"glsl(#version 450
layout (location = 0) in vec3 aPos;
layout(location = 0) out vec3 WorldPos;
layout(set = 0, binding = 0, std140) uniform Capture { mat4 projection; mat4 view; vec4 Parameters; };

void main()
{
    WorldPos = aPos;
    gl_Position =  projection * view * vec4(WorldPos, 1.0);
}
)glsl";

    inline constexpr const char* PrefilterFragment = R"glsl(#version 450
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec3 WorldPos;
layout(set = 0, binding = 1) uniform samplerCube environmentMap;
layout(set = 0, binding = 0, std140) uniform Capture { mat4 projection; mat4 view; vec4 Parameters; };

const float PI = 3.14159265359;
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
     bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
     bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
     bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
     bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
     return float(bits) * 2.3283064365386963e-10;
}
vec2 Hammersley(uint i, uint N)
{
    return vec2(float(i)/float(N), RadicalInverse_VdC(i));
}
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return nom / denom;
}
vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness*roughness;
	float phi = 2.0 * PI * Xi.x;
	float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
	float sinTheta = sqrt(1.0 - cosTheta*cosTheta);
	vec3 H;
	H.x = cos(phi) * sinTheta;
	H.y = sin(phi) * sinTheta;
	H.z = cosTheta;
	vec3 up          = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent   = normalize(cross(up, N));
	vec3 bitangent = normalize(cross(N, tangent));
	vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
	return normalize(sampleVec);
}
void main()
{
    float roughness = Parameters.x;
    vec3 N = normalize(WorldPos);
    vec3 R = N;
    vec3 V = R;
    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    vec3 prefilteredColor = vec3(0.0);
    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H  = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if(NdotL > 0.0)
        {
            float D   = DistributionGGX(N, H, roughness);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;
            float resolution = 512.0;
            float saTexel  = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);
            float mipLevel = roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);
            prefilteredColor += textureLod(environmentMap, L, mipLevel).rgb * NdotL;
            totalWeight      += NdotL;
        }
    }
    prefilteredColor = prefilteredColor / totalWeight;
    FragColor = vec4(prefilteredColor, 1.0);
}
)glsl";

    inline constexpr const char* BrdfFragment = R"glsl(#version 450
layout(location = 0) out vec2 FragColor;
layout(location = 0) in vec2 TexCoords;
const float PI = 3.14159265359;
float RadicalInverse_VdC(uint bits)
{
     bits = (bits << 16u) | (bits >> 16u);
     bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
     bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
     bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
     bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
     return float(bits) * 2.3283064365386963e-10;
}
vec2 Hammersley(uint i, uint N)
{
	return vec2(float(i)/float(N), RadicalInverse_VdC(i));
}
vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
	float a = roughness*roughness;
	float phi = 2.0 * PI * Xi.x;
	float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
	float sinTheta = sqrt(1.0 - cosTheta*cosTheta);
	vec3 H;
	H.x = cos(phi) * sinTheta;
	H.y = sin(phi) * sinTheta;
	H.z = cosTheta;
	vec3 up          = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent   = normalize(cross(up, N));
	vec3 bitangent = cross(N, tangent);
	vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
	return normalize(sampleVec);
}
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0;
    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / denom;
}
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}
vec2 IntegrateBRDF(float NdotV, float roughness)
{
    vec3 V;
    V.x = sqrt(1.0 - NdotV*NdotV);
    V.y = 0.0;
    V.z = NdotV;
    float A = 0.0;
    float B = 0.0;
    vec3 N = vec3(0.0, 0.0, 1.0);
    const uint SAMPLE_COUNT = 1024u;
    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);
        if(NdotL > 0.0)
        {
            float G = GeometrySmith(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);
    return vec2(A, B);
}
void main()
{
    vec2 integratedBRDF = IntegrateBRDF(TexCoords.x, TexCoords.y);
    FragColor = integratedBRDF;
}
)glsl";

    inline constexpr const char* BrdfVertex = R"glsl(#version 450
layout(set = 0, binding = 0, std140) uniform Capture { mat4 projection; mat4 view; vec4 Parameters; };
layout(location = 0) out vec2 TexCoords;
void main() {
    const vec2 corners[4] = vec2[](vec2(-1,1),vec2(-1,-1),vec2(1,1),vec2(1,-1));
    vec2 p = corners[gl_VertexIndex];
    TexCoords = p * 0.5 + 0.5;
    gl_Position = projection * vec4(p,0,1);
}
)glsl";

    inline constexpr const char* StencilVertex = R"glsl(#version 450
layout(set = 0, binding = 0, std140) uniform SceneDraw { mat4 U_ViewProjection; mat4 U_Transform; mat4 DeviceClip; vec4 U_ViewPos; vec4 Settings; };

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TextureCroods;
layout(location = 3) in vec4 a_BaseColor;


layout(location = 0) out vec3 V_Normal;
layout(location = 1) out vec2 V_TexCroods;
layout(location = 2) out vec4 V_BaseColor;
void main()
{
	gl_Position = DeviceClip * U_ViewProjection*U_Transform*vec4(a_Position, 1.0);
	V_TexCroods = a_TextureCroods;
	V_BaseColor = a_BaseColor;
	V_Normal = mat3(transpose(inverse(U_Transform)))*a_Normal;
}
)glsl";

    inline constexpr const char* StencilFragment = R"glsl(#version 450
layout(location = 0) out vec4 color;
layout(location = 1) in vec2 V_TexCroods;
layout(location = 2) in vec4 V_BaseColor;
layout(location = 0) in vec3 V_Normal;
void main()
{
	color = vec4(0.89, 0.67, 0.52, 1);
}
)glsl";

    inline constexpr const char* OutlineVertex = R"glsl(#version 450
layout(set = 0, binding = 0, std140) uniform SceneDraw { mat4 U_ViewProjection; mat4 U_Transform; mat4 DeviceClip; vec4 U_ViewPos; vec4 Settings; };

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TextureCroods;
layout(location = 3) in vec4 a_BaseColor;
layout(location = 4) in vec4 a_Tangent;
layout(location = 0) out vec2 V_TexCroods;
layout(location = 1) out vec4 V_BaseColor;
layout(location = 2) out float V_Fade_rate;



float rand(float x)
{
    float y = fract(sin(x)*100000.0);
    return y;
}
float noise(float x)
{
    float i = floor(x);
    float f = fract(x);
    float u = f * f * (3.0 - 2.0 * f );
    float y = mix(rand(i), rand(i + 1.0), u);
    return y ;
}
float OutlineAlpha(float dist, float k, float n) {
    float d_over_k = dist / k;
    float alpha = 1.0 / (1.0 + pow(d_over_k, n));
    return alpha;
}
void main()
{
    V_TexCroods = a_TextureCroods;
    V_BaseColor = a_BaseColor;
	vec3 Normal =  mat3(U_ViewProjection)*mat3(transpose(inverse(U_Transform)))*a_Normal;
    Normal = normalize(Normal);
	vec3 tangent = mat3(U_ViewProjection)*mat3(transpose(inverse(U_Transform)))*a_Tangent.xyz;
    tangent = normalize(tangent);
	vec3 bitangent =  cross(Normal,tangent)*a_Tangent.w;
    bitangent = normalize(bitangent);
	mat3 TBN = mat3(tangent, bitangent, Normal);
	Normal = TBN*a_BaseColor.rgb;
    Normal.z = -0.5;
    Normal = normalize(Normal);
    vec4 s_Position = U_ViewProjection*U_Transform*vec4(a_Position, 1.0);
    vec4 world_pos = U_Transform*vec4(a_Position, 1.0);
    float dist = distance(world_pos.xyz/world_pos.w, U_ViewPos.xyz);
    V_Fade_rate = OutlineAlpha(dist,10, 5.5);
    gl_Position = DeviceClip * vec4(s_Position.xy + Normal.xy*s_Position.w*0.004*V_Fade_rate,s_Position.z,s_Position.w);
}
)glsl";

    inline constexpr const char* OutlineFragment = R"glsl(#version 450
layout(location = 0) out vec4 O_Color;
layout(location = 0) in vec2  V_TexCroods;
layout(location = 1) in vec4  V_BaseColor;
layout(location = 2) in float V_Fade_rate;
void main() { O_Color = vec4(0,0,0,V_Fade_rate); }
)glsl";

    inline constexpr const char* SkyboxVertex = R"glsl(#version 450
layout(set = 0, binding = 0, std140) uniform SceneDraw { mat4 U_ViewProjection; mat4 U_Transform; mat4 DeviceClip; vec4 U_ViewPos; vec4 Settings; };

layout (location = 0) in vec3 aPos;


layout(location = 0) out vec3 localPos;
void main()
{
    localPos = aPos;
    mat4 rotView = mat4(mat3(U_Transform));
    vec4 clipPos = U_ViewProjection * rotView * vec4(localPos, 1.0);
    gl_Position = DeviceClip * clipPos.xyww;
}
)glsl";

    inline constexpr const char* SkyboxFragment = R"glsl(#version 450
layout(set = 0, binding = 0, std140) uniform SceneDraw { mat4 U_ViewProjection; mat4 U_Transform; mat4 DeviceClip; vec4 U_ViewPos; vec4 Settings; };
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec3 localPos;
layout(set = 0, binding = 1) uniform samplerCube environmentMap;
void main()
{
    vec3 direction = localPos;
    if (Settings.yz != vec2(1.0, 0.0))
        direction = vec3(Settings.y * localPos.x - Settings.z * localPos.z, localPos.y,
                         Settings.z * localPos.x + Settings.y * localPos.z);
    vec3 envColor = texture(environmentMap, direction).rgb;
    if (Settings.x != 1.0) envColor *= Settings.x;
    if (Settings.w != 0) { FragColor = vec4(envColor,1); return; }
    envColor = envColor / (envColor + vec3(1.0));
    envColor = pow(envColor, vec3(1.0/2.2));
    FragColor = vec4(envColor, 1.0);
}
)glsl";

    inline constexpr const char* NormalVertex = R"glsl(#version 450
layout(set = 0, binding = 0, std140) uniform SceneDraw { mat4 U_ViewProjection; mat4 U_Transform; mat4 DeviceClip; vec4 U_ViewPos; vec4 Settings; };
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in float a_End;
void main() {
    vec3 normal = normalize(mat3(U_ViewProjection) * mat3(transpose(inverse(U_Transform))) * a_Normal);
    gl_Position = U_ViewProjection * U_Transform * vec4(a_Position,1);
    gl_Position += vec4(normal,0) * (0.05 * a_End);
    gl_Position = DeviceClip * gl_Position;
}
)glsl";

    inline constexpr const char* NormalFragment = R"glsl(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1); }
)glsl";

} // namespace Kans::SceneShaders
