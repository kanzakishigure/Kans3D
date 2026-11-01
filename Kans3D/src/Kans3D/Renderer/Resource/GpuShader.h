#pragma once
#include <map>
#include <variant>
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "Kans3D/RHI/RHI.h"

namespace Kans
{
    // RHI upload-buffer owner.
    class GpuUniformBuffer
    {
    public:
        explicit GpuUniformBuffer(size_t                                  bytes,
                                  const void*                             data   = nullptr,
                                  std::shared_ptr<RenderHardware::Device> device = {});
        ~GpuUniformBuffer();
        GpuUniformBuffer(GpuUniformBuffer&& other) noexcept;
        GpuUniformBuffer& operator=(GpuUniformBuffer&& other) noexcept;
        GpuUniformBuffer(const GpuUniformBuffer&)            = delete;
        GpuUniformBuffer& operator=(const GpuUniformBuffer&) = delete;

        GpuUniformBuffer Snapshot() const { return GpuUniformBuffer(m_Device, m_Buffer); }
        void             Update(const void* data, size_t bytes);
        const RenderHardware::Handle<RenderHardware::Buffer>& Resource() const { return m_Buffer; }

    private:
        GpuUniformBuffer(std::shared_ptr<RenderHardware::Device>        device,
                         RenderHardware::Handle<RenderHardware::Buffer> buffer) :
            m_Device(std::move(device)), m_Buffer(std::move(buffer))
        {}
        std::shared_ptr<RenderHardware::Device>        m_Device;
        RenderHardware::Handle<RenderHardware::Buffer> m_Buffer;
    };

    struct GpuShaderPass
    {
        std::string                                                                  Name, Tag;
        ShaderPassState                                                              State;
        RenderHardware::Handle<RenderHardware::Shader>                               Program;
        mutable RenderHardware::Handle<RenderHardware::Shader>                       SkinnedProgram;
        mutable std::map<uint64_t, RenderHardware::Handle<RenderHardware::Pipeline>> Pipelines;
        mutable RenderHardware::Handle<RenderHardware::Shader>                       SkinnedNormalFallbackProgram;
        bool                                                                         UsesSmoothNormals = false;
    };
    // Validated material product backed by an RHI Shader and cached state pipelines.
    class GpuShader
    {
        std::shared_ptr<RenderHardware::Device> m_Device;

    public:
        // Validates the material contract; the owning device must be current.
        // Omitting the device is a transitional current-context construction path.
        [[nodiscard]] static std::variant<GpuShader, AssetError>
        Create(const ShaderData& data, std::shared_ptr<RenderHardware::Device> device = {});

        GpuShader                                                       Snapshot() const;
        std::shared_ptr<const ShaderData>                               Source() const { return m_Source; }
        bool                                                            PrepareParticleSprite() const;
        bool                                                            PrepareParticleMesh() const;
        bool                                                            PrepareParticleRibbon() const;
        mutable RenderHardware::Handle<RenderHardware::Shader>          RibbonProgram, RibbonDepthProgram;
        mutable RenderHardware::Handle<RenderHardware::BindGroupLayout> RibbonLayout;
        mutable RenderHardware::Handle<RenderHardware::Shader>          MeshProgram, MeshDepthProgram;
        mutable RenderHardware::Handle<RenderHardware::BindGroupLayout> MeshLayout;
        mutable RenderHardware::Handle<RenderHardware::Shader>          SpriteProgram;
        mutable RenderHardware::Handle<RenderHardware::BindGroupLayout> SpriteLayout;
        bool PrepareSkinning(bool smoothNormalFallback = false) const;
        ~GpuShader();
        GpuShader(GpuShader&& other) noexcept;
        GpuShader& operator=(GpuShader&& other) noexcept;
        GpuShader(const GpuShader&)            = delete;
        GpuShader& operator=(const GpuShader&) = delete;

        RenderHardware::Handle<RenderHardware::Shader>                               Program;
        RenderHardware::Handle<RenderHardware::BindGroupLayout>                      EngineLayout, MaterialLayout;
        std::vector<ShaderResourceBinding>                                           EngineResources;
        mutable std::map<uint64_t, RenderHardware::Handle<RenderHardware::Pipeline>> Pipelines;

        mutable RenderHardware::Handle<RenderHardware::Shader>          SkinnedProgram;
        mutable RenderHardware::Handle<RenderHardware::BindGroupLayout> SkinLayout;
        std::vector<GpuShaderPass>                                      Passes;
        mutable RenderHardware::Handle<RenderHardware::Shader>          SkinnedNormalFallbackProgram;
        bool                                                            UsesSmoothNormals = false;
        ShaderDomain                                                    Domain            = ShaderDomain::Surface;
        PostProcessStage                                                PostStage   = PostProcessStage::BeforeToneMap;
        ShaderOutputSpace                                               OutputSpace = ShaderOutputSpace::Display;

        const std::map<uint32_t, ShaderTextureFallback>& MaterialSamplerFallbacks() const { return m_SamplerFallbacks; }

    private:
        GpuShader() = default;
        std::shared_ptr<const ShaderData> m_Source;

        std::map<uint32_t, ShaderTextureFallback> m_SamplerFallbacks;
    };
} // namespace Kans
