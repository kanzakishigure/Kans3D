#include <algorithm>
#include <cstdio>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "KansEditorCore/Audio/AudioClipPreview.h"
#include "KansEditorCore/Audio/AudioEditing.h"
#include "KansEditorCore/Audio/AudioMixerEditing.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    namespace
    {
        std::string Clock(double seconds)
        {
            char       text[32];
            const auto minutes = int(std::max(0.0, seconds) / 60);
            std::snprintf(text, sizeof(text), "%d:%05.2f", minutes, std::max(0.0, seconds) - minutes * 60.0);
            return text;
        }

        // A row reserved for a later phase: its control is shown disabled with the phase that brings it.
        void Reserved(const char* label, const char* value, const char* why)
        {
            if (!PropertyGrid::Row(label, why))
                return;
            ImGui::BeginDisabled();
            ImGui::PushID(label);
            if (ImGui::BeginCombo("##reserved", value))
                ImGui::EndCombo();
            ImGui::PopID();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", why);
        }

        // Play mode drives the scene's own voice; edit mode auditions the clip as a plain 2D sound.
        void DrawPlayback(Entity entity, const AudioSourceComponent& c, ComponentUI& ui)
        {
            Scene* scene = ui.GetScene();
            if (scene && scene->IsAudioRunning())
            {
                if (!PropertyGrid::Row("Playback", "This source in the running scene."))
                    return;
                const bool playing = scene->IsAudioSourcePlaying(entity);
                if (ImGui::Button(playing ? ICON_FA_STOP " Stop##source" : ICON_FA_PLAY " Play##source"))
                {
                    if (playing)
                        scene->StopAudioSource(entity);
                    else
                        scene->PlayAudioSource(entity);
                }
                const auto* engine = scene->GetAudioEngine();
                const auto* assets = ui.GetAssets();
                const auto  clip   = assets ? assets->GetLoaded<AudioClipAsset>(c.Clip) : nullptr;
                if (playing && engine && clip && clip->GetData().Info.SampleRate)
                {
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled(
                        "%s",
                        Clock(double(engine->GetCursorFrames(c.GetVoiceHandle())) / clip->GetData().Info.SampleRate)
                            .c_str());
                }
                return;
            }
            if (!PropertyGrid::Row("Preview", "Hear the clip in the editor; edit mode is otherwise silent."))
                return;
            auto* preview = ui.GetAudioPreview();
            if (!preview || !preview->IsAvailable())
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Audio is unavailable");
                return;
            }
            ImGui::BeginDisabled(!c.Clip.IsValid());
            const bool playing = preview->IsPlaying(c.Clip);
            if (ImGui::Button(playing ? ICON_FA_STOP " Stop##preview" : ICON_FA_PLAY " Play##preview"))
            {
                if (playing)
                    preview->Stop();
                else
                    preview->Play(c.Clip, {c.Volume, c.Pitch, c.Loop, 0.0});
            }
            ImGui::EndDisabled();
            if (const auto position = preview->GetPosition(c.Clip))
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", Clock(*position).c_str());
            }
        }
        // The bus a source plays through: a dropdown of the active layout's buses. A name the layout
        // does not have is kept (it plays through Master) and shown in the warning colour.
        void DrawBus(AudioSourceComponent& c, ComponentUI& ui)
        {
            const AudioEngine* engine = ui.GetScene() ? ui.GetScene()->GetAudioEngine() : nullptr;
            if (!engine && ui.GetAudioPreview())
                engine = ui.GetAudioPreview()->GetEngine();
            static const AudioBusLayout fallback = AudioBusLayout::Default();
            const auto& buses = engine && !engine->GetBuses().empty() ? engine->GetBuses() : fallback.Buses;
            const bool  known =
                std::any_of(buses.begin(), buses.end(), [&](const auto& bus) { return bus.Name == c.Bus; });
            const bool modified = c.Bus != MasterAudioBus;
            if (!PropertyGrid::Row("Bus",
                                   known ? "The mixer bus this source plays through" :
                                           "Not a bus of the active layout: the source plays through Master",
                                   modified))
                return;
            ImGui::PushID("Bus");
            if (!known)
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Warning);
            char preview[160];
            std::snprintf(preview,
                          sizeof(preview),
                          known ? "%s" : ICON_FA_TRIANGLE_EXCLAMATION " %s (missing)",
                          c.Bus.empty() ? MasterAudioBus : c.Bus.c_str());
            const bool open = ImGui::BeginCombo("##value", preview);
            if (!known)
                ImGui::PopStyleColor();
            if (open)
            {
                for (const auto& entry : AudioMixerEditing::TreeOrder(AudioBusLayout {buses}))
                {
                    const auto& name = buses[entry.Index].Name;
                    ImGui::Indent(EditorTheme::Px(10.0f) * float(entry.Depth) + 1.0f);
                    if (ImGui::Selectable(name.c_str(), name == c.Bus))
                        c.Bus = name;
                    ImGui::Unindent(EditorTheme::Px(10.0f) * float(entry.Depth) + 1.0f);
                }
                ImGui::EndCombo();
            }
            if (PropertyGrid::RowReset(modified))
                c.Bus = MasterAudioBus;
            ImGui::PopID();
        }
    } // namespace

    void DrawAudioSource(Entity entity, ComponentUI& ui)
    {
        static const AudioSourceComponent defaults;
        entity.PatchComponent<AudioSourceComponent>([&](AudioSourceComponent& c) {
            // The groups keep this order; later phases add rows to them: the Event under Source (A2b),
            // Priority under Playback (A2b), the attenuation asset under
            // Spatial (A2c).
            if (PropertyGrid::BeginGroup("Source"))
            {
                ui.AssetPicker("Clip", AssetType::AudioClip, c.Clip);
                Reserved("Event", "None", "AudioEvent assets (randomized clips) arrive in A2b.");
                DrawPlayback(entity, c, ui);
                if (const auto& error = c.GetLastError(); error && PropertyGrid::Row("Error"))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
                    ImGui::TextWrapped("%s", error->Message.c_str());
                    ImGui::PopStyleColor();
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Playback"))
            {
                float       decibels = AudioEditing::VolumeToDecibels(c.Volume);
                const float reset    = AudioEditing::VolumeToDecibels(defaults.Volume);
                if (PropertyGrid::Float("Volume",
                                        decibels,
                                        0.1f,
                                        AudioEditing::SilenceDecibels,
                                        AudioEditing::MaximumDecibels(),
                                        c.Volume <= 0.0f ? "Silent" : "%.1f dB",
                                        &reset))
                    c.Volume = AudioEditing::DecibelsToVolume(decibels);
                PropertyGrid::Float("Pitch", c.Pitch, 0.01f, 0.01f, 4.0f, "%.2fx", &defaults.Pitch);
                PropertyGrid::Checkbox("Loop", c.Loop, &defaults.Loop);
                PropertyGrid::Checkbox("Play on start", c.PlayOnStart, &defaults.PlayOnStart);
                Reserved("Priority", "Default", "Voice priority and limits arrive in A2b.");
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Output"))
            {
                DrawBus(c, ui);
                if (!c.Spatial)
                    PropertyGrid::Float("Pan", c.Pan, 0.01f, -1.0f, 1.0f, "%.2f", &defaults.Pan);
                PropertyGrid::EndGroup();
            }
            bool toggled = false;
            if (PropertyGrid::BeginGroup("Spatial", &c.Spatial, &toggled))
            {
                const char* models[] = {"None", "Inverse", "Linear", "Exponential"};
                int         model = int(c.Attenuation), modelReset = int(defaults.Attenuation);
                if (PropertyGrid::Combo("Attenuation", models, 4, model, &modelReset))
                    c.Attenuation = AudioAttenuationModel(model);
                Reserved("Attenuation asset", "Built-in model", "Shared attenuation curves arrive in A2c.");
                PropertyGrid::Float(
                    "Min distance", c.MinDistance, 0.05f, 0.001f, 100000.0f, "%.2f m", &defaults.MinDistance);
                PropertyGrid::Float(
                    "Max distance", c.MaxDistance, 0.5f, c.MinDistance, 100000.0f, "%.2f m", &defaults.MaxDistance);
                if (c.Attenuation != AudioAttenuationModel::None)
                    PropertyGrid::Float("Rolloff", c.Rolloff, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.Rolloff);
                PropertyGrid::Float("Doppler", c.DopplerFactor, 0.01f, 0.0f, 10.0f, "%.2f", &defaults.DopplerFactor);
                PropertyGrid::EndGroup();
            }
            if (toggled && ui.GetScene() && ui.GetScene()->IsAudioRunning())
                ImGui::SetTooltip("Takes effect the next time the source starts.");
            if (c.Spatial && PropertyGrid::BeginGroup("Cone"))
            {
                PropertyGrid::Float(
                    "Inner angle", c.ConeInnerAngle, 0.5f, 0.0f, c.ConeOuterAngle, "%.1f°", &defaults.ConeInnerAngle);
                PropertyGrid::Float(
                    "Outer angle", c.ConeOuterAngle, 0.5f, 0.0f, 360.0f, "%.1f°", &defaults.ConeOuterAngle);
                PropertyGrid::Float("Outer gain", c.ConeOuterGain, 0.01f, 0.0f, 1.0f, "%.2f", &defaults.ConeOuterGain);
                PropertyGrid::Text("Aim", "Transform local -Z", true);
                PropertyGrid::EndGroup();
            }
            AudioEditing::Validate(c);
        });
    }

    void DrawAudioListener(Entity entity, ComponentUI&)
    {
        static const AudioListenerComponent defaults;
        entity.PatchComponent<AudioListenerComponent>([&](AudioListenerComponent& c) {
            if (PropertyGrid::BeginGroup("Listener"))
            {
                PropertyGrid::Checkbox("Active", c.Active, &defaults.Active);
                PropertyGrid::Text("Hears",
                                   "The scene from here instead of the primary camera. With several active "
                                   "listeners, the one with the lowest entity ID is used.",
                                   true);
                PropertyGrid::EndGroup();
            }
        });
    }
} // namespace Kans::ComponentEditors
