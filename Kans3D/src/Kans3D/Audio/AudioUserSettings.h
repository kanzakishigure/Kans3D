#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Kans
{
    enum class SpeakerMode
    {
        Auto,
        Mono,
        Stereo,
        Surround51,
        Surround71
    };

    inline constexpr char DefaultAudioOutputDevice[] = "default";

    struct AudioUserBusSettings
    {
        // Additive post-fader gain relative to the current authored/runtime bus level.
        // Player mute and authored mute combine, and pre-fader sends remain independent.
        float VolumeDb = 0;
        bool  Mute     = false;
        bool  operator==(const AudioUserBusSettings& other) const
        {
            return VolumeDb == other.VolumeDb && Mute == other.Mute;
        }
    };

    struct AudioUserSettings
    {
        std::map<std::string, AudioUserBusSettings, std::less<>> Buses;
        SpeakerMode                                              Speakers     = SpeakerMode::Auto;
        std::string                                              OutputDevice = DefaultAudioOutputDevice;
        bool                                                     operator==(const AudioUserSettings& other) const
        {
            return Buses == other.Buses && Speakers == other.Speakers && OutputDevice == other.OutputDevice;
        }

        static std::filesystem::path DefaultPath();
        // Missing files use defaults. Malformed values return diagnostics alongside usable settings.
        static AudioUserSettings   Load(const std::filesystem::path& file, std::vector<std::string>& reasons);
        std::optional<std::string> Save(const std::filesystem::path& file) const;
        void                       Normalize(std::vector<std::string>& reasons);
    };
} // namespace Kans
