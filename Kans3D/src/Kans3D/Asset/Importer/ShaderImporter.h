#pragma once
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "AssetImporterBase.h"
namespace Kans
{
    // Engine-owned sources use the same compiler and Product validation as project sources.
    std::variant<ShaderData, AssetError> CompileEmbeddedShaderProgram(const std::string& manifest,
                                                                      const std::map<std::string, std::string>& stages);
    // Compiles GLSL stages (.vert/.frag/.comp) or a .shaderprog stage list to a
    // SPIR-V Shader Product. Quoted includes are relative and resolved inside the
    // asset root; <Kans3D/...> includes are engine headers (MaterialShaderInterface.h).
    class ShaderImporter final : public AssetImporterBase
    {
    public:
        std::string GetEnvironmentFingerprint() const override { return GetEngineShaderDigest(); }
        // Registered shader text and the explicit engine contract version also participate in the fingerprint.
        uint32_t GetVersion() const override { return 18; }
        [[nodiscard]] std::variant<ImportInputs, AssetError>
        DiscoverInputs(const ImportContext& context) const override;
        [[nodiscard]] std::variant<ImportResult, AssetError> Import(const ImportContext& context) const override;
    };
} // namespace Kans
