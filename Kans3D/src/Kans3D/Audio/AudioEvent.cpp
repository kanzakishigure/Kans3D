#include "kspch.h"
#include "AudioEvent.h"
#include <cmath>
#include <yaml-cpp/yaml.h>

namespace Kans
{
    namespace
    {
        AssetError Invalid(std::string message) { return {AssetErrorCode::InvalidData, std::move(message)}; }

        constexpr const char* Selections[] = {"Random", "Shuffle", "Sequential"};
        constexpr const char* Rules[]      = {"StopOldest", "StopQuietest", "StopLowestPriority", "RejectNew"};
        constexpr const char* Models[]     = {"None", "Inverse", "Linear", "Exponential"};

        template<class T, size_t N>
        T ReadEnum(const YAML::Node& node, const char* field, T fallback, const char* const (&names)[N])
        {
            if (!node[field])
                return fallback;
            const auto value = node[field].as<std::string>();
            for (size_t i = 0; i < N; ++i)
                if (value == names[i])
                    return static_cast<T>(i);
            throw std::runtime_error(std::string("Unknown ") + field + ": " + value);
        }

        template<class T>
        void Read(const YAML::Node& node, const char* field, T& value)
        {
            if (node[field])
                value = node[field].as<T>();
        }

        template<class Data, class Visit>
        void Fields(Data& data, Visit visit)
        {
            visit("AvoidRepeatingLast", data.AvoidRepeatingLast);
            visit("VolumeDb", data.VolumeDb);
            visit("PitchSemitones", data.PitchSemitones);
            visit("CooldownSeconds", data.CooldownSeconds);
            visit("ConcurrencyGroup", data.ConcurrencyGroup);
            visit("Bus", data.Bus);
            visit("Loop", data.Loop);
            visit("Spatial", data.Spatial);
        }

        template<class Spatial, class Visit>
        void AttenuationFields(Spatial& spatial, Visit visit)
        {
            visit("MinDistance", spatial.MinDistance);
            visit("MaxDistance", spatial.MaxDistance);
            visit("Rolloff", spatial.Rolloff);
            visit("DopplerFactor", spatial.DopplerFactor);
            visit("ConeInnerAngle", spatial.ConeInnerAngle);
            visit("ConeOuterAngle", spatial.ConeOuterAngle);
            visit("ConeOuterGain", spatial.ConeOuterGain);
        }

        void ReadRange(const YAML::Node& root, const char* field, AudioEventRange& range)
        {
            const auto node = root[field];
            if (!node)
                return;
            if (!node.IsSequence() || node.size() != 2)
                throw std::runtime_error(std::string(field) + " must contain [min, max]");
            range = {node[0].as<float>(), node[1].as<float>()};
        }

        YAML::Node EncodeRange(AudioEventRange range)
        {
            YAML::Node node;
            node.SetStyle(YAML::EmitterStyle::Flow);
            node.push_back(range.Min);
            node.push_back(range.Max);
            return node;
        }

        template<class T>
        bool Nonnegative(T value)
        {
            return std::isfinite(value) && value >= 0;
        }
        bool ValidRange(AudioEventRange range)
        {
            return std::isfinite(range.Min) && std::isfinite(range.Max) && range.Min <= range.Max;
        }
    } // namespace

    std::optional<AssetError> ValidateAudioEvent(const AudioEventData& data)
    {
        if (data.Entries.empty())
            return Invalid("Audio event Entries must not be empty");
        if (data.Selection < AudioEventSelection::Random || data.Selection > AudioEventSelection::Sequential)
            return Invalid("Invalid audio event Selection");
        if (!ValidRange(data.VolumeRangeDb))
            return Invalid("VolumeRangeDb must have finite min <= max");
        if (!ValidRange(data.PitchRangeSemitones))
            return Invalid("PitchRangeSemitones must have finite min <= max");
        if (!ValidRange(data.StartOffsetSeconds) || data.StartOffsetSeconds.Min < 0)
            return Invalid("StartOffsetSeconds must have finite 0 <= min <= max");
        if (!Nonnegative(data.CooldownSeconds))
            return Invalid("CooldownSeconds must be finite and nonnegative");
        if (!data.InstanceLimit.MaxCount)
            return Invalid("InstanceLimit MaxCount must be greater than zero");
        if (data.InstanceLimit.Rule < AudioConcurrencyRule::StopOldest ||
            data.InstanceLimit.Rule > AudioConcurrencyRule::RejectNew)
            return Invalid("Invalid InstanceLimit Rule");
        if (!Nonnegative(data.InstanceLimit.ReleaseSeconds))
            return Invalid("InstanceLimit ReleaseSeconds must be finite and nonnegative");
        const auto& a = data.Attenuation;
        if (a.Attenuation < AudioAttenuationModel::None || a.Attenuation > AudioAttenuationModel::Exponential ||
            !Nonnegative(a.MinDistance) || a.MinDistance == 0 || !Nonnegative(a.MaxDistance) ||
            a.MaxDistance < a.MinDistance || !Nonnegative(a.Rolloff) || !Nonnegative(a.DopplerFactor) ||
            !Nonnegative(a.ConeInnerAngle) || !Nonnegative(a.ConeOuterAngle) || a.ConeInnerAngle > a.ConeOuterAngle ||
            a.ConeOuterAngle > 360 || !Nonnegative(a.ConeOuterGain) || a.ConeOuterGain > 1)
            return Invalid("Invalid audio event Attenuation parameters");
        if (!IsValidAudioBusLevel(data.LfeSendDb))
            return Invalid("Audio event LfeSendDb must convert to a finite gain");
        if (!std::isfinite(data.VolumeDb) || !std::isfinite(data.PitchSemitones))
            return Invalid("Audio event base volume and pitch must be finite");
        for (size_t i = 0; i < data.Entries.size(); ++i)
        {
            const auto& entry = data.Entries[i];
            const auto  error = [i](const char* reason) {
                return Invalid("Audio event entry " + std::to_string(i + 1) + ": " + reason);
            };
            if (!entry.Clip.IsValid())
                return error("missing or invalid Clip ID");
            if (!std::isfinite(entry.Weight) || entry.Weight <= 0)
                return error("Weight must be finite and greater than zero");
            const auto gain =
                float(std::pow(10.0, (double(data.VolumeDb) + entry.VolumeDb + data.VolumeRangeDb.Max) / 20));
            const auto minPitch = float(
                std::exp2((double(data.PitchSemitones) + entry.PitchSemitones + data.PitchRangeSemitones.Min) / 12));
            const auto maxPitch = float(
                std::exp2((double(data.PitchSemitones) + entry.PitchSemitones + data.PitchRangeSemitones.Max) / 12));
            if (!std::isfinite(gain) || !std::isfinite(maxPitch) || !(minPitch > 0))
                return error("combined volume and pitch must convert to finite gain and positive finite pitch");
        }
        return std::nullopt;
    }

    std::variant<AudioEventData, AssetError> ParseAudioEvent(const std::string& text)
    {
        try
        {
            const auto documents = YAML::LoadAll(text);
            if (documents.size() != 1 || !documents.front().IsMap())
                return Invalid("Expected one audio event mapping");
            const auto& root = documents.front();
            if (root["Version"].as<unsigned>() != 1)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported audio event version"};

            AudioEventData data;
            const auto     entries = root["Entries"];
            if (!entries || !entries.IsSequence())
                return Invalid("Expected an Entries sequence");
            for (const auto& node : entries)
            {
                AudioEventEntry entry;
                if (node["Clip"])
                    entry.Clip = AssetID::FromString(node["Clip"].as<std::string>());
                Read(node, "Weight", entry.Weight);
                Read(node, "VolumeDb", entry.VolumeDb);
                Read(node, "PitchSemitones", entry.PitchSemitones);
                data.Entries.push_back(entry);
            }
            data.Selection = ReadEnum(root, "Selection", data.Selection, Selections);
            Fields(data, [&](const char* key, auto& value) { Read(root, key, value); });
            Read(root, "ReverbZoneSend", data.ReverbZoneSend);
            Read(root, "LfeSendDb", data.LfeSendDb);
            ReadRange(root, "VolumeRangeDb", data.VolumeRangeDb);
            ReadRange(root, "PitchRangeSemitones", data.PitchRangeSemitones);
            ReadRange(root, "StartOffsetSeconds", data.StartOffsetSeconds);
            unsigned priority = data.Priority;
            Read(root, "Priority", priority);
            if (priority > 255)
                return Invalid("Priority must be between 0 and 255");
            data.Priority = static_cast<uint8_t>(priority);
            if (const auto limit = root["InstanceLimit"])
            {
                Read(limit, "MaxCount", data.InstanceLimit.MaxCount);
                Read(limit, "ReleaseSeconds", data.InstanceLimit.ReleaseSeconds);
                data.InstanceLimit.Rule = ReadEnum(limit, "Rule", data.InstanceLimit.Rule, Rules);
            }
            if (const auto asset = root["AttenuationAsset"])
            {
                data.AttenuationAsset = AssetID::FromString(asset.as<std::string>());
                if (!data.AttenuationAsset.IsValid())
                    return Invalid("Invalid AttenuationAsset ID");
            }
            if (const auto attenuation = root["Attenuation"])
            {
                data.Attenuation.Attenuation = ReadEnum(attenuation, "Model", data.Attenuation.Attenuation, Models);
                AttenuationFields(data.Attenuation,
                                  [&](const char* key, auto& value) { Read(attenuation, key, value); });
            }
            if (auto error = ValidateAudioEvent(data))
                return *error;
            return data;
        }
        catch (const YAML::Exception& error)
        {
            return Invalid(error.what());
        }
        catch (const std::runtime_error& error)
        {
            return Invalid(error.what());
        }
    }

    std::variant<std::string, AssetError> SerializeAudioEvent(const AudioEventData& data)
    {
        if (auto error = ValidateAudioEvent(data))
            return *error;
        YAML::Node root;
        root["Version"]   = 1;
        root["Selection"] = Selections[int(data.Selection)];
        Fields(data, [&](const char* key, const auto& value) { root[key] = value; });
        if (!data.ReverbZoneSend)
            root["ReverbZoneSend"] = false;
        if (data.LfeSendDb != AudioSendOffDb)
            root["LfeSendDb"] = data.LfeSendDb;
        root["Priority"]            = unsigned(data.Priority);
        root["VolumeRangeDb"]       = EncodeRange(data.VolumeRangeDb);
        root["PitchRangeSemitones"] = EncodeRange(data.PitchRangeSemitones);
        root["StartOffsetSeconds"]  = EncodeRange(data.StartOffsetSeconds);
        auto limit                  = root["InstanceLimit"];
        limit["MaxCount"]           = data.InstanceLimit.MaxCount;
        limit["Rule"]               = Rules[int(data.InstanceLimit.Rule)];
        limit["ReleaseSeconds"]     = data.InstanceLimit.ReleaseSeconds;
        if (data.AttenuationAsset.IsValid())
            root["AttenuationAsset"] = data.AttenuationAsset.ToString();
        auto attenuation            = root["Attenuation"];
        attenuation["Model"]        = Models[int(data.Attenuation.Attenuation)];
        AttenuationFields(data.Attenuation, [&](const char* key, float value) { attenuation[key] = value; });
        for (const auto& entry : data.Entries)
        {
            YAML::Node node;
            node["Clip"]           = entry.Clip.ToString();
            node["Weight"]         = entry.Weight;
            node["VolumeDb"]       = entry.VolumeDb;
            node["PitchSemitones"] = entry.PitchSemitones;
            root["Entries"].push_back(node);
        }
        return YAML::Dump(root);
    }
} // namespace Kans
