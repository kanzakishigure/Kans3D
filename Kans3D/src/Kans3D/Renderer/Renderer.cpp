#include "kspch.h"
#include "Renderer.h"
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <glm/gtc/packing.hpp>
#include "Kans3D/Asset/Importer/TextureSource.h"
#include "Kans3D/RHI/PipelineKey.h"
#include "RenderFeature.h"
#include "RenderFrame.h"
#include "RenderPipeline.h"
#include "Resource/InternalShader.h"
#include "Resource/MeshFactory.h"
#include "Resource/SceneShaders.h"
#include "SpriteBatch.h"
#include "TextRenderer.h"
#include "UIRenderer.h"
namespace Kans
{
    namespace
    {
        // Replaces the facade's existing global white-texture owner. Explicit
        // application Init/Shutdown owns this lease; headless views own theirs.
        std::shared_ptr<Renderer> ApplicationRenderer;
    } // namespace
    std::shared_ptr<Renderer> Renderer::Acquire(std::shared_ptr<RenderHardware::Device> device)
    {
        if (!device)
            device = RenderHardware::GetSharedDevice();
        // Keep engine-module discovery above the hardware interface. Entries are
        // weak; the renderer's device lease prevents key reuse while it is live.
        static std::mutex                                                                 mutex;
        static std::unordered_map<const RenderHardware::Device*, std::weak_ptr<Renderer>> renderers;
        std::lock_guard<std::mutex>                                                       lock(mutex);
        for (auto it = renderers.begin(); it != renderers.end();)
            if (it->second.expired())
                it = renderers.erase(it);
            else
                ++it;
        auto& entry    = renderers[device.get()];
        auto  renderer = entry.lock();
        if (!renderer)
        {
            renderer = std::shared_ptr<Renderer>(new Renderer(std::move(device)));
            entry    = renderer;
        }
        return renderer;
    }
    Renderer::Renderer(std::shared_ptr<RenderHardware::Device> device) : m_Device(std::move(device)), m_Picker(m_Device)
    {
        m_Outline.SetDevice(m_Device);
    }
    Renderer::~Renderer() { m_Device->WaitIdle(); }
    void Renderer::Init()
    {
        ApplicationRenderer = Acquire(RenderHardware::InitializeDevice(RenderHardware::GetBackend()));
    }
    void Renderer::Init(const Scope<Window>& window)
    {
        ApplicationRenderer = Acquire(RenderHardware::InitializeDevice(RenderHardware::GetBackend(), window.get()));
    }
    void Renderer::Shutdown()
    {
        ApplicationRenderer.reset();
        RenderHardware::ShutdownDevice();
    }
    Ref<Texture2D> Renderer::GetWhiteTexture()
    {
        if (!ApplicationRenderer)
            ApplicationRenderer = Acquire();
        auto& renderer = *ApplicationRenderer;
        if (!renderer.m_White)
        {
            TextureSpecification spec;
            spec.Width = spec.Height = 1;
            uint32_t pixel           = 0xffffffff;
            renderer.m_White         = Texture2D::Create(spec, Buffer(&pixel, sizeof(pixel)));
        }
        return renderer.m_White;
    }
    std::shared_ptr<RenderAssetCache> Renderer::AcquireAssetCache(AssetManager& assets)
    {
        auto& entry = m_AssetCaches[&assets];
        auto  lease = entry.Lease.lock();
        if (!lease)
        {
            entry.Cache = CreateScope<RenderAssetCache>(assets, m_Device);
            // The module owns the cache. Clients retain its module and membership,
            // not an independently owned cache. Last detach removes the manager
            // entry before the caller can destroy that manager; no cycle exists.
            lease = std::shared_ptr<RenderAssetCache>(
                entry.Cache.get(), [owner = shared_from_this(), manager = &assets](RenderAssetCache*) mutable {
                    owner->m_AssetCaches.erase(manager);
                    // Weak observers can retain the lease control block and its
                    // deleter. They must not retain the module or its GPU resources.
                    owner.reset();
                });
            entry.Lease = lease;
        }
        return lease;
    }
    namespace
    {
        using namespace RenderHardware;
        struct CaptureData
        {
            glm::mat4 Projection {1}, View {1};
            glm::vec4 Parameters {0};
        };
        Handle<RenderHardware::Shader> Build(Device& device, const char* name, const char* vertex, const char* fragment)
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
            return std::get<Handle<RenderHardware::Shader>>(std::move(result));
        }
        template<class T>
        Handle<BindGroup> DrawGroup(Device&                         device,
                                    const Handle<BindGroupLayout>&  layout,
                                    const T&                        data,
                                    Handle<RenderHardware::Texture> texture = {},
                                    Handle<Sampler>                 sampler = {})
        {
            auto buffer = device.CreateBuffer({sizeof(T), BufferUsage::Uniform, MemoryUsage::Upload});
            buffer->Write(0, &data, sizeof(T));
            BindGroupDesc group;
            group.Layout = layout;
            group.Entries.push_back({0, buffer, 0, sizeof(T), {}, {}});
            if (texture)
                group.Entries.push_back({1, {}, 0, 0, std::move(texture), std::move(sampler)});
            return device.CreateBindGroup(group);
        }
        uint32_t Mips(uint32_t size)
        {
            uint32_t levels = 1;
            while (size > 1)
            {
                size >>= 1;
                ++levels;
            }
            return levels;
        }
    } // namespace

    RenderHardware::Handle<RenderHardware::Shader>
    Renderer::GetInternalProgram(const char* name, const char* vertex, const char* fragment)
    {
        auto& program = m_InternalPrograms[std::make_tuple(name, vertex, fragment)];
        if (!program)
            program = Build(*m_Device, name, vertex, fragment);
        return program;
    }
    const GpuMesh& Renderer::GetCube()
    {
        if (!m_Cube)
        {
            auto mesh = UploadMesh(MeshFactory::CreateCubeData(glm::vec3(2)), m_Device);
            if (auto* error = std::get_if<AssetError>(&mesh))
                throw std::runtime_error(error->Message);
            m_Cube = std::get<GpuMesh>(std::move(mesh));
        }
        return *m_Cube;
    }
    RenderHardware::Handle<RenderHardware::Pipeline> Renderer::CachedPipeline(const RenderHardware::PipelineDesc& desc)
    {
        auto& pipeline = m_InternalPipelines[RenderHardware::Detail::PipelineKey(desc)];
        if (!pipeline)
            pipeline = m_Device->CreatePipeline(desc);
        return pipeline;
    }

    RenderHardware::PipelineDesc
    Renderer::ScenePipeline(const char* name, const char* vertex, const char* fragment, bool sampled)
    {
        using namespace RenderHardware;
        PipelineDesc desc;
        // GPU programs are device-module owned; CPU products are cached by InternalShader.
        auto program = GetInternalProgram(name, vertex, fragment);
        desc.Program = program;
        BindGroupLayoutDesc layout {0, {{0, BindingType::UniformBuffer}}};
        if (sampled)
            layout.Entries.push_back({1, BindingType::CombinedTextureSampler});
        desc.GroupLayouts           = {m_Device->CreateBindGroupLayout(layout)};
        desc.ColorFormats           = {Format::RGBA8Unorm, Format::RGBA8Unorm};
        desc.HasDepthStencil        = true;
        desc.DepthStencilFormat     = Format::Depth24Stencil8;
        desc.DepthStencil.DepthTest = true;
        desc.Rasterizer.Cull        = CullMode::None;
        // DeviceProjection already converts clip Y using the coordinate query.
        // The converted projection preserves the mesh's visual CCW facing.
        desc.Rasterizer.Front = FrontFace::CounterClockwise;
        BlendState blend;
        blend.WriteMask = 7;
        desc.Blends     = {blend, blend};
        return desc;
    }

    std::array<RenderHardware::Handle<RenderHardware::Texture>, 3>
    Renderer::GetMaterialEnvironment(const EnvironmentLighting& lighting)
    {
        using namespace RenderHardware;
        if (!m_IBLSampler)
        {
            SamplerDesc desc;
            desc.U = desc.V = desc.W = AddressMode::ClampToEdge;
            m_IBLSampler             = m_Device->CreateSampler(desc);
        }
        std::array<Handle<RenderHardware::Texture>, 3>       environment;
        const std::array<Handle<RenderHardware::Texture>, 3> maps {
            lighting.Maps.IrradianceMap, lighting.Maps.RadianceMap, lighting.BrdfLUT};
        for (size_t i = 0; i < 3; ++i)
        {
            const auto dimension = i == 2 ? TextureDimension::Texture2D : TextureDimension::Cube;
            if (maps[i])
                environment[i] = maps[i];
            else
            {
                if (m_BlackEnvironment[i])
                {
                    environment[i] = m_BlackEnvironment[i];
                    continue;
                }
                TextureDesc desc;
                desc.Dimension   = dimension;
                desc.PixelFormat = Format::RGBA32Float;
                desc.Usage     = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
                environment[i] = m_BlackEnvironment[i] = m_Device->CreateTexture(desc);
                const std::array<float, 4> black {0, 0, 0, 1};
                auto upload = m_Device->CreateBuffer({sizeof(black), BufferUsage::CopySource, MemoryUsage::Upload});
                upload->Write(0, black.data(), sizeof(black));
                auto copy = m_Device->CreateCommandList();
                for (uint32_t face = 0; face < (i == 2 ? 1u : 6u); ++face)
                    copy->CopyBufferToTexture(upload, 0, environment[i], {0, face});
                m_Device->Submit(copy);
            }
        }
        return environment;
    }

    std::shared_ptr<EnvironmentLighting> Renderer::UpdateEnvironment(const std::filesystem::path& path,
                                                                     EnvironmentPrecomputeSizes   sizes)
    {
        // Refresh in place so existing leases, including differently sized views,
        // observe a source edit. A failed refresh retains the last usable images.
        const auto source = std::filesystem::absolute(path).lexically_normal().generic_string();
        std::vector<EnvironmentPrecomputeSizes> variants;
        for (const auto& entry : m_Environments)
            if (std::get<0>(entry.first) == source && entry.second)
                variants.push_back({std::get<1>(entry.first),
                                    std::get<2>(entry.first),
                                    std::get<3>(entry.first),
                                    std::get<4>(entry.first)});
        for (auto variant : variants)
            if (variant.Cubemap != sizes.Cubemap || variant.Irradiance != sizes.Irradiance ||
                variant.Prefilter != sizes.Prefilter || variant.BrdfLUT != sizes.BrdfLUT)
                PrepareEnvironment(path, variant, true);
        return PrepareEnvironment(path, sizes, true);
    }

    std::shared_ptr<EnvironmentLighting>
    Renderer::PrepareEnvironment(const std::filesystem::path& path, EnvironmentPrecomputeSizes sizes, bool refresh)
    {
        using namespace RenderHardware;
        if (!sizes.Cubemap || !sizes.Irradiance || sizes.Prefilter < 16 || !sizes.BrdfLUT)
            throw std::invalid_argument("IBL sizes must be positive; prefilter needs five mips (at least 16 texels)");
        const auto key    = std::make_tuple(std::filesystem::absolute(path).lexically_normal().generic_string(),
                                            sizes.Cubemap,
                                            sizes.Irradiance,
                                            sizes.Prefilter,
                                            sizes.BrdfLUT);
        auto&      cached = m_Environments[key];
        if (cached && !refresh)
            return cached;
        // The asset decoder owns its stb configuration. Decode bottom-first rows,
        // then match the legacy HDR image's half-float quantization before sampling.
        TextureImportSettings settings;
        settings.FlipY        = true;
        settings.GenerateMips = false;
        auto decoded          = ParseTextureSource(std::filesystem::absolute(path), settings);
        if (auto* error = std::get_if<AssetError>(&decoded))
        {
            if (Log::GetCoreLogger())
                CORE_ERROR("{}", error->Message);
            else
                std::cerr << error->Message << std::endl;
            return cached;
        }
        const auto& source = std::get<TextureData>(decoded);
        if (source.Format != TexturePixelFormat::RGBA32Float)
        {
            if (Log::GetCoreLogger())
                CORE_ERROR("Environment must be a floating-point HDR image");
            return cached;
        }
        const uint32_t        width = source.Width, height = source.Height;
        std::vector<uint16_t> half(size_t(width) * height * 4);
        for (size_t i = 0; i < half.size(); ++i)
        {
            const auto* bytes = source.Mips[0].Pixels.data() + i * 4;
            uint32_t    word  = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
                                (uint32_t(bytes[3]) << 24);
            float       value;
            std::memcpy(&value, &word, sizeof(value));
            half[i] = glm::packHalf1x16(value);
        }
        TextureDesc hdrDesc;
        hdrDesc.Width       = width;
        hdrDesc.Height      = height;
        hdrDesc.PixelFormat = Format::RGBA16Float;
        hdrDesc.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
        auto hdr            = m_Device->CreateTexture(hdrDesc);
        auto upload =
            m_Device->CreateBuffer({half.size() * sizeof(uint16_t), BufferUsage::CopySource, MemoryUsage::Upload});
        upload->Write(0, half.data(), half.size() * sizeof(uint16_t));
        auto commands = m_Device->CreateCommandList();
        commands->CopyBufferToTexture(upload, 0, hdr);
        const auto& cube    = GetCube();
        auto        texture = [&](uint32_t size, uint32_t levels, TextureDimension dimension) {
            TextureDesc desc;
            desc.Width = desc.Height = size;
            desc.MipLevels           = levels;
            desc.Dimension           = dimension;
            desc.PixelFormat         = Format::RGBA16Float;
            desc.Usage = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::RenderTarget |
                         RenderHardware::TextureUsage::CopySource | RenderHardware::TextureUsage::CopyDestination;
            return m_Device->CreateTexture(desc);
        };
        auto        sky        = texture(sizes.Cubemap, Mips(sizes.Cubemap), TextureDimension::Cube);
        auto        irradiance = texture(sizes.Irradiance, 1, TextureDimension::Cube);
        auto        prefilter  = texture(sizes.Prefilter, Mips(sizes.Prefilter), TextureDimension::Cube);
        auto        brdf       = texture(sizes.BrdfLUT, 1, TextureDimension::Texture2D);
        SamplerDesc sampling;
        sampling.U = sampling.V = sampling.W = AddressMode::ClampToEdge;
        auto sampler                         = m_Device->CreateSampler(sampling);
        auto pipeline = [&](const char* name, const char* vertex, const char* fragment, bool sampled, bool cube) {
            PipelineDesc desc;
            auto         program = GetInternalProgram(name, vertex, fragment);
            desc.Program         = program;
            if (!desc.Program)
                return Handle<Pipeline> {};
            BindGroupLayoutDesc layout {0, {{0, BindingType::UniformBuffer}}};
            if (sampled)
                layout.Entries.push_back({1, BindingType::CombinedTextureSampler});
            desc.GroupLayouts           = {m_Device->CreateBindGroupLayout(layout)};
            desc.ColorFormats           = {Format::RGBA16Float};
            desc.HasDepthStencil        = true;
            desc.DepthStencilFormat     = Format::Depth24Stencil8;
            desc.DepthStencil.DepthTest = desc.DepthStencil.DepthWrite = true;
            desc.DepthStencil.DepthCompare                             = CompareOp::LessEqual;
            desc.Blends.resize(1);
            desc.Rasterizer.Cull = CullMode::None;
            if (cube)
            {
                desc.VertexBindings = {{0, 64, false}};
                desc.Attributes     = {{0, 0, 0, VertexFormat::Float3}};
            }
            else
                desc.Primitive = Topology::TriangleStrip;
            return CachedPipeline(desc);
        };
        auto hdrPipeline = pipeline("HdrToCubeMap", SceneShaders::HdrVertex, SceneShaders::HdrFragment, true, true);
        auto irradiancePipeline =
            pipeline("genIrradiance", SceneShaders::IrradianceVertex, SceneShaders::IrradianceFragment, true, true);
        auto prefilterPipeline =
            pipeline("prefilter", SceneShaders::PrefilterVertex, SceneShaders::PrefilterFragment, true, true);
        auto brdfPipeline = pipeline("brdf", SceneShaders::BrdfVertex, SceneShaders::BrdfFragment, false, false);
        if (!hdrPipeline || !irradiancePipeline || !prefilterPipeline || !brdfPipeline)
            return cached;
        const glm::vec3 directions[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec3 up[]         = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
        CaptureData     data;
        data.Projection = DeviceTextureProjection(glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f),
                                                  m_Device->GetCoordinateConventions());
        std::map<uint32_t, Handle<RenderHardware::Texture>> depths;
        auto                                                depth = [&](uint32_t size) {
            auto& target = depths[size];
            if (!target)
            {
                TextureDesc desc;
                desc.Width = desc.Height = size;
                desc.PixelFormat         = Format::Depth24Stencil8;
                desc.Usage               = RenderHardware::TextureUsage::DepthStencil;
                target                   = m_Device->CreateTexture(desc);
            }
            DepthAttachment attachment;
            attachment.Target = target;
            attachment.Load   = LoadOp::Clear;
            return attachment;
        };
        auto capture = [&](const Handle<Pipeline>&                pass,
                           const Handle<RenderHardware::Texture>& target,
                           const Handle<RenderHardware::Texture>& source,
                           uint32_t                               mip,
                           float                                  roughness) {
            uint32_t size     = std::max(1u, target->GetDesc().Width >> mip);
            data.Parameters.x = roughness;
            for (uint32_t face = 0; face < 6; ++face)
            {
                data.View = glm::lookAt(glm::vec3(0), directions[face], up[face]);
                RenderingDesc render;
                render.Colors.push_back({target, {mip, face}, LoadOp::Clear, StoreOp::Store, {0.02f, 0.02f, 0.02f, 1}});
                render.Depth = depth(size);
                commands->BeginRendering(render);
                commands->SetViewport({0, 0, float(size), float(size)});
                commands->BindPipeline(pass);
                commands->BindGroup(DrawGroup(*m_Device, pass->GetDesc().GroupLayouts[0], data, source, sampler));
                commands->BindVertexBuffers({{0, cube.Vertices, 0}});
                commands->BindIndexBuffer(cube.Indices, IndexType::Uint32);
                commands->DrawIndexed(cube.IndexCount);
                commands->EndRendering();
            }
        };
        capture(hdrPipeline, sky, hdr, 0, 0);
        commands->GenerateMips(sky);
        capture(irradiancePipeline, irradiance, sky, 0, 0);
        // Legacy prefilter renders exactly five roughness levels. The remaining allocated
        // levels are zero, as in the original allocation followed by mip generation.
        const std::array<uint16_t, 4> zero {0, 0, 0, 0};
        auto                          zeros = m_Device->CreateBuffer(
            {size_t(sizes.Prefilter) * sizes.Prefilter * sizeof(zero), BufferUsage::CopySource, MemoryUsage::Upload});
        std::vector<uint16_t> zeroPixels(size_t(sizes.Prefilter) * sizes.Prefilter * 4, 0);
        zeros->Write(0, zeroPixels.data(), zeroPixels.size() * sizeof(uint16_t));
        // Initialize lower levels explicitly; avoid generating over the rendered roughness levels.
        for (uint32_t mip = 5; mip < prefilter->GetDesc().MipLevels; ++mip)
            for (uint32_t face = 0; face < 6; ++face)
                commands->CopyBufferToTexture(zeros, 0, prefilter, {mip, face});
        for (uint32_t mip = 0; mip < 5; ++mip)
            capture(prefilterPipeline, prefilter, sky, mip, float(mip) / 4.0f);
        RenderingDesc render;
        render.Colors.push_back({brdf});
        render.Depth = depth(sizes.BrdfLUT);
        commands->BeginRendering(render);
        commands->SetViewport({0, 0, float(sizes.BrdfLUT), float(sizes.BrdfLUT)});
        commands->BindPipeline(brdfPipeline);
        data.Projection = DeviceTextureProjection(glm::mat4(1), m_Device->GetCoordinateConventions());
        commands->BindGroup(DrawGroup(*m_Device, brdfPipeline->GetDesc().GroupLayouts[0], data));
        commands->Draw(4);
        commands->EndRendering();
        m_Device->Submit(commands);
        m_Device->WaitIdle();
        if (!cached)
            cached = std::make_shared<EnvironmentLighting>();
        cached->Skybox             = std::move(sky);
        cached->Maps.IrradianceMap = std::move(irradiance);
        cached->Maps.RadianceMap   = std::move(prefilter);
        cached->BrdfLUT            = std::move(brdf);
        ++m_EnvironmentPrecomputeCount;
        return cached;
    }

} // namespace Kans

namespace Kans
{
    RenderResult Renderer::Render(RenderWorld& world, const RenderView& input, ViewTarget& target)
    {
        if (target.GetDevice() != m_Device)
            throw std::invalid_argument("Renderer target belongs to another device");
        const auto   totalStart = RenderClockMilliseconds();
        RenderResult result;
        auto&        statistics = result.FrameStats;
        statistics.Frame        = target.NextFrameNumber();
        result.View = input;
        auto& view  = result.View;
        if (!view.Features)
            view.Features = &target.GetDefaultFeatures();
        if (view.Viewport == glm::uvec2(0))
            view.Viewport = target.GetSize();
        if (target.GetSize() != view.Viewport)
            target.Resize(view.Viewport);
        if (!view.Assets && world.GetAssetManager())
            view.Assets = AcquireAssetCache(*world.GetAssetManager());
        if (view.Assets)
        {
            if (view.Assets->GetDevice() != m_Device)
                throw std::invalid_argument("Renderer asset cache belongs to another device");
            world.SetAssetManager(&view.Assets->GetAssetManager());
        }
        const auto extractStart = RenderClockMilliseconds();
        world.Synchronize();
        auto texts = world.CollectTexts();
        if (!texts.empty())
        {
            GetTextRenderer().BeginFrame();
            for (const auto& text : texts)
                text.Layout->Get().TouchGlyphs();
            GetTextRenderer().Upload(texts);
            // Rasterization can replace placeholders; only invalidated layouts are rebuilt.
            world.Synchronize();
            for (const auto& text : world.CollectTexts())
                for (const auto& glyph : text.Layout->Get().Glyphs)
                    result.Complete &=
                        glyph.Ready || glyph.Position.x == glyph.Position.z || glyph.Position.y == glyph.Position.w;
        }
        if (view.Assets)
            view.Assets->BeginFrame();
        if (view.ExtractWorld)
        {
            world.Extract(view, world.Settings);
            const auto spriteOffset = view.Sprites.size();
            for (auto& immediate : view.ImmediateOrder)
                if (immediate.Kind == ViewImmediateKind::Sprites)
                    immediate.Count = spriteOffset;
            view.Draws.insert(view.Draws.end(), input.Draws.begin(), input.Draws.end());
            view.Meshes.insert(view.Meshes.end(), input.Meshes.begin(), input.Meshes.end());
            view.Geometry.insert(view.Geometry.end(), input.Geometry.begin(), input.Geometry.end());
            view.Sprites.insert(view.Sprites.end(), input.Sprites.begin(), input.Sprites.end());
            if (!input.Grids.empty())
                view.Grids = input.Grids; // Per-viewport plane/origin overrides the world default.
            for (auto immediate : input.ImmediateOrder)
            {
                if (immediate.Kind == ViewImmediateKind::Sprites)
                {
                    if (immediate.Count == SIZE_MAX)
                        immediate.Count = input.Sprites.size() - immediate.Index;
                    immediate.Index += spriteOffset;
                }
                view.ImmediateOrder.push_back(immediate);
            }
        }
        statistics.WorldSyncExtractMilliseconds = RenderClockMilliseconds() - extractStart;
        statistics.ProxiesCreated               = world.GetStats().Created;
        statistics.ProxiesUpdated               = world.GetStats().Updated;
        statistics.ProxiesRemoved               = world.GetStats().Removed;
        if (view.ExtractWorld)
        {
            auto uis = world.CollectUIs(view.Scene.sceneCamera);
            view.UIs.insert(view.UIs.end(), uis.begin(), uis.end());
        }
        std::stable_sort(view.UIs.begin(), view.UIs.end(), [](auto& a, auto& b) { return a.SortOrder < b.SortOrder; });
        if (!view.UIs.empty())
        {
            if (texts.empty())
                GetTextRenderer().BeginFrame();
            result.Complete &= GetUIRenderer().Prepare(view.UIs, view.Assets.get(), view.Viewport);
        }
        for (auto& sprite : view.Sprites)
            if (sprite.TextureAsset.IsValid() && view.Assets)
            {
                auto status = view.Assets->GetAssetManager().Request<TextureAsset>(sprite.TextureAsset);
                result.Complete &= status.State != AssetLoadState::Loading;
                sprite.Texture = view.Assets->GetTexture(sprite.TextureAsset);
            }
        // SortingOrder precedes back-to-front distance; equal distances retain submission order.
        std::stable_sort(view.Sprites.begin(), view.Sprites.end(), [&](auto& a, auto& b) {
            if (a.SortingOrder != b.SortingOrder)
                return a.SortingOrder < b.SortingOrder;
            auto da = glm::vec3(a.Transform[3]) - view.Scene.sceneCamera.Position,
                 db = glm::vec3(b.Transform[3]) - view.Scene.sceneCamera.Position;
            return glm::dot(da, da) > glm::dot(db, db);
        });
        const auto buildStart                   = RenderClockMilliseconds();
        const bool sceneEnvironment = view.UseWorldEnvironment && world.Environment.has_value();
        if (view.UseWorldEnvironment)
            view.Environment = world.Environment.value_or(EnvironmentSettings {});
        const auto path = view.EnvironmentPath.value_or(
            sceneEnvironment ? (world.Environment->Map.empty() ? std::filesystem::path {} :
                                                                 world.EnvironmentRoot / world.Environment->Map) :
            view.UseWorldEnvironment ? world.EnvironmentPath :
                                       std::filesystem::path {});
        // A scene that explicitly disables the map also suppresses caller-submitted skyboxes.
        if (sceneEnvironment && path.empty())
            view.Environment.ShowSkybox = false;
        const bool reuseEnvironment = !path.empty() && view.EnvironmentMaps.IrradianceMap &&
                                      !view.EnvironmentMaps.Source.empty() &&
                                      std::filesystem::absolute(path).lexically_normal() ==
                                          std::filesystem::absolute(view.EnvironmentMaps.Source).lexically_normal();
        const bool knownWorldMap    = view.UseWorldEnvironment && !path.empty() && !view.EnvironmentMaps.Source.empty();
        if ((sceneEnvironment || view.EnvironmentPath.has_value() || knownWorldMap) && !reuseEnvironment)
        {
            // An explicit empty map or failed replacement must not reuse the previous scene's maps.
            view.EnvironmentMaps = {};
            view.BrdfLUT.reset();
        }
        if (!path.empty() && !reuseEnvironment &&
            (sceneEnvironment || view.EnvironmentPath.has_value() || !view.EnvironmentMaps.IrradianceMap))
        {
            if (auto environment = PrepareEnvironment(path))
            {
                view.EnvironmentMaps = {environment->Maps.IrradianceMap, environment->Maps.RadianceMap, path};
                view.BrdfLUT         = environment->BrdfLUT;
            }
        }
        if (view.DrawEnvironment && view.Environment.ShowSkybox && view.EnvironmentMaps.IrradianceMap)
        {
            view.ImmediateOrder.push_back({ViewImmediateKind::Skybox, view.Skyboxes.size()});
            view.Skyboxes.push_back({view.EnvironmentMaps.IrradianceMap, {}});
        }
        RenderFrame frame {world, view, target, *this, result.Stats, result.SpriteStats, result.Complete};
        auto        pipeline = RenderPipelineRegistry::Create(view.Pipeline);
        const auto  requestedAA = view.AntiAliasing.value_or(view.Scene.sceneCamera.camera.GetAntiAliasing());
        uint32_t    supportedAA = 15;
        for (const auto& color : target.GetColors())
            supportedAA &= m_Device->GetSupportedSampleCounts(color->GetDesc());
        supportedAA &= m_Device->GetSupportedSampleCounts(target.GetHDR()->GetDesc());
        supportedAA &= m_Device->GetSupportedSampleCounts(target.GetDepth()->GetDesc());
        if (AntiAliasingSamples(requestedAA) > 1 && !m_Device->SupportsSampleRateShading())
            supportedAA = 1;
        view.EffectiveAA            = EffectiveAntiAliasing(requestedAA, supportedAA);
        frame.Samples               = AntiAliasingSamples(view.EffectiveAA);
        statistics.EffectiveAA      = view.EffectiveAA;
        statistics.EffectiveSamples = frame.Samples;
        if (frame.Samples == 1)
            target.GetTransientPool().DiscardMultisampled();
        m_Device->BeginFrame();
        double                        executeStart = 0;
        std::shared_ptr<GraphCapture> capture;
        try
        {
            if (view.Features)
                view.Features->Prepare(view, frame);
            frame.PrepareDrawLists();
            pipeline->Prepare(view);
            if (view.Debug.CaptureGraph)
            {
                capture = target.GetGraphCapture();
                capture->BeginFrame(view.Debug,
                                    statistics.Frame,
                                    DeviceProjection(view.Scene.sceneCamera.camera.GetProjectionMatrix(),
                                                     m_Device->GetCoordinateConventions()));
            }
            result.Graph.SetExecutionDebug(&target.GetFrameProfiler(), statistics.Frame, capture.get());
            pipeline->Build(result.Graph, frame);
            statistics.GraphBuildMilliseconds = RenderClockMilliseconds() - buildStart;
            const auto compileStart           = RenderClockMilliseconds();
            result.Graph.Compile();
            statistics.CompileMilliseconds = RenderClockMilliseconds() - compileStart;
            executeStart                   = RenderClockMilliseconds();
            result.Graph.Execute(*m_Device, target.GetTransientPool());
            if (capture)
                result.Captures = capture->Latest();
            result.Graph.ReleaseRecorders();
        }
        catch (...)
        {
            if (m_Sprites)
                m_Sprites->ClearTarget();
            m_Device->EndFrame();
            throw;
        }
        m_Device->EndFrame();
        statistics.ExecuteSubmitMilliseconds = RenderClockMilliseconds() - executeStart;
        statistics.Passes                    = result.Graph.Statistics();
        statistics.Gpu                       = target.GetFrameProfiler().Latest();
        statistics.Draws                     = RenderHardware::CommandStatistics {};
        for (auto& pass : statistics.Passes)
        {
            if (pass.Culled)
                continue;
            if (!pass.Draws)
                statistics.Draws.reset();
            else if (statistics.Draws)
            {
                statistics.Draws->DrawCalls += pass.Draws->DrawCalls;
                statistics.Draws->Triangles += pass.Draws->Triangles;
                statistics.Draws->IndirectDraws += pass.Draws->IndirectDraws;
            }
            if (statistics.Gpu)
                for (auto& gpu : statistics.Gpu->Passes)
                    if (gpu.ID == pass.ID)
                        pass.GpuMilliseconds = gpu.EndMilliseconds - gpu.BeginMilliseconds;
        }
        statistics.Culling               = view.Culling;
        statistics.Shadows               = view.ShadowStats.PerPass;
        statistics.ShadowedLights        = view.ShadowStats.ShadowedLights;
        statistics.DirectionalLights     = view.Scene.HasDirectionalLight ||
                                                   view.Scene.dirLight.Diffuse_Intensity != glm::vec3(0) ||
                                                   view.Scene.dirLight.Ambient_Intensity != glm::vec3(0) ?
                                               1 :
                                               0;
        statistics.PointLights           = view.Scene.pointLights.empty() ?
                                               (view.Scene.pointLight.Diffuse_Intensity != glm::vec3(0) ? 1 : 0) :
                                               uint32_t(view.Scene.pointLights.size());
        statistics.BloomMipCount         = view.BloomMipCount;
        statistics.TransientTextureSlots = target.GetTransientPool().Size();
        const auto memory                = m_Device->GetStats();
        statistics.DeviceTextureBytes    = memory.TextureBytes;
        statistics.DeviceBufferBytes     = memory.BufferBytes;
        statistics.TotalMilliseconds     = RenderClockMilliseconds() - totalStart;
        return result;
    }
    void Renderer::AppendEntityDraws(RenderWorld&           world,
                                     const RenderView&      view,
                                     UUID                   entity,
                                     uint32_t               id,
                                     std::vector<MeshDraw>& draws)
    {
        auto cache = view.Assets;
        if (!cache && world.GetAssetManager())
            cache = AcquireAssetCache(*world.GetAssetManager());
        if (cache)
            world.SetAssetManager(&cache->GetAssetManager());
        world.Synchronize();
        for (const auto& text : world.CollectTexts())
            if (!text.Style.Text.empty() && view.Highlights.Isolate.empty() &&
                world.MatchesSubtree(text.EntityID, text.InstanceRoot, entity))
                draws.push_back(GetTextRenderer().BoundsDraw(text, view.Scene.sceneCamera, id));
        for (const auto& proxy : world.CollectDraws(entity))
        {
            const auto  resource = view.MeshResources.find(proxy.Mesh);
            const auto* mesh     = resource != view.MeshResources.end() ? resource->second.get() :
                                   cache                                ? cache->GetMesh(proxy.Mesh) :
                                                                          nullptr;
            if (mesh && proxy.Section < mesh->Sections.size() &&
                world.IsIsolated(
                    view.Highlights, proxy.EntityID, proxy.InstanceRoot, mesh->Sections[proxy.Section].MaterialSlot))
                draws.push_back({mesh,
                                 proxy.Transform,
                                 id,
                                 proxy.Section,
                                 cache ? cache->GetSkinningPalette(proxy.SkinPose, mesh) :
                                         RenderHardware::Handle<RenderHardware::Buffer>()});
        }
    }
    ScenePickHit Renderer::PickSceneSurface(RenderWorld& world, const RenderView& view, glm::uvec2 pixel)
    {
        auto cache = view.Assets;
        if (!cache && world.GetAssetManager())
            cache = AcquireAssetCache(*world.GetAssetManager());
        if (cache)
            world.SetAssetManager(&cache->GetAssetManager());
        world.Synchronize();
        const auto            proxies = world.CollectDraws();
        std::vector<MeshDraw> draws;
        for (size_t i = 0; i < proxies.size(); ++i)
        {
            const auto  resource = view.MeshResources.find(proxies[i].Mesh);
            const auto* mesh     = resource != view.MeshResources.end() ? resource->second.get() :
                                   cache                                ? cache->GetMesh(proxies[i].Mesh) :
                                                                          nullptr;
            if (mesh && proxies[i].Section < mesh->Sections.size() &&
                world.IsIsolated(view.Highlights,
                                 proxies[i].EntityID,
                                 proxies[i].InstanceRoot,
                                 mesh->Sections[proxies[i].Section].MaterialSlot))
                draws.push_back({mesh,
                                 proxies[i].Transform,
                                 uint32_t(i + 1),
                                 proxies[i].Section,
                                 cache ? cache->GetSkinningPalette(proxies[i].SkinPose, mesh) :
                                         RenderHardware::Handle<RenderHardware::Buffer>()});
        }
        const auto texts = world.CollectTexts();
        for (size_t i = 0; i < texts.size(); ++i)
            if (!texts[i].Style.Text.empty() && view.Highlights.Isolate.empty())
            {
                auto draw =
                    GetTextRenderer().BoundsDraw(texts[i], view.Scene.sceneCamera, uint32_t(proxies.size() + i + 1));
                draw.IgnoreDepth = texts[i].Style.AlwaysOnTop;
                draw.DoubleSided = texts[i].Style.DoubleSided;
                draws.push_back(draw);
            }
        std::stable_partition(draws.begin(), draws.end(), [](const auto& d) { return !d.IgnoreDepth; });
        const auto hit = m_Picker.PickSurface(draws,
                                              view.Scene.sceneCamera.camera.GetProjectionMatrix() *
                                                  view.Scene.sceneCamera.viewMatrix,
                                              view.Viewport,
                                              pixel);
        if (!hit.ID)
            return {};
        if (hit.ID > proxies.size())
        {
            auto index = hit.ID - proxies.size() - 1;
            if (index >= texts.size())
                return {};
            return {texts[index].EntityID, texts[index].InstanceRoot, hit.Position, {}, {}};
        }
        const auto& proxy = proxies[hit.ID - 1];
        return {proxy.EntityID, proxy.InstanceRoot, hit.Position, hit.MaterialSlot, hit.Section};
    }
} // namespace Kans

namespace Kans
{
    UIRenderer& Renderer::GetUIRenderer()
    {
        if (!m_UI)
            m_UI = std::make_unique<UIRenderer>(*this);
        return *m_UI;
    }
    TextRenderer& Renderer::GetTextRenderer()
    {
        if (!m_Text)
            m_Text = std::make_unique<TextRenderer>(*this);
        return *m_Text;
    }
    SpriteBatch& Renderer::GetSpriteBatch()
    {
        if (!m_Sprites)
        {
            m_Sprites = std::make_unique<SpriteBatch>();
            m_Sprites->Init(m_Device);
        }
        return *m_Sprites;
    }
} // namespace Kans
