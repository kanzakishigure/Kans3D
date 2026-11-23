#include "kspch.h"
#include "AudioBusLayout.h"

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
            if (root["Version"].as<unsigned>() != 1)
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
                bus.Mute     = ReadOr<bool>(entry, "Mute", false);
                bus.Solo     = ReadOr<bool>(entry, "Solo", false);
                bus.Bypass   = ReadOr<bool>(entry, "Bypass", false);
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
        out << YAML::BeginMap << YAML::Key << "Version" << YAML::Value << 1 << YAML::Key << "Buses" << YAML::Value
            << YAML::BeginSeq;
        for (const auto& bus : layout.Buses)
        {
            out << YAML::BeginMap << YAML::Key << "Name" << YAML::Value << bus.Name << YAML::Key << "Parent"
                << YAML::Value << bus.Parent << YAML::Key << "VolumeDb" << YAML::Value << bus.VolumeDb << YAML::Key
                << "Mute" << YAML::Value << bus.Mute << YAML::Key << "Solo" << YAML::Value << bus.Solo << YAML::Key
                << "Bypass" << YAML::Value << bus.Bypass << YAML::Key << "Sends" << YAML::Value << YAML::BeginSeq;
            for (const auto& send : bus.Sends)
                out << YAML::BeginMap << YAML::Key << "Target" << YAML::Value << send.Target << YAML::Key << "LevelDb"
                    << YAML::Value << send.LevelDb << YAML::Key << "Tap" << YAML::Value
                    << (send.Tap == AudioSendTap::PreFader ? "PreFader" : "PostFader") << YAML::EndMap;
            out << YAML::EndSeq << YAML::EndMap;
        }
        out << YAML::EndSeq << YAML::EndMap;
        if (!out.good())
            return Invalid(out.GetLastError());
        return std::string(out.c_str());
    }
} // namespace Kans
