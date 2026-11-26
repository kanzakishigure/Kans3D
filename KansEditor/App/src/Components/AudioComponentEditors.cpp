#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Audio/AudioEventPlayer.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "KansEditorCore/Audio/AudioAttenuationEditing.h"
#include "KansEditorCore/Audio/AudioClipPreview.h"
#include "KansEditorCore/Audio/AudioEditing.h"
#include "KansEditorCore/Audio/AudioEventEditing.h"
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

        ImRect LastItem() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

        // A float row whose field's rectangle is kept in `box`.
        bool FloatRow(const char*  label,
                      float&       value,
                      float        speed,
                      float        lower,
                      float        upper,
                      const char*  format,
                      const float* reset,
                      ImRect&      box,
                      const char*  tooltip = nullptr)
        {
            const bool modified = reset && std::abs(value - *reset) > 1e-6f;
            if (!PropertyGrid::Row(label, tooltip, modified))
                return false;
            ImGui::PushID(label);
            bool changed =
                ImGui::DragFloat("##value", &value, speed, lower, upper, format, ImGuiSliderFlags_AlwaysClamp);
            box = LastItem();
            if (PropertyGrid::RowReset(modified))
                value = *reset, changed = true;
            ImGui::PopID();
            return changed;
        }

        // A checkbox row whose box is kept in `box`; disabled rows say why in their tooltip.
        bool CheckboxRow(const char* label,
                         bool&       value,
                         const bool* reset,
                         ImRect&     box,
                         const char* tooltip = nullptr,
                         bool        enabled = true)
        {
            const bool modified = reset && value != *reset;
            if (!PropertyGrid::Row(label, tooltip, modified))
                return false;
            ImGui::PushID(label);
            ImGui::BeginDisabled(!enabled);
            bool changed = ImGui::Checkbox("##value", &value);
            ImGui::EndDisabled();
            box = LastItem();
            if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tooltip);
            if (enabled && PropertyGrid::RowReset(modified))
                value = *reset, changed = true;
            ImGui::PopID();
            return changed;
        }

        // One button per choice in a row (the chosen one lit); `boxes` receives each button.
        bool ChoiceRow(const char* label, const char* const names[], int count, int& index, ImRect* boxes, int reset)
        {
            const bool modified = index != reset;
            if (!PropertyGrid::Row(label, nullptr, modified))
                return false;
            const auto& p       = EditorTheme::Colors();
            bool        changed = false;
            ImGui::PushID(label);
            const float spacing = ImGui::GetStyle().ItemSpacing.x * 0.5f;
            const float width   = (ImGui::CalcItemWidth() - spacing * float(count - 1)) / float(count);
            for (int i = 0; i < count; ++i)
            {
                if (i)
                    ImGui::SameLine(0.0f, spacing);
                const bool chosen = i == index;
                if (chosen)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, p.Selection);
                    ImGui::PushStyleColor(ImGuiCol_Text, p.Text);
                }
                if (ImGui::Button(names[i], {width, 0}) && !chosen)
                    index = i, changed = true;
                if (chosen)
                    ImGui::PopStyleColor(2);
                boxes[i] = LastItem();
            }
            if (PropertyGrid::RowReset(modified))
                index = reset, changed = true;
            ImGui::PopID();
            return changed;
        }

        // The volume curve of an attenuation asset, read-only, small.
        void CurvePreview(const AudioAttenuationData& data, ImRect& box)
        {
            if (!PropertyGrid::Row("", "The asset's volume curve (distance across, gain up)"))
                return;
            const auto&  p      = EditorTheme::Colors();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 size {std::max(EditorTheme::Px(60.0f), ImGui::CalcItemWidth()), EditorTheme::Px(44.0f)};
            ImGui::Dummy(size);
            box        = LastItem();
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(box.Min, box.Max, ImGui::GetColorU32(p.Inset), EditorTheme::Px(3.0f));
            const auto    view   = AudioAttenuationEditing::AutoView(data, AudioAttenuationEditing::CurveKind::Volume);
            constexpr int Points = 64;
            ImVec2        line[Points];
            for (int i = 0; i < Points; ++i)
            {
                const float distance =
                    view.DistanceStart + (view.DistanceEnd - view.DistanceStart) * float(i) / float(Points - 1);
                const auto at = AudioAttenuationEditing::ToPlot(
                    view,
                    {origin.x + 2.0f, origin.y + 2.0f},
                    {size.x - 4.0f, size.y - 4.0f},
                    distance,
                    AudioAttenuationEditing::Sample(data, AudioAttenuationEditing::CurveKind::Volume, distance));
                line[i] = {at.x, at.y};
            }
            draw->AddPolyline(line, Points, ImGui::GetColorU32(p.TypeAudio), 0, EditorTheme::Px(1.5f));
            char extent[32];
            std::snprintf(extent, sizeof(extent), "%.0f m", view.DistanceEnd);
            const ImVec2 text = ImGui::CalcTextSize(extent);
            draw->AddText({box.Max.x - text.x - EditorTheme::Px(4.0f), box.Max.y - text.y - EditorTheme::Px(2.0f)},
                          ImGui::GetColorU32(p.TextDim),
                          extent);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Volume over distance, 0 to %.1f m", view.DistanceEnd);
        }

        // The attenuation asset a source plays with: its own, else its event's (none while loading).
        Ref<const AudioAttenuationAsset>
        AttenuationOf(AssetManager* assets, const AudioSourceComponent& c, bool& fromEvent)
        {
            fromEvent = false;
            if (!assets)
                return nullptr;
            AssetID id = c.AttenuationAsset;
            if (!id.IsValid() && c.Event.IsValid())
                if (const auto event = assets->Request<AudioEventAsset>(c.Event).Asset)
                    id = event->GetData().AttenuationAsset, fromEvent = id.IsValid();
            if (!id.IsValid())
                return nullptr;
            return assets->Request<AudioAttenuationAsset>(id).Asset;
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
    } // namespace

    AudioCardBoxes& LastAudioCardBoxes()
    {
        static AudioCardBoxes boxes;
        return boxes;
    }

    bool AudioSendRow(const char* label, float& db, const char* tooltip, ImRect* box)
    {
        const bool modified = std::isfinite(db);
        if (!PropertyGrid::Row(label, tooltip, modified))
            return false;
        ImGui::PushID(label);
        float field   = AudioEditing::SendToField(db);
        bool  changed = false;
        if (ImGui::DragFloat("##value",
                             &field,
                             0.1f,
                             AudioEditing::SilenceDecibels,
                             12.0f,
                             field <= AudioEditing::SilenceDecibels ? "Off" : "%.1f dB",
                             ImGuiSliderFlags_AlwaysClamp))
        {
            const float next = AudioEditing::FieldToSend(field);
            changed          = !(next == db || (!std::isfinite(next) && !std::isfinite(db)));
            db               = next;
        }
        if (box)
            *box = LastItem();
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("%s\nDrag to the left end for Off.", tooltip ? tooltip : label);
        if (PropertyGrid::RowReset(modified))
            db = AudioSendOffDb, changed = true;
        ImGui::PopID();
        return changed;
    }

    namespace
    {
        const std::vector<AudioBusDefinition>& BusesOf(const AudioEngine* engine)
        {
            static const AudioBusLayout fallback = AudioBusLayout::Default();
            return engine && !engine->GetBuses().empty() ? engine->GetBuses() : fallback.Buses;
        }
    } // namespace

    bool HasAudioBus(const AudioEngine* engine, const std::string& bus)
    {
        const auto& buses = BusesOf(engine);
        return std::any_of(buses.begin(), buses.end(), [&](const auto& definition) { return definition.Name == bus; });
    }

    bool AudioBusRow(const char*        label,
                     std::string&       bus,
                     const AudioEngine* engine,
                     const char*        inherited,
                     const char*        fallback,
                     const char*        missing,
                     ImRect*            box)
    {
        const auto& buses    = BusesOf(engine);
        const bool  known    = HasAudioBus(engine, bus);
        const bool  modified = bus != fallback;
        if (!PropertyGrid::Row(label,
                               !known    ? (missing ? missing :
                                                      "Not a bus of the active layout: the sound plays "
                                                      "through Master") :
                               inherited ? inherited :
                                           "The mixer bus this sound plays through",
                               modified))
            return false;
        ImGui::PushID(label);
        if (!known)
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Warning);
        char preview[160];
        std::snprintf(preview,
                      sizeof(preview),
                      !known    ? ICON_FA_TRIANGLE_EXCLAMATION " %s (missing)" :
                      inherited ? "%s (from the event)" :
                                  "%s",
                      bus.empty() ? MasterAudioBus : bus.c_str());
        const bool open = ImGui::BeginCombo("##value", preview);
        if (!known)
            ImGui::PopStyleColor();
        if (box)
            *box = LastItem();
        if (!known && !open && ImGui::IsItemHovered() && missing)
            ImGui::SetTooltip("%s", missing);
        bool changed = false;
        if (open)
        {
            for (const auto& entry : AudioMixerEditing::TreeOrder(AudioBusLayout {buses}))
            {
                const auto& name = buses[entry.Index].Name;
                ImGui::Indent(EditorTheme::Px(10.0f) * float(entry.Depth) + 1.0f);
                if (ImGui::Selectable(name.c_str(), name == bus) && name != bus)
                    bus = name, changed = true;
                ImGui::Unindent(EditorTheme::Px(10.0f) * float(entry.Depth) + 1.0f);
            }
            ImGui::EndCombo();
        }
        if (PropertyGrid::RowReset(modified))
            bus = fallback, changed = true;
        ImGui::PopID();
        return changed;
    }

    void DrawAudioSource(Entity entity, ComponentUI& ui)
    {
        static const AudioSourceComponent defaults;
        entity.PatchComponent<AudioSourceComponent>([&](AudioSourceComponent& c) {
            // The groups keep this order: Source, Playback, Output, Spatial, Cone.
            const auto hints = AudioEditing::EventHintsOf(c);
            if (PropertyGrid::BeginGroup("Source"))
            {
                // An Event picks the clip itself: the Clip row stays editable but dimmed.
                if (hints.Clip)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
                ui.AssetPicker("Clip", AssetType::AudioClip, c.Clip);
                if (hints.Clip)
                {
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", hints.Clip);
                }
                ui.AssetPicker("Event", AssetType::AudioEvent, c.Event);
                if (hints.Clip && PropertyGrid::Row(""))
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", hints.Clip);
                }
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
                const bool modified = c.Priority != defaults.Priority;
                if (PropertyGrid::Row("Priority",
                                      hints.Priority ? hints.Priority :
                                                       "Which voices keep playing when too many sound at once (255 "
                                                       "is highest)",
                                      modified))
                {
                    ImGui::PushID("Priority");
                    int priority = c.Priority;
                    if (ImGui::SliderInt("##value",
                                         &priority,
                                         0,
                                         255,
                                         hints.Priority ? "%d (from the event)" : "%d",
                                         ImGuiSliderFlags_AlwaysClamp))
                        c.Priority = uint8_t(priority);
                    if (PropertyGrid::RowReset(modified))
                        c.Priority = defaults.Priority;
                    ImGui::PopID();
                }
                PropertyGrid::InputText("Concurrency group", c.ConcurrencyGroup);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Sounds sharing a group are limited together (empty: none)");
                PropertyGrid::EndGroup();
            }
            auto& boxes  = LastAudioCardBoxes();
            boxes.Source = {};
            if (PropertyGrid::BeginGroup("Output"))
            {
                const AudioEngine* engine = ui.GetScene() ? ui.GetScene()->GetAudioEngine() : nullptr;
                if (!engine && ui.GetAudioPreview())
                    engine = ui.GetAudioPreview()->GetEngine();
                AudioBusRow("Bus", c.Bus, engine, hints.Bus);
                if (!c.Spatial)
                    PropertyGrid::Float("Pan", c.Pan, 0.01f, -1.0f, 1.0f, "%.2f", &defaults.Pan);
                CheckboxRow("Reverb zone send",
                            c.ReverbZoneSend,
                            &defaults.ReverbZoneSend,
                            boxes.Source.ReverbZoneSend,
                            "Reverb zones around the listener send this sound to their buses");
                AudioSendRow("LFE send",
                             c.LfeSendDb,
                             "Also feed the subwoofer channel (surround outputs) at this level",
                             &boxes.Source.LfeSend);
                // Only a 2D sound has no position to pan from, so only it can be pinned.
                boxes.Source.PinToCenterEnabled = !c.Spatial;
                CheckboxRow("Pin to center",
                            c.PinToCenter,
                            &defaults.PinToCenter,
                            boxes.Source.PinToCenter,
                            c.Spatial ? "Only a 2D source can be pinned: turn Spatial off first (a 3D source is "
                                        "panned by its position)" :
                                        "Play from the centre speaker (half to each side in stereo), e.g. dialogue",
                            !c.Spatial);
                PropertyGrid::EndGroup();
            }
            bool toggled = false;
            if (PropertyGrid::BeginGroup("Spatial", &c.Spatial, &toggled))
            {
                // Shape: the closest point of a sphere, box or line is where the sound comes from.
                const char* shapes[] = {"Point", "Sphere", "Box", "Line"};
                int         shape    = int(c.Shape.Type);
                if (ChoiceRow("Shape", shapes, 4, shape, boxes.Source.Shape, int(defaults.Shape.Type)))
                    c.Shape.Type = AudioSourceShape(shape);
                const auto& d = defaults.Shape;
                switch (c.Shape.Type)
                {
                    case AudioSourceShape::Point:
                        break;
                    case AudioSourceShape::Sphere:
                        FloatRow("Radius",
                                 c.Shape.Radius,
                                 0.01f,
                                 0.0f,
                                 100000.0f,
                                 "%.2f m",
                                 &d.Radius,
                                 boxes.Source.Size[0]);
                        break;
                    case AudioSourceShape::Box:
                        if (PropertyGrid::Row("Half extents",
                                              "Half the box's size along its local axes",
                                              c.Shape.HalfExtents != d.HalfExtents))
                        {
                            ImGui::PushID("HalfExtents");
                            const float spacing = ImGui::GetStyle().ItemSpacing.x * 0.5f;
                            const float width   = (ImGui::CalcItemWidth() - 2.0f * spacing) / 3.0f;
                            for (int axis = 0; axis < 3; ++axis)
                            {
                                if (axis)
                                    ImGui::SameLine(0.0f, spacing);
                                ImGui::PushID(axis);
                                ImGui::SetNextItemWidth(width);
                                const char* formats[] = {"X %.2f", "Y %.2f", "Z %.2f"};
                                ImGui::DragFloat("##axis",
                                                 &c.Shape.HalfExtents[axis],
                                                 0.01f,
                                                 0.0f,
                                                 100000.0f,
                                                 formats[axis],
                                                 ImGuiSliderFlags_AlwaysClamp);
                                boxes.Source.Size[axis] = LastItem();
                                ImGui::PopID();
                            }
                            if (PropertyGrid::RowReset(c.Shape.HalfExtents != d.HalfExtents))
                                c.Shape.HalfExtents = d.HalfExtents;
                            ImGui::PopID();
                        }
                        break;
                    case AudioSourceShape::Line:
                        FloatRow("Length",
                                 c.Shape.Length,
                                 0.01f,
                                 0.0f,
                                 100000.0f,
                                 "%.2f m",
                                 &d.Length,
                                 boxes.Source.Size[0],
                                 "The line runs along the local X axis, centred on the entity");
                        break;
                }

                // A shared attenuation asset replaces the built-in model (and the cone when it has one).
                ui.AssetPicker("Attenuation asset", AssetType::AudioAttenuation, c.AttenuationAsset);
                boxes.Source.AttenuationAsset = LastItem();
                bool       fromEvent          = false;
                const auto asset              = AttenuationOf(ui.GetAssets(), c, fromEvent);
                const bool overridden         = c.AttenuationAsset.IsValid() || fromEvent;
                if (asset)
                    CurvePreview(asset->GetData(), boxes.Source.AttenuationPreview);
                const char* why = c.AttenuationAsset.IsValid() ? "Overridden by the attenuation asset" :
                                                                 "Overridden by the event's attenuation asset";
                boxes.Source.ModelOverridden = overridden;
                if (overridden)
                {
                    if (PropertyGrid::Row(""))
                    {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", why);
                    }
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
                }
                const char* models[] = {"None", "Inverse", "Linear", "Exponential"};
                int         model = int(c.Attenuation), modelReset = int(defaults.Attenuation);
                if (PropertyGrid::Combo("Attenuation", models, 4, model, &modelReset))
                    c.Attenuation = AudioAttenuationModel(model);
                if (overridden && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", why);
                PropertyGrid::Float(
                    "Min distance", c.MinDistance, 0.05f, 0.001f, 100000.0f, "%.2f m", &defaults.MinDistance);
                PropertyGrid::Float(
                    "Max distance", c.MaxDistance, 0.5f, c.MinDistance, 100000.0f, "%.2f m", &defaults.MaxDistance);
                if (c.Attenuation != AudioAttenuationModel::None)
                    PropertyGrid::Float("Rolloff", c.Rolloff, 0.01f, 0.0f, 100.0f, "%.2f", &defaults.Rolloff);
                PropertyGrid::Float("Doppler", c.DopplerFactor, 0.01f, 0.0f, 10.0f, "%.2f", &defaults.DopplerFactor);
                if (overridden)
                    ImGui::PopStyleVar();
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

    void DrawAudioStats(const AudioStatsInput& input)
    {
        if (PropertyGrid::BeginCard("Audio", ICON_FA_VOLUME_HIGH, "Audio"))
        {
            PropertyGrid::Begin("Audio");
            const auto* engine = input.Engine && input.Engine->IsInitialized() ? input.Engine : nullptr;
            PropertyGrid::Text("Engine",
                               engine ? std::to_string(engine->GetSampleRate()) + " Hz, " +
                                            std::to_string(engine->GetChannels()) + " channels" :
                                        std::string("unavailable"),
                               !engine);
            if (engine)
                for (const auto& [label, value] : AudioEventEditing::VoiceStatsRows(engine->GetVoiceStats()))
                    PropertyGrid::Text(label.c_str(), value);
            PropertyGrid::Text("Timeline voices", std::to_string(input.TimelineVoices));
            char drift[32];
            std::snprintf(drift, sizeof(drift), "%+.1f ms", input.TimelineDriftSeconds * 1000.0);
            PropertyGrid::Text("Timeline drift", input.TimelineVoices ? drift : "-", !input.TimelineVoices);
            PropertyGrid::End();
            // Events triggered in Play, one row each.
            if (engine && input.Playing && input.EventName)
            {
                const auto& player = engine->GetEventPlayer();
                bool        any    = false;
                for (const auto& event : player.GetTriggeredEvents())
                {
                    const auto name = input.EventName(event);
                    if (name.empty())
                        continue;
                    if (!any && !PropertyGrid::BeginGroup("Events"))
                        break;
                    any = true;
                    PropertyGrid::Text(name.c_str(), AudioEventEditing::EventStatsText(player.GetStats(event)));
                }
                if (any)
                    PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();
    }

    void DrawAudioListener(Entity entity, ComponentUI& ui)
    {
        static const AudioListenerComponent defaults;
        auto&                               boxes = LastAudioCardBoxes();
        boxes.ListenerSlot.clear();
        entity.PatchComponent<AudioListenerComponent>([&](AudioListenerComponent& c) {
            if (PropertyGrid::BeginGroup("Listener"))
            {
                PropertyGrid::Checkbox("Active", c.Active, &defaults.Active);
                // The engine slot this listener gets (split screen): active listeners in entity ID order.
                Scene*         scene    = ui.GetScene();
                const uint32_t capacity = scene ? scene->GetAudioListenerCapacity() : MaxAudioListeners;
                const auto     slot     = scene ? scene->GetAudioListenerSlot(entity) : std::nullopt;
                boxes.ListenerSlot      = AudioEditing::ListenerSlotText(c.Active, slot, capacity);
                if (PropertyGrid::Row("Slot",
                                      "Active listeners take the engine's listener slots in entity ID order; each "
                                      "3D sound is heard by the closest one"))
                {
                    ImGui::AlignTextToFramePadding();
                    if (slot || !c.Active)
                        ImGui::TextDisabled("%s", boxes.ListenerSlot.c_str());
                    else
                        ImGui::TextColored(EditorTheme::Colors().Warning,
                                           ICON_FA_TRIANGLE_EXCLAMATION " %s",
                                           boxes.ListenerSlot.c_str());
                }
                PropertyGrid::Text("Hears",
                                   "The scene from here instead of the primary camera. Up to " +
                                       std::to_string(capacity) + " active listeners are heard at once (split screen).",
                                   true);
                PropertyGrid::EndGroup();
            }
        });
    }

    void DrawAudioReverbZone(Entity entity, ComponentUI& ui)
    {
        static const AudioReverbZoneComponent defaults;
        auto&                                 boxes = LastAudioCardBoxes();
        boxes.Zone                                  = {};
        entity.PatchComponent<AudioReverbZoneComponent>([&](AudioReverbZoneComponent& c) {
            if (PropertyGrid::BeginGroup("Zone"))
            {
                const char* shapes[] = {"Sphere", "Box"};
                int         shape    = int(c.Shape);
                if (ChoiceRow("Shape", shapes, 2, shape, boxes.Zone.Shape, int(defaults.Shape)))
                    c.Shape = AudioReverbZoneShape(shape);
                if (c.Shape == AudioReverbZoneShape::Sphere)
                    FloatRow(
                        "Radius", c.Radius, 0.01f, 0.0f, 100000.0f, "%.2f m", &defaults.Radius, boxes.Zone.Size[0]);
                else if (PropertyGrid::Row("Half extents",
                                           "Half the zone's size along its local axes",
                                           c.HalfExtents != defaults.HalfExtents))
                {
                    ImGui::PushID("HalfExtents");
                    const float spacing = ImGui::GetStyle().ItemSpacing.x * 0.5f;
                    const float width   = (ImGui::CalcItemWidth() - 2.0f * spacing) / 3.0f;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        if (axis)
                            ImGui::SameLine(0.0f, spacing);
                        ImGui::PushID(axis);
                        ImGui::SetNextItemWidth(width);
                        const char* formats[] = {"X %.2f", "Y %.2f", "Z %.2f"};
                        ImGui::DragFloat("##axis",
                                         &c.HalfExtents[axis],
                                         0.01f,
                                         0.0f,
                                         100000.0f,
                                         formats[axis],
                                         ImGuiSliderFlags_AlwaysClamp);
                        boxes.Zone.Size[axis] = LastItem();
                        ImGui::PopID();
                    }
                    if (PropertyGrid::RowReset(c.HalfExtents != defaults.HalfExtents))
                        c.HalfExtents = defaults.HalfExtents;
                    ImGui::PopID();
                }
                FloatRow("Fade distance",
                         c.FadeDistance,
                         0.01f,
                         0.0f,
                         100000.0f,
                         "%.2f m",
                         &defaults.FadeDistance,
                         boxes.Zone.FadeDistance,
                         "The send fades in over this band outside the shape");
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Send"))
            {
                const AudioEngine* engine = ui.GetScene() ? ui.GetScene()->GetAudioEngine() : nullptr;
                if (!engine && ui.GetAudioPreview())
                    engine = ui.GetAudioPreview()->GetEngine();
                boxes.Zone.MissingBus = !HasAudioBus(engine, c.Bus);
                AudioBusRow("Target bus",
                            c.Bus,
                            engine,
                            nullptr,
                            defaults.Bus.c_str(),
                            "Not a bus of the active layout: this zone sends nothing. Add the bus in the Audio "
                            "Mixer or pick another.",
                            &boxes.Zone.Bus);
                FloatRow("Send level",
                         c.SendDb,
                         0.1f,
                         -80.0f,
                         12.0f,
                         "%.1f dB",
                         &defaults.SendDb,
                         boxes.Zone.SendLevel,
                         "How much of each sound inside the zone goes to the target bus");
                const bool modified = c.Priority != defaults.Priority;
                if (PropertyGrid::Row("Priority", "Where zones overlap, the higher priority wins", modified))
                {
                    ImGui::PushID("Priority");
                    ImGui::DragInt("##value", &c.Priority, 0.1f, -1000, 1000, "%d", ImGuiSliderFlags_AlwaysClamp);
                    boxes.Zone.Priority = LastItem();
                    if (PropertyGrid::RowReset(modified))
                        c.Priority = defaults.Priority;
                    ImGui::PopID();
                }
                PropertyGrid::EndGroup();
            }
            if (!std::isfinite(c.Radius) || c.Radius < 0.0f)
                c.Radius = defaults.Radius;
            if (!std::isfinite(c.FadeDistance) || c.FadeDistance < 0.0f)
                c.FadeDistance = defaults.FadeDistance;
            if (!std::isfinite(c.SendDb))
                c.SendDb = defaults.SendDb;
            c.SendDb = std::clamp(c.SendDb, -80.0f, 12.0f);
        });
    }
} // namespace Kans::ComponentEditors
