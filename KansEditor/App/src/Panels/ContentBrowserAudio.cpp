#include <algorithm>
#include <cstdio>
#include "Kans3D/ImGui/EditorTheme.h"
#include "ContentBrowserPanel.h"
#include "KansEditorCore/Audio/AudioClipPreview.h"
#include "KansEditorCore/Audio/AudioEditing.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "UI/AudioWaveform.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
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

        const char* EncodingName(AudioEncoding encoding)
        {
            switch (encoding)
            {
                case AudioEncoding::Wav:
                    return "WAV";
                case AudioEncoding::Vorbis:
                    return "Ogg Vorbis";
                case AudioEncoding::Mp3:
                    return "MP3";
                case AudioEncoding::Flac:
                    return "FLAC";
            }
            return "Unknown";
        }

        std::pair<float, float> LoopOf(const AudioClipData& data)
        {
            if (!data.Info.LengthFrames)
                return {0.0f, 1.0f};
            return {float(double(data.LoopBegin) / data.Info.LengthFrames),
                    float(double(data.LoopEnd) / data.Info.LengthFrames)};
        }
    } // namespace

    const AudioClipSummary* ContentBrowserPanel::AudioClipOf(const std::filesystem::path& file)
    {
        if (!m_Service || !m_Registry || !AudioEditing::IsAudioFile(file))
            return nullptr;
        const auto* status = StatusOf(file);
        if (!status || !status->Products)
            return nullptr;
        return m_AudioClips.Find(m_Registry, m_Service->GetProductDirectory(), status->Source.sourceAssetID);
    }

    bool ContentBrowserPanel::DrawAudioIcon(const Browser::Entry& entry, ImVec2 min, ImVec2 max)
    {
        // List rows keep the small file icon; a waveform needs room.
        if (entry.Directory || max.x - min.x < EditorTheme::Px(40.0f))
            return false;
        const auto* clip = AudioClipOf(entry.Path);
        if (!clip)
            return false;
        const float          side   = max.x - min.x;
        const auto&          colors = EditorTheme::Colors();
        AudioWaveform::Style style;
        const bool           playing = m_AudioPreview && m_AudioPreview->IsPlaying(clip->ID);
        if (const auto position = playing ? m_AudioPreview->GetPosition(clip->ID) : std::nullopt;
            position && clip->Seconds() > 0)
            style.Playhead = float(*position / clip->Seconds());
        AudioWaveform::Draw(clip->Data.Waveform, {min.x, min.y + side * 0.18f}, {max.x, max.y - side * 0.18f}, style);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText({min.x + EditorTheme::Px(4.0f), min.y + side * 0.18f + EditorTheme::Px(3.0f)},
                      ImGui::GetColorU32(playing ? colors.Warning : colors.TextDim),
                      playing ? ICON_FA_VOLUME_HIGH : ICON_FA_MUSIC);
        const auto   length = Clock(clip->Seconds());
        const ImVec2 size   = ImGui::CalcTextSize(length.c_str());
        draw->AddText({max.x - size.x - EditorTheme::Px(4.0f), max.y - side * 0.18f - size.y - EditorTheme::Px(2.0f)},
                      ImGui::GetColorU32(colors.TextDim),
                      length.c_str());
        return true;
    }

    void ContentBrowserPanel::DrawAudioPreview(const AudioClipSummary& clip, float width)
    {
        const auto&  data      = clip.Data;
        const double length    = clip.Seconds();
        const bool   available = m_AudioPreview && m_AudioPreview->IsAvailable();
        const bool   active    = available && m_AudioPreview->IsPlaying(clip.ID);
        const auto   position  = active ? m_AudioPreview->GetPosition(clip.ID) : std::nullopt;

        AudioWaveform::Style style;
        style.Loop = LoopOf(data);
        if (position && length > 0)
            style.Playhead = float(*position / length);
        float seek = 0.0f;
        if (AudioWaveform::Scrubber("##waveform", data.Waveform, {width, EditorTheme::Px(72.0f)}, style, seek) &&
            available)
        {
            // Click to play from there, drag to scrub the playing clip.
            if (active)
                m_AudioPreview->Seek(seek * length);
            else
                m_AudioPreview->Play(clip.ID, {1.0f, 1.0f, false, seek * length});
        }
        if (ImGui::IsItemHovered() && length > 0)
        {
            const float at = std::clamp((ImGui::GetIO().MousePos.x - ImGui::GetItemRectMin().x) /
                                            std::max(1.0f, ImGui::GetItemRectSize().x),
                                        0.0f,
                                        1.0f);
            ImGui::SetTooltip("%s\nClick to play from here.", Clock(at * length).c_str());
        }

        ImGui::BeginDisabled(!available);
        if (ImGui::Button(active ? ICON_FA_STOP " Stop##audio" : ICON_FA_PLAY " Play##audio"))
        {
            if (active)
                m_AudioPreview->Stop();
            else
                m_AudioPreview->Play(clip.ID);
        }
        ImGui::EndDisabled();
        if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The editor has no audio engine.");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s / %s", Clock(position ? *position : 0.0).c_str(), Clock(length).c_str());

        if (PropertyGrid::Begin("##AudioClip", 72.0f))
        {
            char format[96];
            std::snprintf(format,
                          sizeof(format),
                          "%s, %u ch, %u Hz",
                          EncodingName(data.Encoding),
                          data.Info.Channels,
                          data.Info.SampleRate);
            PropertyGrid::Text("Format", format);
            PropertyGrid::Text("Load mode", data.LoadMode == AudioLoadMode::Stream ? "Stream" : "Decompress");
            if (style.Loop.first > 0.0f || style.Loop.second < 1.0f)
                PropertyGrid::Text("Loop",
                                   Clock(double(data.LoopBegin) / data.Info.SampleRate) + " - " +
                                       Clock(double(data.LoopEnd) / data.Info.SampleRate));
            PropertyGrid::End();
        }
    }
} // namespace Kans
