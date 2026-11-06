#include "kspch.h"
#include "SelectionTools.h"
#include <cstring>
#include <iostream>
#include "Kans3D/Renderer/Shading/InternalShader.h"

namespace Kans
{
    namespace
    {
        using namespace RenderHardware;
        // Embedded here so shaders and their uniform layouts can be reviewed together.
        const char* const PositionVertex    = R"(#version 450
layout(location = 0) in vec3 a_Position;
layout(set = 0, binding = 0, std140) uniform Draw { mat4 MVP; uvec4 ID; } p;
void main() { gl_Position = p.MVP * vec4(a_Position, 1.0); }
)";
        const char* const IdFragment        = R"(#version 450
layout(set = 0, binding = 0, std140) uniform Draw { mat4 MVP; uvec4 ID; } p;
layout(location = 0) out uint o_ID;
void main() { o_ID = p.ID.x; }
)";
        const char* const MaskFragment      = R"(#version 450
layout(location = 0) out float o_Mask;
void main() { o_Mask = 1.0; }
)";
        const char* const FullscreenVertex  = R"(#version 450
void main()
{
    const vec2 corners[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
)";
        const char* const CompositeFragment = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D u_Mask;
layout(set = 0, binding = 1, std140) uniform Composite { vec4 Color; ivec4 Width; } p;
layout(location = 0) out vec4 o_Color;
void main()
{
    vec2 texel = 1.0 / vec2(textureSize(u_Mask, 0));
    // Fragment coordinates and texture rows share the device framebuffer origin.
    vec2 uv = gl_FragCoord.xy * texel;
    if (texture(u_Mask, uv).r > 0.5)
    {
        if (p.Width.y != 0)
        {
            // Keep slot borders inside coverage; adjacent unhighlighted slots stay unchanged.
            for (int y = -p.Width.x; y <= p.Width.x; ++y)
                for (int x = -p.Width.x; x <= p.Width.x; ++x)
                    if (texture(u_Mask, uv + vec2(x, y) * texel).r < 0.5)
                    {
                        o_Color = p.Color;
                        return;
                    }
        }
        o_Color = vec4(p.Color.rgb, p.Color.a * (p.Width.y != 0 ? 0.28 : 0.12));
        return;
    }
    if (p.Width.y != 0) discard;
    for (int y = -p.Width.x; y <= p.Width.x; ++y)
        for (int x = -p.Width.x; x <= p.Width.x; ++x)
            if (texture(u_Mask, uv + vec2(x, y) * texel).r > 0.5)
            {
                o_Color = p.Color;
                return;
            }
    discard;
}
)";
        struct DrawData
        {
            glm::mat4  MVP;
            glm::uvec4 ID;
        };
        struct CompositeData
        {
            glm::vec4  Color;
            glm::ivec4 Width;
        };

        Handle<Shader> Build(Device& device, const char* name, const char* vertex, const char* fragment)
        {
            auto result = CreateInternalShader(device, name, vertex, fragment);
            if (auto* error = std::get_if<AssetError>(&result))
            {
                if (Log::GetCoreLogger())
                    CORE_ERROR("{}", error->Message);
                else
                    std::cerr << error->Message << std::endl;
                return {};
            }
            return std::get<Handle<Shader>>(std::move(result));
        }
        PipelineDesc MeshPipeline(Device& device, const char* name, const char* fragment, Format format)
        {
            PipelineDesc desc;
            desc.Program         = Build(device, name, PositionVertex, fragment);
            desc.GroupLayouts    = {device.CreateBindGroupLayout({0, {{0, BindingType::UniformBuffer}}})};
            desc.VertexBindings  = {{0, 64, false}};
            desc.Attributes      = {{0, 0, 0, VertexFormat::Float3}};
            desc.Rasterizer.Cull = CullMode::None;
            desc.ColorFormats    = {format};
            desc.Blends.resize(1);
            return desc;
        }
        void DrawMeshes(Device&                      device,
                        CommandList&                 commands,
                        const Handle<Pipeline>&      pipeline,
                        Handle<Pipeline>&            skinnedPipeline,
                        const std::vector<MeshDraw>& draws,
                        const glm::mat4&             projection,
                        bool                         pick = false)
        {
            for (size_t i = 0; i < draws.size(); ++i)
            {
                const auto& draw = draws[i];
                if (!draw.Mesh || !draw.Mesh->Vertices || !draw.Mesh->Indices ||
                    draw.Section >= draw.Mesh->Sections.size())
                    continue;
                auto active = pipeline;
                if (draw.Mesh->Skin && draw.Palette)
                {
                    auto& variant = skinnedPipeline;
                    if (!variant)
                    {
                        auto       desc = pipeline->GetDesc();
                        const bool ids  = desc.ColorFormats[0] == Format::R32Uint;
                        desc.Program    = GetSkinnedInternalProgram(device,
                                                                    ids ? "SkinnedPick" : "SkinnedMask",
                                                                    PositionVertex,
                                                                    ids ? IdFragment : MaskFragment);
                        desc.GroupLayouts.push_back(device.CreateBindGroupLayout({1, {}}));
                        desc.GroupLayouts.push_back(SkinningLayout(device));
                        desc.VertexBindings.push_back({1, 32, false});
                        desc.Attributes.push_back({5, 1, 0, VertexFormat::Uint4});
                        desc.Attributes.push_back({6, 1, 16, VertexFormat::Float4});
                        variant = device.CreatePipeline(desc);
                    }
                    active = variant;
                }
                if (draw.IgnoreDepth || !draw.DoubleSided)
                {
                    auto desc = active->GetDesc();
                    if (draw.IgnoreDepth)
                    {
                        desc.DepthStencil.DepthTest    = true;
                        desc.DepthStencil.DepthCompare = CompareOp::Always;
                    }
                    if (!draw.DoubleSided)
                        desc.Rasterizer.Cull = CullMode::Back;
                    active = device.CreatePipeline(desc);
                }
                commands.BindPipeline(active);
                const DrawData data {projection * draw.Transform,
                                     glm::uvec4(pick ? uint32_t(i + 1) : draw.ID, 0, 0, 0)};
                auto           buffer = device.CreateBuffer({sizeof(data), BufferUsage::Uniform, MemoryUsage::Upload});
                buffer->Write(0, &data, sizeof(data));
                commands.BindGroup(device.CreateBindGroup({pipeline->GetDesc().GroupLayouts[0], {{0, buffer}}}));
                if (draw.Mesh->Skin && draw.Palette)
                {
                    commands.BindGroup(device.CreateBindGroup({active->GetDesc().GroupLayouts[1], {}}));
                    BindSkinning(device,
                                 commands,
                                 active->GetDesc().GroupLayouts[2],
                                 draw.Palette ? draw.Palette : draw.Mesh->BindPalette,
                                 draw.Mesh->MorphDeltas);
                    commands.BindVertexBuffers({{0, draw.Mesh->Vertices, 0}, {1, draw.Mesh->Skin, 0}});
                }
                else
                    commands.BindVertexBuffers(
                        {{0, draw.Mesh->BindVertices ? draw.Mesh->BindVertices : draw.Mesh->Vertices, 0}});
                commands.BindIndexBuffer(draw.Mesh->Indices, IndexType::Uint32);
                const auto& section = draw.Mesh->Sections[draw.Section];
                commands.DrawIndexed(section.IndexCount, 1, section.FirstIndex);
            }
        }
    }

    PickHit ResolveMeshPickHit(const std::vector<MeshDraw>& draws, uint32_t rasterID)
    {
        if (!rasterID || rasterID > draws.size())
            return {};
        const auto& draw = draws[rasterID - 1];
        if (!draw.Mesh || draw.Section >= draw.Mesh->Sections.size())
            return {};
        return {draw.ID, {}, draw.Mesh->Sections[draw.Section].MaterialSlot, draw.Section};
    }

    void EntityPicker::Resize(glm::uvec2 size)
    {
        if (size == m_Size && m_Color)
            return;
        using namespace RenderHardware;
        TextureDesc desc;
        desc.Width       = size.x;
        desc.Height      = size.y;
        desc.PixelFormat = Format::R32Uint;
        desc.Usage       = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::CopySource;
        m_Color          = m_Device->CreateTexture(desc);
        desc.PixelFormat = Format::Depth32Float;
        desc.Usage       = RenderHardware::TextureUsage::DepthStencil | RenderHardware::TextureUsage::CopySource;
        m_Depth          = m_Device->CreateTexture(desc);
        m_Size = size;
    }

    uint32_t EntityPicker::Pick(const std::vector<MeshDraw>& draws,
                                const glm::mat4&             projection,
                                glm::uvec2                   viewport,
                                glm::uvec2                   pixel)
    {
        return PickSurface(draws, projection, viewport, pixel).ID;
    }

    PickHit EntityPicker::PickSurface(const std::vector<MeshDraw>& draws,
                                      const glm::mat4&             projection,
                                      glm::uvec2                   viewport,
                                      glm::uvec2                   pixel)
    {
        if (draws.empty() || !viewport.x || !viewport.y || pixel.x >= viewport.x || pixel.y >= viewport.y)
            return {};
        using namespace RenderHardware;
        if (!m_Device)
        {
            for (const auto& draw : draws)
                if (draw.Mesh && draw.Mesh->Device)
                {
                    m_Device = draw.Mesh->Device;
                    break;
                }
            if (!m_Device)
                return {};
        }
        if (!m_Pipeline)
        {
            auto desc = MeshPipeline(*m_Device, "EntityPicker", IdFragment, Format::R32Uint);
            if (!desc.Program)
                return {};
            desc.HasDepthStencil        = true;
            desc.DepthStencilFormat     = Format::Depth32Float;
            desc.DepthStencil.DepthTest = desc.DepthStencil.DepthWrite = true;
            m_Pipeline                                                 = m_Device->CreatePipeline(desc);
        }
        Resize(viewport);
        const auto     coordinates = m_Device->GetCoordinateConventions();
        const uint32_t row         = DeviceRow(pixel.y, viewport.y, coordinates);
        auto           commands    = m_Device->CreateCommandList();
        RenderingDesc  target;
        target.Colors       = {{m_Color}};
        target.Depth.Target = m_Depth;
        commands->BeginRendering(target);
        commands->SetViewport({0, 0, float(viewport.x), float(viewport.y), 0, 1});
        commands->SetScissor({int32_t(pixel.x), int32_t(row), 1, 1});
        DrawMeshes(*m_Device,
                   *commands,
                   m_Pipeline,
                   m_SkinnedPipeline,
                   draws,
                   DeviceProjection(projection, coordinates),
                   true);
        commands->EndRendering();
        const size_t bytes = size_t(viewport.x) * viewport.y * 4;
        auto readback      = m_Device->CreateBuffer({bytes * 2, BufferUsage::CopyDestination, MemoryUsage::Readback});
        commands->CopyTextureToBuffer(m_Color, {}, readback);
        commands->CopyTextureToBuffer(m_Depth, {}, readback, bytes);
        m_Device->Submit(commands);
        m_Device->WaitIdle();
        const size_t offset = (size_t(row) * viewport.x + pixel.x) * 4;
        const auto   id     = readback->Read(offset, 4);
        uint32_t     rasterID = 0;
        std::memcpy(&rasterID, id.data(), 4);
        PickHit hit = ResolveMeshPickHit(draws, rasterID);
        if (hit.ID)
        {
            const auto z = readback->Read(bytes + offset, 4);
            float      depth;
            std::memcpy(&depth, z.data(), 4);
            // Editor projections and pixels stay in their existing [-1,1], bottom-left convention.
            const glm::vec2 ndc = (glm::vec2(pixel) + 0.5f) / glm::vec2(viewport) * 2.0f - 1.0f;
            const glm::vec4 world = glm::inverse(projection) * glm::vec4(ndc, depth * 2.0f - 1.0f, 1.0f);
            hit.Position = glm::vec3(world) / world.w;
        }
        return hit;
    }

    void SelectionOutline::Resize(glm::uvec2 size)
    {
        if (size == m_Size && m_Texture)
            return;
        using namespace RenderHardware;
        TextureDesc desc;
        desc.Width       = size.x;
        desc.Height      = size.y;
        desc.PixelFormat = Format::R8Unorm;
        desc.Usage       = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::Sampled;
        m_Texture        = m_Device->CreateTexture(desc);
        m_Size = size;
    }

    void SelectionOutline::Draw(const RenderHardware::RenderingDesc& target,
                                const std::vector<MeshDraw>&         draws,
                                const glm::mat4&                     projection,
                                glm::uvec2                           viewport,
                                const glm::vec4&                     color,
                                float                                width)
    {
        if (draws.empty() || !viewport.x || !viewport.y || target.Colors.empty())
            return;
        using namespace RenderHardware;
        if (!m_Device)
        {
            for (const auto& draw : draws)
                if (draw.Mesh && draw.Mesh->Device)
                {
                    m_Device = draw.Mesh->Device;
                    break;
                }
            if (!m_Device)
                return;
        }
        Resize(viewport);
        auto commands = m_Device->CreateCommandList();
        RecordMask(*commands, m_Texture, draws, projection, viewport);
        RecordComposite(*commands, target, m_Texture, viewport, color, width);
        m_Device->Submit(commands);
    }

    void SelectionOutline::RecordMask(RenderHardware::CommandList&                           commands,
                                      const RenderHardware::Handle<RenderHardware::Texture>& maskTexture,
                                      const std::vector<MeshDraw>&                           draws,
                                      const glm::mat4&                                       projection,
                                      glm::uvec2                                             viewport,
                                      RenderHardware::Handle<RenderHardware::Texture>        depth)
    {
        using namespace RenderHardware;
        if (!m_Mask)
        {
            auto desc = MeshPipeline(*m_Device, "SelectionMask", MaskFragment, Format::R8Unorm);
            if (!desc.Program)
                return;
            m_Mask   = m_Device->CreatePipeline(desc);
            m_Layout = m_Device->CreateBindGroupLayout(
                {0, {{0, BindingType::CombinedTextureSampler}, {1, BindingType::UniformBuffer}}});
            SamplerDesc sampler;
            sampler.Min = sampler.Mag = sampler.Mip = Filter::Nearest;
            sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
            m_Sampler                         = m_Device->CreateSampler(sampler);
        }
        auto pipeline = m_Mask;
        if (depth)
        {
            if (!m_VisibleMask || m_VisibleMask->GetDesc().DepthStencilFormat != depth->GetDesc().PixelFormat)
            {
                auto desc                      = m_Mask->GetDesc();
                desc.HasDepthStencil           = true;
                desc.DepthStencilFormat        = depth->GetDesc().PixelFormat;
                desc.DepthStencil.DepthTest    = true;
                desc.DepthStencil.DepthCompare = CompareOp::LessEqual;
                m_VisibleMask                  = m_Device->CreatePipeline(desc);
                m_VisibleSkinnedPipeline.reset();
            }
            pipeline = m_VisibleMask;
        }
        RenderingDesc mask;
        mask.Colors = {{maskTexture}};
        mask.Depth.Target = depth;
        mask.Depth.Load   = LoadOp::Load;
        commands.BeginRendering(mask);
        commands.SetViewport({0, 0, float(viewport.x), float(viewport.y), 0, 1});
        commands.SetScissor({0, 0, viewport.x, viewport.y});
        DrawMeshes(*m_Device,
                   commands,
                   pipeline,
                   depth ? m_VisibleSkinnedPipeline : m_SkinnedPipeline,
                   draws,
                   DeviceProjection(projection, m_Device->GetCoordinateConventions()));
        commands.EndRendering();
    }

    void SelectionOutline::RecordComposite(RenderHardware::CommandList&                           commands,
                                           const RenderHardware::RenderingDesc&                   target,
                                           const RenderHardware::Handle<RenderHardware::Texture>& maskTexture,
                                           glm::uvec2                                             viewport,
                                           const glm::vec4&                                       color,
                                           float                                                  width,
                                           bool                                                   surfaceHighlight)
    {
        using namespace RenderHardware;
        if (!m_Layout)
            return;
        // Target formats are part of the pipeline, so a different target cannot reuse incompatible state.
        PipelineDesc desc;
        desc.Program = m_Composite ? m_Composite->GetDesc().Program :
                                     Build(*m_Device, "SelectionComposite", FullscreenVertex, CompositeFragment);
        if (!desc.Program)
            return;
        desc.GroupLayouts    = {m_Layout};
        desc.Rasterizer.Cull = CullMode::None;
        desc.ColorFormats    = {target.Colors[0].Target->GetDesc().PixelFormat};
        desc.HasDepthStencil = bool(target.Depth.Target);
        if (desc.HasDepthStencil)
            desc.DepthStencilFormat = target.Depth.Target->GetDesc().PixelFormat;
        BlendState blend;
        blend.Enabled          = true;
        blend.SourceColor      = BlendFactor::SourceAlpha;
        blend.DestinationColor = BlendFactor::OneMinusSourceAlpha;
        blend.SourceAlpha      = BlendFactor::Zero;
        blend.DestinationAlpha = BlendFactor::One;
        desc.Blends.resize(desc.ColorFormats.size(), blend);
        m_Composite = m_Device->CreatePipeline(desc);
        auto composite = target;
        composite.Colors.resize(1);
        for (auto& attachment : composite.Colors)
            attachment.Load = LoadOp::Load;
        composite.Depth.Load = LoadOp::Load;
        commands.BeginRendering(composite);
        commands.SetViewport({0, 0, float(viewport.x), float(viewport.y), 0, 1});
        commands.SetScissor({0, 0, viewport.x, viewport.y});
        commands.BindPipeline(m_Composite);
        BindGroupEntry entry;
        entry.TextureResource = maskTexture;
        entry.SamplerResource = m_Sampler;
        const CompositeData data {color,
                                  glm::ivec4(std::clamp(int(width + 0.5f), 1, 6), surfaceHighlight ? 1 : 0, 0, 0)};
        auto parameters = m_Device->CreateBuffer({sizeof(data), BufferUsage::Uniform, MemoryUsage::Upload});
        parameters->Write(0, &data, sizeof(data));
        commands.BindGroup(m_Device->CreateBindGroup({m_Layout, {entry, {1, parameters}}}));
        commands.Draw(3);
        commands.EndRendering();
    }
} // namespace Kans
