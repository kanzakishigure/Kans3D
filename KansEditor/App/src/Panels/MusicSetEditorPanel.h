#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "KansEditorCore/Audio/AudioPreviewCache.h"
#include "KansEditorCore/Audio/MusicSetEditing.h"
#include "KansEditorCore/Audio/MusicSetPreview.h"
#include "KansEditorCore/EditorAssetChoice.h"

namespace Kans
{
    class EditorAssetImportService;

    // The Music Set editor: the open .musicset's segments (a list, and a timeline of the selected one with
    // its stems as lanes: waveform, bus, level, mute and solo for the audition; a bar/beat ruler from the
    // segment's tempo; entry, exit and custom cues dragged along it and snapped to beats or bars; the
    // pre-entry and post-exit tails shaded), its parameters with the stem volume curves they drive, its
    // states (a segment or a playlist), the transition matrix (from rows, to columns, Any included; a cell
    // shows its rule and opens it in the inspector, and the rule that wins for the selected pair is lit),
    // and its stingers. Validation reasons show next to the item they concern and an invalid set is never
    // saved. Every edit goes into the panel's own undo history (a drag is one step); Save writes the source
    // and rebuilds it, so hot reload applies it. The audition plays the edited set through MusicSetPreview
    // (states, parameters, stingers, a playhead with bar.beat and the pending transition on the timeline).
    class MusicSetEditorPanel
    {
    public:
        enum class Tab
        {
            Segments,
            Parameters,
            States,
            Transitions,
            Stingers
        };
        static constexpr int TabCount = 5;

        // The project's import service (may be null), the editor's engine and asset manager (may be null).
        void Bind(EditorAssetImportService* service, AudioEngine* engine, AssetManager* assets);
        // The published assets; the AudioClips are offered for stems and stingers.
        void SetAssetChoices(const std::vector<EditorAssetChoice>& choices);
        // Peaks of a clip for its lane's waveform; without one, the import service's clip previews.
        void SetWaveformSource(std::function<const std::vector<uint8_t>*(AssetID)> source)
        {
            m_Waveforms = std::move(source);
        }
        // A clip's length in seconds (none: unknown or missing); without one, the import service's previews.
        void SetClipDurationSource(std::function<std::optional<double>(AssetID)> source)
        {
            m_Durations = std::move(source);
        }
        // True while the scene plays: the audition is off (the scene's audio takes over).
        void SetPlayModeQuery(std::function<bool()> query) { m_PlayMode = std::move(query); }

        // Loads a .musicset; false (with the reason in the status) when it cannot be read.
        bool Open(const std::filesystem::path& file);
        // Writes the file when the set is valid; false (with the reason in the status) otherwise.
        bool Save();
        void StopPreview() { m_Preview.Stop(); }

        void Draw();
        bool Undo();
        bool Redo();
        bool HasFocus() const { return m_Focused; }

        MusicSetEditing::MusicSetDocument&       GetDocument() { return m_Document; }
        const MusicSetEditing::MusicSetDocument& GetDocument() const { return m_Document; }
        MusicSetPreview&                         GetPreview() { return m_Preview; }
        const std::filesystem::path&             GetFile() const { return m_File; }
        const std::string&                       GetStatus() const { return m_Status; }
        // The issues of the set as edited (with clip lengths when known), as the last frame computed them.
        const std::vector<MusicSetEditing::Issue>& GetIssues() const { return m_Issues; }

        Tab    GetTab() const { return m_Tab; }
        void   SetTab(Tab tab) { m_Tab = tab, m_SelectTab = true; }
        size_t GetSegment() const { return m_Segment; }
        void   SelectSegment(size_t index) { m_Segment = index; }
        void   SelectState(size_t index) { m_State = index; }
        // The selected matrix cell (from, to).
        const std::pair<std::string, std::string>& GetCell() const { return m_Cell; }
        void                  SelectCell(const std::string& from, const std::string& to) { m_Cell = {from, to}; }
        MusicSetEditing::Snap GetSnap() const { return m_Snap; }
        void                  SetSnap(MusicSetEditing::Snap snap) { m_Snap = snap; }
        // The stem whose volume curve the Parameters tab edits (segment, stem).
        void SelectCurve(size_t segment, size_t stem) { m_Curve = {segment, stem}; }
        bool IsMuted(size_t segment, size_t stem) const { return m_Muted.count({segment, stem}) != 0; }
        bool IsSoloed(size_t segment, size_t stem) const { return m_Soloed.count({segment, stem}) != 0; }
        // The set the audition plays: the edited set with muted (or not soloed) stems silent.
        const MusicSetData& GetPreviewData() const { return m_PreviewData; }

        // Where the last frame drew each control (screen space), for tooltips and UI tests.
        struct LaneBoxes
        {
            ImRect      Lane, Clip, Bus, Volume, Mute, Solo, Remove, Wave;
            std::string Issue; // The stem's validation reason, as drawn.
        };
        struct CueBox
        {
            MusicSetEditing::CueRef Cue;
            ImRect                  Handle;
            float                   X = 0;
        };
        using Pair = std::pair<std::string, std::string>;
        struct Boxes
        {
            ImRect Validation, Save, Undo, Redo, Play, Stop, Tabs[TabCount];
            // The audition bar.
            std::vector<std::pair<std::string, ImRect>> StateButtons, StingerButtons, ParameterSliders;
            std::string                                 Position, Pending;
            // Segments.
            std::vector<ImRect>    SegmentRows;
            ImRect                 AddSegment, Name, BPM, Meter, FirstBeat, Loop, PreEntry, PostExit, Snap;
            ImRect                 Ruler, Timeline, AddStem, AddCue, PreEntryShade, PostExitShade;
            std::vector<LaneBoxes> Lanes;
            std::vector<CueBox>    Cues;
            std::vector<ImRect>    CueRows;
            std::vector<std::pair<AssetID, ImRect>> ClipItems;                         // Items of an open clip list.
            float                                   Playhead = -1, PendingMarker = -1; // X on the timeline (-1: none).
            std::string                             PlayheadLabel, PendingLabel;
            // Parameters.
            std::vector<ImRect>                 ParameterRows;
            ImRect                              AddParameter, CurveStem, CurveDriver, CurvePlot;
            std::vector<ImVec2>                 CurveKeys;
            std::vector<std::pair<int, ImRect>> DriverItems; // Items of the open "Driven by" list (-1: none).
            // States.
            std::vector<ImRect> StateRows;
            ImRect              AddState, AddPlaylistItem;
            // Transitions.
            std::map<Pair, ImRect> Cells;
            std::optional<Pair>    Winner; // The cell of the rule that wins for the selected pair.
            ImRect AddRule, RemoveRule, Sync, Bars, SyncCue, FadeOut, FadeOutCurve, FadeIn, FadeInCurve, Entry,
                EntryCue, Via;
            std::vector<std::pair<int, ImRect>> SyncItems; // Items of the open sync list (MusicSync order).
            // Stingers.
            std::vector<ImRect> StingerRows;
            ImRect              AddStinger;
            // The validation reasons drawn beside their items this frame, in order.
            std::vector<std::string> ShownIssues;
        };
        const Boxes& GetBoxes() const { return m_Boxes; }

    private:
        void DrawHeader();
        void DrawAudition();
        void DrawSegments();
        void DrawSegmentFields(size_t index);
        void DrawTimeline(size_t index);
        void DrawCueList(size_t index);
        void DrawParameters();
        void DrawCurve();
        void DrawStates();
        void DrawTransitions();
        void DrawRuleInspector();
        void DrawStingers();
        // A validation reason drawn in red beside its item (and recorded in the boxes).
        void IssueLine(const MusicSetEditing::Issue* issue);
        // Changes a copy of the set, committed at the end of the frame (controls keep reading the set as it
        // was); a continuous edit (a field or handle held) joins the gesture.
        void                        Edit(const std::function<void(MusicSetData&)>& change);
        void                        CommitPending();
        const MusicSetData&         Current() const { return m_Pending ? *m_Pending : m_Document.Get(); }
        const EditorAssetChoice*    FindClip(AssetID clip) const;
        AssetID                     ClipAt(const std::filesystem::path& file) const;
        const std::vector<uint8_t>* PeaksOf(AssetID clip);
        std::optional<double>       DurationOf(AssetID clip);
        // A list of the imported clips; also takes an audio file dropped on it. True when `clip` changed.
        bool ClipCombo(const char* id, AssetID& clip, const char* none);
        bool BusCombo(const char* id, std::string& bus);
        void RefreshPreviewData();
        bool Auditioning() const;

        EditorAssetImportService*                           m_Service = nullptr;
        AudioEngine*                                        m_Engine  = nullptr;
        std::vector<EditorAssetChoice>                      m_Clips;
        std::function<const std::vector<uint8_t>*(AssetID)> m_Waveforms;
        std::function<std::optional<double>(AssetID)>       m_Durations;
        std::function<bool()>                               m_PlayMode;
        AudioPreviewCache                                   m_PeakCache;
        MusicSetEditing::MusicSetDocument                   m_Document;
        MusicSetPreview                                     m_Preview;
        std::optional<MusicSetData>                         m_Pending;
        bool                                                m_PendingContinuous = false;
        std::filesystem::path                               m_File;
        bool                                                m_Opened = false, m_Focused = false;
        std::string                                         m_Status;
        std::vector<MusicSetEditing::Issue>                 m_Issues;
        Tab                                                 m_Tab       = Tab::Segments;
        bool                                                m_SelectTab = false;
        size_t                                              m_Segment = 0, m_State = 0;
        std::pair<size_t, size_t>                           m_Curve {0, 0};
        std::optional<size_t>                               m_CurveKey;
        std::pair<std::string, std::string>                 m_Cell {"Any", "Any"};
        MusicSetEditing::Snap                               m_Snap = MusicSetEditing::Snap::Beat;
        std::set<std::pair<size_t, size_t>>                 m_Muted, m_Soloed;
        MusicSetData                                        m_PreviewData;
        uint64_t                                            m_PreviewRevision = 0, m_SeenRevision = UINT64_MAX;
        bool                                                m_MuteChanged = false;
        std::optional<MusicSetEditing::CueRef>              m_DragCue;
        std::optional<size_t>                               m_DragKey;
        std::string                                         m_NameEdit; // The segment name being typed.
        Boxes                                               m_Boxes;
    };
} // namespace Kans
