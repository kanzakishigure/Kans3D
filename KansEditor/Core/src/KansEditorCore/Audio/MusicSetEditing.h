#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Audio/MusicSet.h"

// The Music Set editor's model, without ImGui: naming and reference-keeping edits (renaming a
// state renames its rules), the segment's tempo grid (ruler ticks, cue snapping to beats and bars,
// bar.beat labels), the transition matrix (the explicit rule of a cell and the rule that wins for a
// from/to pair, most specific first), the layer volume curves, the validation reasons attached to
// the item they concern, the edited set with its undo history, and music set files.
namespace Kans::MusicSetEditing
{
    // ---- Names ----
    const char* SyncName(MusicSync sync);
    const char* EntryName(MusicEntry entry);
    const char* OrderName(MusicPlaylistOrder order);
    const char* FadeCurveName(AudioFadeCurve curve);
    // `base`, or "`base` 2", "`base` 3"... the first not in `taken`.
    std::string                  UniqueName(const std::vector<std::string>& taken, const std::string& base);
    inline constexpr const char* AnyState = "Any";

    // ---- Edits that keep references ----
    // A segment named "Segment", "Segment 2"... (120 BPM, 4/4, no stems yet); returns its index.
    size_t AddSegment(MusicSetData& data);
    // Removes the segment; states, playlists and bridges naming it keep the name (validation shows them).
    bool RemoveSegment(MusicSetData& data, size_t index);
    // Renames and updates the states, playlist items and transition bridges that name it. False for an
    // empty or taken name.
    bool   RenameSegment(MusicSetData& data, size_t index, const std::string& name);
    size_t AddLayer(MusicSegment& segment, AssetID clip);
    bool   RemoveLayer(MusicSegment& segment, size_t index);
    // A custom cue "Cue", "Cue 2"... at `time`; returns its index.
    size_t AddCue(MusicSegment& segment, const MusicTime& time);
    bool   RemoveCue(MusicSegment& segment, size_t index);

    size_t AddParameter(MusicSetData& data);
    // Removing a parameter also drops the curves of the stems it drove.
    bool RemoveParameter(MusicSetData& data, size_t index);
    // Renames and updates the stems it drives. False for an empty or taken name.
    bool RenameParameter(MusicSetData& data, size_t index, const std::string& name);
    // Drives a stem by `parameter` (empty: none). A new curve runs 0 dB across the parameter's range.
    void SetLayerParameter(const MusicSetData& data, MusicLayer& layer, const std::string& parameter);

    // A state playing the first segment (when there is one); returns its index.
    size_t AddState(MusicSetData& data);
    // Removing a state also removes the transition rules from or to it.
    bool RemoveState(MusicSetData& data, size_t index);
    // Renames and updates the rules from or to it. False for an empty, taken or reserved ("Any") name.
    bool RenameState(MusicSetData& data, size_t index, const std::string& name);

    // A rule for this from/to pair; none (nothing added) when the pair already has one.
    std::optional<size_t> AddTransition(MusicSetData& data, const std::string& from, const std::string& to);
    bool                  RemoveTransition(MusicSetData& data, size_t index);
    size_t                AddStinger(MusicSetData& data, AssetID clip);
    bool                  RemoveStinger(MusicSetData& data, size_t index);

    // ---- The tempo grid ----
    double BeatSeconds(const MusicSegment& segment);
    double BarSeconds(const MusicSegment& segment);
    enum class Snap
    {
        Off,
        Beat,
        Bar
    };
    // The time a cue dragged to `seconds` takes: plain seconds, or the nearest beat or bar line written
    // as zero-based bar and beat (never before the first beat).
    MusicTime SnapTime(const MusicSegment& segment, double seconds, Snap snap);
    // "Bar 3, beat 2" (one-based, as the ruler counts) for a bar/beat time, "1.250 s" otherwise.
    std::string TimeLabel(const MusicSegment& segment, const MusicTime& time);
    // "3.2": one-based bar and beat of a zero-based position.
    std::string BarBeatLabel(uint64_t bar, double beatInBar);
    struct RulerTick
    {
        double   Seconds = 0;
        uint32_t Bar = 0, Beat = 0; // Zero-based.
        bool     IsBar = false;
    };
    // The ticks between 0 and `length` seconds at `pixelsPerSecond`: every beat while beats stay at
    // least `minSpacing` pixels apart, else bars only, thinned to every 2nd, 4th... bar when bars crowd.
    std::vector<RulerTick>
    RulerTicks(const MusicSegment& segment, double length, double pixelsPerSecond, double minSpacing);

    // ---- Cues on the timeline ----
    enum class CueKind
    {
        Entry,
        Exit,
        Custom
    };
    struct CueRef
    {
        CueKind Kind  = CueKind::Entry;
        size_t  Index = 0; // Custom cues.
    };
    // Where a cue is (seconds); the exit without one of its own is the segment's end (`length`).
    double CueSeconds(const MusicSegment& segment, CueRef cue, double length);
    // Moves a cue to `seconds` clamped to [0, length] (with `snap`). False when the cue does not exist.
    bool MoveCue(MusicSegment& segment, CueRef cue, double seconds, Snap snap, double length);
    // The playing length of a segment: its shortest stem (none while a stem's clip is unknown).
    std::optional<double> SegmentLength(const MusicSegment& segment, const MusicClipDuration& duration);

    // ---- Parameter curves ----
    // The stem's curve offset (dB) at `input`, as the player shapes it (0 without a curve).
    float CurveDb(const MusicLayer& layer, float input);
    // Inserts a key where its input belongs; none when a key already sits within `gap` of it.
    std::optional<size_t> InsertKey(std::vector<MusicVolumeKey>& keys, MusicVolumeKey key, float gap);
    // Moves a key, its input kept strictly between its neighbours' (`gap` inside them).
    bool MoveKey(std::vector<MusicVolumeKey>& keys, size_t index, float input, float db, float gap);
    bool RemoveKey(std::vector<MusicVolumeKey>& keys, size_t index);

    // ---- The transition matrix ----
    // Rows (from) and columns (to): "Any" then the states in order.
    std::vector<std::string> MatrixStates(const MusicSetData& data);
    // The rule written for exactly this pair, if any.
    std::optional<size_t> RuleAt(const MusicSetData& data, const std::string& from, const std::string& to);
    // The rule the player uses going from `from` to `to`: the most specific match (from and to, then
    // from and Any, then Any and to, then Any and Any); none when no rule applies (the default cut).
    std::optional<size_t> WinningRule(const MusicSetData& data, const std::string& from, const std::string& to);
    // "Next bar · out 0.50 s · in 0.25 s" (fades left out when zero; NextNBar says how many bars).
    std::string RuleSummary(const MusicTransition& rule);

    // ---- Validation, item by item ----
    enum class Where
    {
        Set,
        Segment,
        Layer,
        Cue,
        Parameter,
        State,
        PlaylistItem,
        Transition,
        Stinger
    };
    struct Issue
    {
        Where       Kind  = Where::Set;
        size_t      Index = 0; // The segment, parameter, state, transition or stinger.
        size_t      Item  = 0; // The layer or cue of a segment, the playlist item of a state.
        std::string Message;
    };
    // Every reason ValidateMusicSet (with the same `duration`) can give, each attached to the item it
    // concerns; empty exactly when the set validates.
    std::vector<Issue> CollectIssues(const MusicSetData& data, const MusicClipDuration& duration = {});
    // The first issue about this item (Item is compared only for layers, cues and playlist items).
    const Issue* FindIssue(const std::vector<Issue>& issues, Where kind, size_t index, size_t item = 0);

    // ---- The edited set ----
    bool Equal(const MusicSetData& a, const MusicSetData& b);
    // The set as edited, with its undo history. Every edit is kept (also one that makes the set invalid:
    // the issues say why it cannot be saved); a continuous edit (a drag) joins the open gesture.
    class MusicSetDocument
    {
    public:
        static constexpr size_t MaxHistory = 200;

        void                Reset(MusicSetData data, bool saved = true);
        const MusicSetData& Get() const { return m_Data; }
        uint64_t            GetRevision() const { return m_Revision; }
        // False when `next` equals the current set (nothing recorded).
        bool Commit(MusicSetData next, bool continuous = false);
        void EndGesture() { m_GestureOpen = false; }
        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        bool Undo();
        bool Redo();
        bool IsDirty() const { return !m_Saved || !Equal(m_Data, *m_Saved); }
        void MarkSaved() { m_Saved = m_Data; }

    private:
        void Set(MusicSetData data);

        MusicSetData                m_Data;
        std::optional<MusicSetData> m_Saved;
        std::vector<MusicSetData>   m_Undo, m_Redo;
        bool                        m_GestureOpen = false;
        uint64_t                    m_Revision    = 0;
    };

    // ---- Files ----
    // `directory`/`stem`.musicset, or `stem` 2, 3... when taken.
    std::filesystem::path UniqueMusicSetFile(const std::filesystem::path& directory, const std::string& stem);
    // Writes `data` to `file`; refuses an invalid set without touching the file.
    std::optional<AssetError> SaveMusicSetFile(const std::filesystem::path& file, const MusicSetData& data);
    std::variant<MusicSetData, AssetError> LoadMusicSetFile(const std::filesystem::path& file);
    // One looping segment named `name` with the clips as its stems, and a state of the same name playing it.
    MusicSetData FromClips(const std::vector<AssetID>& clips, const std::string& name);
    // A new (by default empty, which is valid) set beside the others; returns its path.
    std::variant<std::filesystem::path, AssetError>
    CreateMusicSetFile(const std::filesystem::path& directory, const std::string& stem, const MusicSetData& data = {});
} // namespace Kans::MusicSetEditing
