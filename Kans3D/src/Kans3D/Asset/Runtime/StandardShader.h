#pragma once
#include "ModelData.h"
#include "ShaderData.h"

namespace Kans
{
    // Reserved engine identity, never a project dependency or file.
    [[nodiscard]] AssetID                      StandardShaderID();
    [[nodiscard]] AssetID                      SixWaySmokeShaderID();
    [[nodiscard]] AssetID                      SixWaySmokeMaterialID();
    [[nodiscard]] Ref<const MaterialDataAsset> GetSixWaySmokeMaterialAsset();
    [[nodiscard]] bool                         IsStandardShader(AssetID id);
    // Compiled and reflected once on the CPU; requires no graphics context.
    [[nodiscard]] const ShaderData&          GetStandardShader(bool particle = false);
    [[nodiscard]] Ref<const ShaderDataAsset> GetStandardShaderAsset();
    // Kans3D/SixWaySmoke has its own schema; mesh use evaluates lighting per pixel.
    [[nodiscard]] const ShaderData&          GetSixWaySmokeShader(bool particle = false);
    [[nodiscard]] Ref<const ShaderDataAsset> GetSixWaySmokeShaderAsset();
    // Adapts pre-recipe KMAT materials and imported semantic texture bindings.
    [[nodiscard]] std::variant<MaterialData, AssetError>
    ResolveStandardMaterial(const MaterialData& legacy, std::optional<std::array<float, 3>> emissive = std::nullopt);
} // namespace Kans
