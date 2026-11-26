#pragma once
#include <filesystem>
#include "SourceInventory.h"
#include "TextureImportSettings.h"

namespace Kans
{
    [[nodiscard]] std::variant<SourceInventory, AssetError> InspectTextureSource(const std::filesystem::path& file);
    // Owned CPU decoding. Never modifies the legacy renderer's stb configuration.
    [[nodiscard]] std::variant<TextureData, AssetError>
    ParseTextureSource(const std::filesystem::path& file,
                       const TextureImportSettings& settings = {},
                       std::vector<std::string>*    warnings = nullptr);
} // namespace Kans
