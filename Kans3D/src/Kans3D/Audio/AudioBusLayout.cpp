#include "kspch.h"
#include "AudioBusLayout.h"
#include "AudioBusEffectFields.h"

#include <algorithm>

#include <cmath>
#include <limits>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

namespace Kans
{
    namespace
    {
        template<typename T>
        T ReadOr(const YAML::Node& node, const char* key, T fallback)
        {
            const auto field = node[key];
            return field ? field.as<T>() : fallback;
        }

        constexpr unsigned BusLayoutVersion = 3;

        bool IsBoolean(const YAML::Node& node)
        {
            bool value = false;
            return YAML::convert<bool>::decode(node, value);
        }

        template<size_t... I>
        auto EffectDefaults(std::index_sequence<I...>)
        {
            return std::array {AudioBusEffectParameters(std::in_place_index<I>)...};
        }

        AssetError Invalid(std::string message) { return {AssetErrorCode::InvalidData, std::move(message)}; }
    } // namespace

    AudioBusLayout AudioBusLayout::Default()
    {
        return {{{MasterAudioBus, {}, 0, false, false, false, {}},
                 {"Music", MasterAudioBus, 0, false, false, false, {}},
                 {"SFX", MasterAudioBus, 0, false, false, false, {}},
                 {"Voice", MasterAudioBus, 0, false, false, false, {}},
                 {"UI", MasterAudioBus, 0, false, false, false, {}},
                 {"Ambience", MasterAudioBus, 0, false, false, false, {}}}};
    }

    bool IsValidAudioBusLevel(float db)
    {
        return db == -std::numeric_limits<float>::infinity() ||
               (std::isfinite(db) && std::isfinite(std::pow(10.f, db / 20.f)));
    }

    std::optional<AssetError> ValidateAudioBusEffect(const AudioBusEffectParameters&        parameters,
                                                     const std::vector<AudioBusDefinition>& buses)
    {
        if (parameters.valueless_by_exception())
            return Invalid("Missing audio bus effect parameters");
        std::optional<AssetError> error;
        std::visit(
            [&](const auto& params) {
                using P = std::decay_t<decltype(params)>;
                VisitAudioEffectFields(params, [&](const char* name, const auto& value, auto... range) {
                    if constexpr (sizeof...(range) == 2)
                    {
                        const auto [min, max] = std::array {range...};
                        if (!(value >= min && value <= max))
                            error = Invalid(std::string("Out-of-range audio bus effect parameter ") + name);
                    }
                    else if constexpr (std::is_same_v<P, AudioDucking>)
                    {
                        if (std::none_of(
                                buses.begin(), buses.end(), [&](const auto& bus) { return bus.Name == value; }))
                            error = Invalid("Unknown sidechain source audio bus: " + value);
                    }
                    else if (value.empty())
                        error = Invalid("Missing custom DSP effect name");
                });
            },
            parameters);
        return error;
    }

    std::optional<AssetError> ValidateAudioBusLayout(const AudioBusLayout& layout)
    {
        const auto&                             buses = layout.Buses;
        std::unordered_map<std::string, size_t> names;
        for (size_t i = 0; i < buses.size(); ++i)
        {
            const auto& bus = buses[i];
            if (bus.Name.empty() || !names.emplace(bus.Name, i).second)
                return Invalid("Empty or duplicate audio bus name: " + bus.Name);
            if (!IsValidAudioBusLevel(bus.VolumeDb))
                return Invalid("Invalid volume for audio bus " + bus.Name);
        }
        if (!names.count(MasterAudioBus))
            return Invalid("Audio bus layout requires a Master bus");

        std::vector<std::vector<size_t>> edges(buses.size());
        std::vector<size_t>              incoming(buses.size());
        for (size_t i = 0; i < buses.size(); ++i)
        {
            const auto& bus     = buses[i];
            const auto  addEdge = [&](const std::string& target) {
                const auto found = names.find(target);
                if (found == names.end())
                    return false;
                edges[i].push_back(found->second);
                ++incoming[found->second];
                return true;
            };
            if (bus.Name == MasterAudioBus ? !bus.Parent.empty() : !addEdge(bus.Parent))
                return Invalid("Invalid parent for audio bus " + bus.Name + ": " + bus.Parent);
            for (const auto& effect : bus.Effects)
                if (auto error = ValidateAudioBusEffect(effect.Parameters, buses))
                    return Invalid(bus.Name + ": " + error->Message);
            // One miniaudio output per send and one for the primary route.
            if (bus.Sends.size() > 253)
                return Invalid("Too many sends from audio bus " + bus.Name);
            for (const auto& send : bus.Sends)
            {
                if (!addEdge(send.Target))
                    return Invalid("Unknown send target from audio bus " + bus.Name + ": " + send.Target);
                if (!IsValidAudioBusLevel(send.LevelDb) ||
                    (send.Tap != AudioSendTap::PreFader && send.Tap != AudioSendTap::PostFader))
                    return Invalid("Invalid send from audio bus " + bus.Name + " to " + send.Target);
            }
        }

        std::vector<size_t> ready;
        for (size_t i = 0; i < incoming.size(); ++i)
            if (incoming[i] == 0)
                ready.push_back(i);
        for (size_t i = 0; i < ready.size(); ++i)
            for (size_t target : edges[ready[i]])
                if (--incoming[target] == 0)
                    ready.push_back(target);
        if (ready.size() != buses.size())
            return Invalid("Audio bus routing contains a cycle through parents or sends");
        return std::nullopt;
    }

    std::variant<AudioBusLayout, AssetError> ParseAudioBusLayout(const std::string& text)
    {
        try
        {
            const auto documents = YAML::LoadAll(text);
            if (documents.size() != 1 || !documents.front().IsMap())
                return Invalid("Expected one audio bus layout mapping");
            const auto& root = documents.front();
            const auto  version = root["Version"].as<unsigned>();
            if (version < 1 || version > BusLayoutVersion)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported audio bus layout version"};
            const auto buses = root["Buses"];
            if (!buses.IsSequence())
                return Invalid("Expected a Buses sequence");
            AudioBusLayout layout;
            for (const auto& entry : buses)
            {
                AudioBusDefinition bus;
                bus.Name     = entry["Name"].as<std::string>();
                bus.Parent   = ReadOr<std::string>(entry, "Parent", "");
                bus.VolumeDb = ReadOr<float>(entry, "VolumeDb", 0);
                bus.PinToCenter = ReadOr<bool>(entry, "PinToCenter", false);
                bus.Mute     = ReadOr<bool>(entry, "Mute", false);
                bus.Solo     = ReadOr<bool>(entry, "Solo", false);
                bus.Bypass   = ReadOr<bool>(entry, "Bypass", false);
                if (version >= 3)
                {
                    for (const char* field : {"FollowTimeScale", "PausesWithGame"})
                        if (const auto node = entry[field]; node && !IsBoolean(node))
                            return Invalid("Expected a boolean " + std::string(field) + " for audio bus " + bus.Name);
                    bus.FollowTimeScale = ReadOr<bool>(entry, "FollowTimeScale", false);
                    bus.PausesWithGame  = ReadOr<bool>(entry, "PausesWithGame", true);
                }
                if (const auto sends = entry["Sends"])
                {
                    if (!sends.IsSequence())
                        return Invalid("Expected a Sends sequence for audio bus " + bus.Name);
                    for (const auto& node : sends)
                    {
                        const auto tap = ReadOr<std::string>(node, "Tap", "PostFader");
                        if (tap != "PreFader" && tap != "PostFader")
                            return Invalid("Invalid send tap for audio bus " + bus.Name + ": " + tap);
                        bus.Sends.push_back({node["Target"].as<std::string>(),
                                             ReadOr<float>(node, "LevelDb", 0),
                                             tap == "PreFader" ? AudioSendTap::PreFader : AudioSendTap::PostFader});
                    }
                }
                if (const auto effects = entry["Effects"]; version >= 2 && effects)
                {
                    if (!effects.IsSequence())
                        return Invalid("Expected an Effects sequence for audio bus " + bus.Name);
                    for (const auto& node : effects)
                    {
                        const auto type  = node["Type"].as<std::string>();
                        const auto found = std::find(AudioEffectTypes.begin(), AudioEffectTypes.end(), type);
                        if (found == AudioEffectTypes.end())
                            return Invalid("Unknown audio bus effect type: " + type);
                        static const auto defaults =
                            EffectDefaults(std::make_index_sequence<std::variant_size_v<AudioBusEffectParameters>> {});
                        AudioBusEffect effect {defaults[found - AudioEffectTypes.begin()],
                                               ReadOr<bool>(node, "Enabled", true)};
                        if (const auto parameters = node["Parameters"])
                        {
                            if (!parameters.IsMap())
                                return Invalid("Expected a Parameters mapping for audio bus effect " + type);
                            std::visit(
                                [&](auto& params) {
                                    VisitAudioEffectFields(params, [&](const char* key, auto& value, auto...) {
                                        value = ReadOr(parameters, key, value);
                                    });
                                },
                                effect.Parameters);
                        }
                        bus.Effects.push_back(std::move(effect));
                    }
                }
                layout.Buses.push_back(std::move(bus));
            }
            if (auto error = ValidateAudioBusLayout(layout))
                return *error;
            return layout;
        }
        catch (const YAML::Exception& e)
        {
            return Invalid(e.what());
        }
    }

    std::variant<std::string, AssetError> SerializeAudioBusLayout(const AudioBusLayout& layout)
    {
        if (auto error = ValidateAudioBusLayout(layout))
            return *error;
        YAML::Emitter out;
        out.SetFloatPrecision(std::numeric_limits<float>::max_digits10);
        out << YAML::BeginMap << YAML::Key << "Version" << YAML::Value << BusLayoutVersion << YAML::Key << "Buses"
            << YAML::Value << YAML::BeginSeq;
        for (const auto& bus : layout.Buses)
        {
            out << YAML::BeginMap << YAML::Key << "Name" << YAML::Value << bus.Name << YAML::Key << "Parent"
                << YAML::Value << bus.Parent;
            out << YAML::Key << "FollowTimeScale" << YAML::Value << bus.FollowTimeScale << YAML::Key << "PausesWithGame"
                << YAML::Value << bus.PausesWithGame;
            if (bus.PinToCenter)
                out << YAML::Key << "PinToCenter" << YAML::Value << true;
            out << YAML::Key << "VolumeDb" << YAML::Value << bus.VolumeDb << YAML::Key << "Mute" << YAML::Value
                << bus.Mute << YAML::Key << "Solo" << YAML::Value << bus.Solo << YAML::Key << "Bypass" << YAML::Value
                << bus.Bypass << YAML::Key << "Sends" << YAML::Value << YAML::BeginSeq;
            for (const auto& send : bus.Sends)
                out << YAML::BeginMap << YAML::Key << "Target" << YAML::Value << send.Target << YAML::Key << "LevelDb"
                    << YAML::Value << send.LevelDb << YAML::Key << "Tap" << YAML::Value
                    << (send.Tap == AudioSendTap::PreFader ? "PreFader" : "PostFader") << YAML::EndMap;
            out << YAML::EndSeq << YAML::Key << "Effects" << YAML::Value << YAML::BeginSeq;
            for (const auto& effect : bus.Effects)
            {
                out << YAML::BeginMap << YAML::Key << "Type" << YAML::Value
                    << std::string(AudioEffectTypes[effect.Parameters.index()]) << YAML::Key << "Enabled" << YAML::Value
                    << effect.Enabled << YAML::Key << "Parameters" << YAML::Value << YAML::BeginMap;
                std::visit(
                    [&](const auto& params) {
                        VisitAudioEffectFields(params, [&](const char* key, const auto& value, auto...) {
                            out << YAML::Key << key << YAML::Value << value;
                        });
                    },
                    effect.Parameters);
                out << YAML::EndMap << YAML::EndMap;
            }
            out << YAML::EndSeq << YAML::EndMap;
        }
        out << YAML::EndSeq << YAML::EndMap;
        if (!out.good())
            return Invalid(out.GetLastError());
        return std::string(out.c_str());
    }
} // namespace Kans
