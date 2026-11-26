#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"
#include "AudioBusEffect.h"

namespace Kans
{
    inline constexpr char MasterAudioBus[] = "Master";

    enum class AudioSendTap
    {
        PreFader,
        PostFader
    };

    struct AudioBusSend
    {
        std::string  Target;
        float        LevelDb = 0;
        AudioSendTap Tap     = AudioSendTap::PostFader;
    };

    struct AudioBusDefinition
    {
        std::string Name;
        std::string Parent;
        float       VolumeDb = 0;
        bool        Mute     = false;
        bool        Solo     = false;
        bool                        Bypass   = false;
        std::vector<AudioBusSend> Sends;
        std::vector<AudioBusEffect> Effects {};
        bool                        PinToCenter = false;
    };

    struct AudioBusLayout
    {
        std::vector<AudioBusDefinition> Buses;

        static AudioBusLayout Default();
    };

    class AudioBusLayoutAsset final : public Asset
    {
    public:
        explicit AudioBusLayoutAsset(AudioBusLayout layout) : m_Layout(std::move(layout)) {}
        static AssetType      GetStaticType() { return AssetType::AudioBusLayout; }
        AssetType             GetAssetType() const override { return GetStaticType(); }
        const AudioBusLayout& GetData() const { return m_Layout; }

    private:
        AudioBusLayout m_Layout;
    };

    // Negative infinity is a closed fader. Other values must convert to a finite gain.
    bool                                     IsValidAudioBusLevel(float db);
    std::optional<AssetError>                ValidateAudioBusEffect(const AudioBusEffectParameters&        parameters,
                                                                    const std::vector<AudioBusDefinition>& buses);
    std::optional<AssetError>                ValidateAudioBusLayout(const AudioBusLayout& layout);
    std::variant<AudioBusLayout, AssetError> ParseAudioBusLayout(const std::string& text);
    std::variant<std::string, AssetError>    SerializeAudioBusLayout(const AudioBusLayout& layout);
} // namespace Kans
