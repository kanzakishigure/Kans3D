#include "kspch.h"
#include "AudioAttenuation.h"
#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <yaml-cpp/yaml.h>

namespace Kans
{
    namespace
    {
        using Curve                    = std::vector<AudioCurveKey>;
        constexpr const char* Models[] = {"None", "Inverse", "Linear", "Exponential"};

        AssetError Invalid(std::string message) { return {AssetErrorCode::InvalidData, std::move(message)}; }

        bool InRange(float value, float lower, float upper)
        {
            return std::isfinite(value) && value >= lower && value <= upper;
        }

        std::optional<AssetError>
        ValidateCurve(const Curve& keys, const char* field, float lower, float upper, bool descending = false)
        {
            float previous = -1, previousValue = upper;
            for (const auto& key : keys)
            {
                if (!std::isfinite(key.Distance) || key.Distance < 0)
                    return Invalid(std::string(field) + " distances must be finite and nonnegative");
                if (key.Distance <= previous)
                    return Invalid(std::string(field) + " distances must be strictly increasing");
                if (!InRange(key.Value, lower, upper))
                    return Invalid(std::string(field) + (lower == 0 ? " values must be finite and in [0, 1]" :
                                                                      " cutoff must be finite and in [20, 24000] Hz"));
                if (key.Interpolation != AudioCurveInterpolation::Linear &&
                    key.Interpolation != AudioCurveInterpolation::Smooth)
                    return Invalid(std::string(field) + " has invalid interpolation");
                if (descending && key.Value > previousValue)
                    return Invalid("AirAbsorption cutoff must be nonincreasing with distance");
                previous      = key.Distance;
                previousValue = key.Value;
            }
            return std::nullopt;
        }

        double Slope(const AudioCurveKey& lhs, const AudioCurveKey& rhs)
        {
            return (double(rhs.Value) - lhs.Value) / (double(rhs.Distance) - lhs.Distance);
        }

        double Tangent(const Curve& keys, size_t index)
        {
            if (index == 0)
                return Slope(keys[0], keys[1]);
            if (index + 1 == keys.size())
                return Slope(keys[index - 1], keys[index]);
            const double before = Slope(keys[index - 1], keys[index]);
            const double after  = Slope(keys[index], keys[index + 1]);
            if (before * after <= 0)
                return 0;
            // PCHIP weighted harmonic mean. Flat sections and local extrema have zero slope.
            const double leftWidth   = double(keys[index].Distance) - keys[index - 1].Distance;
            const double rightWidth  = double(keys[index + 1].Distance) - keys[index].Distance;
            const double leftWeight  = 2 * rightWidth + leftWidth;
            const double rightWeight = rightWidth + 2 * leftWidth;
            return (leftWeight + rightWeight) / (leftWeight / before + rightWeight / after);
        }

        float EvaluateCurve(const Curve& keys, float distance)
        {
            if (!(distance > keys.front().Distance))
                return keys.front().Value;
            if (distance >= keys.back().Distance)
                return keys.back().Value;
            const auto right = std::upper_bound(
                keys.begin(), keys.end(), distance, [](float d, const AudioCurveKey& key) { return d < key.Distance; });
            const auto&  lhs   = *(right - 1);
            const auto&  rhs   = *right;
            const double width = double(rhs.Distance) - lhs.Distance;
            const double t     = (distance - double(lhs.Distance)) / width;
            if (lhs.Interpolation == AudioCurveInterpolation::Linear)
                return float(glm::mix(double(lhs.Value), double(rhs.Value), t));
            const auto   index      = static_cast<size_t>(right - keys.begin());
            const double complement = 1 - t;
            const double blend      = t * t * (3 - 2 * t);
            const double value =
                glm::mix(double(lhs.Value), double(rhs.Value), blend) +
                width * t * complement * (complement * Tangent(keys, index - 1) - t * Tangent(keys, index));
            return std::clamp(float(value), std::min(lhs.Value, rhs.Value), std::max(lhs.Value, rhs.Value));
        }

        template<class T>
        void Read(const YAML::Node& node, const char* field, T& value)
        {
            if (node[field])
                value = node[field].as<T>();
        }

        Curve ReadCurve(const YAML::Node& node, const char* field, const char* valueField)
        {
            if (!node.IsSequence())
                throw std::runtime_error(std::string(field) + " must be a sequence");
            Curve result;
            result.reserve(node.size());
            for (const auto& item : node)
            {
                AudioCurveKey key {item["Distance"].as<float>(), item[valueField].as<float>()};
                if (const auto interpolation = item["Interpolation"])
                {
                    const auto name = interpolation.as<std::string>();
                    if (name == "Smooth")
                        key.Interpolation = AudioCurveInterpolation::Smooth;
                    else if (name != "Linear")
                        throw std::runtime_error(std::string(field) + " has unknown interpolation: " + name);
                }
                result.push_back(key);
            }
            return result;
        }

        YAML::Node WriteCurve(const Curve& keys, const char* valueField)
        {
            YAML::Node result(YAML::NodeType::Sequence);
            for (const auto& key : keys)
            {
                YAML::Node item;
                item["Distance"]      = key.Distance;
                item[valueField]      = key.Value;
                item["Interpolation"] = key.Interpolation == AudioCurveInterpolation::Smooth ? "Smooth" : "Linear";
                item.SetStyle(YAML::EmitterStyle::Flow);
                result.push_back(item);
            }
            return result;
        }
    } // namespace

    std::optional<AssetError> ValidateAudioAttenuation(const AudioAttenuationData& data)
    {
        if (const auto* preset = std::get_if<AudioAttenuationPreset>(&data.Volume))
        {
            if (preset->Model < AudioAttenuationModel::Inverse || preset->Model > AudioAttenuationModel::Exponential)
                return Invalid("Volume has an invalid model");
            if (!std::isfinite(preset->MinDistance) || preset->MinDistance <= 0)
                return Invalid("MinDistance must be finite and positive");
            if (!std::isfinite(preset->MaxDistance) || preset->MaxDistance < preset->MinDistance)
                return Invalid("MaxDistance must be finite and at least MinDistance");
            if (!std::isfinite(preset->Rolloff) || preset->Rolloff < 0)
                return Invalid("Rolloff must be finite and nonnegative");
        }
        else
        {
            const auto& keys = std::get<Curve>(data.Volume);
            if (keys.empty())
                return Invalid("Volume Keys must not be empty");
            if (auto error = ValidateCurve(keys, "Volume", 0, 1))
                return error;
        }
        if (auto error = ValidateCurve(data.AirAbsorption, "AirAbsorption", 20, 24000, true))
            return error;
        if (auto error = ValidateCurve(data.Spread, "Spread", 0, 1))
            return error;
        if (!std::isfinite(data.DopplerFactor) || data.DopplerFactor < 0)
            return Invalid("DopplerFactor must be finite and nonnegative");
        if (data.Cone)
        {
            const auto& cone = *data.Cone;
            if (!InRange(cone.InnerAngle, 0, 360) || !InRange(cone.OuterAngle, cone.InnerAngle, 360))
                return Invalid("Cone angles must satisfy 0 <= InnerAngle <= OuterAngle <= 360");
            if (!InRange(cone.OuterGain, 0, 1))
                return Invalid("Cone OuterGain must be finite and in [0, 1]");
        }
        return std::nullopt;
    }

    std::variant<AudioAttenuationData, AssetError> ParseAudioAttenuation(const std::string& text)
    {
        try
        {
            const auto documents = YAML::LoadAll(text);
            if (documents.size() != 1 || !documents.front().IsMap())
                return Invalid("Expected one audio attenuation mapping");
            const auto& root = documents.front();
            if (root["Version"].as<unsigned>() != 1)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported audio attenuation version"};

            AudioAttenuationData data;
            if (const auto volume = root["Volume"])
            {
                if (!volume.IsMap())
                    return Invalid("Volume must be a mapping");
                if (volume["Keys"] &&
                    (volume["Model"] || volume["MinDistance"] || volume["MaxDistance"] || volume["Rolloff"]))
                    return Invalid("Volume must choose either Keys or a built-in model");
                if (volume["Keys"])
                    data.Volume = ReadCurve(volume["Keys"], "Volume", "Gain");
                else
                {
                    auto& preset = std::get<AudioAttenuationPreset>(data.Volume);
                    if (const auto model = volume["Model"])
                    {
                        const auto name  = model.as<std::string>();
                        const auto match = std::find(std::begin(Models), std::end(Models), name);
                        if (match == std::end(Models))
                            return Invalid("Unknown Volume Model: " + name);
                        preset.Model = static_cast<AudioAttenuationModel>(match - std::begin(Models));
                    }
                    Read(volume, "MinDistance", preset.MinDistance);
                    Read(volume, "MaxDistance", preset.MaxDistance);
                    Read(volume, "Rolloff", preset.Rolloff);
                }
            }
            if (const auto air = root["AirAbsorption"])
            {
                if (air.IsMap())
                {
                    if (!air["StartDistance"] || !air["EndDistance"] || !air["NearCutoffHz"] || !air["FarCutoffHz"])
                        return Invalid(
                            "AirAbsorption range requires StartDistance, EndDistance, NearCutoffHz and FarCutoffHz");
                    data.AirAbsorption = {{air["StartDistance"].as<float>(), air["NearCutoffHz"].as<float>()},
                                          {air["EndDistance"].as<float>(), air["FarCutoffHz"].as<float>()}};
                }
                else
                    data.AirAbsorption = ReadCurve(air, "AirAbsorption", "CutoffHz");
            }
            if (const auto spread = root["Spread"])
                data.Spread = ReadCurve(spread, "Spread", "Value");
            if (const auto cone = root["Cone"])
            {
                if (!cone.IsMap())
                    return Invalid("Cone must be a mapping");
                auto& settings = data.Cone.emplace();
                Read(cone, "InnerAngle", settings.InnerAngle);
                Read(cone, "OuterAngle", settings.OuterAngle);
                Read(cone, "OuterGain", settings.OuterGain);
            }
            Read(root, "DopplerFactor", data.DopplerFactor);
            if (auto error = ValidateAudioAttenuation(data))
                return *error;
            return data;
        }
        catch (const std::runtime_error& error)
        {
            return Invalid(error.what());
        }
    }

    std::variant<std::string, AssetError> SerializeAudioAttenuation(const AudioAttenuationData& data)
    {
        if (auto error = ValidateAudioAttenuation(data))
            return *error;
        YAML::Node root;
        root["Version"] = 1;
        auto volume     = root["Volume"];
        if (const auto* preset = std::get_if<AudioAttenuationPreset>(&data.Volume))
        {
            volume["Model"]       = Models[int(preset->Model)];
            volume["MinDistance"] = preset->MinDistance;
            volume["MaxDistance"] = preset->MaxDistance;
            volume["Rolloff"]     = preset->Rolloff;
        }
        else
            volume["Keys"] = WriteCurve(std::get<Curve>(data.Volume), "Gain");
        if (!data.AirAbsorption.empty())
            root["AirAbsorption"] = WriteCurve(data.AirAbsorption, "CutoffHz");
        if (!data.Spread.empty())
            root["Spread"] = WriteCurve(data.Spread, "Value");
        if (data.Cone)
        {
            auto cone          = root["Cone"];
            cone["InnerAngle"] = data.Cone->InnerAngle;
            cone["OuterAngle"] = data.Cone->OuterAngle;
            cone["OuterGain"]  = data.Cone->OuterGain;
        }
        root["DopplerFactor"] = data.DopplerFactor;
        return YAML::Dump(root);
    }

    float EvaluateAttenuationPreset(const AudioAttenuationPreset& preset, float distance)
    {
        if (preset.MinDistance >= preset.MaxDistance)
            return 1;
        const auto d = std::clamp(distance, preset.MinDistance, preset.MaxDistance);
        switch (preset.Model)
        {
            case AudioAttenuationModel::Inverse:
                return preset.MinDistance / (preset.MinDistance + preset.Rolloff * (d - preset.MinDistance));
            case AudioAttenuationModel::Linear:
                return std::clamp(1 - preset.Rolloff * (d - preset.MinDistance) /
                                          (preset.MaxDistance - preset.MinDistance),
                                  0.f,
                                  1.f);
            case AudioAttenuationModel::Exponential:
                return float(std::pow(double(d / preset.MinDistance), -double(preset.Rolloff)));
            case AudioAttenuationModel::None:
                return 1;
        }
        return 1;
    }

    AudioAttenuationSample EvaluateAttenuation(const AudioAttenuationData& data, float distance)
    {
        const auto* preset = std::get_if<AudioAttenuationPreset>(&data.Volume);
        return {preset ? EvaluateAttenuationPreset(*preset, distance) :
                         EvaluateCurve(std::get<Curve>(data.Volume), distance),
                data.AirAbsorption.empty() ? 0 : EvaluateCurve(data.AirAbsorption, distance),
                data.Spread.empty() ? 0 : EvaluateCurve(data.Spread, distance)};
    }
} // namespace Kans
