#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "Kans3D/Audio/AudioEngine.h"

namespace Kans
{
    // The Audio Mixer's Output pane: this machine's output device (System default or a listed
    // device, refreshed when the engine reports a device change, with a note when it fell back from
    // a device that vanished), the speaker mode, and the player's own volume layer (a dB offset and
    // mute per bus, applied after the authored faders, like a game's options menu). These are per-user
    // settings, not project data: Save writes the engine's audio-settings.yaml, whose path it shows.
    class AudioOutputSettingsPanel
    {
    public:
        void Bind(AudioEngine* engine);
        void Draw();

        const std::string& GetStatus() const { return m_Status; }
        // Re-reads the engine's device list (also done on a device change and when the list opens).
        void                                  RefreshDevices();
        const std::vector<AudioOutputDevice>& GetDevices() const { return m_Devices; }

        // Where the last frame drew each control (screen space), for tooltips and UI tests.
        struct PlayerBusBoxes
        {
            std::string Bus;
            ImRect      Volume, Mute, Reset;
        };
        struct Boxes
        {
            ImRect                      Device, Refresh, Speakers, Save, ResetAll;
            std::string                 Hint, Fallback, File; // The texts drawn (empty when not).
            std::vector<PlayerBusBoxes> Buses;
            // Items of the open device or speaker-mode dropdown, by label.
            std::vector<std::pair<std::string, ImRect>> Menu;
        };
        const Boxes& GetBoxes() const { return m_Boxes; }

    private:
        void DrawOutput();
        void DrawPlayerVolume();

        AudioEngine*                   m_Engine = nullptr;
        std::vector<AudioOutputDevice> m_Devices;
        uint64_t                       m_DeviceRevision = UINT64_MAX;
        std::string                    m_Status;
        Boxes                          m_Boxes;
    };
} // namespace Kans
