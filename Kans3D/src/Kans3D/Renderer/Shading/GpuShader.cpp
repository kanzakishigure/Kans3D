#include "kspch.h"
#include "Kans3D/Renderer/Shading/GpuShader.h"
#include "Kans3D/RHI/Backend.h"
#include "Kans3D/Renderer/Particles/ParticleSprite.h"
#include "Kans3D/Renderer/Shading/Skinning.h"
namespace Kans
{
    GpuUniformBuffer::GpuUniformBuffer(size_t bytes, const void* data, std::shared_ptr<RenderHardware::Device> device)
    {
        m_Device = device ? std::move(device) : RenderHardware::GetSharedDevice();
        m_Buffer =
            m_Device->CreateBuffer({bytes, RenderHardware::BufferUsage::Uniform, RenderHardware::MemoryUsage::Upload});
        if (data)
            m_Buffer->Write(0, data, bytes);
    }
    GpuUniformBuffer::~GpuUniformBuffer()                                      = default;
    GpuUniformBuffer::GpuUniformBuffer(GpuUniformBuffer&&) noexcept            = default;
    GpuUniformBuffer& GpuUniformBuffer::operator=(GpuUniformBuffer&&) noexcept = default;
    void              GpuUniformBuffer::Update(const void* data, size_t bytes) { m_Buffer->Write(0, data, bytes); }
    namespace
    {
        bool ReadsSmoothNormals(const std::vector<ShaderStageReflection>& reflection)
        {
            for (const auto& stage : reflection)
                if (stage.Stage == ShaderProductStage::Vertex)
                    for (const auto& input : stage.Inputs)
                        if (input.Location == 7)
                            return true;
            return false;
        }
        AssetError Failure(std::string text) { return {AssetErrorCode::InvalidData, "Material Shader: " + text}; }
    } // namespace
    std::variant<GpuShader, AssetError> GpuShader::Create(const ShaderData&                       data,
                                                          std::shared_ptr<RenderHardware::Device> device)
    {
        if (!device)
        {
            try
            {
                device = RenderHardware::GetSharedDevice();
            }
            catch (const std::exception&)
            {
                return AssetError {AssetErrorCode::InvalidArgument, "Shader upload requires a current context"};
            }
        }
        if (uint32_t(data.Domain) > 1 || uint32_t(data.PostStage) > 1 ||
            (data.Domain == ShaderDomain::PostProcess && !data.Passes.empty()))
            return Failure("invalid domain/stage or post-process passes");
        auto inspected = InspectShaderProgram(data.Code);
        if (auto* error = std::get_if<AssetError>(&inspected))
            return *error;
        const auto& reflection = std::get<std::vector<ShaderStageReflection>>(inspected);
        auto        contract   = ValidateMaterialShaderInterface(reflection, data.Domain);
        if (auto* error = std::get_if<AssetError>(&contract))
            return *error;
        auto validPasses = ValidateMaterialShaderPasses(data.Code, data.Passes);
        if (auto* error = std::get_if<AssetError>(&validPasses))
            return *error;
        const auto resources = MaterialShaderResources(reflection);
        for (const auto& [name, role] : data.TextureFallbacks)
            if (std::none_of(resources.begin(), resources.end(), [&](const auto& resource) {
                    return resource.Kind == ShaderBindingKind::CombinedImageSampler && resource.Name == name;
                }))
                return Failure("texture fallback '" + name + "' does not name a material sampler");
        GpuShader shader;
        shader.UsesSmoothNormals = ReadsSmoothNormals(reflection);
        shader.OutputSpace       = data.OutputSpace;
        shader.Domain            = data.Domain;
        shader.PostStage         = data.PostStage;
        shader.m_Source          = std::make_shared<const ShaderData>(data);
        shader.m_Device          = std::move(device);
        try
        {
            shader.Program = shader.m_Device->CreateShader(data);
            for (const auto& pass : data.Passes)
            {
                ShaderData passData;
                passData.Code = pass.Code;
                passData.DebugName    = shader.Program->DebugName + "/Pass/" + pass.Name;
                passData.DebugSources = pass.DebugSources;
                shader.Passes.push_back({pass.Name, pass.Tag, pass.State, shader.m_Device->CreateShader(passData), {}});
                auto passReflection = InspectShaderProgram(pass.Code);
                if (auto* error = std::get_if<AssetError>(&passReflection))
                    return *error;
                shader.Passes.back().UsesSmoothNormals =
                    ReadsSmoothNormals(std::get<std::vector<ShaderStageReflection>>(passReflection));
            }
        }
        catch (const std::exception& e)
        {
            return Failure(e.what());
        }
        RenderHardware::BindGroupLayoutDesc engine, material;
        engine.Group   = 0;
        material.Group = 1;
        std::map<uint32_t, ShaderResourceBinding> engineBindings;
        for (const auto& stage : reflection)
            for (const auto& binding : stage.Bindings)
                if (binding.Set == 0)
                    engineBindings.emplace(binding.Binding, binding);
        for (const auto& [slot, binding] : engineBindings)
        {
            shader.EngineResources.push_back(binding);
            engine.Entries.push_back({slot,
                                      binding.Kind == ShaderBindingKind::UniformBuffer ?
                                          RenderHardware::BindingType::UniformBuffer :
                                          RenderHardware::BindingType::CombinedTextureSampler});
        }
        for (const auto& binding : resources)
            material.Entries.push_back({binding.Binding,
                                        binding.Kind == ShaderBindingKind::UniformBuffer ?
                                            RenderHardware::BindingType::UniformBuffer :
                                            RenderHardware::BindingType::CombinedTextureSampler});
        try
        {
            shader.EngineLayout   = shader.m_Device->CreateBindGroupLayout(engine);
            shader.MaterialLayout = shader.m_Device->CreateBindGroupLayout(material);
        }
        catch (const std::exception& e)
        {
            return Failure(e.what());
        }
        for (const auto& resource : resources)
        {
            if (resource.Count != 1)
                return Failure("descriptor arrays are not supported ('" + resource.Name + "')");
            if (resource.Kind == ShaderBindingKind::CombinedImageSampler)
            {
                const auto found = data.TextureFallbacks.find(resource.Name);
                const auto role  = found == data.TextureFallbacks.end() ? ShaderTextureFallback::White : found->second;
                if (uint32_t(role) > uint32_t(ShaderTextureFallback::Normal))
                    return Failure("invalid texture fallback");
                shader.m_SamplerFallbacks.emplace(resource.Binding, role);
            }
        }

        return shader;
    }
    bool GpuShader::PrepareParticleSprite() const
    {
        if (SpriteProgram)
            return true;
        if (!m_Source)
            return false;
        auto converted = MakeParticleSpriteShader(*m_Source);
        if (auto e = std::get_if<AssetError>(&converted))
        {
            CORE_ERROR("Particle sprite shader: {0}", e->Message);
            return false;
        }
        SpriteProgram = m_Device->CreateShader(std::get<ShaderData>(converted));
        SpriteLayout  = ParticleSpriteLayout(*m_Device);
        return true;
    }
    bool GpuShader::PrepareParticleMesh() const
    {
        if (MeshProgram)
            return true;
        if (!m_Source)
            return false;
        auto converted = MakeParticleSpriteShader(*m_Source, true);
        if (auto e = std::get_if<AssetError>(&converted))
        {
            CORE_ERROR("Particle mesh shader: {0}", e->Message);
            return false;
        }
        MeshProgram = m_Device->CreateShader(std::get<ShaderData>(converted));
        MeshLayout  = ParticleSpriteLayout(*m_Device);
        return true;
    }
    bool GpuShader::PrepareParticleRibbon() const
    {
        if (RibbonProgram)
            return true;
        if (!m_Source)
            return false;
        auto converted = MakeParticleSpriteShader(*m_Source, false, true);
        if (auto e = std::get_if<AssetError>(&converted))
        {
            CORE_ERROR("Particle mesh shader: {0}", e->Message);
            return false;
        }
        RibbonProgram = m_Device->CreateShader(std::get<ShaderData>(converted));
        RibbonLayout  = ParticleSpriteLayout(*m_Device);
        return true;
    }
    bool GpuShader::PrepareSkinning(bool smoothNormalFallback) const
    {
        auto& program = smoothNormalFallback ? SkinnedNormalFallbackProgram : SkinnedProgram;
        if (program)
            return true;
        if (!m_Source)
            return false;
        auto converted = MakeSkinnedShader(*m_Source, smoothNormalFallback);
        if (auto* error = std::get_if<AssetError>(&converted))
        {
            CORE_ERROR("Skinning shader: {0}", error->Message);
            return false;
        }
        auto& data = std::get<ShaderData>(converted);
        program    = m_Device->CreateShader(data);
        if (!SkinLayout)
            SkinLayout = SkinningLayout(*m_Device);
        for (size_t i = 0; i < Passes.size(); ++i)
        {
            ShaderData pass;
            pass.Code = data.Passes[i].Code;
            pass.DebugName    = program->DebugName + "/Pass/" + data.Passes[i].Name;
            pass.DebugSources = data.Passes[i].DebugSources;
            auto& passProgram =
                smoothNormalFallback ? Passes[i].SkinnedNormalFallbackProgram : Passes[i].SkinnedProgram;
            passProgram = m_Device->CreateShader(pass);
        }
        return true;
    }
    GpuShader::~GpuShader()                               = default;
    GpuShader::GpuShader(GpuShader&&) noexcept            = default;
    GpuShader& GpuShader::operator=(GpuShader&&) noexcept = default;
} // namespace Kans

namespace Kans
{
    GpuShader GpuShader::Snapshot() const
    {
        GpuShader copy;
        copy.m_Device                     = m_Device;
        copy.Program                      = Program;
        copy.RibbonProgram                = RibbonProgram;
        copy.RibbonDepthProgram           = RibbonDepthProgram;
        copy.RibbonLayout                 = RibbonLayout;
        copy.MeshDepthProgram             = MeshDepthProgram;
        copy.MeshProgram                  = MeshProgram;
        copy.MeshLayout                   = MeshLayout;
        copy.SpriteProgram                = SpriteProgram;
        copy.SpriteLayout                 = SpriteLayout;
        copy.m_Source                     = m_Source;
        copy.SkinnedNormalFallbackProgram = SkinnedNormalFallbackProgram;
        copy.SkinnedProgram               = SkinnedProgram;
        copy.SkinLayout                   = SkinLayout;
        copy.EngineLayout                 = EngineLayout;
        copy.MaterialLayout               = MaterialLayout;
        copy.EngineResources              = EngineResources;
        copy.Pipelines                    = Pipelines;
        copy.Passes                       = Passes;
        copy.UsesSmoothNormals            = UsesSmoothNormals;
        copy.OutputSpace                  = OutputSpace;
        copy.Domain                       = Domain;
        copy.PostStage                    = PostStage;
        copy.m_SamplerFallbacks           = m_SamplerFallbacks;
        return copy;
    }
} // namespace Kans
