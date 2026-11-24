#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Timeline/Timeline.h"

// Editing the audio tracks of a timeline in the Timeline window, and its undo history. No UI.
namespace Kans::TimelineAudioEditing
{
    // Shortest clip a trim leaves (seconds).
    inline constexpr double MinimumClipSeconds = 1.0 / 240.0;

    // Why audio track `track` cannot play as it is (overlapping or unsorted clips, a clip outside
    // the timeline or past the end of its source, a fade longer than its clip, an invalid level, an
    // unknown binding), or none. Follows ValidateTimeline's audio rules with a reason for the user.
    // `sourceSeconds` gives a clip's source length when known (0 or absent: not checked).
    std::optional<std::string>
    ValidateTrack(const TimelineData& data, size_t track, const std::function<double(AssetID)>& sourceSeconds = {});

    // Appends a track on `bus`, 2D when `binding` is empty; returns its index.
    size_t AddTrack(TimelineData& data, const std::string& binding = {}, const std::string& bus = "Music");

    // Adds `clip` (`seconds` long) to `track` at `start`: the clip ends where the next clip starts,
    // and the timeline grows to hold it. Refused (with the reason, data unchanged) when `start` is
    // inside another clip or outside the timeline. Returns the new clip's index in the sorted track.
    std::variant<size_t, std::string>
    AddClip(TimelineData& data, size_t track, AssetID clip, double start, double seconds);

    // What a drag on a clip changes.
    enum class Edge
    {
        Move,    // Start and End together.
        Start,   // Trims the start: ClipIn follows, so the audio stays where it was.
        End,     // Trims the end.
        FadeIn,  // Fade-in length (from Start).
        FadeOut, // Fade-out length (to End).
    };
    // Applies a drag of `edge` to `time` on clip `index`, starting from `original` (the clip when the
    // drag began). Trims stay within the source (`sourceSeconds`, 0 when unknown) and the timeline,
    // fades within the clip. The track stays sorted and `index` follows the clip. Refused (data
    // unchanged, the reason returned) when the result does not validate, e.g. it overlaps a clip.
    std::optional<std::string> DragClip(TimelineData&            data,
                                        size_t                   track,
                                        size_t&                  index,
                                        const TimelineAudioClip& original,
                                        Edge                     edge,
                                        double                   time,
                                        double                   sourceSeconds = 0);

    // Replaces clip `index` (re-sorting the track, `index` follows) or the track's settings with
    // `clip`/`settings` when the track still validates; otherwise data stays and the reason returns.
    std::optional<std::string> SetClip(TimelineData& data, size_t track, size_t& index, const TimelineAudioClip& clip);
    std::optional<std::string> SetTrack(TimelineData& data, size_t track, const TimelineAudioTrack& settings);

    // Peak columns for the part [from, to) (seconds into the source) of a clip `clipSeconds` long
    // whose envelope is `peaks` (evenly spaced over the clip): each column is the largest peak of
    // the buckets it touches; columns past the end of the source are 0.
    std::vector<uint8_t>
    EnvelopeColumns(const std::vector<uint8_t>& peaks, double clipSeconds, double from, double to, size_t columns);

    // Audio offset conversions: seconds to frames at `fps` (rounded to whole frames when `whole`),
    // frames to seconds, and seconds to milliseconds.
    double        OffsetFrames(double seconds, double fps, bool whole = false);
    double        FramesToSeconds(double frames, double fps);
    inline double Milliseconds(double seconds) { return seconds * 1000.0; }

    // Several players' stats as one: voices add up, the drift is the one furthest from zero.
    TimelineAudioStats CombineStats(const TimelineAudioStats& a, const TimelineAudioStats& b);
    // The combined audio stats of the Timeline Players running in `scene`.
    TimelineAudioStats SceneStats(const Scene& scene);

    // Snapshot undo for the Timeline window: edits mark the history changed; Settle turns them into
    // one step once no gesture (drag, text field) is active, so a drag is one step.
    class History
    {
    public:
        static constexpr size_t MaxSteps = 200;

        // Starts over from `data` (a timeline loaded or replaced).
        void Reset(const TimelineData& data);
        void MarkChanged() { m_Pending = true; }
        bool HasPending() const { return m_Pending; }
        // True when a step was recorded.
        bool Settle(const TimelineData& current, bool gestureActive);
        // Restore the state before the last step (after) into `current`; false when there is none.
        bool   Undo(TimelineData& current);
        bool   Redo(TimelineData& current);
        bool   CanUndo() const { return !m_Undo.empty() || m_Pending; }
        bool   CanRedo() const { return !m_Redo.empty(); }
        size_t UndoSteps() const { return m_Undo.size(); }

    private:
        TimelineData              m_Base;
        std::vector<TimelineData> m_Undo, m_Redo;
        bool                      m_Pending = false;
    };
} // namespace Kans::TimelineAudioEditing
