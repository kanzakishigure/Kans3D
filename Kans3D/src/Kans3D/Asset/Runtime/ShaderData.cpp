#include "kspch.h"
#include "ShaderData.h"
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <spirv-tools/libspirv.hpp>
#include <spirv_cross/spirv_cross.hpp>

namespace Kans
{
    namespace
    {
        bool Identifier(const std::string& value)
        {
            if (value.empty() || value.size() > 255)
                return false;
            for (size_t i = 0; i < value.size(); ++i)
            {
                const auto c = value[i];
                if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i && c >= '0' && c <= '9')))
                    return false;
            }
            return true;
        }
        bool SameLayout(const std::vector<ShaderUniformMember>& a, const std::vector<ShaderUniformMember>& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                const auto& x = a[i];
                const auto& y = b[i];
                if (x.Offset != y.Offset || x.Bytes != y.Bytes || x.Type != y.Type || x.Components != y.Components ||
                    x.Columns != y.Columns || x.ArrayCount != y.ArrayCount || x.ArrayStride != y.ArrayStride ||
                    x.MatrixStride != y.MatrixStride || x.RowMajor != y.RowMajor || !SameLayout(x.Members, y.Members))
                    return false;
            }
            return true;
        }
        std::variant<std::vector<ShaderUniformMember>, AssetError>
        UniformMembers(spirv_cross::Compiler&       compiler,
                       const spirv_cross::SPIRType& structure,
                       size_t                       depth,
                       size_t&                      budget)
        {
            if (depth > 16 || structure.basetype != spirv_cross::SPIRType::Struct)
                return AssetError {AssetErrorCode::InvalidData, "Shader uniform nesting exceeds limits"};
            std::vector<ShaderUniformMember> result;
            const auto                       blockBytes = compiler.get_declared_struct_size(structure);
            for (uint32_t index = 0; index < structure.member_types.size(); ++index)
            {
                if (!budget)
                    return AssetError {AssetErrorCode::InvalidData, "Shader uniform member count exceeds limits"};
                --budget;
                const auto&         type = compiler.get_type(structure.member_types[index]);
                ShaderUniformMember member;
                member.Name   = compiler.get_member_name(structure.self, index);
                member.Offset = compiler.type_struct_member_offset(structure, index);
                member.Bytes  = compiler.get_declared_struct_member_size(structure, index);
                if (member.Name.size() > 4096 || !member.Bytes || member.Offset > blockBytes ||
                    member.Bytes > blockBytes - member.Offset)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader uniform member span"};
                if (type.array.size() > 1)
                    return AssetError {AssetErrorCode::NotImplemented,
                                       "Multidimensional uniform arrays are unsupported"};
                if (!type.array.empty())
                {
                    if (!type.array_size_literal[0] || !type.array[0] || type.array[0] > 65536)
                        return AssetError {AssetErrorCode::NotImplemented,
                                           "Unbounded/specialized uniform arrays are unsupported"};
                    member.ArrayCount  = type.array[0];
                    member.ArrayStride = compiler.type_struct_member_array_stride(structure, index);
                    if (!member.ArrayStride || member.ArrayStride > member.Bytes / member.ArrayCount)
                        return AssetError {AssetErrorCode::InvalidData, "Invalid Shader uniform array stride"};
                }
                if (type.basetype == spirv_cross::SPIRType::Struct)
                {
                    member.Type   = ShaderUniformType::Struct;
                    auto children = UniformMembers(compiler, type, depth + 1, budget);
                    if (const auto* error = std::get_if<AssetError>(&children))
                        return *error;
                    member.Members = std::get<std::vector<ShaderUniformMember>>(std::move(children));
                }
                else
                {
                    if ((type.width != 32 && type.basetype != spirv_cross::SPIRType::Boolean) || !type.vecsize ||
                        type.vecsize > 4 || !type.columns || type.columns > 4)
                        return AssetError {AssetErrorCode::NotImplemented, "Uniform scalar shape is unsupported"};
                    if (type.basetype == spirv_cross::SPIRType::Float)
                        member.Type = ShaderUniformType::Float;
                    else if (type.basetype == spirv_cross::SPIRType::Int)
                        member.Type = ShaderUniformType::Signed;
                    else if (type.basetype == spirv_cross::SPIRType::UInt)
                        member.Type = ShaderUniformType::Unsigned;
                    else if (type.basetype == spirv_cross::SPIRType::Boolean)
                        member.Type = ShaderUniformType::Boolean;
                    else
                        return AssetError {AssetErrorCode::NotImplemented, "Uniform scalar type is unsupported"};
                    member.Components = type.vecsize;
                    member.Columns    = type.columns;
                    if (type.columns > 1)
                    {
                        member.MatrixStride = compiler.type_struct_member_matrix_stride(structure, index);
                        member.RowMajor =
                            compiler.has_member_decoration(structure.self, index, spv::DecorationRowMajor);
                        if (!member.MatrixStride)
                            return AssetError {AssetErrorCode::InvalidData, "Invalid uniform matrix stride"};
                    }
                }
                result.push_back(std::move(member));
            }
            return result;
        }
    } // namespace
    std::variant<std::vector<ShaderStageReflection>, AssetError> InspectShaderProgram(const ShaderProgramCode& code)
    {
        if (code.Modules.empty() || code.Modules.size() > 3)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader module count"};
        std::set<ShaderProductStage>       stages;
        std::vector<ShaderStageReflection> result;
        size_t                             memberBudget = 4096;
        try
        {
            for (const auto& module : code.Modules)
            {
                if ((module.Stage != ShaderProductStage::Vertex && module.Stage != ShaderProductStage::Fragment &&
                     module.Stage != ShaderProductStage::Compute) ||
                    !stages.insert(module.Stage).second || !Identifier(module.EntryPoint) || module.Words.size() < 5 ||
                    module.Words.size() > 1048576 || module.Words[0] != 0x07230203 || !module.Words[3] ||
                    module.Words[3] > 65536)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader stage/entry/bytecode budget"};
                spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
                if (!validator.IsValid())
                    return AssetError {AssetErrorCode::IoError, "SPIR-V validator is unavailable"};
                std::array<char, 2048> diagnostic {};
                size_t                 diagnosticSize = 0;
                validator.SetMessageConsumer(
                    [&](spv_message_level_t, const char*, const spv_position_t&, const char* message) noexcept {
                        if (!diagnosticSize && message)
                            while (diagnosticSize < diagnostic.size() && message[diagnosticSize])
                            {
                                diagnostic[diagnosticSize] = message[diagnosticSize];
                                ++diagnosticSize;
                            }
                    });
                spvtools::ValidatorOptions limits;
                limits.SetUniversalLimit(spv_validator_limit_max_struct_depth, 32);
                if (!validator.Validate(module.Words.data(), module.Words.size(), limits))
                    return AssetError {AssetErrorCode::InvalidData,
                                       "Invalid SPIR-V: " + std::string(diagnostic.data(), diagnosticSize)};
                for (size_t cursor = 5; cursor < module.Words.size(); cursor += module.Words[cursor] >> 16)
                {
                    const auto opcode = module.Words[cursor] & 65535;
                    // ImageQuery: textureSize / textureQueryLod.
                    if ((opcode == spv::OpCapability && module.Words[cursor + 1] != spv::CapabilityShader &&
                         module.Words[cursor + 1] != spv::CapabilityMatrix &&
                         module.Words[cursor + 1] != spv::CapabilitySampleRateShading &&
                         module.Words[cursor + 1] != spv::CapabilityImageQuery &&
                         module.Words[cursor + 1] != spv::CapabilityStorageImageExtendedFormats) ||
                        opcode == spv::OpExtension)
                        return AssetError {AssetErrorCode::NotImplemented,
                                           "Shader capability/extension is unsupported"};
                }
                spirv_cross::Compiler compiler(module.Words);
                const auto            entries = compiler.get_entry_points_and_stages();
                const auto            model   = static_cast<spv::ExecutionModel>(module.Stage);
                if (entries.size() != 1 || entries[0].name != module.EntryPoint || entries[0].execution_model != model)
                    return AssetError {AssetErrorCode::InvalidData, "Shader entry point does not match its stage"};
                compiler.set_entry_point(module.EntryPoint, model);
                const auto resources = compiler.get_shader_resources();
                if (!resources.subpass_inputs.empty() || !resources.atomic_counters.empty())
                    return AssetError {AssetErrorCode::NotImplemented,
                                       "Shader reflection resource category is unsupported"};
                if (resources.uniform_buffers.size() + resources.sampled_images.size() +
                            resources.storage_buffers.size() + resources.storage_images.size() +
                            resources.separate_images.size() + resources.separate_samplers.size() >
                        4096 ||
                    resources.push_constant_buffers.size() > 1 || resources.stage_inputs.size() > 256 ||
                    resources.stage_outputs.size() > 256)
                    return AssetError {AssetErrorCode::InvalidData, "Shader reflection resource budget exceeded"};
                ShaderStageReflection                   reflection {module.Stage, {}, {}, {}};
                if (module.Stage == ShaderProductStage::Compute)
                    for (uint32_t axis = 0; axis < 3; ++axis)
                        reflection.WorkgroupSize[axis] =
                            compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, axis);
                for (const auto& push : resources.push_constant_buffers)
                {
                    const auto bytes = compiler.get_declared_struct_size(compiler.get_type(push.base_type_id));
                    if (bytes > 128)
                        return AssetError {AssetErrorCode::InvalidData, "Shader push constants exceed 128 bytes"};
                    reflection.PushConstantBytes = uint32_t(bytes);
                }
                std::set<std::pair<uint32_t, uint32_t>> bindings;
                const auto add = [&](const auto& resource, ShaderBindingKind kind) -> std::optional<AssetError> {
                    ShaderResourceBinding binding {kind};
                    if (!compiler.has_decoration(resource.id, spv::DecorationDescriptorSet) ||
                        !compiler.has_decoration(resource.id, spv::DecorationBinding))
                        return AssetError {AssetErrorCode::InvalidData, "Shader resource has no descriptor identity"};
                    binding.Set     = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                    binding.Binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                    binding.Name    = resource.name;
                    if (binding.Set > 31 || binding.Binding > 65535 || binding.Name.size() > 4096 ||
                        !bindings.emplace(binding.Set, binding.Binding).second)
                        return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate Shader descriptor"};
                    const auto& type = compiler.get_type(resource.type_id);
                    for (size_t i = 0; i < type.array.size(); ++i)
                    {
                        if (!type.array_size_literal[i] || !type.array[i] || type.array[i] > 4096 / binding.Count)
                            return AssetError {AssetErrorCode::NotImplemented,
                                               "Unbounded/specialized Shader descriptor array"};
                        binding.Count *= type.array[i];
                    }
                    if (kind == ShaderBindingKind::UniformBuffer)
                    {
                        binding.BlockBytes =
                            compiler.get_declared_struct_size(compiler.get_type(resource.base_type_id));
                        if (!binding.BlockBytes || binding.BlockBytes > 512ull * 1024 * 1024)
                            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader uniform block size"};
                        auto members =
                            UniformMembers(compiler, compiler.get_type(resource.base_type_id), 0, memberBudget);
                        if (const auto* error = std::get_if<AssetError>(&members))
                            return *error;
                        binding.Members = std::get<std::vector<ShaderUniformMember>>(std::move(members));
                    }
                    else if (kind == ShaderBindingKind::StorageBuffer)
                    {
                        if (!type.array.empty())
                            return AssetError {AssetErrorCode::NotImplemented,
                                               "Storage binding arrays are unsupported"};
                        binding.BlockBytes =
                            compiler.get_declared_struct_size(compiler.get_type(resource.base_type_id));
                        if (compiler.get_buffer_block_flags(resource.id).get(spv::DecorationNonWritable))
                            binding.Kind = ShaderBindingKind::ReadOnlyStorageBuffer;
                    }
                    else if (kind == ShaderBindingKind::Sampler)
                    {
                    }
                    else if ((type.image.dim != spv::Dim2D && type.image.dim != spv::DimCube &&
                              type.image.dim != spv::Dim3D) ||
                             type.image.arrayed ||
                             (compiler.get_type(type.image.type).basetype != spirv_cross::SPIRType::Float &&
                              compiler.get_type(type.image.type).basetype != spirv_cross::SPIRType::UInt))
                        return AssetError {AssetErrorCode::NotImplemented, "Shader sampled image shape is unsupported"};
                    else
                        binding.Dimension = type.image.dim == spv::Dim3D   ? ShaderImageDimension::Texture3D :
                                            type.image.dim == spv::DimCube ? ShaderImageDimension::Cube :
                                                                             ShaderImageDimension::Texture2D;
                    reflection.Bindings.push_back(std::move(binding));
                    return {};
                };
                for (const auto& resource : resources.uniform_buffers)
                    if (auto error = add(resource, ShaderBindingKind::UniformBuffer))
                        return *error;
                for (const auto& resource : resources.sampled_images)
                    if (auto error = add(resource, ShaderBindingKind::CombinedImageSampler))
                        return *error;
                for (const auto& resource : resources.storage_buffers)
                    if (auto error = add(resource, ShaderBindingKind::StorageBuffer))
                        return *error;
                for (const auto& resource : resources.storage_images)
                    if (auto error = add(resource, ShaderBindingKind::StorageImage))
                        return *error;
                for (const auto& resource : resources.separate_images)
                    if (auto error = add(resource, ShaderBindingKind::SampledImage))
                        return *error;
                for (const auto& resource : resources.separate_samplers)
                    if (auto error = add(resource, ShaderBindingKind::Sampler))
                        return *error;
                const auto interfaces = [&](const auto& source, auto& destination) -> std::optional<AssetError> {
                    std::set<uint32_t> occupied;
                    for (const auto& resource : source)
                    {
                        const auto& type = compiler.get_type(resource.type_id);
                        if (compiler.has_decoration(resource.id, spv::DecorationBuiltIn))
                            continue;
                        if (!compiler.has_decoration(resource.id, spv::DecorationLocation) ||
                            compiler.has_decoration(resource.id, spv::DecorationComponent) ||
                            compiler.has_decoration(resource.id, spv::DecorationIndex) || !type.array.empty() ||
                            type.width != 32 || !type.vecsize || type.vecsize > 4 || !type.columns ||
                            type.columns > 4 ||
                            (type.basetype != spirv_cross::SPIRType::Float &&
                             type.basetype != spirv_cross::SPIRType::Int &&
                             type.basetype != spirv_cross::SPIRType::UInt))
                            return AssetError {AssetErrorCode::NotImplemented,
                                               "Shader stage interface shape is unsupported"};
                        ShaderInterfaceVariable variable;
                        variable.Location   = compiler.get_decoration(resource.id, spv::DecorationLocation);
                        variable.Components = type.vecsize;
                        variable.Columns    = type.columns;
                        variable.Name       = resource.name;
                        variable.Scalar = type.basetype == spirv_cross::SPIRType::Float ? ShaderInterfaceScalar::Float :
                                          type.basetype == spirv_cross::SPIRType::Int ? ShaderInterfaceScalar::Signed :
                                                                                        ShaderInterfaceScalar::Unsigned;
                        if (variable.Location > 65535 || variable.Columns - 1 > 65535 - variable.Location ||
                            variable.Name.size() > 4096)
                            return AssetError {AssetErrorCode::InvalidData, "Shader interface budget exceeded"};
                        for (uint32_t i = 0; i < variable.Columns; ++i)
                            if (!occupied.insert(variable.Location + i).second)
                                return AssetError {AssetErrorCode::InvalidData,
                                                   "Overlapping Shader interface locations"};
                        destination.push_back(std::move(variable));
                    }
                    std::sort(destination.begin(), destination.end(), [](const auto& a, const auto& b) {
                        return a.Location < b.Location;
                    });
                    return {};
                };
                if (auto error = interfaces(resources.stage_inputs, reflection.Inputs))
                    return *error;
                if (auto error = interfaces(resources.stage_outputs, reflection.Outputs))
                    return *error;
                std::sort(reflection.Bindings.begin(), reflection.Bindings.end(), [](const auto& a, const auto& b) {
                    return std::tie(a.Set, a.Binding) < std::tie(b.Set, b.Binding);
                });
                result.push_back(std::move(reflection));
            }
            if (stages.count(ShaderProductStage::Compute) && stages.size() != 1)
                return AssetError {AssetErrorCode::InvalidData, "Compute Shader cannot mix with graphics stages"};
            std::map<std::pair<uint32_t, uint32_t>, ShaderResourceBinding> programBindings;
            const ShaderStageReflection*                                   vertex   = nullptr;
            const ShaderStageReflection*                                   fragment = nullptr;
            for (const auto& reflection : result)
            {
                if (reflection.Stage == ShaderProductStage::Vertex)
                    vertex = &reflection;
                if (reflection.Stage == ShaderProductStage::Fragment)
                    fragment = &reflection;
                for (const auto& binding : reflection.Bindings)
                {
                    auto inserted = programBindings.emplace(std::make_pair(binding.Set, binding.Binding), binding);
                    const auto& previous = inserted.first->second;
                    if (!inserted.second &&
                        (previous.Kind != binding.Kind || previous.Count != binding.Count ||
                         previous.Dimension != binding.Dimension ||
                         previous.BlockBytes != binding.BlockBytes || !SameLayout(previous.Members, binding.Members)))
                        return AssetError {AssetErrorCode::InvalidData, "Shader descriptors disagree between stages"};
                }
            }
            if (vertex && fragment)
                for (const auto& input : fragment->Inputs)
                {
                    const auto found =
                        std::find_if(vertex->Outputs.begin(), vertex->Outputs.end(), [&](const auto& output) {
                            return output.Location == input.Location;
                        });
                    if (found == vertex->Outputs.end() || found->Scalar != input.Scalar ||
                        found->Components != input.Components || found->Columns != input.Columns)
                        return AssetError {AssetErrorCode::InvalidData, "Shader stage interfaces disagree"};
                }
            return result;
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData,
                               "Shader reflection failed: " + std::string(error.what()).substr(0, 2048)};
        }
        catch (...)
        {
            return AssetError {AssetErrorCode::InvalidData, "Shader reflection failed"};
        }
    }
} // namespace Kans
