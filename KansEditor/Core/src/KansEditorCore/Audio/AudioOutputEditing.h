#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "Kans3D/Audio/AudioEngine.h"

// The audio output settings' model, without ImGui: the device and speaker-mode choices and the
// status lines of the Audio Mixer's Output pane.
namespace Kans::AudioOutputEditing
{
    // One entry of the output device dropdown. Id is what AudioEngine::SetOutputDevice takes.
    struct DeviceChoice
    {
        std::string Id, Label;
        bool        Selected = false;
        bool        Missing  = false; // The saved device is not connected.
    };
    // "System default (Speakers)" first (it follows the system), then each device with "(default)"
    // on the system's default; a selected id the list lacks is kept last as missing.
    std::vector<DeviceChoice> DeviceChoices(const std::vector<AudioOutputDevice>& devices, std::string_view selected);
    // The dropdown's closed text for `selected`.
    std::string SelectedDeviceLabel(const std::vector<AudioOutputDevice>& devices, std::string_view selected);

    // Speaker modes in SpeakerMode order; Auto names the channel count it resolved to.
    inline constexpr int SpeakerModeCount = 5;
    std::string          SpeakerModeLabel(SpeakerMode mode, uint32_t resolvedChannels);

    // The line under the device dropdown: why the device changed when the engine fell back from a
    // vanished device ("... disconnected; playing through the system default"); empty otherwise.
    std::string FallbackText(const AudioDeviceChange& change, const std::vector<AudioOutputDevice>& devices);

    // Shown instead of an empty device list: an Offline engine (renders without a device), a silent
    // null device, or no engine at all. Empty when the engine lists devices.
    std::string NoDevicesHint(const AudioEngine* engine, const std::vector<AudioOutputDevice>& devices);
} // namespace Kans::AudioOutputEditing
