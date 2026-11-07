#pragma once
#include <filesystem>
#include <string>
#include "Kans3D/Asset/Asset.h"

namespace Kans
{
    // A published Product the editor can reference from components.
    struct EditorAssetChoice
    {
        AssetID               ID;
        AssetType             Type = AssetType::None;
        std::string           Name;
        std::filesystem::path SourcePath; // Absolute path of the source file.
    };
} // namespace Kans
