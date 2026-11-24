#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "Kans3D/Timeline/Timeline.h"
#include "HierarchyPanel.h"
#include "KansEditorCore/Audio/AudioPreviewCache.h"
#include "KansEditorCore/EditorAssetChoice.h"
#include "KansEditorCore/Panels/TimelineAudioEditing.h"
#include "KansEditorCore/Panels/TimelineFollow.h"

namespace Kans
{
    class AssetManager;
    class AudioClipAsset;
    class EditorAssetImportService;
    class TimelineAudioPlayer;

    // The timeline of a Timeline Player: its bindings on the left, each with its tracks, and on
    // the right a time ruler with the tracks' keys, clips, ranges, events and camera cuts, which
    // drag along the time axis (snapping to frames). The selected item is edited on the far right.
    // Preview evaluates the timeline in the scene at the playhead and restores the bound entities
    // when it ends; edits are saved to the .timeline file, which is then rebuilt.
    // During Play or Simulate the window is read-only and follows the player that runs this
    // timeline: its time, the clips and cuts active now and the events that fired.
    // Audio tracks show their clips' waveforms and fades; while the preview plays, they sound through
    // the bound audio engine (a TimelineAudioPlayer driven by the playhead, silent while scrubbing).
    // Edits go into the window's own undo history (Ctrl+Z while it has focus); a drag is one step.
    class TimelinePanel
    {
    public:
        explicit TimelinePanel(HierarchyPanel& hierarchy);
        ~TimelinePanel();
        void Draw();
        // Edits the timeline of `owner`, an entity with a Timeline Player.
        void Open(Entity owner);
        void SetAssetChoices(std::vector<EditorAssetChoice> choices) { m_Choices = std::move(choices); }
        void SetAssetManager(AssetManager* assets) { m_Assets = assets; }
        void Bind(EditorAssetImportService* service) { m_Service = service; }
        // Shows the viewport through the scene's primary camera (camera cuts) while previewing.
        void SetViewThroughCamera(std::function<void(bool)> show) { m_ViewThrough = std::move(show); }
        bool IsPreviewing() const { return m_Previewing; }
        // The engine the preview's audio tracks play through (may be null); a new engine stops them.
        void SetAudioEngine(AudioEngine* engine);
        bool HasFocus() const { return m_Focused; }
        // The window's undo history (Ctrl+Z / Ctrl+Y while it has focus).
        bool                                 Undo();
        bool                                 Redo();
        const TimelineAudioEditing::History& History() const { return m_History; }
        // Writes the .timeline file and rebuilds it (stops preview first).
        void Save();
        // Restores the scene; called before play mode, saving, undo and closing the project.
        void StopPreview();
        // Preview also ends when the window was not drawn (closed or hidden) last frame.
        void StopPreviewIfHidden();
        // Whether the scene is in Play or Simulate, asked once a frame.
        void SetPlayModeQuery(std::function<bool()> playing) { m_PlayMode = std::move(playing); }
        // What the window follows during Play (updated by Draw).
        const TimelineFollow::Model& Follow() const { return m_Follow; }
        bool                         IsReadOnly() const { return m_Follow.ReadOnly(); }

        // For tests: the timeline being edited, and its unsaved state.
        TimelineData&       Data() { return m_Data; }
        const TimelineData& Data() const { return m_Data; }
        bool                IsDirty() const { return m_Dirty; }
        double              Playhead() const { return m_Time; }
        void                SetPlayhead(double time);
        void                SetPreview(bool preview);
        // Keys the entity's transform (or its offset from where preview started, for a relative
        // track) at the playhead on its binding's transform track, creating them as needed.
        bool KeyTransform(Entity entity);

        // Audio tracks, as the menus and drops add them: a track (2D when `binding` is empty) and a
        // clip at `start` (seconds), sized to the clip's length; the reason when it is refused.
        size_t                     AddAudioTrack(const std::string& binding = {});
        std::optional<std::string> AddAudioClip(size_t track, AssetID clip, double start);
        void                       SelectAudioClip(size_t track, size_t clip);
        // Plays or pauses the preview (and its audio), as the toolbar's play button does.
        void SetPlaying(bool playing);
        bool IsPlaying() const { return m_Playing; }
        // The preview's audio: whether it has a player, and its voices and drift.
        bool               HasPreviewAudio() const { return bool(m_Audio); }
        TimelineAudioStats GetPreviewAudioStats() const;
        const std::string& GetStatus() const { return m_Status; }
        // Audio clips the preview plays and draws without an asset manager (tests).
        std::map<AssetID, Ref<const AudioClipAsset>> AudioClips;
        // Where the last frame drew each audio clip (screen space), for tooltips and UI tests.
        struct AudioClipBox
        {
            size_t Track = 0, Clip = 0;
            ImRect Bar;
            ImVec2 FadeIn, FadeOut; // Fade handles.
            size_t WaveformColumns = 0;
        };
        const std::vector<AudioClipBox>& GetAudioClipBoxes() const { return m_AudioBoxes; }

    private:
        enum class RowKind
        {
            Binding,
            Transform,
            Animation,
            Activation,
            Particle,
            Property,
            Audio,
            Events,
            Cuts
        };
        struct Row
        {
            RowKind     Kind;
            size_t      Track = 0; // Index into the track list of its kind (Binding: into Bindings).
            std::string Binding;
        };
        // What the inspector edits: a track's item (key time for transforms).
        struct Selection
        {
            RowKind Kind  = RowKind::Binding;
            size_t  Track = SIZE_MAX, Item = SIZE_MAX;
            double  Time = 0;
            bool    Valid() const { return Track != SIZE_MAX; }
        };
        enum class DragKind
        {
            None,
            Playhead,
            Move,
            Start,
            End,
            BlendIn,
            BlendOut,
            FadeIn,
            FadeOut
        };

        Scene*                   GetScene() const { return m_Hierarchy.GetScene(); }
        TimelinePlayerComponent* Component() const;
        Entity                   BoundEntity(const std::string& binding) const;
        void                     Load();
        void                     CreateAsset();
        void                     Changed();
        void                     UpdatePreview();
        // Drops the preview without restoring the scene (Play owns it now).
        void                     DiscardPreview();
        // The time the playhead shows: the followed player's during Play.
        double                   DisplayTime() const;
        std::vector<Row>         Rows() const;
        double                   Snap(double time) const;
        float                    ToX(double time) const;
        double                   ToTime(float x) const;

        void DrawToolbar();
        void DrawFollowPill();
        void DrawNames(const std::vector<Row>& rows, float rowHeight);
        void DrawCanvas(const std::vector<Row>& rows, float rowHeight);
        void DrawRuler(ImVec2 origin, float width);
        void DrawRow(const Row& row, ImVec2 min, ImVec2 max);
        void DrawInspector();
        void DrawAddTrackMenu(const std::string& binding);
        void DeleteSelection();
        bool AssetCombo(const char* label, AssetType type, AssetID& id);
        void DrawAudioInspector();
        bool BusCombo(std::string& bus);
        // An AudioClip choice for a file dropped from the Content Browser.
        AssetID AudioChoice(const ImGuiPayload& payload) const;
        // What the editor knows of a clip without playing it: its length and peak envelope.
        const AudioClipData* AudioClipInfo(AssetID clip);
        double               AudioSourceSeconds(AssetID clip);
        const std::vector<uint8_t>&
                     WaveformColumns(const TimelineAudioClip& clip, double from, double to, size_t columns);
        AudioEngine* PreviewEngine() const { return m_Engine; }
        // Starts, follows and stops the preview's audio after the frame's edits and playhead moves.
        void UpdatePreviewAudio();
        void StopPreviewAudio();

        HierarchyPanel&                m_Hierarchy;
        std::vector<EditorAssetChoice> m_Choices;
        AssetManager*                  m_Assets  = nullptr;
        EditorAssetImportService*      m_Service = nullptr;
        std::function<void(bool)>      m_ViewThrough;

        Scene*                m_Scene = nullptr;
        UUID                  m_Owner = 0;
        AssetID               m_Asset;
        std::filesystem::path m_File, m_PendingFile; // Pending: a new file waiting for its first build.
        TimelineData          m_Data;
        bool                  m_Loaded = false, m_Dirty = false;
        std::string           m_Status;

        double    m_Time = 0, m_Scroll = 0;
        float     m_Zoom = 120; // Pixels per second.
        bool      m_Snap = true, m_Playing = false;
        float     m_CanvasX = 0;
        Selection m_Selected;
        DragKind  m_Drag = DragKind::None;
        Selection m_DragTarget;
        double    m_DragOffset = 0;

        bool                               m_Previewing = false, m_PlayerDirty = true, m_NeedsEvaluate = true;
        bool                               m_ViewingCamera = false;
        std::unique_ptr<TimelinePlayer>    m_Player;
        std::map<UUID, TransformComponent> m_Bases; // Bound transforms when preview started.
        int                                m_LastDrawnFrame   = -1;

        std::function<bool()>  m_PlayMode;
        bool                   m_WasPlayMode = false;
        TimelineFollow::Model  m_Follow;
        TimelineActiveSections m_Active;                  // Sections active at the followed time.
        double                 m_UserScrolledAt = -1e9;   // ImGui time of the last wheel or middle drag.
        char                               m_PropertyPath[96] = "Light.Intensity";

        TimelineAudioEditing::History        m_History;
        bool                                 m_Focused = false;
        TimelineAudioClip                    m_DragAudio;  // The dragged audio clip when its drag began.
        std::string                          m_AudioError; // Why the last audio edit was refused.
        AudioEngine*                         m_Engine = nullptr;
        std::unique_ptr<TimelineAudioPlayer> m_Audio;
        bool                                 m_AudioDirty = true, m_AudioSeek = false, m_AudioPlaying = false;
        std::vector<AudioClipBox>            m_AudioBoxes;
        AudioPreviewCache                    m_AudioSummaries;
        struct WaveformKey
        {
            AssetID Clip;
            double  From = 0, To = 0;
            size_t  Columns = 0;
            bool    operator<(const WaveformKey& o) const
            {
                return Clip != o.Clip ? Clip < o.Clip :
                       From != o.From ? From < o.From :
                       To != o.To     ? To < o.To :
                                        Columns < o.Columns;
            }
        };
        std::map<WaveformKey, std::vector<uint8_t>> m_Waveforms; // Computed once per clip, range and width.
    };
} // namespace Kans
