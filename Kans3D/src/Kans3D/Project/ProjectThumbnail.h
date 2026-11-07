#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    class ProjectConfig;
    // Accepts resolved or root-relative CacheDirectory, without opening/locking Project.
    std::optional<std::filesystem::path> FindProjectThumbnail(const ProjectConfig& config);
    // Tightly packed top-to-bottom RGBA8; pixelBytes is validated before reading.
    // Never upscales; fits within 512x288. Atomic replacement keeps the previous PNG on failure.
    std::optional<AssetError> WriteProjectThumbnail(const ProjectConfig& config,
                                                    const uint8_t*       pixels,
                                                    size_t               pixelBytes,
                                                    uint32_t             width,
                                                    uint32_t             height);
} // namespace Kans
