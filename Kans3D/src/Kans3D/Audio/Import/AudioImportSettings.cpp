#include "kspch.h"
#include "AudioImportSettings.h"
#include <charconv>
#include <cmath>

namespace Kans
{
    std::variant<std::monostate, AssetError> ValidateAudioImportSettings(const AudioImportSettings& settings)
    {
        if ((settings.LoadMode && *settings.LoadMode != AudioLoadMode::Decompress &&
             *settings.LoadMode != AudioLoadMode::Stream) ||
            !std::isfinite(settings.Volume) || settings.Volume < 0 || settings.Volume > 4 ||
            !std::isfinite(settings.LoopBegin) || !std::isfinite(settings.LoopEnd) || settings.LoopBegin < 0 ||
            settings.LoopEnd < 0 || (settings.LoopEnd != 0 && settings.LoopBegin >= settings.LoopEnd))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Audio import settings"};
        return std::monostate {};
    }

    std::variant<AudioImportSettings, AssetError>
    ParseAudioImportSettings(const std::map<std::string, std::string>& values)
    {
        AudioImportSettings settings;
        for (const auto& [key, value] : values)
        {
            if (key == "load_mode")
            {
                if (value == "auto")
                    settings.LoadMode.reset();
                else if (value == "decompress")
                    settings.LoadMode = AudioLoadMode::Decompress;
                else if (value == "stream")
                    settings.LoadMode = AudioLoadMode::Stream;
                else
                    return AssetError {AssetErrorCode::InvalidArgument, "Invalid Audio load_mode: " + value};
            }
            else if (key == "volume" || key == "loop_begin" || key == "loop_end")
            {
                float      number = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() || !std::isfinite(number))
                    return AssetError {AssetErrorCode::InvalidArgument, "Invalid Audio number: " + key};
                if (key == "volume")
                    settings.Volume = number;
                else if (key == "loop_begin")
                    settings.LoopBegin = number;
                else
                    settings.LoopEnd = number;
            }
            else
                return AssetError {AssetErrorCode::InvalidArgument, "Unknown Audio import setting: " + key};
        }
        if (auto valid = ValidateAudioImportSettings(settings); auto* error = std::get_if<AssetError>(&valid))
            return *error;
        return settings;
    }
} // namespace Kans
