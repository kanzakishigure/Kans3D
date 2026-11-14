#include "kspch.h"
#include "Kans3D/Renderer/2D/SpriteBatch.h"
#include <iostream>
#include "Kans3D/RHI/Backend.h"
#include "Kans3D/RHI/PipelineKey.h"
#include "Kans3D/Renderer/RenderStatistics.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"

namespace Kans
{
    namespace
    {
        using namespace RenderHardware;
        constexpr uint32_t MaxQuads = 10000;
        struct QuadVertex
        {
            glm::vec3 Position;
            glm::vec4 Color;
            glm::vec2 UV;
            float     Tiling;
        };
        const char* Vertex   = R"(#version 450
layout(location = 0) in vec3 Position;
layout(location = 1) in vec4 Color;
layout(location = 2) in vec2 UV;
layout(location = 3) in float Tiling;
layout(set = 0, binding = 0, std140) uniform Frame { mat4 ViewProjection; };
layout(location = 0) out vec4 V_Color;
layout(location = 1) out vec2 V_UV;
layout(location = 2) out float V_Tiling;
void main() {
    gl_Position = ViewProjection * vec4(Position,1);
    V_Color = Color; V_UV = UV; V_Tiling = Tiling;
})";
        const char* Fragment = R"(#version 450
layout(location = 0) in vec4 V_Color;
layout(location = 1) in vec2 V_UV;
layout(location = 2) in float V_Tiling;
layout(set = 0, binding = 1) uniform sampler2D Image;
layout(location = 0) out vec4 Color;
void main() { Color = V_Color * texture(Image,V_UV * V_Tiling); }
)";
        glm::mat4   Transform(glm::vec3 position, glm::vec2 size, float rotation = 0)
        {
            return glm::translate(glm::mat4(1), position) * glm::rotate(glm::mat4(1), rotation, glm::vec3(0, 0, 1)) *
                   glm::scale(glm::mat4(1), glm::vec3(size, 1));
        }
        constexpr glm::vec2 UV[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    } // namespace
    struct SpriteBatch::Impl
    {
        std::shared_ptr<Device>                           GPU;
        Handle<RenderHardware::Shader>                    Program;
        Handle<BindGroupLayout>                           Layout;
        std::unordered_map<std::string, Handle<Pipeline>> Pipelines;
        Handle<RenderHardware::Texture>                   White;
        Handle<RenderHardware::Buffer>                    Indices;
        std::vector<QuadVertex>                           Vertices;
        Ref<Texture2D>                                    Texture;
        glm::mat4                                         ViewProjection {1};
        SpriteBatch::Statistics                           Stats;
        double                                            LastRecordMilliseconds = 0;
        ImmediatePassState                                State;
        std::shared_ptr<Device>                           TargetDevice;
        RenderingDesc                                     Target;
        std::function<void(ViewSpriteBatch)>              BatchSubmission;
        SubmissionSink                                    Submission;
        CommandList*                                      Recording = nullptr;
        uint32_t                                          Width = 0, Height = 0;
    };
    SpriteBatch::SpriteBatch() : m_Data(std::make_unique<Impl>()) {}
    SpriteBatch::~SpriteBatch() = default;
    void SpriteBatch::Append(const glm::mat4&      transform,
                             const Ref<Texture2D>& texture,
                             const glm::vec4&      color,
                             const glm::vec2*      uv,
                             float                 tiling)
    {
        // Consecutive quads with the same texture share one draw. Flushing on a
        // texture change preserves blend order without descriptor arrays/indexing.
        if (m_Data->Vertices.size() == MaxQuads * 4 || m_Data->Texture != texture)
            SpriteBatch::Flush();
        m_Data->Texture                 = texture;
        constexpr glm::vec4 positions[] = {{-.5f, -.5f, 0, 1}, {.5f, -.5f, 0, 1}, {.5f, .5f, 0, 1}, {-.5f, .5f, 0, 1}};
        for (uint32_t i = 0; i < 4; ++i)
            m_Data->Vertices.push_back({glm::vec3(transform * positions[i]), color, uv[i], tiling});
        ++m_Data->Stats.QuadCount;
    }
    void SpriteBatch::DrawSprite(const glm::mat4&      t,
                                 const Ref<Texture2D>& image,
                                 const glm::vec4&      c,
                                 float                 tiling,
                                 glm::vec4             rect,
                                 bool                  flipX,
                                 bool                  flipY)
    {
        if (flipX)
            std::swap(rect.x, rect.z);
        if (flipY)
            std::swap(rect.y, rect.w);
        const glm::vec2 uv[] = {{rect.x, rect.y}, {rect.z, rect.y}, {rect.z, rect.w}, {rect.x, rect.w}};
        Append(t, image, c, uv, tiling);
    }
    void SpriteBatch::Init(std::shared_ptr<Device> device)
    {
        if (m_Data->GPU)
            return;
        m_Data->GPU = device ? std::move(device) : m_Data->TargetDevice;
        if (!m_Data->GPU)
        {
            m_Data->GPU = RenderHardware::GetSharedDevice();
        }
        auto result = CreateInternalShader(*m_Data->GPU, "Renderer2D", Vertex, Fragment);
        if (auto* error = std::get_if<AssetError>(&result))
        {
            if (Log::GetCoreLogger())
                CORE_ERROR("{}", error->Message);
            else
                std::cerr << error->Message << std::endl;
            return;
        }
        m_Data->Program = std::get<Handle<RenderHardware::Shader>>(std::move(result));
        m_Data->Layout  = m_Data->GPU->CreateBindGroupLayout(
            {0, {{0, BindingType::UniformBuffer}, {1, BindingType::CombinedTextureSampler}}});
        std::vector<uint32_t> indices;
        indices.reserve(MaxQuads * 6);
        for (uint32_t i = 0; i < MaxQuads; ++i)
            for (uint32_t n : {0u, 1u, 2u, 2u, 3u, 0u})
                indices.push_back(i * 4 + n);
        m_Data->Indices =
            m_Data->GPU->CreateBuffer({indices.size() * sizeof(uint32_t), BufferUsage::Index, MemoryUsage::Upload});
        m_Data->Indices->Write(0, indices.data(), indices.size() * sizeof(uint32_t));
        TextureDesc desc;
        desc.Usage      = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
        m_Data->White   = m_Data->GPU->CreateTexture(desc);
        auto     buffer = m_Data->GPU->CreateBuffer({4, BufferUsage::CopySource, MemoryUsage::Upload});
        uint32_t white  = 0xffffffff;
        buffer->Write(0, &white, 4);
        auto copy = m_Data->GPU->CreateCommandList();
        copy->CopyBufferToTexture(buffer, 0, m_Data->White);
        m_Data->GPU->Submit(copy);
        m_Data->GPU->WaitIdle();
        m_Data->Vertices.reserve(MaxQuads * 4);
    }
    void SpriteBatch::SetTarget(std::shared_ptr<Device> device, const RenderingDesc& target)
    {
        m_Data->TargetDevice = std::move(device);
        m_Data->Target       = target;
        for (auto& color : m_Data->Target.Colors)
            color.Load = LoadOp::Load;
        m_Data->Target.Depth.Load = LoadOp::Load;
        const auto& desc          = m_Data->Target.Colors.at(0).Target->GetDesc();
        m_Data->Width             = std::max(1u, desc.Width >> m_Data->Target.Colors[0].Subresource.Mip);
        m_Data->Height            = std::max(1u, desc.Height >> m_Data->Target.Colors[0].Subresource.Mip);
    }
    void SpriteBatch::SetSubmissionSink(SubmissionSink sink)
    {
        m_Data->BatchSubmission = {};
        m_Data->Submission      = std::move(sink);
    }
    void SpriteBatch::SetBatchSubmissionSink(std::function<void(ViewSpriteBatch)> sink)
    {
        m_Data->Submission      = {};
        m_Data->BatchSubmission = std::move(sink);
    }
    void SpriteBatch::SetCommandList(CommandList* commands) { m_Data->Recording = commands; }
    void SpriteBatch::ClearTarget()
    {
        m_Data->BatchSubmission = {};
        m_Data->Submission = {};
        m_Data->Recording  = nullptr;
        m_Data->Target     = {};
        m_Data->TargetDevice.reset();
    }
    void SpriteBatch::BeginScene(const OrthographicCamera& camera, ImmediatePassState state)
    {
        BeginScene(Camera(camera.GetViewProjectionMatrtix()), glm::mat4(1), state);
    }
    void SpriteBatch::BeginScene(const Camera& camera, const glm::mat4& view, ImmediatePassState state)
    {
        m_Data->Vertices.clear();
        m_Data->Texture.reset();
        m_Data->State = state;
        m_Data->ViewProjection =
            DeviceProjection(camera.GetProjectionMatrix() * view, m_Data->GPU->GetCoordinateConventions());
    }
    void SpriteBatch::EndScene() { Flush(); }
    void SpriteBatch::Shutdown()
    {
        ClearTarget();
        if (m_Data->GPU)
            m_Data->GPU->WaitIdle();
        *m_Data = {};
    }
    double SpriteBatch::GetLastRecordMilliseconds() const { return m_Data->LastRecordMilliseconds; }
    void SpriteBatch::Flush()
    {
        if (m_Data->Vertices.empty())
            return;
        if (!m_Data->Program || m_Data->Target.Colors.empty())
            throw std::logic_error("Renderer2D requires an RHI target and initialized shader");
        if (m_Data->TargetDevice != m_Data->GPU)
            throw std::logic_error("Renderer2D target belongs to another device");
        const auto  recordStart = RenderClockMilliseconds();
        auto&       device = *m_Data->GPU;
        auto        image  = m_Data->White;
        SamplerDesc sampling;
        if (m_Data->Texture)
        {
            image    = m_Data->Texture->GetResource();
            sampling = m_Data->Texture->GetSampling();
        }
        auto vertices = device.CreateBuffer(
            {m_Data->Vertices.size() * sizeof(QuadVertex), BufferUsage::Vertex, MemoryUsage::Upload});
        vertices->Write(0, m_Data->Vertices.data(), m_Data->Vertices.size() * sizeof(QuadVertex));
        auto frame = device.CreateBuffer({sizeof(glm::mat4), BufferUsage::Uniform, MemoryUsage::Upload});
        frame->Write(0, &m_Data->ViewProjection, sizeof(glm::mat4));
        PipelineDesc desc;
        desc.Program         = m_Data->Program;
        desc.Samples         = m_Data->Target.Colors[0].Target->GetDesc().Samples;
        desc.GroupLayouts    = {m_Data->Layout};
        desc.VertexBindings  = {{0, sizeof(QuadVertex), false}};
        desc.Attributes      = {{0, 0, offsetof(QuadVertex, Position), VertexFormat::Float3},
                                {1, 0, offsetof(QuadVertex, Color), VertexFormat::Float4},
                                {2, 0, offsetof(QuadVertex, UV), VertexFormat::Float2},
                                {3, 0, offsetof(QuadVertex, Tiling), VertexFormat::Float}};
        desc.Rasterizer.Cull = CullMode::None;
        desc.HasDepthStencil = bool(m_Data->Target.Depth.Target);
        if (desc.HasDepthStencil)
            desc.DepthStencilFormat = m_Data->Target.Depth.Target->GetDesc().PixelFormat;
        desc.DepthStencil.DepthTest    = desc.HasDepthStencil;
        desc.DepthStencil.DepthWrite   = desc.HasDepthStencil && m_Data->State.DepthWrite;
        desc.DepthStencil.DepthCompare = m_Data->State.DepthCompare;
        BlendState blend;
        blend.Enabled   = m_Data->State.Blend;
        blend.WriteMask = m_Data->State.WriteMask;
        // Preserve the original straight-alpha equation, including target alpha.
        blend.SourceAlpha      = BlendFactor::SourceAlpha;
        blend.DestinationAlpha = BlendFactor::OneMinusSourceAlpha;
        blend.SourceColor      = BlendFactor::SourceAlpha;
        blend.DestinationColor = BlendFactor::OneMinusSourceAlpha;
        for (const auto& color : m_Data->Target.Colors)
        {
            desc.ColorFormats.push_back(color.Target->GetDesc().PixelFormat);
            desc.Blends.push_back(blend);
        }
        auto  owned    = m_Data->Recording ? Handle<CommandList> {} : device.CreateCommandList();
        auto* commands = m_Data->Recording ? m_Data->Recording : owned.get();
        commands->BeginRendering(m_Data->Target);
        commands->SetViewport({0, 0, float(m_Data->Width), float(m_Data->Height)});
        auto& pipeline = m_Data->Pipelines[Detail::PipelineKey(desc)];
        if (!pipeline)
            pipeline = device.CreatePipeline(desc);
        commands->BindPipeline(pipeline);
        auto group = device.CreateBindGroup(
            {m_Data->Layout,
             {{0, frame, 0, sizeof(glm::mat4), {}, {}}, {1, {}, 0, 0, image, device.CreateSampler(sampling)}}});
        commands->BindGroup(group);
        commands->BindVertexBuffers({{0, vertices, 0}});
        commands->BindIndexBuffer(m_Data->Indices, IndexType::Uint32);
        commands->DrawIndexed(uint32_t(m_Data->Vertices.size() / 4) * 6);
        commands->EndRendering();
        m_Data->LastRecordMilliseconds = RenderClockMilliseconds() - recordStart;
        if (owned)
        {
            if (m_Data->BatchSubmission)
            {
                auto record = [gpu = m_Data->GPU,
                               pipeline,
                               group,
                               vertices,
                               indices = m_Data->Indices,
                               count   = uint32_t(m_Data->Vertices.size() / 4) * 6](const RenderingDesc& target,
                                                                                  glm::uvec2           size) {
                    auto list = gpu->CreateCommandList();
                    list->BeginRendering(target);
                    list->SetViewport({0, 0, float(size.x), float(size.y)});
                    list->SetScissor({0, 0, size.x, size.y});
                    list->BindPipeline(pipeline);
                    list->BindGroup(group);
                    list->BindVertexBuffers({{0, vertices, 0}});
                    list->BindIndexBuffer(indices, IndexType::Uint32);
                    list->DrawIndexed(count);
                    list->EndRendering();
                    return list;
                };
                m_Data->BatchSubmission({owned, image, frame, m_Data->ViewProjection, std::move(record)});
            }
            else if (m_Data->Submission)
                m_Data->Submission(owned, image);
            else
                device.Submit(owned);
        }
        ++m_Data->Stats.DrawCalls;
        m_Data->Vertices.clear();
        m_Data->Texture.reset();
    }
    void SpriteBatch::DrawQuad(const glm::vec2& p, const glm::vec2& s, const glm::vec4& c)
    {
        DrawQuad(glm::vec3(p, 0), s, c);
    }
    void SpriteBatch::DrawQuad(const glm::vec3& p, const glm::vec2& s, const glm::vec4& c)
    {
        DrawQuad(Transform(p, s), c);
    }
    void SpriteBatch::DrawQuad(const glm::mat4& t, const glm::vec4& c) { Append(t, {}, c, UV, 1); }
    void SpriteBatch::DrawQuad(const glm::vec2&      p,
                               const glm::vec2&      s,
                               const Ref<Texture2D>& image,
                               const glm::vec4&      c,
                               float                 tiling)
    {
        DrawQuad(glm::vec3(p, 0), s, image, c, tiling);
    }
    void SpriteBatch::DrawQuad(const glm::vec3&      p,
                               const glm::vec2&      s,
                               const Ref<Texture2D>& image,
                               const glm::vec4&      c,
                               float                 tiling)
    {
        DrawQuad(Transform(p, s), image, c, tiling);
    }
    void SpriteBatch::DrawQuad(const glm::mat4& t, const Ref<Texture2D>& image, const glm::vec4& c, float tiling)
    {
        Append(t, image, c, UV, tiling);
    }
    void SpriteBatch::DrawQuad(const glm::vec2&         p,
                               const glm::vec2&         s,
                               const Ref<SubTexture2D>& image,
                               const glm::vec4&         c,
                               float                    tiling)
    {
        DrawQuad(glm::vec3(p, 0), s, image, c, tiling);
    }
    void SpriteBatch::DrawQuad(const glm::vec3&         p,
                               const glm::vec2&         s,
                               const Ref<SubTexture2D>& image,
                               const glm::vec4&         c,
                               float                    tiling)
    {
        DrawQuad(Transform(p, s), image, c, tiling);
    }
    void SpriteBatch::DrawQuad(const glm::mat4& t, const Ref<SubTexture2D>& image, const glm::vec4& c, float tiling)
    {
        Append(t, image->GetTexture(), c, image->GetCroods(), tiling);
    }
    void SpriteBatch::DrawRotateQuad(const glm::vec2& p, float r, const glm::vec2& s, const glm::vec4& c)
    {
        DrawRotateQuad(glm::vec3(p, 1), r, s, c);
    }
    void SpriteBatch::DrawRotateQuad(const glm::vec3& p, float r, const glm::vec2& s, const glm::vec4& c)
    {
        DrawQuad(Transform(p, s, r), c);
    }
    void SpriteBatch::DrawRotateQuad(const glm::vec2&      p,
                                     float                 r,
                                     const glm::vec2&      s,
                                     const Ref<Texture2D>& image,
                                     const glm::vec4&      c,
                                     float                 tiling)
    {
        DrawRotateQuad(glm::vec3(p, 1), r, s, image, c, tiling);
    }
    void SpriteBatch::DrawRotateQuad(const glm::vec3&      p,
                                     float                 r,
                                     const glm::vec2&      s,
                                     const Ref<Texture2D>& image,
                                     const glm::vec4&      c,
                                     float                 tiling)
    {
        DrawQuad(Transform(p, s, r), image, c, tiling);
    }
    void                     SpriteBatch::ResetStats() { m_Data->Stats = {}; }
    SpriteBatch::Statistics& SpriteBatch::GetStats() { return m_Data->Stats; }
    void                     SpriteBatch::FlushAndReset() { Flush(); }
} // namespace Kans
