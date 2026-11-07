#pragma once
#include <map>
#include <optional>
#include "Kans3D/Asset/Runtime/AudioClipData.h"

namespace Kans
{
    struct AudioImportSettings
    {
        std::optional<AudioLoadMode> LoadMode; // Unset means auto, resolved by the importer.
        float                        Volume = 1, LoopBegin = 0, LoopEnd = 0;
    };
    [[nodiscard]] std::variant<AudioImportSettings, AssetError>
    ParseAudioImportSettings(const std::map<std::string, std::string>& values);
    [[nodiscard]] std::variant<std::monostate, AssetError>
    ValidateAudioImportSettings(const AudioImportSettings& settings);
} // namespace Kans
