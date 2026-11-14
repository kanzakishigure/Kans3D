#include "kspch.h"
#include "Kans3D/Renderer/Lighting/ShadowMaps.h"
#include <glm/gtc/matrix_transform.hpp>
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "Kans3D/Renderer/2D/TextRenderer.h"
#include "Kans3D/Renderer/Features/ParticleRenderFeature.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Kans3D/Renderer/Shading/MaterialPass.h"

namespace Kans
{
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
        result->Directional       = DepthTexture(device, 1, false);
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
        auto buffer = device->CreateBuffer({sizeof(uniforms), BufferUsage::Uniform, MemoryUsage::Upload});
        buffer->Write(0, &uniforms, sizeof(uniforms));
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
            auto ob               = device->CreateBuffer({sizeof(object), BufferUsage::Uniform, MemoryUsage::Upload});
            ob->Write(0, &object, sizeof(object));
            MaterialPassData data;
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
    void BuildShadowPasses(RenderGraph& graph, RenderFrame& frame)
    {
        auto& view = frame.View;
        if (view.Scene.pointLights.empty() && view.Scene.pointLight.Shadows.Enabled)
            view.Scene.pointLights.push_back(view.Scene.pointLight);
        const auto directional = view.Scene.dirLight.Shadows.Validated();
        const auto selected    = SelectPointShadows(view.Scene, view.MaxShadowedPointLights);
        const bool dirEnabled  = directional.Enabled && glm::length(view.Scene.dirLight.Dirction) > 1e-6f;
        view.Shadows.reset();
        view.ShadowStats = {};
        if (view.LightingUniforms)
        {
            view.LightingUniforms->ShadowInfo = glm::vec4(0);
            view.LightingUniforms->ShadowPass = glm::vec4(0);
            for (auto& index : view.LightingUniforms->PointShadowIndices)
                index = glm::vec4(-1);
        }
        if (!dirEnabled && selected.empty())
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
        auto shadows               = storage;
        view.Shadows               = shadows;
        auto       uniforms        = view.LightingUniforms.value_or(MakeLightingUniforms(view.Scene));
        const auto row             = glm::transpose(view.Scene.sceneCamera.viewMatrix);
        uniforms.ShadowCameraDepth = -row[2];
        uniforms.ShadowInfo        = glm::vec4(0);
        for (auto& indices : uniforms.PointShadowIndices)
            indices = glm::vec4(-1);
        const auto proxies     = frame.World.CollectDraws(0, view.HiddenHierarchy);
        const auto environment = frame.GetMaterialEnvironment();
        auto       record      = [&](std::string                            name,
                                     const Handle<RenderHardware::Texture>& texture,
                                     uint32_t                               face,
                                     uint32_t                               cascade,
                                     const glm::mat4&                       matrix,
                                     ShadowSettings                         settings,
                                     glm::vec4                              position,
                                     bool                                   point) {
            const auto           recordStart = RenderClockMilliseconds();
            ShadowPassStatistics stats {name, point, cascade, face};
            const Frustum frustum(matrix);
            auto          commands = device->CreateCommandList();
            RenderingDesc rendering;
            rendering.Depth.Target           = texture;
            rendering.Depth.Subresource.Face = face;
            rendering.Depth.Load             = point || cascade == 0 ? LoadOp::Clear : LoadOp::Load;
            commands->BeginRendering(rendering);
            uint32_t x = point ? 0 : (cascade & 1) * settings.Resolution;
            uint32_t y = point ? 0 : (cascade >> 1) * settings.Resolution;
            commands->SetViewport({float(x), float(y), float(settings.Resolution), float(settings.Resolution)});
            commands->SetScissor({int32_t(x), int32_t(y), settings.Resolution, settings.Resolution});
            auto data               = MakeLightingUniforms(view.Scene);
            data.ViewProjection     = matrix;
            data.Time               = view.Time;
            data.CameraPosition     = glm::vec4(view.Scene.sceneCamera.Position, 1);
            data.ShadowPass         = {point ? 2.0f : 1.0f, settings.ConstantBias, settings.SlopeBias, 0};
            data.ShadowPassPosition = position;
            auto buffer             = device->CreateBuffer({sizeof(data), BufferUsage::Uniform, MemoryUsage::Upload});
            buffer->Write(0, &data, sizeof(data));
            std::vector<GraphUse> uses;
            auto                  resource = graph.ImportTexture(name, texture);
            uses.push_back({resource, GraphAccess::DepthReadWrite});
            for (const auto& proxy : proxies)
            {
                if (!proxy.CastShadows || !frame.Assets())
                    continue;
                if (!frustum.Intersects(proxy.WorldBounds))
                {
                    ++view.ShadowStats.CastersCulled;
                    ++stats.CastersCulled;
                    continue;
                }
                const auto* mesh     = frame.GetMesh(proxy.Mesh);
                if (!frame.IncludesSection(proxy.EntityID, proxy.InstanceRoot, mesh, proxy.Section))
                    continue;
                const auto* bindings = frame.GetBindings(proxy.Material, false);
                const auto* shader   = bindings ? frame.GetShader(proxy.Material, bindings->Shader) : nullptr;
                if (!mesh || proxy.Section >= mesh->Sections.size() || !bindings || !shader ||
                    shader->Domain == ShaderDomain::PostProcess)
                    continue;
                const GpuShaderPass* caster = nullptr;
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
                object.Transform      = proxy.Transform;
                object.NormalMatrix   = glm::transpose(glm::inverse(proxy.Transform));
                object.MaterialRender = {float(bindings->Render.AlphaMode),
                                         bindings->Render.AlphaCutoff,
                                         bindings->Render.DoubleSided ? 1.0f : 0.0f,
                                         0};
                auto ob = device->CreateBuffer({sizeof(object), BufferUsage::Uniform, MemoryUsage::Upload});
                ob->Write(0, &object, sizeof(object));
                MaterialPassData pass;
                pass.MaterialMipBias    = frame.MaterialMipBias;
                pass.Palette            = mesh->Skin ? frame.Assets()->GetSkinningPalette(proxy.SkinPose, mesh) :
                                                       Handle<RenderHardware::Buffer>();
                pass.Frame              = buffer;
                pass.Object             = ob;
                pass.Environment        = environment;
                pass.EnvironmentSampler = frame.Services.GetMaterialEnvironmentSampler();
                pass.Shadows            = fallback;
                pass.ShadowDepth        = true;
                pass.DepthFront         = point ? FrontFace::Clockwise : FrontFace::CounterClockwise;
                if (!caster)
                    pass.DepthProgram =
                        pass.Palette ? GetSkinnedInternalProgram(*device, "ShadowDepth", DepthVertex, DepthFragment) :
                                       frame.Services.GetInternalProgram("ShadowDepth", DepthVertex, DepthFragment);
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
                    auto id = graph.ImportTexture("ShadowCasterMaterial", image);
                    if (std::none_of(uses.begin(), uses.end(), [&](auto use) { return use.Texture == id; }))
                        uses.push_back({id, GraphAccess::SampledRead});
                }
            }
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
                                                      point);
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
            commands->EndRendering();
            auto pass = graph.AddRecordedPass(
                std::move(name), std::move(uses), commands, RenderClockMilliseconds() - recordStart);
            view.ShadowStats.PerPass.push_back(std::move(stats));
            if (pass)
                graph.OrderAfter(pass, pass - 1);
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
                           false);
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
        view.LightingUniforms = uniforms;
    }
} // namespace Kans
