#include "KansEditorCore/Audio/AudioOutputEditing.h"
#include <algorithm>

namespace Kans::AudioOutputEditing
{
    namespace
    {
        const AudioOutputDevice* Find(const std::vector<AudioOutputDevice>& devices, std::string_view id)
        {
            const auto found =
                std::find_if(devices.begin(), devices.end(), [&](const auto& device) { return device.Id == id; });
            return found == devices.end() ? nullptr : &*found;
        }

        const AudioOutputDevice* SystemDefault(const std::vector<AudioOutputDevice>& devices)
        {
            const auto found =
                std::find_if(devices.begin(), devices.end(), [](const auto& device) { return device.IsDefault; });
            return found == devices.end() ? nullptr : &*found;
        }

        std::string NameOf(const AudioOutputDevice& device) { return device.Name.empty() ? device.Id : device.Name; }

        std::string DefaultLabel(const std::vector<AudioOutputDevice>& devices)
        {
            const auto* system = SystemDefault(devices);
            return system ? "System default (" + NameOf(*system) + ")" : std::string("System default");
        }
    } // namespace

    std::vector<DeviceChoice> DeviceChoices(const std::vector<AudioOutputDevice>& devices, std::string_view selected)
    {
        const bool                followsDefault = selected.empty() || selected == DefaultAudioOutputDevice;
        std::vector<DeviceChoice> choices;
        choices.push_back({DefaultAudioOutputDevice, DefaultLabel(devices), followsDefault, false});
        for (const auto& device : devices)
            choices.push_back(
                {device.Id, NameOf(device) + (device.IsDefault ? " (default)" : ""), device.Id == selected, false});
        if (!followsDefault && !Find(devices, selected))
            choices.push_back({std::string(selected), std::string(selected) + " (not connected)", true, true});
        return choices;
    }

    std::string SelectedDeviceLabel(const std::vector<AudioOutputDevice>& devices, std::string_view selected)
    {
        for (const auto& choice : DeviceChoices(devices, selected))
            if (choice.Selected)
                return choice.Label;
        return DefaultLabel(devices);
    }

    std::string SpeakerModeLabel(SpeakerMode mode, uint32_t resolvedChannels)
    {
        switch (mode)
        {
            case SpeakerMode::Auto:
                return resolvedChannels ? "Auto (" + std::to_string(resolvedChannels) +
                                              (resolvedChannels == 1 ? " channel)" : " channels)") :
                                          std::string("Auto");
            case SpeakerMode::Mono:
                return "Mono";
            case SpeakerMode::Stereo:
                return "Stereo";
            case SpeakerMode::Surround51:
                return "5.1 surround";
            case SpeakerMode::Surround71:
                return "7.1 surround";
        }
        return "Auto";
    }

    std::string FallbackText(const AudioDeviceChange& change, const std::vector<AudioOutputDevice>& devices)
    {
        if (change.Reason != AudioDeviceChangeReason::FellBackToDefault || change.Revision == 0)
            return {};
        const auto* now = Find(devices, change.DeviceId);
        return "The chosen device disconnected; playing through the system default" +
               (now ? " (" + NameOf(*now) + ")." : std::string("."));
    }

    std::string NoDevicesHint(const AudioEngine* engine, const std::vector<AudioOutputDevice>& devices)
    {
        if (!engine || !engine->IsInitialized())
            return "Audio is unavailable: there is no output to choose.";
        if (!devices.empty())
            return {};
        if (engine->GetMode() == AudioOutputMode::Offline)
            return "Offline rendering: this engine plays through no device.";
        return "No output devices found: audio plays silently (null device).";
    }
} // namespace Kans::AudioOutputEditing
