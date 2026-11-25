#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    enum class AudioAttenuationModel
    {
        None,
        Inverse,
        Linear,
        Exponential
    };
    enum class AudioCurveInterpolation
    {
        Linear,
        Smooth
    };

    struct AudioCurveKey
    {
        float Distance = 0, Value = 1;
        // Interpolation for the segment starting at this key.
        AudioCurveInterpolation Interpolation = AudioCurveInterpolation::Linear;
    };

    struct AudioAttenuationPreset
    {
        AudioAttenuationModel Model       = AudioAttenuationModel::Inverse;
        float                 MinDistance = 1, MaxDistance = 100, Rolloff = 1;
    };

    struct AudioAttenuationCone
    {
        float InnerAngle = 360, OuterAngle = 360, OuterGain = 0;
    };

    struct AudioAttenuationData
    {
        std::variant<AudioAttenuationPreset, std::vector<AudioCurveKey>> Volume;
        std::vector<AudioCurveKey>                                       AirAbsorption; // Empty bypasses the filter.
        std::vector<AudioCurveKey>                                       Spread;        // Empty is a point source.
        std::optional<AudioAttenuationCone>                              Cone;
        float                                                            DopplerFactor = 1;
    };

    class AudioAttenuationAsset final : public Asset
    {
    public:
        explicit AudioAttenuationAsset(AudioAttenuationData data) : m_Data(std::move(data)) {}
        static AssetType            GetStaticType() { return AssetType::AudioAttenuation; }
        AssetType                   GetAssetType() const override { return GetStaticType(); }
        const AudioAttenuationData& GetData() const { return m_Data; }

    private:
        AudioAttenuationData m_Data;
    };

    struct AudioAttenuationSample
    {
        float Gain = 1, CutoffHz = 0, Spread = 0; // A zero cutoff means bypass.
    };

    std::optional<AssetError>                      ValidateAudioAttenuation(const AudioAttenuationData& data);
    std::variant<AudioAttenuationData, AssetError> ParseAudioAttenuation(const std::string& text);
    std::variant<std::string, AssetError>          SerializeAudioAttenuation(const AudioAttenuationData& data);

    // Uses validated data. Distances outside a curve hold its endpoint values.
    AudioAttenuationSample EvaluateAttenuation(const AudioAttenuationData& data, float distance);
    float                  EvaluateAttenuationPreset(const AudioAttenuationPreset& preset, float distance);
} // namespace Kans
