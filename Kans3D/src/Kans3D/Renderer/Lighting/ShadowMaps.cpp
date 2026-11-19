#include "kspch.h"
#include "Kans3D/Renderer/Lighting/ShadowMaps.h"
#include <glm/gtc/matrix_transform.hpp>
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "Kans3D/Renderer/2D/TextRenderer.h"
#include "Kans3D/Renderer/Features/ParticleRenderFeature.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Kans3D/Renderer/Shading/MaterialPass.h"
#include "Kans3D/VFX/VfxWorld.h"

namespace Kans
{
    bool StaticShadowKey::operator==(const StaticShadowKey& b) const
    {
        return World == b.World && Casters == b.Casters && Matrix == b.Matrix && Position == b.Position &&
               Settings == b.Settings && Split == b.Split;
    }
    std::array<float, 4> ShadowSplits(float n, float f, uint32_t count, float lambda)
    {
        std::array<float, 4> splits {};
        count = std::clamp(count, 1u, 4u);
        if (!(n > 0 && f > n))
            return splits;
        for (uint32_t i = 0; i < count; ++i)
        {
            float t   = float(i + 1) / count;
            splits[i] = i + 1 == count ? f : lambda * n * std::pow(f / n, t) + (1 - lambda) * (n + (f - n) * t);
        }
        return splits;
    }
    std::array<ShadowCascade, 4>
    BuildShadowCascades(const RenderCamera& camera, glm::vec3 direction, ShadowSettings settings)
    {
        const auto       s                 = settings.Validated();
        const glm::dmat4 inverseProjection = glm::inverse(glm::dmat4(camera.camera.GetProjectionMatrix()));
        auto             unproject         = [&](double x, double y, double z) {
            auto p = inverseProjection * glm::dvec4(x, y, z, 1);
            return glm::dvec3(p) / p.w;
        };
        const double firstDepth = -unproject(0, 0, -1).z, lastDepth = -unproject(0, 0, 1).z;
        const double nearPlane = std::min(firstDepth, lastDepth), cameraFar = std::max(firstDepth, lastDepth);
        const float  farPlane = std::min(float(cameraFar), s.MaxDistance);
        std::array<ShadowCascade, 4> result {};
        if (!(nearPlane > 0 && farPlane > nearPlane) || glm::length(direction) < 1e-6f)
            return result;
        const auto splits      = ShadowSplits(float(nearPlane), farPlane, s.Cascades, s.Lambda);
        const auto inverseView = glm::inverse(glm::dmat4(camera.viewMatrix));
        const auto dir         = glm::normalize(glm::dvec3(direction));
        const auto lightView =
            glm::lookAt(glm::dvec3(0), dir, std::abs(dir.y) < .99 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1));
        float start = float(nearPlane);
        for (uint32_t i = 0; i < s.Cascades; ++i)
        {
            // Include the previous cascade's full blend band in the next slice.
            double                    sliceNear = i ? result[i - 1].BlendStart : nearPlane;
            std::array<glm::dvec3, 8> corners;
            glm::dvec3                center(0);
            unsigned                  index = 0;
            for (double z : {sliceNear, double(splits[i])})
                for (double y : {-1.0, 1.0})
                    for (double x : {-1.0, 1.0})
                    {
                        auto a = unproject(x, y, -1), b = unproject(x, y, 1);
                        if (a.z < b.z)
                            std::swap(a, b);
                        auto p           = glm::mix(a, b, (z - nearPlane) / (cameraFar - nearPlane));
                        corners[index++] = p;
                        center += p / 8.0;
                    }
            double radius = 0;
            for (const auto& p : corners)
                radius = std::max(radius, glm::length(p - center));
            // Radius is computed in camera space: camera rotation cannot change the texel scale.
            radius = std::ceil(radius * 16.0) / 16.0;
            radius *= double(s.Resolution) / (s.Resolution - 2.0); // One texel guard for snapping.
            double texel = 2.0 * radius / s.Resolution;
            auto   c     = glm::dvec3(lightView * inverseView * glm::dvec4(center, 1));
            for (unsigned axis = 0; axis < 3; ++axis)
                c[axis] = std::floor(c[axis] / texel + .5) * texel;
            double nearZ = c.z + radius + s.MaxDistance, farZ = c.z - radius;
            auto   projection      = glm::ortho(c.x - radius, c.x + radius, c.y - radius, c.y + radius, -nearZ, -farZ);
            auto&  cascade         = result[i];
            cascade.ViewProjection = DeviceProjection(glm::mat4(projection * lightView),
                                                      {RenderHardware::ClipYDirection::Down,
                                                       RenderHardware::DepthRange::ZeroToOne,
                                                       RenderHardware::FramebufferOrigin::UpperLeft});
            cascade.Split          = splits[i];
            cascade.BlendStart     = splits[i] - (splits[i] - start) * s.Blend;
            cascade.Radius         = float(radius);
            cascade.TexelSize      = float(texel);
            cascade.DepthRange     = float(nearZ - farZ);
            start                  = splits[i];
        }
        return result;
    }
    std::vector<uint32_t> SelectPointShadows(const SceneInfo& scene, uint32_t cap)
    {
        std::vector<uint32_t> result;
        for (uint32_t i = 0; i < std::min(size_t(EnginePointLightLimit), scene.pointLights.size()); ++i)
            if (scene.pointLights[i].Shadows.Enabled)
                result.push_back(i);
        std::sort(result.begin(), result.end(), [&](auto a, auto b) {
            const auto& x  = scene.pointLights[a];
            const auto& y  = scene.pointLights[b];
            auto        dx = x.Position - scene.sceneCamera.Position, dy = y.Position - scene.sceneCamera.Position;
            float       ax = glm::dot(dx, dx), by = glm::dot(dy, dy);
            if (!std::isfinite(ax))
                ax = std::numeric_limits<float>::max();
            if (!std::isfinite(by))
                by = std::numeric_limits<float>::max();
            if (ax != by)
                return ax < by;
            if (x.EntityID != y.EntityID)
                return uint64_t(x.EntityID) < uint64_t(y.EntityID);
            return a < b;
        });
        result.resize(std::min(result.size(), size_t(std::min(cap, 4u))));
        return result;
    }
    glm::mat4 SpotViewProjection(const SpotLight& light)
    {
        auto direction = glm::length(light.Direction) > 1e-6f ? glm::normalize(light.Direction) : glm::vec3(0, 0, -1);
        auto up        = light.Up;
        if (glm::length(glm::cross(direction, up)) < 1e-5f)
            up = std::abs(direction.y) < .99f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
        float range = light.Range > 0 ? light.Range : light.Shadows.Validated().MaxDistance;
        if (light.Shadows.Enabled)
            range = std::min(range, light.Shadows.Validated().MaxDistance);
        auto projection = glm::perspective(glm::radians(2 * std::clamp(light.OuterConeAngle, .01f, 89.f)),
                                           1.f,
                                           std::min(.01f, range * .01f),
                                           std::max(range, .001f));
        return DeviceProjection(projection,
                                {RenderHardware::ClipYDirection::Down,
                                 RenderHardware::DepthRange::ZeroToOne,
                                 RenderHardware::FramebufferOrigin::UpperLeft}) *
               glm::lookAt(light.Position, light.Position + direction, up);
    }
    std::vector<SpotShadowTile> AllocateSpotShadows(const SceneInfo& scene, uint32_t size, uint32_t cap)
    {
        auto power = [](uint32_t v) {
            uint32_t p = 1;
            while (p <= v / 2)
                p *= 2;
            return p;
        };
        size = power(std::clamp(size, 64u, 4096u));
        struct Candidate
        {
            uint32_t Index, Resolution;
            float    Coverage, Distance;
        };
        std::vector<Candidate> candidates;
        for (uint32_t i = 0; i < std::min(scene.spotLights.size(), size_t(EngineSpotLightLimit)); ++i)
        {
            const auto& light = scene.spotLights[i];
            if (!light.Shadows.Enabled)
                continue;
            auto  delta    = light.Position - scene.sceneCamera.Position;
            float distance = glm::dot(delta, delta);
            float radius   = (light.Range > 0 ? light.Range : light.Shadows.Validated().MaxDistance) *
                             std::tan(glm::radians(std::clamp(light.OuterConeAngle, .01f, 89.f)));
            float coverage = radius * radius / std::max(distance, .01f);
            if (!std::isfinite(distance) || !std::isfinite(coverage))
                continue;
            candidates.push_back({i, power(std::min(size, light.Shadows.Validated().Resolution)), coverage, distance});
        }
        std::sort(candidates.begin(), candidates.end(), [&](auto a, auto b) {
            if (a.Coverage != b.Coverage)
                return a.Coverage > b.Coverage;
            if (a.Distance != b.Distance)
                return a.Distance < b.Distance;
            auto x = uint64_t(scene.spotLights[a.Index].EntityID), y = uint64_t(scene.spotLights[b.Index].EntityID);
            return x != y ? x < y : a.Index < b.Index;
        });
        candidates.resize(std::min(candidates.size(), size_t(std::min(cap, 8u))));
        auto area = [&] {
            uint64_t sum = 0;
            for (auto c : candidates)
                sum += uint64_t(c.Resolution) * c.Resolution;
            return sum;
        };
        while (area() > uint64_t(size) * size)
        {
            // Reduce the largest tile, with reverse priority breaking ties. Power-of-two
            // squares fit exactly when their total area fits this quadtree.
            size_t largest = 0;
            for (size_t i = 0; i < candidates.size(); ++i)
                if (candidates[i].Resolution >= candidates[largest].Resolution)
                    largest = i;
            if (candidates[largest].Resolution <= 64)
            {
                candidates.pop_back();
                continue;
            }
            candidates[largest].Resolution /= 2;
        }
        struct Square
        {
            uint32_t X, Y, Size;
        };
        std::vector<Square>   free {{0, 0, size}};
        std::vector<uint32_t> order;
        for (uint32_t i = 0; i < candidates.size(); ++i)
            order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
            return candidates[a].Resolution > candidates[b].Resolution;
        });
        std::vector<SpotShadowTile> result(candidates.size());
        for (auto index : order)
        {
            const auto c      = candidates[index];
            auto       it     = std::min_element(free.begin(), free.end(), [](auto a, auto b) {
                return std::tie(a.Size, a.Y, a.X) < std::tie(b.Size, b.Y, b.X);
            });
            auto       square = *it;
            free.erase(it);
            while (square.Size > c.Resolution)
            {
                square.Size /= 2;
                free.push_back({square.X + square.Size, square.Y, square.Size});
                free.push_back({square.X, square.Y + square.Size, square.Size});
                free.push_back({square.X + square.Size, square.Y + square.Size, square.Size});
            }
            result[index] = {c.Index, square.X, square.Y, c.Resolution};
        }
        return result;
    }
    namespace
    {
        using namespace RenderHardware;
        const char*                     DepthVertex   = R"(#version 450
#include <Kans3D/Engine.glsl>
#include <Kans3D/VertexInput.glsl>
layout(location=0) out vec2 v_UV;
layout(location=1) out vec3 v_WorldPosition;
layout(location=2) out vec4 v_Color;
void main() {
    v_UV = a_UV; v_Color = a_Color;
    v_WorldPosition = vec3(u_Object.Transform * vec4(a_Position,1));
    gl_Position = u_Frame.ViewProjection * vec4(v_WorldPosition,1);
})";
        const char*                     DepthFragment = R"(#version 450
#include <Kans3D/ShadowCaster.glsl>
layout(set=1,binding=0,std140) uniform StandardParameters {
    vec4 BaseColor; float Metallic; float Roughness;
    vec3 EmissiveColor; float EmissiveIntensity; float NormalStrength; float OcclusionStrength;
    vec2 UVTiling; vec2 UVOffset; float VertexColorStrength;
} p;
layout(set=1,binding=1) uniform sampler2D BaseColorMap;
layout(location=0) in vec2 v_UV;
layout(location=1) in vec3 v_WorldPosition;
layout(location=2) in vec4 v_Color;
void main() {
    vec2 uv = v_UV * p.UVTiling + p.UVOffset;
    float alpha = p.BaseColor.a * mix(1.0,v_Color.a,p.VertexColorStrength) * texture(BaseColorMap,uv).a;
    KansAlphaClip(alpha);
    KansWriteShadowDepth(v_WorldPosition);
})";
        Handle<RenderHardware::Texture> DepthTexture(Device& device, uint32_t size, bool cube)
        {
            TextureDesc desc;
            desc.Width = desc.Height = size;
            desc.Dimension           = cube ? TextureDimension::Cube : TextureDimension::Texture2D;
            desc.PixelFormat         = Format::Depth32Float;
            desc.Usage = RenderHardware::TextureUsage::DepthStencil | RenderHardware::TextureUsage::Sampled |
                         RenderHardware::TextureUsage::CopyDestination | RenderHardware::TextureUsage::CopySource;
            return device.CreateTexture(desc);
        }
    } // namespace
    std::shared_ptr<ShadowResources> CreateShadowFallback(RenderHardware::Device& device)
    {
        auto        result = std::make_shared<ShadowResources>();
        SamplerDesc sampler;
        sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
        sampler.Mip                       = Filter::Nearest;
        sampler.MaxLod                    = 0;
        sampler.CompareEnabled            = true;
        result->Compare                   = device.CreateSampler(sampler);
        sampler.CompareEnabled            = false;
        sampler.Min = sampler.Mag = Filter::Nearest;
        result->Raw               = device.CreateSampler(sampler);
        sampler.Min = sampler.Mag = Filter::Linear;
        result->CookieSampler     = device.CreateSampler(sampler);
        result->Directional       = DepthTexture(device, 1, false);
        result->Spots             = result->Directional;
        // Inactive cookie rectangles return white without sampling. Alias the
        // initialized raw depth fallback instead of adding an unused image to
        // every ordinary scene's graph; active cookies allocate a color atlas.
        result->Cookies = result->Directional;
        result->Points.fill(DepthTexture(device, 1, true));
        float lit    = 1;
        auto  buffer = device.CreateBuffer({sizeof(lit), BufferUsage::CopySource, MemoryUsage::Upload});
        buffer->Write(0, &lit, sizeof(lit));
        auto commands = device.CreateCommandList();
        commands->CopyBufferToTexture(buffer, 0, result->Directional);
        for (uint32_t face = 0; face < 6; ++face)
            commands->CopyBufferToTexture(buffer, 0, result->Points[0], {0, face});
        device.Submit(commands);
        return result;
    }
} // namespace Kans

namespace Kans
{
    GraphTexture BuildCustomDepthPass(RenderGraph& graph, RenderFrame& frame)
    {
        using namespace RenderHardware;
        auto        device = frame.GetDevice();
        TextureDesc desc;
        desc.Width        = frame.View.Viewport.x;
        desc.Height       = frame.View.Viewport.y;
        desc.PixelFormat  = Format::Depth24Stencil8;
        desc.Usage        = RenderHardware::TextureUsage::DepthStencil | RenderHardware::TextureUsage::Sampled;
        const auto target = graph.CreateTexture("CustomDepthStencil", desc);
        struct Draw
        {
            std::shared_ptr<GpuShader>           Shader;
            std::shared_ptr<GpuMaterialBindings> Bindings;
            const GpuMesh*                       Mesh;
            MaterialPassData                     Data;
            size_t                               Pass = SIZE_MAX;
        };
        std::vector<Draw>     draws;
        std::vector<GraphUse> uses {{target, GraphAccess::DepthWrite}};
        auto                  sampled = [&](const auto& texture) {
            if (!texture)
                return;
            const auto id = graph.ImportTexture("CustomDepth material input", texture);
            if (std::none_of(uses.begin(), uses.end(), [&](auto u) { return u.Texture == id; }))
                uses.push_back({id, GraphAccess::SampledRead});
        };
        auto uniforms              = frame.View.LightingUniforms.value_or(MakeLightingUniforms(frame.View.Scene));
        uniforms.View              = frame.View.Scene.sceneCamera.viewMatrix;
        uniforms.Projection        = DeviceProjection(frame.View.Scene.sceneCamera.camera.GetProjectionMatrix(),
                                                      device->GetCoordinateConventions());
        uniforms.ViewProjection    = uniforms.Projection * uniforms.View;
        uniforms.InverseView       = glm::inverse(uniforms.View);
        uniforms.InverseProjection = glm::inverse(uniforms.Projection);
        uniforms.Time              = frame.View.Time;
        uniforms.ShadowPass        = glm::vec4(0);
        const auto frameUpload     = frame.AllocateUniforms(sizeof(uniforms));
        auto       buffer          = frameUpload.Buffer;
        buffer->Write(frameUpload.Offset, &uniforms, sizeof(uniforms));
        // MaterialOrder has the main camera's frustum culling, independent of depth visibility.
        for (const auto& item : frame.View.MaterialOrder)
        {
            if (item.Text || item.UI)
                continue;
            const auto& draw = frame.View.Draws[item.DrawIndex];
            if (!draw.RenderCustomDepth)
                continue;
            const auto* mesh     = frame.GetMesh(draw.Mesh);
            const auto* bindings = frame.GetBindings(draw.Material, false);
            const auto* shader   = bindings ? frame.GetShader(draw.Material, bindings->Shader) : nullptr;
            if (!mesh || !bindings || !shader || shader->Domain != ShaderDomain::Surface)
                continue;
            size_t selected = SIZE_MAX;
            for (const auto* tag : {"DepthOnly", "ShadowCaster"})
            {
                for (size_t i = 0; i < shader->Passes.size(); ++i)
                    if (shader->Passes[i].Tag == tag && bindings->PassEnabled(shader->Passes[i].Name))
                    {
                        selected = i;
                        break;
                    }
                if (selected != SIZE_MAX)
                    break;
            }
            if (selected == SIZE_MAX)
            {
                if (!IsStandardShader(bindings->Shader))
                {
                    bindings = frame.Assets()->GetMaterialBindings(draw.Material, true);
                    shader   = bindings ? frame.Assets()->GetShader(bindings->Shader) : nullptr;
                }
                if (!bindings || !shader)
                    continue;
            }
            EngineObjectUniforms object;
            object.Transform      = draw.Transform;
            object.NormalMatrix   = glm::transpose(glm::inverse(draw.Transform));
            object.MaterialRender = {float(bindings->Render.AlphaMode),
                                     bindings->Render.AlphaCutoff,
                                     bindings->Render.DoubleSided ? 1.0f : 0.0f,
                                     0};
            const auto objectUpload = frame.AllocateUniforms(sizeof(object));
            auto       ob           = objectUpload.Buffer;
            ob->Write(objectUpload.Offset, &object, sizeof(object));
            MaterialPassData data;
            data.UniformStorage = frame.UniformStorage;
            data.FrameOffset    = frameUpload.Offset;
            data.ObjectOffset   = objectUpload.Offset;
            data.Frame         = buffer;
            data.Object        = ob;
            data.CustomDepth   = true;
            data.CustomStencil = draw.CustomStencil;
            data.ColorFormats.clear();
            data.Section            = draw.Section;
            data.Render             = bindings->Render;
            data.Parameters         = draw.Parameters;
            data.Environment        = frame.GetMaterialEnvironment();
            data.EnvironmentSampler = frame.Services.GetMaterialEnvironmentSampler();
            data.Shadows            = frame.View.Shadows ? frame.View.Shadows : frame.Assets()->GetShadowFallback();
            data.Palette =
                mesh->Skin ? frame.Assets()->GetSkinningPalette(draw.SkinPose, mesh) : Handle<RenderHardware::Buffer>();
            if (selected == SIZE_MAX)
                data.DepthProgram = data.Palette ?
                                        GetSkinnedInternalProgram(*device, "ShadowDepth", DepthVertex, DepthFragment) :
                                        frame.Services.GetInternalProgram("ShadowDepth", DepthVertex, DepthFragment);
            const bool smooth =
                selected == SIZE_MAX ? shader->UsesSmoothNormals : shader->Passes[selected].UsesSmoothNormals;
            // Warm the owning shader before snapshotting; MSAA records its main draws later.
            if (data.Palette && !shader->PrepareSkinning(smooth && !mesh->SmoothNormals))
            {
                frame.Complete = false;
                continue;
            }
            const auto* instance = frame.Assets()->GetInstanceMaterialBindings(*bindings, draw.Parameters);
            bool        complete = true;
            data.MaterialGroup   = frame.Assets()->GetMaterialGroup(*shader, *instance, &complete);
            frame.Complete &= complete;
            if (!data.MaterialGroup)
                continue;
            for (const auto& image : instance->Images)
                sampled(image);
            for (const auto& image : data.Environment)
                sampled(image);
            if (data.Shadows)
            {
                sampled(data.Shadows->Directional);
                sampled(data.Shadows->Spots);
                sampled(data.Shadows->Cookies);
                for (const auto& image : data.Shadows->Points)
                    sampled(image);
            }
            draws.push_back({std::make_shared<GpuShader>(shader->Snapshot()),
                             std::make_shared<GpuMaterialBindings>(instance->Snapshot()),
                             mesh,
                             data,
                             selected});
        }
        const auto pass =
            graph.AddPass("Custom depth/stencil", std::move(uses), [&frame, target, draws = std::move(draws)](auto& c) {
                RenderingDesc rendering;
                rendering.Depth.Target = c.Texture(target);
                rendering.Depth.Load   = LoadOp::Clear; // Device depth=1, stencil=0.
                c.Commands.BeginRendering(rendering);
                c.Commands.SetViewport({0, 0, float(frame.View.Viewport.x), float(frame.View.Viewport.y)});
                c.Commands.SetScissor({0, 0, frame.View.Viewport.x, frame.View.Viewport.y});
                for (const auto& draw : draws)
                {
                    frame.Complete &=
                        RecordMaterialDraw(*frame.Assets(),
                                           c.Commands,
                                           *draw.Shader,
                                           *draw.Bindings,
                                           *draw.Mesh,
                                           draw.Data,
                                           draw.Pass == SIZE_MAX ? nullptr : &draw.Shader->Passes[draw.Pass]);
                    ++frame.Stats.DrawCalls;
                    frame.Stats.Triangles += draw.Mesh->Sections[draw.Data.Section].IndexCount / 3;
                }
                c.Commands.EndRendering();
            });
        if (pass)
            graph.OrderAfter(pass, pass - 1);
        return target;
    }
    static void
    BuildSpotCookies(RenderGraph& graph, RenderFrame& frame, ShadowResources& resources, EngineFrameUniforms& uniforms)
    {
        using namespace RenderHardware;
        std::vector<Handle<RenderHardware::Texture>> images;
        std::map<AssetID, uint32_t>                  slots;
        for (size_t i = 0; i < std::min(frame.View.Scene.spotLights.size(), size_t(EngineSpotLightLimit)); ++i)
        {
            auto id = frame.View.Scene.spotLights[i].Cookie;
            if (!id.IsValid() || !frame.Assets())
                continue;
            auto image = frame.Assets()->GetTextureResource(id);
            if (!image)
            {
                frame.Complete = false;
                continue;
            }
            if (image->GetDesc().Dimension != TextureDimension::Texture2D || image->GetDesc().ArrayLayers != 1)
                continue;
            auto [it, inserted] = slots.emplace(id, uint32_t(images.size()));
            if (inserted)
                images.push_back(image);
            // 64 independent 128px tiles in a 1024px atlas; sampling clamps to the
            // half-texel interior, so no adjacent cookie can bleed through.
            const auto slot            = it->second;
            uniforms.SpotCookieRect[i] = glm::vec4(float(slot % 8) / 8, float(slot / 8) / 8, 1.f / 8, 1.f / 8);
        }
        if (images.empty())
            return;
        auto device = frame.GetDevice();
        if (resources.Cookies->GetDesc().Width != 1024)
        {
            TextureDesc desc;
            desc.Width = desc.Height = 1024;
            desc.Usage        = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::Sampled;
            resources.Cookies = device->CreateTexture(desc);
        }
        const auto            target = graph.ImportTexture("Spot cookie atlas", resources.Cookies);
        std::vector<GraphUse> uses {{target, GraphAccess::ColorWrite}};
        for (auto image : images)
            uses.push_back({graph.ImportTexture("Spot cookie source", image), GraphAccess::SampledRead});
        auto         program = frame.Services.GetInternalProgram("SpotCookieAtlas",
                                                                 R"(#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p*2-1,0,1);
})",
                                                                 R"(#version 450
layout(set=0,binding=0) uniform sampler2D Cookie;
layout(push_constant) uniform Tile { vec2 Origin; } tile;
layout(location=0) out vec4 Color;
void main() {
    vec2 uv = (gl_FragCoord.xy-tile.Origin)/128.0;
    // Imported textures use bottom-origin material UVs; the projection atlas
    // stores top-origin image rows, matching the light's device projection.
    uv.y = 1.0-uv.y;
    Color = vec4(textureLod(Cookie,uv,0).rgb,1);
}
)");
        auto         layout  = device->CreateBindGroupLayout({0, {{0, BindingType::CombinedTextureSampler}}});
        PipelineDesc desc;
        desc.Program           = program;
        desc.GroupLayouts      = {layout};
        desc.PushConstantBytes = 8;
        desc.ColorFormats      = {Format::RGBA8Unorm};
        desc.Rasterizer.Cull   = CullMode::None;
        desc.Blends.resize(1);
        auto pipeline = frame.Services.CachedPipeline(desc);
        graph.AddPass("Spot cookie projection atlas",
                      std::move(uses),
                      [target, images, layout, pipeline, sampler = resources.CookieSampler](auto& c) {
                          RenderingDesc render;
                          render.Colors.push_back({c.Texture(target)});
                          c.Commands.BeginRendering(render);
                          c.Commands.BindPipeline(pipeline);
                          for (uint32_t slot = 0; slot < images.size(); ++slot)
                          {
                              uint32_t x = slot % 8 * 128, y = slot / 8 * 128;
                              c.Commands.SetViewport({float(x), float(y), 128, 128});
                              c.Commands.SetScissor({int32_t(x), int32_t(y), 128, 128});
                              glm::vec2 origin(x, y);
                              c.Commands.PushConstants(0, &origin, sizeof(origin));
                              BindGroupDesc group;
                              group.Layout = layout;
                              group.Entries.push_back({0, {}, 0, 0, images[slot], sampler});
                              c.Commands.BindGroup(c.Device.CreateBindGroup(group));
                              c.Commands.Draw(3);
                          }
                          c.Commands.EndRendering();
                      });
    }
    void BuildShadowPasses(RenderGraph& graph, RenderFrame& frame)
    {
        auto& view = frame.View;
        if (view.Scene.pointLights.empty() && view.Scene.pointLight.Shadows.Enabled)
            view.Scene.pointLights.push_back(view.Scene.pointLight);
        const auto directional = view.Scene.dirLight.Shadows.Validated();
        const auto selected    = SelectPointShadows(view.Scene, view.MaxShadowedPointLights);
        const auto spots       = AllocateSpotShadows(view.Scene, view.SpotShadowAtlasSize, view.MaxShadowedSpotLights);
        const bool cookies = std::any_of(view.Scene.spotLights.begin(), view.Scene.spotLights.end(), [](const auto& l) {
            return l.Cookie.IsValid();
        });
        const bool dirEnabled  = directional.Enabled && glm::length(view.Scene.dirLight.Dirction) > 1e-6f;
        view.Shadows.reset();
        view.ShadowStats = {};
        if (view.LightingUniforms)
        {
            view.LightingUniforms->ShadowInfo = glm::vec4(0);
            view.LightingUniforms->ShadowPass = glm::vec4(0);
            for (auto& rect : view.LightingUniforms->SpotShadowRect)
                rect = glm::vec4(0);
            for (auto& rect : view.LightingUniforms->SpotCookieRect)
                rect = glm::vec4(0);
            for (auto& index : view.LightingUniforms->PointShadowIndices)
                index = glm::vec4(-1);
        }
        if (!dirEnabled && selected.empty() && spots.empty() && !cookies)
        {
            // Drop full-size maps when no light uses them; submitted frames retain their own handles.
            frame.Output.GetShadowStorage().reset();
            return;
        }
        auto       device   = frame.GetDevice();
        const auto fallback = frame.Assets() ? frame.Assets()->GetShadowFallback() : CreateShadowFallback(*device);
        auto&      storage  = frame.Output.GetShadowStorage();
        if (!storage)
            storage = std::make_shared<ShadowResources>(*fallback);
        if (!dirEnabled)
            storage->Directional = fallback->Directional;
        for (size_t slot = selected.size(); slot < storage->Points.size(); ++slot)
            storage->Points[slot] = fallback->Points[slot];
        if (spots.empty())
            storage->Spots = fallback->Spots;
        if (!cookies)
            storage->Cookies = fallback->Cookies;
        auto shadows               = storage;
        view.Shadows               = shadows;
        auto       uniforms        = view.LightingUniforms.value_or(MakeLightingUniforms(view.Scene));
        const auto row             = glm::transpose(view.Scene.sceneCamera.viewMatrix);
        uniforms.ShadowCameraDepth = -row[2];
        uniforms.ShadowInfo        = glm::vec4(0);
        for (auto& rect : uniforms.SpotShadowRect)
            rect = glm::vec4(0);
        for (auto& rect : uniforms.SpotCookieRect)
            rect = glm::vec4(0);
        BuildSpotCookies(graph, frame, *shadows, uniforms);
        if (std::none_of(
                std::begin(uniforms.SpotCookieRect), std::end(uniforms.SpotCookieRect), [](auto r) { return r.z > 0; }))
            shadows->Cookies = fallback->Cookies;
        for (auto& indices : uniforms.PointShadowIndices)
            indices = glm::vec4(-1);
        const auto proxies     = frame.World.CollectDraws(0, view.HiddenHierarchy);
        const auto environment = frame.GetMaterialEnvironment();
        std::vector<Frustum> staticFrusta;
        if (view.CacheStaticShadows && std::any_of(proxies.begin(), proxies.end(), [](const auto& p) {
                return p.RenderMobility == Mobility::Static;
            }))
        {
            if (dirEnabled)
            {
                const auto cascades =
                    BuildShadowCascades(view.Scene.sceneCamera, view.Scene.dirLight.Dirction, directional);
                for (uint32_t i = 0; i < directional.Cascades && cascades[i].Split > 0; ++i)
                    staticFrusta.emplace_back(cascades[i].ViewProjection);
            }
            for (const auto& tile : spots)
                staticFrusta.emplace_back(uniforms.SpotViewProjection[tile.Light]);
            const glm::vec3 directions[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            const glm::vec3 ups[]        = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
            for (const auto index : selected)
            {
                const auto& light    = view.Scene.pointLights[index];
                const auto  settings = light.Shadows.Validated();
                const auto  projection =
                    DeviceTextureProjection(glm::perspective(glm::radians(90.f), 1.f, .01f, settings.MaxDistance),
                                            device->GetCoordinateConventions());
                for (unsigned face = 0; face < 6; ++face)
                    staticFrusta.emplace_back(
                        projection * glm::lookAt(light.Position, light.Position + directions[face], ups[face]));
            }
        }
        std::vector<size_t> staticCasters, movableCasters, allCasters;
        staticCasters.reserve(proxies.size());
        movableCasters.reserve(proxies.size());
        allCasters.reserve(proxies.size());
        std::string casterKey;
        if (!staticFrusta.empty())
            casterKey.reserve(proxies.size() * 256);
        std::vector<std::shared_ptr<void>> casterResources;
        std::unordered_set<const void*>    retained;
        const auto                         add = [&](const auto& value) {
            casterKey.append(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        const auto resource = [&](const auto& value) {
            add(value.get());
            if (value && retained.insert(value.get()).second)
                casterResources.push_back(value);
        };
        add(frame.MaterialMipBias);
        // Resolve material identity once per frame, never once per light. Custom vertex/shadow programs,
        // parameter drivers and render textures may change without an entity transform notification.
        std::map<AssetID, bool> staticMaterials;
        for (size_t i = 0; i < proxies.size(); ++i)
        {
            const auto& proxy = proxies[i];
            if (!proxy.CastShadows)
                continue;
            allCasters.push_back(i);
            bool eligible = view.CacheStaticShadows && frame.Assets() && proxy.RenderMobility == Mobility::Static &&
                            std::any_of(staticFrusta.begin(),
                                        staticFrusta.end(),
                                        [&](const auto& f) { return f.Intersects(proxy.WorldBounds); }) &&
                            frame.World.IsStaticShadowCaster(proxy);
            const GpuMaterialBindings* bindings = nullptr;
            const GpuShader*           shader   = nullptr;
            const GpuMesh*             mesh     = nullptr;
            if (eligible)
            {
                bindings = frame.GetBindings(proxy.Material, false);
                shader   = bindings ? frame.GetShader(proxy.Material, bindings->Shader) : nullptr;
                mesh     = frame.GetMesh(proxy.Mesh);
                eligible = bindings && shader && mesh && IsStandardShader(bindings->Shader) && !mesh->Skin &&
                           mesh->MorphTargets.empty();
            }
            if (eligible)
            {
                auto found = staticMaterials.find(proxy.Material);
                if (found == staticMaterials.end())
                {
                    bool stable = true;
                    for (const auto& texture : bindings->Textures)
                        if (texture.second.IsValid())
                        {
                            const auto asset = frame.Assets()->GetAssetManager().Request<TextureAsset>(texture.second);
                            if (!asset.IsReady() || asset.Asset->GetData().RenderTexture)
                                stable = false;
                        }
                    bool       complete = true;
                    const auto group =
                        stable ?
                            frame.Assets()->GetMaterialGroup(*shader, *bindings, &complete, frame.MaterialMipBias) :
                            nullptr;
                    stable &= complete && bool(group);
                    found = staticMaterials.emplace(proxy.Material, stable).first;
                    if (stable)
                    {
                        resource(shader->Program);
                        resource(group);
                        add(bindings->Images.size());
                        for (const auto& image : bindings->Images)
                            resource(image);
                        add(bindings->Blocks.size());
                        add(bindings->Textures.size());
                        for (const auto& block : bindings->Blocks)
                        {
                            add(block.first);
                            resource(block.second.Resource());
                            add(block.second.Revision());
                        }
                        for (const auto& texture : bindings->Textures)
                        {
                            add(texture.first);
                            if (texture.second.IsValid())
                                resource(frame.Assets()->GetTextureResource(texture.second));
                        }
                        add(bindings->Render.AlphaMode);
                        add(bindings->Render.AlphaCutoff);
                        add(bindings->Render.DoubleSided);
                    }
                }
                eligible = found->second;
            }
            if (!eligible)
            {
                movableCasters.push_back(i);
                continue;
            }
            staticCasters.push_back(i);
            add(uint64_t(proxy.EntityID));
            add(uint64_t(proxy.InstanceRoot));
            add(proxy.Section);
            add(uint64_t(proxy.Material.GetSourceID().GetUUID()));
            add(proxy.Material.GetLocalID());
            add(frame.IncludesSection(proxy.EntityID, proxy.InstanceRoot, mesh, proxy.Section));
            for (unsigned col = 0; col < 4; ++col)
                for (unsigned row = 0; row < 4; ++row)
                    add(proxy.Transform[col][row]);
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                add(proxy.WorldBounds.Min[axis]);
                add(proxy.WorldBounds.Max[axis]);
            }
            add(mesh->VertexBindings.size());
            add(mesh->Attributes.size());
            for (const auto& binding : mesh->VertexBindings)
            {
                add(binding.Slot);
                add(binding.Stride);
                add(binding.PerInstance);
            }
            for (const auto& attribute : mesh->Attributes)
            {
                add(attribute.Location);
                add(attribute.BufferSlot);
                add(attribute.Offset);
                add(attribute.Type);
            }
            resource(mesh->Vertices);
            resource(mesh->Indices);
            if (proxy.Section < mesh->Sections.size())
            {
                const auto& section = mesh->Sections[proxy.Section];
                add(section.FirstIndex);
                add(section.IndexCount);
            }
        }
        if (casterKey != shadows->StaticCasterKey)
        {
            ++shadows->StaticRevision;
            shadows->StaticCasterKey       = std::move(casterKey);
            shadows->StaticCasterResources = std::move(casterResources);
        }
        if (staticCasters.empty())
        {
            shadows->StaticCaches.clear();
            shadows->StaticImages.clear();
            shadows->StaticCasterResources.clear();
        }
        std::unordered_set<std::string>                     usedCaches;
        std::unordered_set<const RenderHardware::Texture*>  usedImages;
        std::map<const RenderHardware::Texture*, GraphPass> previousLivePass;
        const auto*                                         vfx = frame.World.GetVfxWorld();
        const bool                                          movableExtras =
            (vfx && !vfx->Instances().empty()) ||
            std::any_of(view.Texts.begin(), view.Texts.end(), [](const auto& text) { return text.Style.CastShadows; });
        struct PreparedCaster
        {
            bool                                           Resolved = false;
            const GpuMesh*                                 Mesh     = nullptr;
            const GpuMaterialBindings*                     Bindings = nullptr;
            const GpuShader*                               Shader   = nullptr;
            const GpuShaderPass*                           Caster   = nullptr;
            UniformRange                                   Object;
            RenderHardware::Handle<RenderHardware::Buffer> Palette;
        };
        std::vector<PreparedCaster>    prepared(proxies.size());
        Handle<RenderHardware::Shader> depthProgram, skinnedDepthProgram;
        auto                           recordGeometry = [&](std::string                            name,
                                                            const Handle<RenderHardware::Texture>& texture,
                                                            uint32_t                               face,
                                                            uint32_t                               cascade,
                                                            const glm::mat4&                       matrix,
                                                            ShadowSettings                         settings,
                                                            glm::vec4                              position,
                                                            bool                                   point,
                                                            const SpotShadowTile*                  spot,
                                                            const std::vector<size_t>&             indices,
                                                            bool                                   staticOnly,
                                                            bool                                   loadCache,
                                                            ShadowPassStatistics&                  stats) {
            const auto           recordStart = RenderClockMilliseconds();
            const Frustum frustum(matrix);
            auto          commands = device->CreateCommandList();
            RenderingDesc rendering;
            rendering.Depth.Target           = texture;
            rendering.Depth.Subresource.Face = face;
            rendering.Depth.Load = staticOnly ? (point ? LoadOp::Clear : LoadOp::Load) :
                                                (!loadCache && (point || cascade == 0) ? LoadOp::Clear : LoadOp::Load);
            commands->BeginRendering(rendering);
            const uint32_t x = spot ? spot->X : point ? 0 : (cascade & 1) * settings.Resolution;
            const uint32_t y = spot ? spot->Y : point ? 0 : (cascade >> 1) * settings.Resolution;
            commands->SetViewport({float(x), float(y), float(settings.Resolution), float(settings.Resolution)});
            commands->SetScissor({int32_t(x), int32_t(y), settings.Resolution, settings.Resolution});
            if (staticOnly && !point)
            {
                // A scissored far-depth draw clears only the dirty tile, preserving its neighbours.
                auto& clearPipeline = shadows->DepthPipelines["Static cache tile clear"];
                if (!clearPipeline)
                {
                    PipelineDesc desc;
                    desc.Program = frame.Services.GetInternalProgram(
                        "StaticShadowClear",
                        "#version 450\nvoid main(){ vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); "
                        "gl_Position=vec4(p*2-1,1,1); }",
                        "#version 450\nvoid main(){ gl_FragDepth=1.0; }");
                    desc.ColorFormats.clear();
                    desc.HasDepthStencil        = true;
                    desc.Rasterizer.Cull        = CullMode::None;
                    desc.DepthStencil.DepthTest = desc.DepthStencil.DepthWrite = true;
                    desc.DepthStencil.DepthCompare                             = CompareOp::Always;
                    clearPipeline                                              = device->CreatePipeline(desc);
                }
                commands->BindPipeline(clearPipeline);
                commands->Draw(3);
                ++frame.Stats.DrawCalls;
            }
            auto data               = MakeLightingUniforms(view.Scene);
            data.ViewProjection     = matrix;
            data.Time               = view.Time;
            data.CameraPosition     = glm::vec4(view.Scene.sceneCamera.Position, 1);
            data.ShadowPass         = {point ? 2.0f : 1.0f, settings.ConstantBias, settings.SlopeBias, 0};
            data.ShadowPassPosition = position;
            const auto frameUpload  = frame.AllocateUniforms(*commands, sizeof(data));
            auto       buffer       = frameUpload.Buffer;
            buffer->Write(frameUpload.Offset, &data, sizeof(data));
            std::vector<GraphUse> uses;
            auto                  resource = graph.ImportTexture(name, texture);
            uses.push_back({resource, GraphAccess::DepthReadWrite});
            std::unordered_set<const RenderHardware::Texture*> sampledImages;
            for (size_t casterIndex : indices)
            {
                const auto& proxy = proxies[casterIndex];
                if (!proxy.CastShadows || !frame.Assets())
                    continue;
                if (!frustum.Intersects(proxy.WorldBounds))
                {
                    ++view.ShadowStats.CastersCulled;
                    ++stats.CastersCulled;
                    continue;
                }
                auto& entry = prepared[casterIndex];
                if (!entry.Resolved)
                {
                    entry.Resolved   = true;
                    const auto* mesh = frame.GetMesh(proxy.Mesh);
                    if (!frame.IncludesSection(proxy.EntityID, proxy.InstanceRoot, mesh, proxy.Section))
                        continue;
                    const auto* bindings = frame.GetBindings(proxy.Material, false);
                    const auto* shader   = bindings ? frame.GetShader(proxy.Material, bindings->Shader) : nullptr;
                    if (!mesh || proxy.Section >= mesh->Sections.size() || !bindings || !shader ||
                        shader->Domain == ShaderDomain::PostProcess)
                        continue;
                    const GpuShaderPass* caster    = nullptr;
                    bool                 hasCaster = false;
                    for (const auto& pass : shader->Passes)
                        if (pass.Tag == "ShadowCaster")
                        {
                            hasCaster = true;
                            if (bindings->PassEnabled(pass.Name))
                            {
                                caster = &pass;
                                break;
                            }
                        }
                    if (hasCaster && !caster)
                        continue;
                    if (!caster)
                    {
                        // Built-in Standard depth path retains texture alpha, UV transforms and vertex colour.
                        bindings = frame.GetBindings(proxy.Material, true);
                        shader   = bindings ? frame.GetShader(proxy.Material, bindings->Shader) : nullptr;
                        if (!bindings || !shader)
                            continue;
                    }
                    EngineObjectUniforms object;
                    object.Transform        = proxy.Transform;
                    object.NormalMatrix     = glm::transpose(glm::inverse(proxy.Transform));
                    object.MaterialRender   = {float(bindings->Render.AlphaMode),
                                               bindings->Render.AlphaCutoff,
                                               bindings->Render.DoubleSided ? 1.0f : 0.0f,
                                               0};
                    const auto objectUpload = frame.AllocateUniforms(*commands, sizeof(object));
                    objectUpload.Buffer->Write(objectUpload.Offset, &object, sizeof(object));
                    entry.Mesh     = mesh;
                    entry.Bindings = bindings;
                    entry.Shader   = shader;
                    entry.Caster   = caster;
                    entry.Object   = objectUpload;
                    entry.Palette  = mesh->Skin ? frame.Assets()->GetSkinningPalette(proxy.SkinPose, mesh) :
                                                  Handle<RenderHardware::Buffer>();
                }
                const auto* mesh     = entry.Mesh;
                const auto* bindings = entry.Bindings;
                const auto* shader   = entry.Shader;
                const auto* caster   = entry.Caster;
                if (!mesh)
                    continue;
                MaterialPassData pass;
                pass.UniformStorage     = frame.UniformStorage;
                pass.MaterialMipBias    = frame.MaterialMipBias;
                pass.Palette            = entry.Palette;
                pass.Frame              = buffer;
                pass.FrameOffset        = frameUpload.Offset;
                pass.Object             = entry.Object.Buffer;
                pass.ObjectOffset       = entry.Object.Offset;
                pass.Environment        = environment;
                pass.EnvironmentSampler = frame.Services.GetMaterialEnvironmentSampler();
                pass.Shadows            = fallback;
                pass.ShadowDepth        = true;
                pass.DepthFront         = point ? FrontFace::Clockwise : FrontFace::CounterClockwise;
                if (!caster)
                {
                    auto& program = pass.Palette ? skinnedDepthProgram : depthProgram;
                    if (!program)
                        program = pass.Palette ?
                                      GetSkinnedInternalProgram(*device, "ShadowDepth", DepthVertex, DepthFragment) :
                                      frame.Services.GetInternalProgram("ShadowDepth", DepthVertex, DepthFragment);
                    pass.DepthProgram = program;
                }
                pass.ColorFormats.clear();
                pass.Section      = proxy.Section;
                pass.Render       = bindings->Render;
                pass.Parameters   = proxy.Parameters;
                pass.ConstantBias = settings.ConstantBias;
                pass.SlopeBias    = settings.SlopeBias;
                bool recorded     = false;
                frame.Complete &=
                    RecordMaterialDraw(*frame.Assets(), *commands, *shader, *bindings, *mesh, pass, caster, &recorded);
                ++view.ShadowStats.DrawCalls;
                stats.CastersDrawn += recorded;
                ++frame.Stats.DrawCalls;
                frame.Stats.Triangles += mesh->Sections[proxy.Section].IndexCount / 3;
                for (const auto& image : bindings->Images)
                {
                    if (sampledImages.insert(image.get()).second)
                    {
                        auto id = graph.ImportTexture("ShadowCasterMaterial", image);
                        uses.push_back({id, GraphAccess::SampledRead});
                    }
                }
            }
            if (!staticOnly)
            {
                for (const auto& image : frame.Services.GetTextRenderer().Textures())
                {
                    const auto id = graph.ImportTexture("Shadow font atlas", image);
                    uses.push_back({id, GraphAccess::SampledRead});
                }
                for (auto text : view.Texts) // Collected once per frame by RenderWorld.
                {
                    if (!text.Style.CastShadows || !frustum.Intersects(text.WorldBounds) ||
                        !view.Highlights.Isolate.empty())
                        continue;
                    text.Transform = TextWorldTransform(
                        text.Style, text.Transform, view.Scene.sceneCamera.Position, view.Scene.sceneCamera.viewMatrix);
                    frame.Services.GetTextRenderer().Draw(*commands,
                                                          text,
                                                          matrix,
                                                          rendering,
                                                          true,
                                                          point ? position : glm::vec4(0),
                                                          settings.ConstantBias,
                                                          settings.SlopeBias,
                                                          point,
                                                          frame.UniformStorage);
                    ++view.ShadowStats.DrawCalls;
                    ++stats.CastersDrawn;
                    ++frame.Stats.DrawCalls;
                    frame.Stats.Triangles += text.Layout->Get().Glyphs.size() * 2;
                }
                const auto& particleDepth = GetInternalShaderData("ShadowDepth", DepthVertex, DepthFragment);
                if (auto source = std::get_if<ShaderData>(&particleDepth))
                {
                    const auto before = view.ShadowStats.DrawCalls;
                    ParticleRenderFeature::RecordMeshShadows(
                        graph, frame, *commands, data, *source, uses, point, settings.ConstantBias, settings.SlopeBias);
                    stats.CastersDrawn += view.ShadowStats.DrawCalls - before;
                }
            }
            commands->EndRendering();
            auto pass = graph.AddRecordedPass(
                std::move(name), std::move(uses), commands, RenderClockMilliseconds() - recordStart);
            if (pass)
                graph.OrderAfter(pass, pass - 1);
            return pass;
        };
        auto record = [&](std::string                            name,
                          const Handle<RenderHardware::Texture>& texture,
                          uint32_t                               face,
                          uint32_t                               cascade,
                          const glm::mat4&                       matrix,
                          ShadowSettings                         settings,
                          glm::vec4                              position,
                          bool                                   point,
                          const SpotShadowTile*                  spot  = nullptr,
                          float                                  split = 0) {
            ShadowPassStatistics stats {name, point, cascade, face};
            if (staticCasters.empty())
            {
                recordGeometry(name,
                               texture,
                               face,
                               cascade,
                               matrix,
                               settings,
                               position,
                               point,
                               spot,
                               allCasters,
                               false,
                               false,
                               stats);
                stats.MovableCastersRendered = stats.CastersDrawn;
            }
            else
            {
                usedCaches.insert(name);
                const uint32_t x = spot ? spot->X : point ? 0 : (cascade & 1) * settings.Resolution;
                const uint32_t y = spot ? spot->Y : point ? 0 : (cascade >> 1) * settings.Resolution;
                usedImages.insert(texture.get());
                auto& image = shadows->StaticImages[texture.get()];
                if (!image.Depth)
                {
                    image.Live      = texture;
                    image.Depth     = DepthTexture(*device, texture->GetDesc().Width, point);
                    auto initialize = device->CreateCommandList();
                    for (uint32_t layer = 0; layer < (point ? 6u : 1u); ++layer)
                    {
                        RenderingDesc clear;
                        clear.Depth.Target           = image.Depth;
                        clear.Depth.Subresource.Face = layer;
                        initialize->BeginRendering(clear);
                        initialize->EndRendering();
                    }
                    auto pass = graph.AddRecordedPass(
                        name + " cache initialize",
                        {{graph.ImportTexture(name + " static atlas", image.Depth), GraphAccess::DepthWrite}},
                        initialize);
                    if (pass)
                        graph.OrderAfter(pass, pass - 1);
                }
                auto&           cache = shadows->StaticCaches[name];
                StaticShadowKey key {
                    frame.World.GetIdentity(), shadows->StaticRevision, matrix, position, settings, split};
                if (cache.Depth != image.Depth || cache.Face != face || cache.X != x || cache.Y != y)
                    cache.Valid = false;
                cache.Depth = image.Depth;
                cache.Face  = face;
                cache.X     = x;
                cache.Y     = y;
                if (!cache.Matches(key))
                {
                    stats.CacheUpdated = true;
                    ++view.ShadowStats.CacheMisses;
                    recordGeometry(name + " static cache",
                                   cache.Depth,
                                   face,
                                   cascade,
                                   matrix,
                                   settings,
                                   position,
                                   point,
                                   spot,
                                   staticCasters,
                                   true,
                                   false,
                                   stats);
                    stats.StaticCastersRendered = stats.CastersDrawn;
                    cache.Casters               = stats.CastersDrawn;
                    cache.Key                   = key;
                    cache.Valid                 = true;
                    graph.ExportTexture(graph.ImportTexture(name + " static depth", cache.Depth));
                }
                else
                {
                    stats.CacheHit      = true;
                    stats.CachedCasters = cache.Casters;
                    ++view.ShadowStats.CacheHits;
                }
                const auto start    = RenderClockMilliseconds();
                auto       commands = device->CreateCommandList();
                // Full cube faces and four-cascade atlases are completely overwritten by their copies.
                // Clear unused atlas pixels when only a subset of its tiles is occupied.
                if (!point && cascade == 0 && (spot || directional.Cascades < 4))
                {
                    RenderingDesc clear;
                    clear.Depth.Target           = texture;
                    clear.Depth.Subresource.Face = face;
                    commands->BeginRendering(clear);
                    commands->EndRendering();
                }
                commands->CopyTextureRegion(
                    cache.Depth, {0, face}, texture, {0, face}, {x, y, x, y, settings.Resolution, settings.Resolution});
                auto pass = graph.AddRecordedPass(
                    name + " cache copy",
                    {{graph.ImportTexture(name + " static depth", cache.Depth), GraphAccess::TransferRead},
                     {graph.ImportTexture(name, texture), GraphAccess::TransferWrite}},
                    commands,
                    RenderClockMilliseconds() - start,
                    GraphPassKind::Transfer);
                if (pass)
                    graph.OrderAfter(pass, pass - 1);
                // Partial copies preserve earlier tiles, including their movable overlays.
                if (const auto previous = previousLivePass.find(texture.get()); previous != previousLivePass.end())
                    graph.DependsOn(pass, previous->second);
                GraphPass  livePass = pass;
                const auto before   = stats.CastersDrawn;
                if (!movableCasters.empty() || movableExtras)
                    livePass = recordGeometry(name,
                                              texture,
                                              face,
                                              cascade,
                                              matrix,
                                              settings,
                                              position,
                                              point,
                                              spot,
                                              movableCasters,
                                              false,
                                              true,
                                              stats);
                previousLivePass[texture.get()] = livePass;
                stats.MovableCastersRendered    = stats.CastersDrawn - before;
            }
            view.ShadowStats.CachedCasters += stats.CachedCasters;
            view.ShadowStats.StaticCastersRendered += stats.StaticCastersRendered;
            view.ShadowStats.MovableCastersRendered += stats.MovableCastersRendered;
            view.ShadowStats.PerPass.push_back(stats);
            ++view.ShadowStats.Passes;
        };
        if (dirEnabled)
        {
            const auto cascades =
                BuildShadowCascades(view.Scene.sceneCamera, view.Scene.dirLight.Dirction, directional);
            if (cascades[0].Split > 0)
            {
                if (shadows->Directional->GetDesc().Width != directional.Resolution * 2)
                    shadows->Directional = DepthTexture(*device, directional.Resolution * 2, false);
                uniforms.ShadowInfo = {float(directional.Cascades),
                                       float(directional.Filter),
                                       directional.NormalOffset,
                                       std::tan(glm::radians(directional.AngularRadius))};
                ++view.ShadowStats.ShadowedLights;
                for (uint32_t i = 0; i < directional.Cascades; ++i)
                {
                    const auto& c                     = cascades[i];
                    uniforms.CascadeViewProjection[i] = c.ViewProjection;
                    uniforms.CascadeSplits[i]         = c.Split;
                    uniforms.CascadeBlendStarts[i]    = c.BlendStart;
                    uniforms.CascadeTexel[i] = {c.TexelSize, c.DepthRange, float(directional.Resolution), c.Radius};
                    record("Directional shadow cascade " + std::to_string(i),
                           shadows->Directional,
                           0,
                           i,
                           c.ViewProjection,
                           directional,
                           {},
                           false,
                           nullptr,
                           c.Split);
                }
            }
        }
        const glm::vec3 directions[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec3 ups[]        = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
        for (uint32_t slot = 0; slot < selected.size(); ++slot)
        {
            ++view.ShadowStats.ShadowedLights;
            auto        index    = selected[slot];
            const auto& light    = view.Scene.pointLights[index];
            auto        settings = light.Shadows.Validated();
            if (shadows->Points[slot]->GetDesc().Width != settings.Resolution)
                shadows->Points[slot] = DepthTexture(*device, settings.Resolution, true);
            uniforms.PointShadowIndices[index / 4][index % 4] = float(slot);
            uniforms.PointShadowPositionRange[slot]           = {light.Position, settings.MaxDistance};
            uniforms.PointShadowSourceRadius[slot]            = settings.SourceRadius;
            uniforms.PointShadowParams[slot]                  = {1.0f / settings.Resolution,
                                                                 settings.NormalOffset,
                                                                 settings.ConstantBias / (settings.Resolution * 16.0f),
                                                                 float(settings.Filter)};
            const auto projection =
                DeviceTextureProjection(glm::perspective(glm::radians(90.0f), 1.0f, .01f, settings.MaxDistance),
                                        device->GetCoordinateConventions());
            for (uint32_t face = 0; face < 6; ++face)
                record("Point shadow " + std::to_string(slot) + " face " + std::to_string(face),
                       shadows->Points[slot],
                       face,
                       0,
                       projection * glm::lookAt(light.Position, light.Position + directions[face], ups[face]),
                       settings,
                       {light.Position, settings.MaxDistance},
                       true);
        }
        if (!spots.empty())
        {
            uint32_t   atlasSize = 1;
            const auto requested = std::clamp(view.SpotShadowAtlasSize, 64u, 4096u);
            while (atlasSize <= requested / 2)
                atlasSize *= 2;
            if (shadows->Spots->GetDesc().Width != atlasSize)
                shadows->Spots = DepthTexture(*device, atlasSize, false);
            for (uint32_t slot = 0; slot < spots.size(); ++slot)
            {
                const auto  tile     = spots[slot];
                const auto& light    = view.Scene.spotLights[tile.Light];
                auto        settings = light.Shadows.Validated();
                settings.Resolution  = tile.Resolution;
                uniforms.SpotShadowRect[tile.Light] =
                    glm::vec4(tile.X, tile.Y, tile.Resolution, tile.Resolution) / float(atlasSize);
                uniforms.SpotShadowParams[tile.Light] = {
                    settings.NormalOffset, 1.f / tile.Resolution, float(settings.Filter), settings.SourceRadius};
                ++view.ShadowStats.ShadowedLights;
                record("Spot shadow " + std::to_string(tile.Light),
                       shadows->Spots,
                       0,
                       slot,
                       uniforms.SpotViewProjection[tile.Light],
                       settings,
                       {},
                       false,
                       &tile);
            }
        }
        for (auto it = shadows->StaticCaches.begin(); it != shadows->StaticCaches.end();)
            it = usedCaches.count(it->first) ? std::next(it) : shadows->StaticCaches.erase(it);
        for (auto it = shadows->StaticImages.begin(); it != shadows->StaticImages.end();)
            it = usedImages.count(it->first) ? std::next(it) : shadows->StaticImages.erase(it);
        view.ShadowStats.CacheBytes = shadows->StaticCacheBytes();
        view.LightingUniforms = uniforms;
    }
} // namespace Kans
