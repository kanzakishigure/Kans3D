#include "kspch.h"
#include "AudioUserSettings.h"

#include <array>
#include <cmath>
#include <fstream>
#include "Kans3D/Project/ProjectYaml.h"
#include "Kans3D/Project/RecentProjects.h"

namespace Kans
{
    namespace
    {
        constexpr std::array ModeNames {"Auto", "Mono", "Stereo", "5.1", "7.1"};
        constexpr int        CurrentVersion = 1;

        template<class T>
        T Read(const YAML::Node&         node,
               const char*               key,
               T                         fallback,
               std::vector<std::string>& reasons,
               const std::string&        prefix = {})
        {
            const auto field = node[key];
            if (!field)
                return fallback;
            try
            {
                return field.as<T>();
            }
            catch (const YAML::Exception&)
            {
                reasons.push_back(prefix + key + " has an invalid value; using the default");
                return fallback;
            }
        }
    } // namespace

    std::filesystem::path AudioUserSettings::DefaultPath()
    {
        return RecentProjects::DefaultStorageFile().parent_path() / "audio-settings.yaml";
    }

    AudioUserSettings AudioUserSettings::Load(const std::filesystem::path& file, std::vector<std::string>& reasons)
    {
        AudioUserSettings settings;
        try
        {
            if (!std::filesystem::exists(file))
                return settings;
            std::ifstream input(file);
            if (!input)
            {
                reasons.emplace_back("Cannot read audio settings");
                return settings;
            }
            const auto node = YAML::Load(input);
            if (!node.IsMap())
            {
                reasons.emplace_back("Audio settings must be a mapping; using defaults");
                return settings;
            }
            const auto version = Read(node, "Version", 0, reasons);
            if (version < 0 || version > CurrentVersion)
                reasons.emplace_back("Unknown audio settings version; reading recognized fields");
            const auto mode       = Read(node, "SpeakerMode", std::string("Auto"), reasons);
            const auto found      = std::find(ModeNames.begin(), ModeNames.end(), mode);
            settings.Speakers     = static_cast<SpeakerMode>(found - ModeNames.begin());
            settings.OutputDevice = Read(node, "OutputDevice", settings.OutputDevice, reasons);
            if (const auto buses = node["Buses"]; buses && buses.IsMap())
                for (const auto& entry : buses)
                {
                    if (!entry.first.IsScalar() || !entry.second.IsMap())
                        continue;
                    const auto name      = entry.first.Scalar();
                    settings.Buses[name] = {Read(entry.second, "VolumeDb", 0.f, reasons, name + "."),
                                            Read(entry.second, "Mute", false, reasons, name + ".")};
                }
            settings.Normalize(reasons);
        }
        catch (const YAML::Exception& error)
        {
            reasons.emplace_back(error.what());
        }
        catch (const std::filesystem::filesystem_error& error)
        {
            reasons.emplace_back(error.what());
        }
        return settings;
    }

    void AudioUserSettings::Normalize(std::vector<std::string>& reasons)
    {
        if (static_cast<size_t>(Speakers) >= ModeNames.size())
        {
            Speakers = SpeakerMode::Auto;
            reasons.emplace_back("Invalid SpeakerMode; using Auto");
        }
        if (OutputDevice.empty() || OutputDevice.size() > 1024 || OutputDevice.find('\0') != std::string::npos)
        {
            OutputDevice = DefaultAudioOutputDevice;
            reasons.emplace_back("Invalid OutputDevice ID; using default");
        }
        for (auto& [bus, value] : Buses)
        {
            if (std::isnan(value.VolumeDb))
            {
                value.VolumeDb = 0;
                reasons.push_back(bus + ".VolumeDb is NaN; using 0 dB");
            }
            else if (value.VolumeDb < -80 || value.VolumeDb > 12)
            {
                value.VolumeDb = std::clamp(value.VolumeDb, -80.f, 12.f);
                reasons.push_back(bus + ".VolumeDb clamped to [-80, 12] dB");
            }
        }
    }

    std::optional<std::string> AudioUserSettings::Save(const std::filesystem::path& file) const
    {
        auto                     normalized = *this;
        std::vector<std::string> reasons;
        normalized.Normalize(reasons);
        YAML::Emitter out;
        out << YAML::BeginMap << YAML::Key << "Version" << YAML::Value << CurrentVersion << YAML::Key << "SpeakerMode"
            << YAML::Value << ModeNames[static_cast<size_t>(normalized.Speakers)] << YAML::Key << "OutputDevice"
            << YAML::Value << normalized.OutputDevice << YAML::Key << "Buses" << YAML::Value << YAML::BeginMap;
        for (const auto& [bus, value] : normalized.Buses)
            out << YAML::Key << bus << YAML::Value << YAML::BeginMap << YAML::Key << "VolumeDb" << YAML::Value
                << value.VolumeDb << YAML::Key << "Mute" << YAML::Value << value.Mute << YAML::EndMap;
        out << YAML::EndMap << YAML::EndMap;
        std::error_code error;
        if (!file.parent_path().empty())
            std::filesystem::create_directories(file.parent_path(), error);
        if (error)
            return error.message();
        if (auto failure = ProjectYaml::Save(file, out))
            return failure->Message;
        return std::nullopt;
    }
} // namespace Kans
