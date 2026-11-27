#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Audio/AudioEventPlayer.h"
#include "Kans3D/Audio/MusicSetPlayer.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Scene/Scene.h"
#include "ComponentEditors.h"
#include "KansEditorCore/Audio/AudioAttenuationEditing.h"
#include "KansEditorCore/Audio/AudioClipPreview.h"
#include "KansEditorCore/Audio/AudioEditing.h"
#include "KansEditorCore/Audio/AudioEventEditing.h"
#include "KansEditorCore/Audio/AudioMixerEditing.h"
#include "KansEditorCore/Audio/AudioSpatialEditing.h"
#include "KansEditorCore/Audio/MusicSetEditing.h"
#include "KansEditorCore/Physics/PhysicsEditing.h"
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

    bool AudioBusCombo(const char* id, std::string& bus, const AudioEngine* engine, const char* empty)
    {
        const auto& buses = BusesOf(engine);
        const bool  known = (empty && bus.empty()) || HasAudioBus(engine, bus);
        char        preview[160];
        std::snprintf(preview,
                      sizeof(preview),
                      !known ? ICON_FA_TRIANGLE_EXCLAMATION " %s (missing)" : "%s",
                      bus.empty() ? (empty ? empty : MasterAudioBus) : bus.c_str());
        if (!known)
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Warning);
        const bool open = ImGui::BeginCombo(id, preview);
        if (!known)
            ImGui::PopStyleColor();
        if (!known && !open && ImGui::IsItemHovered())
            ImGui::SetTooltip("Not a bus of the active layout: the sound plays through Master");
        bool changed = false;
        if (open)
        {
            if (empty && ImGui::Selectable(empty, bus.empty()) && !bus.empty())
                bus.clear(), changed = true;
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
        return changed;
    }

    MusicSetCardBoxes& LastMusicSetCardBoxes()
    {
        static MusicSetCardBoxes boxes;
        return boxes;
    }

    void DrawMusicSetPlayer(Entity entity, ComponentUI& ui)
    {
        const auto& p             = EditorTheme::Colors();
        auto&       boxes         = LastMusicSetCardBoxes();
        boxes                     = {};
        Scene*             scene  = ui.GetScene();
        MusicSetPlayer*    player = scene ? scene->GetMusicSetPlayer(entity) : nullptr;
        const AudioEngine* engine = scene ? scene->GetAudioEngine() : nullptr;
        if (!engine && ui.GetAudioPreview())
            engine = ui.GetAudioPreview()->GetEngine();
        // The set's states and parameters: the running player's, else the published asset's.
        const auto&              current = entity.GetComponent<MusicSetPlayerComponent>();
        Ref<const MusicSetAsset> asset;
        const MusicSetData*      data = player ? player->GetData() : nullptr;
        if (!data && current.Asset.IsValid() && ui.GetAssets())
        {
            auto loaded = ui.GetAssets()->Load<MusicSetAsset>(current.Asset);
            if (const auto* found = std::get_if<Ref<const MusicSetAsset>>(&loaded))
                asset = *found, data = &asset->GetData();
            else
                boxes.Warning = std::get<AssetError>(loaded).Message;
        }
        static const MusicSetPlayerComponent defaults;
        entity.PatchComponent<MusicSetPlayerComponent>([&](MusicSetPlayerComponent& c) {
            if (PropertyGrid::BeginGroup("Music set"))
            {
                ui.AssetPicker("Music set", AssetType::MusicSet, c.Asset);
                boxes.Asset = LastItem();
                if (!boxes.Warning.empty() && PropertyGrid::Row(""))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, p.Warning);
                    ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", boxes.Warning.c_str());
                    ImGui::PopStyleColor();
                }
                const bool busModified = !c.BusOverride.empty();
                if (PropertyGrid::Row("Bus override",
                                      "Play every stem and stinger through this bus instead of their own",
                                      busModified))
                {
                    ImGui::PushID("BusOverride");
                    AudioBusCombo("##value", c.BusOverride, engine, "Each stem's own bus");
                    boxes.Bus = LastItem();
                    if (PropertyGrid::RowReset(busModified))
                        c.BusOverride.clear();
                    ImGui::PopID();
                }
                const bool stateModified = !c.InitialState.empty();
                if (PropertyGrid::Row(
                        "Initial state", "The state Play starts in (empty: the set's first state)", stateModified))
                {
                    ImGui::PushID("InitialState");
                    const bool known =
                        c.InitialState.empty() ||
                        (data && std::any_of(data->States.begin(), data->States.end(), [&](const MusicState& s) {
                             return s.Name == c.InitialState;
                         }));
                    std::string preview = c.InitialState.empty() ? (data && !data->States.empty() ?
                                                                        "First (" + data->States.front().Name + ")" :
                                                                        std::string("First state")) :
                                                                   c.InitialState;
                    if (!known)
                        preview = ICON_FA_TRIANGLE_EXCLAMATION " " + preview + " (not in the set)";
                    if (!known)
                        ImGui::PushStyleColor(ImGuiCol_Text, p.Warning);
                    const bool open = ImGui::BeginCombo("##value", preview.c_str());
                    if (!known)
                        ImGui::PopStyleColor();
                    boxes.InitialState = LastItem();
                    if (open)
                    {
                        if (ImGui::Selectable("First state", c.InitialState.empty()))
                            c.InitialState.clear();
                        boxes.StateItems.emplace_back(std::string(), LastItem());
                        if (data)
                            for (const auto& state : data->States)
                            {
                                if (ImGui::Selectable(state.Name.c_str(), state.Name == c.InitialState))
                                    c.InitialState = state.Name;
                                boxes.StateItems.emplace_back(state.Name, LastItem());
                            }
                        ImGui::EndCombo();
                    }
                    if (PropertyGrid::RowReset(stateModified))
                        c.InitialState.clear();
                    ImGui::PopID();
                }
                CheckboxRow("Play on start",
                            c.PlayOnStart,
                            &defaults.PlayOnStart,
                            boxes.PlayOnStart,
                            "Start playing when the scene starts");
                PropertyGrid::EndGroup();
            }
            if (data && (!data->Parameters.empty() || !c.Parameters.empty()) && PropertyGrid::BeginGroup("Parameters"))
            {
                for (const auto& parameter : data->Parameters)
                {
                    const auto found    = c.Parameters.find(parameter.Name);
                    const bool modified = found != c.Parameters.end();
                    char       tooltip[160];
                    std::snprintf(tooltip,
                                  sizeof(tooltip),
                                  "Start value of %s (the set's default is %.2f)",
                                  parameter.Name.c_str(),
                                  double(parameter.Default));
                    if (!PropertyGrid::Row(parameter.Name.c_str(), tooltip, modified))
                        continue;
                    ImGui::PushID(parameter.Name.c_str());
                    float value = modified ? found->second : parameter.Default;
                    if (ImGui::SliderFloat("##value",
                                           &value,
                                           parameter.Min,
                                           std::max(parameter.Max, parameter.Min + 1e-3f),
                                           modified ? "%.2f" : "%.2f (default)",
                                           ImGuiSliderFlags_AlwaysClamp))
                        c.Parameters[parameter.Name] = value;
                    boxes.Parameters.emplace_back(parameter.Name, LastItem());
                    if (PropertyGrid::RowReset(modified))
                        c.Parameters.erase(parameter.Name);
                    ImGui::PopID();
                }
                // Overrides of parameters the set no longer has.
                for (auto it = c.Parameters.begin(); it != c.Parameters.end();)
                {
                    const auto& name = it->first;
                    if (std::any_of(data->Parameters.begin(), data->Parameters.end(), [&](const MusicParameter& q) {
                            return q.Name == name;
                        }))
                    {
                        ++it;
                        continue;
                    }
                    bool erase = false;
                    if (PropertyGrid::Row(name.c_str(), "Not a parameter of the set: ignored", true))
                    {
                        ImGui::PushID(name.c_str());
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(
                            p.Warning, ICON_FA_TRIANGLE_EXCLAMATION " %.2f, not in the set", double(it->second));
                        erase = PropertyGrid::RowReset(true);
                        ImGui::PopID();
                    }
                    it = erase ? c.Parameters.erase(it) : std::next(it);
                }
                PropertyGrid::EndGroup();
            }
        });

        // Play mode: what the running player plays, and buttons that drive it (not saved).
        if (!player || !PropertyGrid::BeginGroup("Playing"))
            return;
        const auto position = player->GetPosition();
        if (position)
            boxes.Live = std::string(position->State) + " \xC2\xB7 " + std::string(position->Segment) + " \xC2\xB7 " +
                         MusicSetEditing::BarBeatLabel(position->Bar, position->BeatInBar);
        else
            boxes.Live = "Stopped";
        PropertyGrid::Text("Now", boxes.Live, !position);
        if (const auto pending = player->GetPendingTransition())
        {
            boxes.Pending = std::string(pending->State) + " at the next sync point";
            PropertyGrid::Text("Next", boxes.Pending);
        }
        if (PropertyGrid::Row("Transport"))
        {
            if (ImGui::Button(ICON_FA_PLAY " Play"))
                scene->PlayMusicSet(entity);
            boxes.Play = LastItem();
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_STOP " Stop"))
                scene->StopMusicSet(entity);
            boxes.Stop = LastItem();
        }
        const auto* data2 = player->GetData();
        if (data2 && !data2->States.empty() && PropertyGrid::Row("Go to state", "Through the set's transition rules"))
        {
            for (size_t i = 0; i < data2->States.size(); ++i)
            {
                const auto& name = data2->States[i].Name;
                if (i)
                    ImGui::SameLine();
                if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(name.c_str()).x + EditorTheme::Px(16.0f) >
                    ImGui::GetWindowContentRegionMax().x)
                    ImGui::NewLine();
                const bool now = position && position->State == name;
                if (now)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, p.Selection);
                    ImGui::PushStyleColor(ImGuiCol_Text, p.Text);
                }
                ImGui::PushID(int(i));
                if (ImGui::Button(name.c_str()))
                    player->SetState(name);
                ImGui::PopID();
                if (now)
                    ImGui::PopStyleColor(2);
                boxes.LiveStates.emplace_back(name, LastItem());
            }
        }
        if (data2)
            for (const auto& parameter : data2->Parameters)
            {
                const std::string label = parameter.Name + " now";
                if (!PropertyGrid::Row(label.c_str(), "Drives the running player only (not saved)"))
                    continue;
                ImGui::PushID(label.c_str());
                float value = player->GetParameter(parameter.Name).value_or(parameter.Default);
                if (ImGui::SliderFloat("##live",
                                       &value,
                                       parameter.Min,
                                       std::max(parameter.Max, parameter.Min + 1e-3f),
                                       "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp))
                    player->SetParameter(parameter.Name, value);
                boxes.LiveParameters.emplace_back(parameter.Name, LastItem());
                ImGui::PopID();
            }
        PropertyGrid::EndGroup();
    }

    AudioSpatialBoxes& LastAudioSpatialBoxes()
    {
        static AudioSpatialBoxes boxes;
        return boxes;
    }

    namespace
    {
        namespace Spatial = AudioSpatialEditing;

        // Starts an overridable row: the toggle (checked while this asset sets the value), then the value
        // cell's remaining width for the field, disabled while inherited. Always pair with EndOverride.
        bool BeginOverride(const char* label, const char* tooltip, bool& overridden, const char* from, ImRect& toggle)
        {
            if (!PropertyGrid::Row(label, tooltip, overridden))
                return false;
            ImGui::PushID(label);
            bool on = overridden;
            ImGui::Checkbox("##override", &on);
            toggle = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(on ? "Overrides %s; uncheck to use it again" : "Uses %s; check to override it", from);
            overridden = on;
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(std::max(EditorTheme::Px(40.0f), ImGui::GetContentRegionAvail().x));
            ImGui::BeginDisabled(!on);
            return true;
        }
        void EndOverride()
        {
            ImGui::EndDisabled();
            ImGui::PopID();
        }

        // An enum combo whose open list's items are kept in `items` (by index).
        bool EnumCombo(const char* const                    names[],
                       int                                  count,
                       int&                                 index,
                       ImRect&                              box,
                       std::vector<std::pair<int, ImRect>>& items)
        {
            const bool open    = ImGui::BeginCombo("##value", names[std::clamp(index, 0, count - 1)]);
            bool       changed = false;
            box                = LastItem();
            if (open)
            {
                for (int i = 0; i < count; ++i)
                {
                    if (ImGui::Selectable(names[i], i == index) && i != index)
                        index = i, changed = true;
                    items.emplace_back(i, LastItem());
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        template<typename T, typename Edit>
        bool OverrideRow(const char*               label,
                         const char*               tooltip,
                         std::optional<T>&         value,
                         T                         inherited,
                         const char*               from,
                         AudioSpatialBoxes::Field& box,
                         Edit&&                    edit)
        {
            bool overridden = value.has_value();
            if (!BeginOverride(label, tooltip, overridden, from, box.Override))
                return false;
            bool changed = overridden != value.has_value();
            if (changed)
                value = overridden ? std::optional<T>(inherited) : std::nullopt;
            T shown = value.value_or(inherited);
            if (edit(shown, box.Value) && value)
                value = shown, changed = true;
            EndOverride();
            return changed;
        }

        const char* FromOf(bool eventSets, const char* fallbackName)
        {
            return eventSets ? "the event's value" : fallbackName;
        }
    } // namespace

    bool AudioSpatializerRow(std::optional<AudioSpatializer>& value, AudioSpatializer inherited, const char* from)
    {
        auto&      boxes    = LastAudioSpatialBoxes();
        const bool modified = value.has_value();
        char       tooltip[160];
        std::snprintf(tooltip,
                      sizeof(tooltip),
                      "How the sound is placed around the listener: Panning (speaker gains) or HRTF (binaural, "
                      "for headphones). Inherit uses %s.",
                      from);
        if (!PropertyGrid::Row("Spatializer", tooltip, modified))
            return false;
        ImGui::PushID("Spatializer");
        char inherit[96];
        std::snprintf(inherit, sizeof(inherit), "Inherit (%s)", Spatial::SpatializerName(inherited));
        const char* names[] = {inherit, "Panning", "HRTF"};
        int         index   = value ? 1 + int(*value) : 0;
        bool        changed = false;
        const bool  open    = ImGui::BeginCombo("##value", names[index]);
        boxes.Spatializer   = LastItem();
        if (!open && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        if (open)
        {
            for (int i = 0; i < 3; ++i)
            {
                if (ImGui::Selectable(names[i], i == index) && i != index)
                {
                    value   = i == 0 ? std::nullopt : std::optional<AudioSpatializer>(AudioSpatializer(i - 1));
                    changed = true;
                }
                boxes.SpatializerItems.emplace_back(i - 1, LastItem());
            }
            ImGui::EndCombo();
        }
        if (PropertyGrid::RowReset(modified))
            value.reset(), changed = true;
        ImGui::PopID();
        return changed;
    }

    bool AudioOcclusionRows(AudioOcclusionOverrides&       o,
                            const AudioOcclusionOverrides* event,
                            const AudioOcclusionSettings&  fallback,
                            const char*                    fallbackName)
    {
        auto&      boxes     = LastAudioSpatialBoxes();
        const auto inherited = Spatial::InheritedOcclusion(event, fallback);
        const auto sets      = [&](auto member) { return event && (event->*member).has_value(); };
        bool       changed   = false;

        changed |=
            OverrideRow("Mode",
                        "Occlusion: rays from the listener to the source test the colliders in between: Raycast casts "
                        "one ray, Volumetric samples a disc of the source's radius",
                        o.Mode,
                        inherited.Mode,
                        FromOf(sets(&AudioOcclusionOverrides::Mode), fallbackName),
                        boxes.Mode,
                        [&](AudioOcclusionMode& mode, ImRect& box) {
                            int index = int(mode);
                            if (!EnumCombo(Spatial::OcclusionModeNames, 3, index, box, boxes.ModeItems))
                                return false;
                            mode = AudioOcclusionMode(index);
                            return true;
                        });
        const auto mode = o.Mode.value_or(inherited.Mode);
        if (mode == AudioOcclusionMode::Volumetric)
        {
            changed |=
                OverrideRow("Source radius",
                            "The disc Volumetric samples around the source, facing the listener",
                            o.SourceRadius,
                            inherited.SourceRadius,
                            FromOf(sets(&AudioOcclusionOverrides::SourceRadius), fallbackName),
                            boxes.Radius,
                            [](float& radius, ImRect& box) {
                                const bool edited = ImGui::DragFloat(
                                    "##value", &radius, 0.01f, 0.0f, 1000.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
                                box = LastItem();
                                return edited;
                            });
            changed |= OverrideRow(
                "Samples",
                "Rays across the disc (1 to 32); more give smoother partial occlusion",
                o.Samples,
                inherited.Samples,
                FromOf(sets(&AudioOcclusionOverrides::Samples), fallbackName),
                boxes.Samples,
                [](uint32_t& samples, ImRect& box) {
                    int        count  = int(samples);
                    const bool edited = ImGui::SliderInt(
                        "##value", &count, 1, int(MaxAudioOcclusionSamples), "%d", ImGuiSliderFlags_AlwaysClamp);
                    box = LastItem();
                    if (edited)
                        samples = uint32_t(count);
                    return edited;
                });
        }
        changed |=
            OverrideRow("Transmission",
                        "Sound passing through what blocks it, by each surface's acoustic material: one "
                        "gain, or low, mid and high bands",
                        o.Transmission,
                        inherited.Transmission,
                        FromOf(sets(&AudioOcclusionOverrides::Transmission), fallbackName),
                        boxes.Transmission,
                        [&](AudioTransmissionMode& transmission, ImRect& box) {
                            int index = int(transmission);
                            if (!EnumCombo(Spatial::TransmissionModeNames, 3, index, box, boxes.TransmissionItems))
                                return false;
                            transmission = AudioTransmissionMode(index);
                            return true;
                        });
        const auto strength = [](float& value, ImRect& box) {
            const bool edited = ImGui::SliderFloat("##value", &value, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            box               = LastItem();
            return edited;
        };
        const auto transmission = o.Transmission.value_or(inherited.Transmission);
        if (mode != AudioOcclusionMode::Off)
            changed |= OverrideRow("Occlusion strength",
                                   "How much of the blocked fraction is applied (0: none, 1: all)",
                                   o.OcclusionStrength,
                                   inherited.OcclusionStrength,
                                   FromOf(sets(&AudioOcclusionOverrides::OcclusionStrength), fallbackName),
                                   boxes.OcclusionStrength,
                                   strength);
        if (transmission != AudioTransmissionMode::Off)
            changed |= OverrideRow("Transmission strength",
                                   "How much the surfaces' transmission colours the blocked sound",
                                   o.TransmissionStrength,
                                   inherited.TransmissionStrength,
                                   FromOf(sets(&AudioOcclusionOverrides::TransmissionStrength), fallbackName),
                                   boxes.TransmissionStrength,
                                   strength);
        if (mode != AudioOcclusionMode::Off || transmission != AudioTransmissionMode::Off)
            changed |= OverrideRow("Layers",
                                   "The physics layers that block this sound (triggers never do)",
                                   o.OcclusionLayerMask,
                                   inherited.OcclusionLayerMask,
                                   FromOf(sets(&AudioOcclusionOverrides::OcclusionLayerMask), fallbackName),
                                   boxes.Layers,
                                   [&](uint32_t& mask, ImRect& box) {
                                       const auto& physics = KansFileSystem::GetProjectPhysicsSettings();
                                       const bool  open =
                                           ImGui::BeginCombo("##value",
                                                             PhysicsEditing::MaskSummary(mask, physics).c_str(),
                                                             ImGuiComboFlags_HeightLarge);
                                       box         = LastItem();
                                       bool edited = false;
                                       if (!open)
                                           return false;
                                       if (ImGui::Selectable("Everything", mask == UINT32_MAX))
                                           mask = UINT32_MAX, edited = true;
                                       boxes.LayerItems.emplace_back(32u, LastItem());
                                       if (ImGui::Selectable("Nothing", mask == 0))
                                           mask = 0, edited = true;
                                       boxes.LayerItems.emplace_back(33u, LastItem());
                                       ImGui::Separator();
                                       // Named layers (and Default); every layer when none is named.
                                       for (const uint32_t layer : PhysicsEditing::MatrixLayers(physics, false))
                                       {
                                           ImGui::PushID(int(layer));
                                           bool        on   = mask & (1u << layer);
                                           const auto& name = physics.LayerNames[layer];
                                           if (ImGui::Checkbox(name.c_str(), &on))
                                               mask = on ? mask | (1u << layer) : mask & ~(1u << layer), edited = true;
                                           boxes.LayerItems.emplace_back(layer, LastItem());
                                           ImGui::PopID();
                                       }
                                       ImGui::EndCombo();
                                       return edited;
                                   });
        if (changed)
            Spatial::Clamp(o);
        return changed;
    }

    bool AcousticMaterialRows(AcousticMaterial& material, AcousticBoxes* boxes)
    {
        AcousticBoxes  scratch;
        AcousticBoxes& b = boxes ? *boxes : scratch;
        b.PresetItems.clear();
        const auto previous = material;
        bool       changed  = false;
        b.PresetLabel       = Spatial::AcousticPresetLabel(material);
        const bool modified = !(material == AcousticMaterial {});
        if (PropertyGrid::Row("Preset",
                              "Steam Audio's standard materials; picking one fills the values below, editing a "
                              "value makes it Custom",
                              modified))
        {
            ImGui::PushID("Preset");
            const auto match = Spatial::MatchAcousticPreset(material);
            const bool open  = ImGui::BeginCombo("##value", b.PresetLabel.c_str());
            b.Preset         = LastItem();
            if (open)
            {
                for (int i = 0; i < Spatial::AcousticPresetCount; ++i)
                {
                    const auto preset = AcousticMaterialPreset(i);
                    if (ImGui::Selectable(AcousticMaterialPresetName(preset), match == preset))
                        material = GetAcousticMaterialPreset(preset), changed = true;
                    b.PresetItems.emplace_back(i, LastItem());
                }
                ImGui::EndCombo();
            }
            if (PropertyGrid::RowReset(modified))
                material = {}, changed = true;
            ImGui::PopID();
        }
        // Three bands side by side: low, mid, high.
        const auto bands = [&](const char* label, const char* tooltip, std::array<float, 3>& values, ImRect* out) {
            if (!PropertyGrid::Row(label, tooltip))
                return;
            ImGui::PushID(label);
            const float spacing   = ImGui::GetStyle().ItemSpacing.x * 0.5f;
            const float width     = (ImGui::CalcItemWidth() - 2.0f * spacing) / 3.0f;
            const char* formats[] = {"Low %.3f", "Mid %.3f", "High %.3f"};
            for (int band = 0; band < 3; ++band)
            {
                if (band)
                    ImGui::SameLine(0.0f, spacing);
                ImGui::PushID(band);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat(
                    "##band", &values[band], 0.001f, 0.0f, 1.0f, formats[band], ImGuiSliderFlags_AlwaysClamp);
                out[band] = LastItem();
                ImGui::PopID();
            }
            ImGui::PopID();
        };
        bands("Absorption",
              "The share of sound a surface absorbs per band (0 to 1), used by reflections and reverb",
              material.Absorption,
              b.Absorption);
        bands("Transmission",
              "The share of sound passing through per band (0 to 1), used by occlusion transmission",
              material.Transmission,
              b.Transmission);
        if (PropertyGrid::Row("Scattering", "How diffusely reflections leave the surface (0: mirror, 1: diffuse)"))
        {
            ImGui::PushID("Scattering");
            changed |=
                ImGui::SliderFloat("##value", &material.Scattering, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            b.Scattering = LastItem();
            ImGui::PopID();
        }
        if (changed)
            Spatial::ClampAcoustics(material, previous);
        b.PresetLabel = Spatial::AcousticPresetLabel(material);
        return changed;
    }

    namespace
    {
        // The Spatializer and Occlusion groups of a spatial source, with the voice's state in Play.
        void DrawSourceSpatializer(AudioSourceComponent& c, ComponentUI& ui)
        {
            auto&        boxes  = LastAudioSpatialBoxes();
            Scene*       scene  = ui.GetScene();
            AudioEngine* engine = scene ? scene->GetAudioEngine() : nullptr;
            if (!engine && ui.GetAudioPreview())
                engine = ui.GetAudioPreview()->GetEngine();
            const bool live = engine && engine->IsInitialized();
            // An Event source plays its event's settings where it leaves a field to inherit.
            Ref<const AudioEventAsset> event;
            if (c.GetSourceKind() == AudioSourceKind::Event && c.Event.IsValid() && ui.GetAssets())
                event = ui.GetAssets()->Request<AudioEventAsset>(c.Event).Asset;
            const AudioEventData* data    = event ? &event->GetData() : nullptr;
            const auto            project = live ? engine->GetDefaultSpatializer() : AudioSpatializer::Panning;
            const auto            inherited =
                Spatial::ResolveSpatializer(std::nullopt, data ? data->Spatializer : std::nullopt, project);
            const auto resolved =
                Spatial::ResolveSpatializer(c.Spatializer, data ? data->Spatializer : std::nullopt, project);
            const bool running = scene && scene->IsAudioRunning() && live;
            // The voice as the engine sees it (Play only).
            static std::vector<AudioVoiceInfo> voices;
            const AudioVoiceInfo*              voice = nullptr;
            if (running && engine->IsVoiceValid(c.GetVoiceHandle()))
            {
                engine->GetVoiceSnapshot(voices);
                const auto handle = c.GetVoiceHandle();
                for (const auto& info : voices)
                    if (info.Voice.Index == handle.Index && info.Voice.Generation == handle.Generation)
                        voice = &info;
            }

            if (PropertyGrid::BeginGroup("Spatializer"))
            {
                AudioSpatializerRow(c.Spatializer,
                                    inherited,
                                    data && data->Spatializer ? "the event's spatializer" :
                                                                "the project default (Project Settings > Audio)");
                if (resolved == AudioSpatializer::HRTF)
                {
                    const float eventBlend = data ? data->SpatialBlend : 1.0f;
                    float       blend      = c.SpatialBlend.value_or(eventBlend);
                    const bool  modified   = c.SpatialBlend.has_value();
                    if (PropertyGrid::Row(
                            "Spatial blend", "0 pans, 1 is fully binaural; values between mix the two", modified))
                    {
                        ImGui::PushID("SpatialBlend");
                        if (ImGui::SliderFloat("##value",
                                               &blend,
                                               0.0f,
                                               1.0f,
                                               modified ? "%.2f" : (data ? "%.2f (from the event)" : "%.2f"),
                                               ImGuiSliderFlags_AlwaysClamp))
                            c.SpatialBlend = std::clamp(blend, 0.0f, 1.0f);
                        boxes.Blend = LastItem();
                        if (PropertyGrid::RowReset(modified))
                            c.SpatialBlend.reset();
                        ImGui::PopID();
                    }
                }
                if (const char* hint = Spatial::StereoHint(resolved, engine); hint && PropertyGrid::Row(""))
                {
                    boxes.StereoHint = hint;
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(EditorTheme::Colors().Warning, ICON_FA_TRIANGLE_EXCLAMATION " %s", hint);
                }
                else if (live && resolved == AudioSpatializer::HRTF && !engine->IsHRTFAvailable() &&
                         PropertyGrid::Row(""))
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(EditorTheme::Colors().Warning,
                                       ICON_FA_TRIANGLE_EXCLAMATION " Steam Audio is not available: panned");
                }
                if (voice)
                {
                    char text[128];
                    if (voice->HRTFFallback != AudioHRTFFallback::None)
                        std::snprintf(text, sizeof(text), "%s", Spatial::FallbackText(voice->HRTFFallback));
                    else if (voice->Spatializer == AudioSpatializer::HRTF)
                        std::snprintf(text, sizeof(text), "HRTF, blend %.2f", double(voice->SpatialBlend));
                    else
                        std::snprintf(text, sizeof(text), "Panning");
                    boxes.LiveSpatializer = text;
                    PropertyGrid::Text(
                        "Heard as", boxes.LiveSpatializer, voice->HRTFFallback == AudioHRTFFallback::None);
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Occlusion"))
            {
                const auto fallback = live ? engine->GetOcclusionDefaults() : AudioOcclusionSettings {};
                AudioOcclusionRows(c.Occlusion,
                                   data ? &data->Occlusion : nullptr,
                                   fallback,
                                   "the project default (Project Settings > Audio)");
                if (voice)
                {
                    char text[64];
                    std::snprintf(text, sizeof(text), "%.2f", double(voice->Acoustics.Occlusion));
                    boxes.LiveOcclusion = text;
                    if (voice->OcclusionStale)
                        boxes.LiveOcclusion += " (stale)";
                    Spatial::FormatBands(voice->Acoustics.Transmission, text, sizeof(text));
                    boxes.LiveTransmission = text;
                    PropertyGrid::Text("Occlusion now", boxes.LiveOcclusion, true);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("The blocked fraction the engine applies (0: clear)");
                    PropertyGrid::Text("Transmission now", boxes.LiveTransmission, true);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Low / mid / high gains of the sound passing through");
                }
                PropertyGrid::EndGroup();
            }
        }

        // Clip and Event sources: the asset pickers and the preview, as before the Source selector.
        void
        DrawAssetSource(Entity entity, AudioSourceComponent& c, ComponentUI& ui, const AudioEditing::EventHints& hints)
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
        }

        // A generator source: the PCM format pushed to it and, in Play, the frames it ran short.
        void DrawGenerator(Entity entity, AudioSourceComponent& c, ComponentUI& ui, AudioCardBoxes::SourceBoxes& boxes)
        {
            static const AudioGeneratorFormat defaults;
            const bool                        channelsModified = c.Generator.Channels != defaults.Channels;
            if (PropertyGrid::Row("Channels", "Interleaved channels in the pushed PCM (1 to 8)", channelsModified))
            {
                ImGui::PushID("Channels");
                int channels = int(c.Generator.Channels);
                if (ImGui::SliderInt("##value", &channels, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp))
                    c.Generator.Channels = uint32_t(channels);
                boxes.Channels = LastItem();
                if (PropertyGrid::RowReset(channelsModified))
                    c.Generator.Channels = defaults.Channels;
                ImGui::PopID();
            }
            const bool rateModified = c.Generator.SampleRate != defaults.SampleRate;
            if (PropertyGrid::Row("Sample rate", "The pushed PCM's rate; the engine resamples it", rateModified))
            {
                ImGui::PushID("SampleRate");
                char preview[32];
                std::snprintf(preview, sizeof(preview), "%u Hz", c.Generator.SampleRate);
                const bool open  = ImGui::BeginCombo("##value", preview);
                boxes.SampleRate = LastItem();
                boxes.SampleRates.clear();
                if (open)
                {
                    for (const uint32_t rate : AudioEditing::GeneratorSampleRates)
                    {
                        char label[32];
                        std::snprintf(label, sizeof(label), "%u Hz", rate);
                        if (ImGui::Selectable(label, rate == c.Generator.SampleRate))
                            c.Generator.SampleRate = rate;
                        boxes.SampleRates.emplace_back(rate, LastItem());
                    }
                    ImGui::EndCombo();
                }
                if (PropertyGrid::RowReset(rateModified))
                    c.Generator.SampleRate = defaults.SampleRate;
                ImGui::PopID();
            }
            Scene* scene = ui.GetScene();
            if (scene && scene->IsAudioRunning())
            {
                if (PropertyGrid::Row("Playback", "This source in the running scene."))
                {
                    const bool playing = scene->IsAudioSourcePlaying(entity);
                    if (ImGui::Button(playing ? ICON_FA_STOP " Stop##source" : ICON_FA_PLAY " Play##source"))
                    {
                        if (playing)
                            scene->StopAudioSource(entity);
                        else
                            scene->PlayAudioSource(entity);
                    }
                }
                const auto* engine = scene->GetAudioEngine();
                boxes.Underruns    = AudioEditing::GeneratorUnderrunText(
                    engine ? engine->GetGeneratorUnderrunFrames(c.GetVoiceHandle()) : 0, c.Generator.SampleRate);
                PropertyGrid::Text("Underruns", boxes.Underruns, true);
            }
            else if (PropertyGrid::Row(""))
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Push PCM from a script or C++ while the game plays");
            }
        }
    } // namespace

    void DrawAudioSource(Entity entity, ComponentUI& ui)
    {
        static const AudioSourceComponent defaults;
        entity.PatchComponent<AudioSourceComponent>([&](AudioSourceComponent& c) {
            // The groups keep this order: Source, Playback, Output, Spatial, Cone, Spatializer, Occlusion.
            const auto hints = AudioEditing::EventHintsOf(c);
            auto&      boxes = LastAudioCardBoxes();
            boxes.Source     = {};
            LastAudioSpatialBoxes() = {};
            if (PropertyGrid::BeginGroup("Source"))
            {
                // Clip and Event play assets; a Generator plays PCM a script or C++ pushes at run time.
                int kind = int(c.GetSourceKind());
                if (ChoiceRow("Source",
                              AudioEditing::SourceKindNames,
                              3,
                              kind,
                              boxes.Source.Kind,
                              int(AudioSourceKind::Clip)))
                    c.Kind = AudioSourceKind(kind);
                if (c.GetSourceKind() == AudioSourceKind::Generator)
                    DrawGenerator(entity, c, ui, boxes.Source);
                else
                    DrawAssetSource(entity, c, ui, hints);
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
            // A 2D sound has no direction to spatialize and no path to occlude.
            if (c.Spatial)
                DrawSourceSpatializer(c, ui);
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
