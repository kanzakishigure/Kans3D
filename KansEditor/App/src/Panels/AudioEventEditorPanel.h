#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "KansEditorCore/Audio/AudioEventEditing.h"
#include "KansEditorCore/Audio/AudioEventPreview.h"
#include "KansEditorCore/Audio/AudioPreviewCache.h"
#include "KansEditorCore/EditorAssetChoice.h"

namespace Kans
{
    class EditorAssetImportService;

    // The AudioEvent editor: the open .audioevent's entries (clip, weight, volume and pitch offsets,
    // share of a Random pick, waveform, play and remove; drag to reorder, drop clips to add) and its
    // Selection, Variation, Limits and Output groups. Every edit goes into the panel's own undo
    // history (a drag is one step); ValidateAudioEvent's reason shows at the top and an invalid event
    // is never saved. Save writes the source and rebuilds it, so hot reload applies it. Trigger and
    // auto-trigger audition the edited event through AudioEventPreview, separate from scene audio.
    class AudioEventEditorPanel
    {
    public:
        // The project's import service (may be null), the editor's engine and asset manager (may be null).
        void Bind(EditorAssetImportService* service, AudioEngine* engine, AssetManager* assets);
        // The published assets; the AudioClips are offered for entries.
        void SetAssetChoices(const std::vector<EditorAssetChoice>& choices);
        // Peaks of a clip for its row's waveform; without one, the import service's clip previews.
        void SetWaveformSource(std::function<const std::vector<uint8_t>*(AssetID)> source)
        {
            m_Waveforms = std::move(source);
        }
        // True while the scene plays: the audition is off (the scene's audio takes over).
        void SetPlayModeQuery(std::function<bool()> query) { m_PlayMode = std::move(query); }

        // Loads a .audioevent; false (with the reason in the status) when it cannot be read.
        bool Open(const std::filesystem::path& file);
        // A new event to be saved as `directory`/NewAudioEvent.audioevent once it has a clip.
        void OpenNew(const std::filesystem::path& directory);
        // Writes the file when the event is valid; false (with the reason in the status) otherwise.
        bool Save();
        // Stops the audition (the window closed, Play started).
        void StopPreview() { m_Preview.Stop(); }

        void Draw();
        bool Undo() { return m_Pending.reset(), m_Document.Undo(); }
        bool Redo() { return m_Pending.reset(), m_Document.Redo(); }
        bool HasFocus() const { return m_Focused; }

        AudioEventEditing::EventDocument&       GetDocument() { return m_Document; }
        const AudioEventEditing::EventDocument& GetDocument() const { return m_Document; }
        AudioEventPreview&                      GetPreview() { return m_Preview; }
        const std::filesystem::path&            GetFile() const { return m_File; }
        const std::string&                      GetStatus() const { return m_Status; }
        // Volume range, pitch range, start offset: the refused value's reason, if any.
        const std::string& GetRangeError(size_t range) const { return m_Ranges[range].GetError(); }

        // Where the last frame drew each control (screen space), for tooltips and UI tests.
        struct EntryBoxes
        {
            ImRect Row, Grip, Clip, Waveform, Weight, Volume, Pitch, Share, Play, Remove;
            float  ShownShare   = -1; // The percentage drawn; -1 when the mode does not pick by weight.
            size_t WaveformBars = 0;
        };
        struct Boxes
        {
            ImRect                  Validation, Save, Undo, Redo, Trigger, Auto, Interval, LastPick, Entries, Add;
            ImRect                  Ranges[3][2]; // Volume, pitch, start offset; min and max.
            std::vector<EntryBoxes> Rows;
        };
        const Boxes& GetBoxes() const { return m_Boxes; }

    private:
        void DrawHeader();
        void DrawAudition();
        void DrawEntries();
        void DrawSelection();
        void DrawVariation();
        void DrawLimits();
        void DrawOutput();
        bool RangeRow(const char* label, size_t index, AudioEventRange stored, const char* format, float speed);
        // Changes a copy of the event, committed at the end of the frame (rows keep reading the event
        // as it was); a continuous edit (a field held) joins the gesture.
        void                        Edit(const std::function<void(AudioEventData&)>& change);
        void                        CommitPending();
        AssetID                     ClipAt(const std::filesystem::path& file) const;
        const EditorAssetChoice*    FindClip(AssetID clip) const;
        const std::vector<uint8_t>* PeaksOf(AssetID clip);
        bool                        ClipCombo(const char* id, AssetID& clip, const char* none);

        EditorAssetImportService*                           m_Service = nullptr;
        AudioEngine*                                        m_Engine  = nullptr;
        std::vector<EditorAssetChoice>                      m_Clips;
        std::function<const std::vector<uint8_t>*(AssetID)> m_Waveforms;
        std::function<bool()>                               m_PlayMode;
        AudioPreviewCache                                   m_PeakCache;
        AudioEventEditing::EventDocument                    m_Document;
        AudioEventPreview                                   m_Preview;
        AudioEventEditing::RangeField                       m_Ranges[3];
        std::optional<AudioEventData>                       m_Pending;
        bool                                                m_PendingContinuous = false;
        std::filesystem::path                               m_File, m_NewDirectory;
        bool                                                m_Opened = false, m_Focused = false;
        std::string                                         m_Status;
        Boxes                                               m_Boxes;
    };
} // namespace Kans
