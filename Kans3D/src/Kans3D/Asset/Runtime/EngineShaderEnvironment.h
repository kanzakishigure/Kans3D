#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace Kans
{
    // Bump for changes to compile options, SPIR-V transforms, reflection/binding validation,
    // or source-generation logic that alter SPIR-V/contracts without changing registered text.
    inline constexpr uint32_t EngineShaderContractVersion = 1;

    // Binary-resident shader text only. Generators use the same registry as the fingerprint.
    [[nodiscard]] const std::map<std::string, std::string>& GetEngineShaderTexts();
    // Pure function: no C++ source files, build identities, paths or other environment inputs.
    [[nodiscard]] std::string        ComputeEngineShaderDigest(const std::map<std::string, std::string>& texts,
                                                               uint32_t contractVersion = EngineShaderContractVersion);
    [[nodiscard]] const std::string& GetEngineShaderDigest();
} // namespace Kans
