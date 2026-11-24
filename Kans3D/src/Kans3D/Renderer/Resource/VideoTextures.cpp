#include "kspch.h"
#include <chrono>
#include <cstring>
#include <shaderc/shaderc.hpp>
#include "Kans3D/Video/VideoDecoder.h"
#include "RenderAssetCache.h"

namespace Kans
{
    using namespace RenderHardware;

    namespace
    {
        constexpr auto VertexSource   = R"glsl(
#version 450
layout(location = 0) out vec2 uv;
void main()
{
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    uv = vec2(corner.x, 1.0 - corner.y);
}
)glsl";
        constexpr auto FragmentSource = R"glsl(
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D videoY;
layout(set = 0, binding = 1) uniform sampler2D videoUV;
layout(push_constant) uniform VideoColor {
    vec4 range;
    vec4 matrix;
    vec4 red;
    vec4 green;
    vec4 blue;
    int transfer;
} p;

void main()
{
    float y = (texture(videoY, uv).r - p.range.x) * p.range.y;
    vec2 c = (texture(videoUV, uv).rg - p.range.zz) * p.range.w;
    vec3 rgb = clamp(vec3(y + p.matrix.x * c.y,
                          y + dot(p.matrix.yz, c),
                          y + p.matrix.w * c.x), 0.0, 1.0);
    vec3 linear;
    if (p.transfer == 1)
        linear = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)), greaterThan(rgb, vec3(0.04045)));
    else if (p.transfer == 2 || p.transfer == 3)
        linear = pow(rgb, vec3(p.transfer == 2 ? 2.2 : 2.8));
    else
        linear = mix(rgb / 4.5, pow((rgb + 0.099) / 1.099, vec3(1.0 / 0.45)), greaterThanEqual(rgb, vec3(0.081)));
    color = vec4(dot(p.red.rgb, linear), dot(p.green.rgb, linear), dot(p.blue.rgb, linear), 1.0);
}
)glsl";

        struct Constants
        {
            VideoColorConstants                 Color;
            std::array<std::array<float, 4>, 3> Gamut;
            int32_t                             Transfer;
        };

        void DrawOutput(Device&                    device,
                        const Handle<CommandList>& commands,
                        const RenderingDesc&       desc,
                        const Handle<Pipeline>&    pipeline  = {},
                        const Handle<BindGroup>&   group     = {},
                        const Constants*           constants = nullptr)
        {
            const auto& target = desc.Colors.front().Target->GetDesc();
            commands->BeginRendering(desc);
            if (pipeline)
            {
                commands->SetViewport({0, 0, float(target.Width), float(target.Height)});
                commands->SetScissor({0, 0, target.Width, target.Height});
                commands->BindPipeline(pipeline);
                commands->BindGroup(group);
                commands->PushConstants(0, constants, sizeof(*constants));
                commands->Draw(3);
            }
            commands->EndRendering();
            if (target.MipLevels > 1)
                commands->GenerateMips(desc.Colors.front().Target);
            device.Submit(commands);
        }
    } // namespace

    void RenderAssetCache::CreateVideoPipeline()
    {
        ShaderData shader;
        shader.DebugName = "Video YUV conversion";
        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        auto add = [&](ShaderProductStage stage, shaderc_shader_kind kind, const char* text) {
            auto code = compiler.CompileGlslToSpv(text, std::strlen(text), kind, "VideoTexture", options);
            if (code.GetCompilationStatus() != shaderc_compilation_status_success)
                throw std::runtime_error(code.GetErrorMessage());
            shader.Code.Modules.push_back({stage, "main", {code.cbegin(), code.cend()}});
        };
        add(ShaderProductStage::Vertex, shaderc_vertex_shader, VertexSource);
        add(ShaderProductStage::Fragment, shaderc_fragment_shader, FragmentSource);
        auto device   = GetDevice();
        m_VideoLayout = device->CreateBindGroupLayout(
            {0, {{0, BindingType::CombinedTextureSampler}, {1, BindingType::CombinedTextureSampler}}});
        SamplerDesc sampler;
        sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
        m_VideoSampler                    = device->CreateSampler(sampler);
        PipelineDesc desc;
        desc.Program      = device->CreateShader(shader);
        desc.GroupLayouts = {m_VideoLayout};
        desc.ColorFormats = {Format::RGBA8Srgb};
        desc.Blends.resize(1);
        desc.DepthStencil.DepthTest = desc.DepthStencil.DepthWrite = false;
        desc.Rasterizer.Cull                                       = CullMode::None;
        desc.PushConstantBytes                                     = sizeof(Constants);
        desc.DebugName                                             = shader.DebugName;
        m_VideoPipeline                                            = device->CreatePipeline(desc);
    }

    void RenderAssetCache::BeginVideoTextures() { ++m_VideoPass; }

    void RenderAssetCache::ClearVideoTexture(VideoTextureState& state)
    {
        if (state.Serial)
        {
            auto device = GetDevice();
            DrawOutput(*device, device->CreateCommandList(), state.Output);
            state.Serial = 0;
        }
    }

    void RenderAssetCache::EndVideoTextures()
    {
        for (auto it = m_VideoTextures.begin(); it != m_VideoTextures.end();)
        {
            if (it->second.Pass == m_VideoPass)
                ++it;
            else
            {
                ClearVideoTexture(it->second);
                it = m_VideoTextures.erase(it);
            }
        }
    }

    void RenderAssetCache::PresentVideoTexture(AssetID              id,
                                               uint64_t             owner,
                                               uint64_t             stream,
                                               bool                 clear,
                                               const VideoClipInfo* info,
                                               const VideoFrame*    frame,
                                               double&              uploadMilliseconds)
    {
        const auto texture = GetTexture(id);
        if (!texture)
            return;
        const auto& spec = m_Textures.at(id).Source->GetData().RenderTexture;
        if (!spec || spec->Update != RenderTextureUpdate::Video)
            return;

        auto& state = m_VideoTextures[id];
        if (state.Pass == m_VideoPass && state.Owner != owner)
            return; // A target has one producer per scene render.
        state.Pass = m_VideoPass;
        if (state.Output.Colors.empty())
            state.Output.Colors.push_back(
                {texture->GetResource(), {}, LoadOp::Clear, StoreOp::Store, spec->ClearColor});
        if (state.Owner != owner || clear)
        {
            ClearVideoTexture(state);
            state.Owner = owner;
        }
        if (clear || !info || !frame || (state.Stream == stream && state.Serial == frame->Serial))
            return;

        const size_t yBytes = size_t(info->Width) * info->Height;
        if (frame->Planes.size() != yBytes + yBytes / 2)
            return;
        auto device = GetDevice();
        if (!m_VideoPipeline)
            CreateVideoPipeline();

        if (!state.Y || state.Y->GetDesc().Width != info->Width || state.Y->GetDesc().Height != info->Height)
        {
            TextureDesc plane;
            plane.Width       = info->Width;
            plane.Height      = info->Height;
            plane.PixelFormat = Format::R8Unorm;
            plane.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
            state.Y           = device->CreateTexture(plane);
            plane.Width /= 2;
            plane.Height /= 2;
            plane.PixelFormat = Format::RG8Unorm;
            state.UV          = device->CreateTexture(plane);
            state.Planes      = device->CreateBindGroup(
                {m_VideoLayout, {{0, {}, 0, 0, state.Y, m_VideoSampler}, {1, {}, 0, 0, state.UV, m_VideoSampler}}});
            state.Uploads.clear();
            state.Uploads.resize(size_t(device->GetDesc().FramesInFlight) + 1);
            for (auto& slot : state.Uploads)
                slot.Buffer =
                    device->CreateBuffer({frame->Planes.size(), BufferUsage::CopySource, MemoryUsage::Upload});
        }

        // The RHI retains submitted command lists until their fence retires. A busy ring holds the old frame.
        auto slot = std::find_if(
            state.Uploads.begin(), state.Uploads.end(), [](const auto& s) { return s.InFlight.expired(); });
        if (slot == state.Uploads.end())
            return;
        const auto start = std::chrono::steady_clock::now();
        slot->Buffer->Write(0, frame->Planes.data(), frame->Planes.size());
        auto commands  = device->CreateCommandList();
        slot->InFlight = commands;
        commands->CopyBufferToTexture(slot->Buffer, 0, state.Y);
        commands->CopyBufferToTexture(slot->Buffer, yBytes, state.UV);
        const Constants constants {GetVideoColorConstants(frame->Matrix, frame->Range),
                                   GetVideoGamutConstants(frame->Primaries),
                                   int32_t(frame->Transfer)};
        DrawOutput(*device, commands, state.Output, m_VideoPipeline, state.Planes, &constants);
        state.Stream = stream;
        state.Serial = frame->Serial;
        uploadMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
} // namespace Kans
