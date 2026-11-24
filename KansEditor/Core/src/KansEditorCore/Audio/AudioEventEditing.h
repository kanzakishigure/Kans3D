#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "Kans3D/Audio/AudioEvent.h"

// The AudioEvent editor's model, without ImGui: entry edits, selection shares, min-max range
// fields, the edited event with its undo history, new event files and the rows the Stats panel
// shows for voices and events.
namespace Kans::AudioEventEditing
{
    // ---- Entries ----
    // Each entry's share of a Random pick in percent (weight / sum of weights). Entries whose
    // weight is not a positive finite number get 0; all are 0 when no weight is positive.
    std::vector<float> SelectionShares(const AudioEventData& data);
    // Appends an entry playing `clip` (weight 1, no offsets); returns its index.
    size_t AddEntry(AudioEventData& data, AssetID clip);
    bool   RemoveEntry(AudioEventData& data, size_t index);
    // Moves an entry (to: the index it ends up at); false when either index is out of range.
    bool MoveEntry(AudioEventData& data, size_t from, size_t to);
    // The largest Avoid Repeating Last that still leaves an entry to pick: entries - 1 (0 for one).
    uint32_t MaxAvoidRepeatingLast(const AudioEventData& data);

    // ---- Ranges ----
    enum class RangeKind
    {
        Signed,     // Volume (dB) and pitch (semitones) offsets.
        Nonnegative // Start offsets (seconds).
    };
    // Why `range` cannot be stored, or nothing when it can.
    std::optional<std::string> CheckRange(AudioEventRange range, RangeKind kind);
    // A min-max pair as its two fields show it: a refused value stays in the field (with its
    // reason) while the stored range keeps its last accepted value.
    class RangeField
    {
    public:
        // Shows `stored` again unless one of the fields is being typed or dragged. The last refusal's
        // reason stays shown until an edit is accepted or the stored range changes elsewhere (undo).
        void Sync(AudioEventRange stored, bool editing);
        // The user changed one side (`upper`: max, else min) to `value`. Returns the range to store when the pair is
        // valid; otherwise none, and GetError says why.
        std::optional<AudioEventRange> Edit(bool upper, float value, RangeKind kind);
        AudioEventRange                GetShown() const { return m_Shown; }
        const std::string&             GetError() const { return m_Error; }

    private:
        AudioEventRange m_Shown, m_Stored;
        std::string     m_Error;
    };

    // ---- Comparison and text ----
    bool        Equal(const AudioEventData& a, const AudioEventData& b);
    const char* SelectionName(AudioEventSelection selection);
    const char* RuleName(AudioConcurrencyRule rule);
    const char* StatusName(AudioEventTriggerStatus status);

    // ---- The edited event ----
    // The event as edited, with its undo history. Every edit is kept (also one that makes the
    // event invalid, such as removing the last entry: GetError says why it cannot be saved); a
    // continuous edit (a field being dragged) joins the open gesture, so a drag is one undo step.
    class EventDocument
    {
    public:
        static constexpr size_t MaxHistory = 200;

        void                  Reset(AudioEventData data, bool saved = true);
        const AudioEventData& Get() const { return m_Data; }
        uint64_t              GetRevision() const { return m_Revision; }
        // False when `next` equals the current event (nothing recorded).
        bool Commit(AudioEventData next, bool continuous = false);
        void EndGesture() { m_GestureOpen = false; }
        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        bool Undo();
        bool Redo();
        bool IsDirty() const { return !m_Saved || !Equal(m_Data, *m_Saved); }
        void MarkSaved() { m_Saved = m_Data; }
        // ValidateAudioEvent's reason, or none when the event can be saved.
        std::optional<std::string> GetError() const;

    private:
        void Set(AudioEventData data);

        AudioEventData                m_Data;
        std::optional<AudioEventData> m_Saved;
        std::vector<AudioEventData>   m_Undo, m_Redo;
        bool                          m_GestureOpen = false;
        uint64_t                      m_Revision    = 0;
    };

    // ---- Files ----
    // An event playing each clip once (weight 1), in order; Random selection.
    AudioEventData FromClips(const std::vector<AssetID>& clips);
    // `directory`/`stem`.audioevent, or `stem` 2, 3... when taken.
    std::filesystem::path UniqueEventFile(const std::filesystem::path& directory, const std::string& stem);
    // Writes `data` (which must be valid) to a new file beside the others; returns its path.
    std::variant<std::filesystem::path, AssetError>
    CreateEventFile(const std::filesystem::path& directory, const std::string& stem, const AudioEventData& data);
    // Writes `data` to `file`; refuses an invalid event without touching the file.
    std::optional<AssetError> SaveEventFile(const std::filesystem::path& file, const AudioEventData& data);
    std::variant<AudioEventData, AssetError> LoadEventFile(const std::filesystem::path& file);

    // ---- Statistics ----
    using StatsRows = std::vector<std::pair<std::string, std::string>>;
    // Real, virtual and allocated voices, steals (this frame / total), rejected starts, then one
    // row per concurrency group ("Group <name>": its active count).
    StatsRows VoiceStatsRows(const AudioVoiceStats& stats);
    // "2 active, 14 triggers, 3 cooldown, 1 limit".
    std::string EventStatsText(const AudioEventStats& stats);
} // namespace Kans::AudioEventEditing
