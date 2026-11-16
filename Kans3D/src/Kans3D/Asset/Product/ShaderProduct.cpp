#include "kspch.h"
#include "ShaderProduct.h"
#include <cmath>
#include <cstring>
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
namespace Kans
{
    namespace
    {
        // v2 defaults, v3 hints, v4 fallbacks, v5 groups, v6 passes, v7 output space.
        // v8 appends post-process domain/stage; v9 appends a nonrecursive render-texture fragment variant.
        // Products without either feature retain the v7 layout. Readers retain v1-v8.
        constexpr uint32_t Magic = 0x4448534b, Version = 9, TargetVulkan12 = 1;
        constexpr size_t   MaximumDefaults = 4096, MaximumDefaultValues = 4096;
        constexpr size_t   MaximumBytes = 16 * 1024 * 1024;
        void               Write(std::vector<std::byte>& bytes, uint32_t word)
        {
            for (unsigned i = 0; i < 4; ++i)
                bytes.push_back(static_cast<std::byte>((word >> (8 * i)) & 255));
        }
        uint32_t Read(const std::byte*& cursor)
        {
            uint32_t word = 0;
            for (unsigned i = 0; i < 4; ++i)
                word |= uint32_t(std::to_integer<uint8_t>(*cursor++)) << (8 * i);
            return word;
        }
        void WriteDouble(std::vector<std::byte>& bytes, double value)
        {
            uint64_t bits = 0;
            std::memcpy(&bits, &value, 8);
            Write(bytes, uint32_t(bits));
            Write(bytes, uint32_t(bits >> 32));
        }
        double ReadDouble(const std::byte*& cursor)
        {
            const uint64_t low = Read(cursor), high = Read(cursor);
            const uint64_t bits  = low | (high << 32);
            double         value = 0;
            std::memcpy(&value, &bits, 8);
            return value;
        }
        bool ValidDefault(const std::string& name, const std::vector<double>& values)
        {
            return !name.empty() && name.size() <= 256 && !values.empty() && values.size() <= MaximumDefaultValues &&
                   std::all_of(name.begin(), name.end(), [](unsigned char c) { return c > 32 && c != 127; }) &&
                   std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
        }
        bool ValidHint(const std::string& name, const ShaderParameterHint& hint)
        {
            return ValidDefault(name, {hint.Min, hint.Max}) &&
                   (hint.Type == ShaderParameterHint::Kind::Toggle || hint.Type == ShaderParameterHint::Kind::Color ||
                    (hint.Type == ShaderParameterHint::Kind::Range && hint.Min < hint.Max));
        }
    } // namespace
    std::variant<std::vector<std::byte>, AssetError>
    EncodeShaderProduct(const ShaderProgramCode&                   code,
                        const ShaderParameterDefaults&             defaults,
                        const ShaderParameterHints&                hints,
                        const ShaderTextureFallbacks&              fallbacks,
                        const std::vector<MaterialParameterGroup>& groups,
                        const std::vector<ShaderPass>&             passes,
                        ShaderOutputSpace                          outputSpace,
                        ShaderDomain                               domain,
                        PostProcessStage                           postStage,
                        const ShaderProgramCode&                   renderTexture)
    {
        if (uint32_t(domain) > 1 || uint32_t(postStage) > 1 || (domain == ShaderDomain::PostProcess && !passes.empty()))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Shader domain/stage"};
        if (uint32_t(outputSpace) > 1)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Shader output space"};
        auto variant = ValidateRenderTextureVariant(code, renderTexture);
        if (auto* error = std::get_if<AssetError>(&variant))
            return *error;
        auto validPasses = ValidateMaterialShaderPasses(code, passes);
        if (auto* error = std::get_if<AssetError>(&validPasses))
            return *error;
        if (hints.size() > MaximumDefaults)
            return AssetError {AssetErrorCode::InvalidArgument, "Too many Shader parameter hints"};
        for (const auto& [name, hint] : hints)
            if (!ValidHint(name, hint))
                return AssetError {AssetErrorCode::InvalidArgument, "Invalid Shader parameter hint '" + name + "'"};
        auto inspected = InspectShaderProgram(code);
        if (auto* error = std::get_if<AssetError>(&inspected))
            return *error;
        if (domain == ShaderDomain::PostProcess)
        {
            auto valid =
                ValidateMaterialShaderInterface(std::get<std::vector<ShaderStageReflection>>(inspected), domain);
            if (auto* error = std::get_if<AssetError>(&valid))
                return *error;
        }
        if (defaults.size() > MaximumDefaults)
            return AssetError {AssetErrorCode::InvalidArgument, "Too many Shader parameter defaults"};
        for (const auto& [name, values] : defaults)
            if (!ValidDefault(name, values))
                return AssetError {AssetErrorCode::InvalidArgument, "Invalid Shader parameter default '" + name + "'"};
        std::vector<std::byte> bytes;
        Write(bytes, Magic);
        Write(bytes, !renderTexture.Modules.empty() ? Version : domain == ShaderDomain::PostProcess ? 8 : 7);
        Write(bytes, uint32_t(AssetType::Shader));
        Write(bytes, TargetVulkan12);
        Write(bytes, uint32_t(code.Modules.size()));
        Write(bytes, 0);
        for (const auto& module : code.Modules)
        {
            Write(bytes, uint32_t(module.Stage));
            Write(bytes, uint32_t(module.EntryPoint.size()));
            Write(bytes, uint32_t(module.Words.size()));
            Write(bytes, 0);
            for (auto c : module.EntryPoint)
                bytes.push_back(static_cast<std::byte>(c));
            for (auto word : module.Words)
                Write(bytes, word);
        }
        Write(bytes, uint32_t(defaults.size()));
        for (const auto& [name, values] : defaults)
        {
            Write(bytes, uint32_t(name.size()));
            Write(bytes, uint32_t(values.size()));
            for (auto c : name)
                bytes.push_back(static_cast<std::byte>(c));
            for (double value : values)
                WriteDouble(bytes, value);
        }
        Write(bytes, uint32_t(hints.size()));
        for (const auto& [name, hint] : hints)
        {
            Write(bytes, uint32_t(name.size()));
            Write(bytes, uint32_t(hint.Type));
            for (auto c : name)
                bytes.push_back(static_cast<std::byte>(c));
            WriteDouble(bytes, hint.Min);
            WriteDouble(bytes, hint.Max);
        }
        if (fallbacks.size() > MaximumDefaults)
            return AssetError {AssetErrorCode::InvalidArgument, "Too many Shader texture fallbacks"};
        for (const auto& [name, role] : fallbacks)
            if (!ValidDefault(name, {0}) || uint32_t(role) > uint32_t(ShaderTextureFallback::Normal))
                return AssetError {AssetErrorCode::InvalidArgument, "Invalid Shader texture fallback"};
        Write(bytes, uint32_t(fallbacks.size()));
        for (const auto& [name, role] : fallbacks)
        {
            Write(bytes, uint32_t(name.size()));
            Write(bytes, uint32_t(role));
            for (auto c : name)
                bytes.push_back(static_cast<std::byte>(c));
        }
        auto validGroups =
            ValidateMaterialParameterGroups(groups, std::get<std::vector<ShaderStageReflection>>(inspected));
        if (auto* error = std::get_if<AssetError>(&validGroups))
            return *error;
        const auto writeString = [&](const std::string& text) {
            Write(bytes, uint32_t(text.size()));
            for (auto c : text)
                bytes.push_back(static_cast<std::byte>(c));
        };
        Write(bytes, uint32_t(groups.size()));
        for (const auto& group : groups)
        {
            writeString(group.Name);
            Write(bytes, uint32_t(group.Members.size()));
            for (const auto& member : group.Members)
                writeString(member);
        }
        Write(bytes, uint32_t(passes.size()));
        for (const auto& pass : passes)
        {
            writeString(pass.Name);
            writeString(pass.Tag);
            const auto& state = pass.State;
            for (uint32_t word : {uint32_t(state.Cull),
                                  uint32_t(state.DepthTest),
                                  uint32_t(state.DepthCompare),
                                  uint32_t(state.DepthWrite),
                                  uint32_t(state.Blend),
                                  uint32_t(state.SourceColor),
                                  uint32_t(state.DestinationColor),
                                  uint32_t(state.SourceAlpha),
                                  uint32_t(state.DestinationAlpha),
                                  state.ColorWriteMask})
                Write(bytes, word);
            auto encoded = EncodeShaderProduct(pass.Code);
            if (auto* error = std::get_if<AssetError>(&encoded))
                return *error;
            auto payload = std::get<std::vector<std::byte>>(std::move(encoded));
            // A bounded v5 code-only record: forbids recursive pass records on read.
            payload.resize(payload.size() - 8);
            payload[4] = std::byte {5};
            Write(bytes, uint32_t(payload.size()));
            bytes.insert(bytes.end(), payload.begin(), payload.end());
        }
        Write(bytes, uint32_t(outputSpace));
        if (domain == ShaderDomain::PostProcess || !renderTexture.Modules.empty())
        {
            Write(bytes, uint32_t(domain));
            Write(bytes, uint32_t(postStage));
        }
        if (!renderTexture.Modules.empty())
        {
            auto encoded = EncodeShaderProduct(renderTexture);
            if (auto* error = std::get_if<AssetError>(&encoded))
                return *error;
            const auto& payload = std::get<std::vector<std::byte>>(encoded);
            Write(bytes, uint32_t(payload.size()));
            bytes.insert(bytes.end(), payload.begin(), payload.end());
        }
        if (bytes.size() > MaximumBytes)
            return AssetError {AssetErrorCode::InvalidArgument, "Shader Product exceeds byte budget"};
        return bytes;
    }
    std::variant<ShaderData, AssetError> DecodeShaderProduct(const std::byte* bytes, size_t size)
    {
        if (!bytes || size < 24 || size > MaximumBytes)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader Product byte length"};
        const auto* cursor = bytes;
        if (Read(cursor) != Magic)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader Product magic"};
        const auto version = Read(cursor);
        // Nested variants cannot recurse: their record must be the ordinary v7 layout.
        if (version < 1 || version > Version)
            return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported Shader Product version"};
        if (Read(cursor) != uint32_t(AssetType::Shader) || Read(cursor) != TargetVulkan12)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader Product type/target"};
        const auto count = Read(cursor);
        if (!count || count > 3 || Read(cursor) != 0)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Shader Product module count/flags"};
        ShaderData result;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (size_t(cursor - bytes) > size - 16)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Shader module header"};
            ShaderModuleCode module;
            module.Stage              = static_cast<ShaderProductStage>(Read(cursor));
            const auto     entryBytes = Read(cursor), words = Read(cursor), flags = Read(cursor);
            const uint64_t payload = uint64_t(entryBytes) + uint64_t(words) * 4;
            if (!entryBytes || entryBytes > 255 || words < 5 || words > 1048576 || flags ||
                payload > size - size_t(cursor - bytes))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Shader module payload"};
            module.EntryPoint.assign(reinterpret_cast<const char*>(cursor), entryBytes);
            cursor += entryBytes;
            module.Words.reserve(words);
            for (uint32_t j = 0; j < words; ++j)
                module.Words.push_back(Read(cursor));
            result.Code.Modules.push_back(std::move(module));
        }
        if (version >= 2)
        {
            const auto remaining = [&] { return size - size_t(cursor - bytes); };
            if (remaining() < 4)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Shader parameter defaults"};
            const auto defaults = Read(cursor);
            if (defaults > MaximumDefaults)
                return AssetError {AssetErrorCode::InvalidData, "Too many Shader parameter defaults"};
            for (uint32_t i = 0; i < defaults; ++i)
            {
                if (remaining() < 8)
                    return AssetError {AssetErrorCode::InvalidData, "Truncated Shader parameter default"};
                const auto nameBytes = Read(cursor), count = Read(cursor);
                if (nameBytes > 256 || count > MaximumDefaultValues ||
                    uint64_t(nameBytes) + uint64_t(count) * 8 > remaining())
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader parameter default"};
                std::string         name(reinterpret_cast<const char*>(cursor), nameBytes);
                std::vector<double> values;
                cursor += nameBytes;
                for (uint32_t j = 0; j < count; ++j)
                    values.push_back(ReadDouble(cursor));
                if (!ValidDefault(name, values) || !result.ParameterDefaults.emplace(name, std::move(values)).second)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader parameter default"};
            }
        }
        if (version >= 3)
        {
            const auto remaining = [&] { return size - size_t(cursor - bytes); };
            if (remaining() < 4)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Shader parameter hints"};
            const auto hints = Read(cursor);
            if (hints > MaximumDefaults)
                return AssetError {AssetErrorCode::InvalidData, "Too many Shader parameter hints"};
            for (uint32_t i = 0; i < hints; ++i)
            {
                if (remaining() < 8)
                    return AssetError {AssetErrorCode::InvalidData, "Truncated Shader parameter hint"};
                const auto nameBytes = Read(cursor), kind = Read(cursor);
                if (nameBytes > 256 || uint64_t(nameBytes) + 16 > remaining())
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader parameter hint"};
                std::string name(reinterpret_cast<const char*>(cursor), nameBytes);
                cursor += nameBytes;
                ShaderParameterHint hint;
                hint.Type = static_cast<ShaderParameterHint::Kind>(kind);
                hint.Min  = ReadDouble(cursor);
                hint.Max  = ReadDouble(cursor);
                if (!ValidHint(name, hint) || !result.ParameterHints.emplace(name, hint).second)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader parameter hint"};
            }
        }
        if (version >= 4)
        {
            const auto remaining = [&] { return size - size_t(cursor - bytes); };
            if (remaining() < 4)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Shader texture fallbacks"};
            const auto count = Read(cursor);
            if (count > MaximumDefaults)
                return AssetError {AssetErrorCode::InvalidData, "Too many Shader texture fallbacks"};
            for (uint32_t i = 0; i < count; ++i)
            {
                if (remaining() < 8)
                    return AssetError {AssetErrorCode::InvalidData, "Truncated Shader texture fallback"};
                const auto length = Read(cursor), role = Read(cursor);
                if (!length || length > 256 || length > remaining() || role > uint32_t(ShaderTextureFallback::Normal))
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader texture fallback"};
                std::string name(reinterpret_cast<const char*>(cursor), length);
                cursor += length;
                if (!ValidDefault(name, {0}) ||
                    !result.TextureFallbacks.emplace(name, ShaderTextureFallback(role)).second)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader texture fallback"};
            }
        }
        if (version >= 5)
        {
            const auto remaining  = [&] { return size - size_t(cursor - bytes); };
            const auto readString = [&](std::string& text) {
                if (remaining() < 4)
                    return false;
                const auto length = Read(cursor);
                if (!length || length > 256 || length > remaining())
                    return false;
                text.assign(reinterpret_cast<const char*>(cursor), length);
                cursor += length;
                return true;
            };
            if (remaining() < 4)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Shader groups"};
            const auto count = Read(cursor);
            if (count > 4096)
                return AssetError {AssetErrorCode::InvalidData, "Too many Shader groups"};
            for (uint32_t i = 0; i < count; ++i)
            {
                MaterialParameterGroup group;
                if (!readString(group.Name) || remaining() < 4)
                    return AssetError {AssetErrorCode::InvalidData, "Truncated Shader group"};
                const auto members = Read(cursor);
                if (!members || members > 4096)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader group members"};
                for (uint32_t j = 0; j < members; ++j)
                {
                    std::string member;
                    if (!readString(member))
                        return AssetError {AssetErrorCode::InvalidData, "Truncated Shader group member"};
                    group.Members.push_back(std::move(member));
                }
                result.Groups.push_back(std::move(group));
            }
        }
        if (version >= 6)
        {
            const auto remaining  = [&] { return size - size_t(cursor - bytes); };
            const auto readString = [&](std::string& text) {
                if (remaining() < 4)
                    return false;
                const auto length = Read(cursor);
                if (!length || length > 256 || length > remaining())
                    return false;
                text.assign(reinterpret_cast<const char*>(cursor), length);
                cursor += length;
                return true;
            };
            if (remaining() < 4)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Shader passes"};
            const auto count = Read(cursor);
            if (count > 32)
                return AssetError {AssetErrorCode::InvalidData, "Too many Shader passes"};
            for (uint32_t i = 0; i < count; ++i)
            {
                ShaderPass pass;
                if (!readString(pass.Name) || !readString(pass.Tag) || remaining() < 44)
                    return AssetError {AssetErrorCode::InvalidData, "Truncated Shader pass"};
                auto& state           = pass.State;
                state.Cull            = ShaderCullMode(Read(cursor));
                const auto depthTest  = Read(cursor);
                state.DepthCompare    = ShaderDepthCompare(Read(cursor));
                const auto depthWrite = Read(cursor), blend = Read(cursor);
                if (depthTest > 1 || depthWrite > 1 || blend > 1)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader pass boolean"};
                state.DepthTest        = depthTest;
                state.DepthWrite       = depthWrite;
                state.Blend            = blend;
                state.SourceColor      = ShaderBlendFactor(Read(cursor));
                state.DestinationColor = ShaderBlendFactor(Read(cursor));
                state.SourceAlpha      = ShaderBlendFactor(Read(cursor));
                state.DestinationAlpha = ShaderBlendFactor(Read(cursor));
                state.ColorWriteMask   = Read(cursor);
                const auto length      = Read(cursor);
                if (length < 24 || length > remaining())
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Shader pass code length"};
                auto header = cursor + 4;
                if (Read(header) != 5)
                    return AssetError {AssetErrorCode::InvalidData,
                                       "Shader pass code must be a nonrecursive v5 record"};
                auto decoded = DecodeShaderProduct(cursor, length);
                if (auto* error = std::get_if<AssetError>(&decoded))
                    return *error;
                auto data       = std::get<ShaderData>(std::move(decoded));
                pass.Code       = std::move(data.Code);
                pass.Reflection = std::move(data.Reflection);
                cursor += length;
                result.Passes.push_back(std::move(pass));
            }
        }
        if (version >= 7)
        {
            if (size - size_t(cursor - bytes) < 4)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Shader output space"};
            const auto space = Read(cursor);
            if (space > 1)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Shader output space"};
            result.OutputSpace = ShaderOutputSpace(space);
        }
        if (version >= 8)
        {
            if (size - size_t(cursor - bytes) < 8)
                return AssetError {AssetErrorCode::InvalidData, "Truncated Shader domain/stage"};
            const auto domain = Read(cursor), stage = Read(cursor);
            if (domain > 1 || stage > 1 || (domain == 1 && !result.Passes.empty()))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Shader domain/stage"};
            result.Domain    = ShaderDomain(domain);
            result.PostStage = PostProcessStage(stage);
        }
        if (version >= 9)
        {
            const auto remaining = size - size_t(cursor - bytes);
            if (remaining < 12)
                return AssetError {AssetErrorCode::InvalidData, "Truncated RenderTexture shader"};
            const auto  length = Read(cursor);
            const auto* header = cursor + 4;
            if (length != remaining - 4 || Read(header) != 7)
                return AssetError {AssetErrorCode::InvalidData, "Invalid RenderTexture shader record"};
            auto decoded = DecodeShaderProduct(cursor, length);
            if (auto* error = std::get_if<AssetError>(&decoded))
                return *error;
            result.RenderTextureCode = std::get<ShaderData>(decoded).Code;
            if (result.RenderTextureCode.Modules.size() != 1 ||
                result.RenderTextureCode.Modules[0].Stage != ShaderProductStage::Fragment)
                return AssetError {AssetErrorCode::InvalidData, "RenderTexture variant must be a fragment"};
            cursor += length;
        }
        if (size_t(cursor - bytes) != size)
            return AssetError {AssetErrorCode::InvalidData, "Trailing Shader Product bytes"};
        auto reflected = InspectShaderProgram(result.Code);
        if (auto* error = std::get_if<AssetError>(&reflected))
            return *error;
        result.Reflection = std::get<std::vector<ShaderStageReflection>>(std::move(reflected));
        if (result.Domain == ShaderDomain::PostProcess)
        {
            auto valid = ValidateMaterialShaderInterface(result.Reflection, result.Domain);
            if (auto* error = std::get_if<AssetError>(&valid))
                return *error;
        }
        auto validGroups  = ValidateMaterialParameterGroups(result.Groups, result.Reflection);
        if (auto* error = std::get_if<AssetError>(&validGroups))
            return *error;
        auto variant = ValidateRenderTextureVariant(result.Code, result.RenderTextureCode);
        if (auto* error = std::get_if<AssetError>(&variant))
            return *error;
        auto validPasses = ValidateMaterialShaderPasses(result.Code, result.Passes);
        if (auto* error = std::get_if<AssetError>(&validPasses))
            return *error;
        return result;
    }
} // namespace Kans
