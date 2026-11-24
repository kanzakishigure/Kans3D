#include "TimelineAudioEditing.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans::TimelineAudioEditing
{
    namespace
    {
        std::string Clip(size_t index) { return "Clip " + std::to_string(index + 1); }

        std::optional<std::string> Validate(const TimelineData&                   data,
                                            const TimelineAudioTrack&             track,
                                            const std::function<double(AssetID)>& sourceSeconds)
        {
            if (!track.Binding.empty() && std::none_of(data.Bindings.begin(), data.Bindings.end(), [&](const auto& b) {
                    return b.ID == track.Binding;
                }))
                return "Unknown binding \"" + track.Binding + "\"";
            if (track.Bus.size() > 4096 || track.Bus.find('\0') != std::string::npos)
                return std::string("Invalid bus name");
            double previousEnd = 0;
            for (size_t i = 0; i < track.Clips.size(); ++i)
            {
                const auto& c = track.Clips[i];
                if (!c.Clip.IsValid())
                    return Clip(i) + " has no audio clip";
                if (!std::isfinite(c.Start) || !std::isfinite(c.End) || c.Start < 0 || c.End > data.Length)
                    return Clip(i) + " is outside the timeline";
                if (c.End <= c.Start)
                    return Clip(i) + " ends before it starts";
                if (c.Start < previousEnd)
                    return Clip(i) + " overlaps the clip before it";
                if (!std::isfinite(c.ClipIn) || c.ClipIn < 0)
                    return Clip(i) + " starts before its audio";
                if (const double source = sourceSeconds ? sourceSeconds(c.Clip) : 0.0;
                    source > 0 && c.ClipIn + (c.End - c.Start) > source + 1e-6)
                    return Clip(i) + " runs past the end of its audio";
                if (!IsValidAudioBusLevel(c.VolumeDb))
                    return Clip(i) + " has an invalid volume";
                for (const auto* fade : {&c.FadeIn, &c.FadeOut})
                    if (!std::isfinite(fade->Length) || fade->Length < 0 || fade->Length > c.End - c.Start)
                        return Clip(i) + ": a fade is longer than the clip";
                previousEnd = c.End;
            }
            return std::nullopt;
        }

        // Sorts the clips by start; `index` follows its clip.
        void Sort(TimelineAudioTrack& track, size_t& index)
        {
            std::vector<size_t> order(track.Clips.size());
            std::iota(order.begin(), order.end(), size_t(0));
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return track.Clips[a].Start < track.Clips[b].Start;
            });
            std::vector<TimelineAudioClip> sorted;
            sorted.reserve(order.size());
            size_t moved = index;
            for (size_t i = 0; i < order.size(); ++i)
            {
                sorted.push_back(track.Clips[order[i]]);
                if (order[i] == index)
                    moved = i;
            }
            track.Clips = std::move(sorted);
            index       = moved;
        }
    } // namespace

    std::optional<std::string>
    ValidateTrack(const TimelineData& data, size_t track, const std::function<double(AssetID)>& sourceSeconds)
    {
        if (track >= data.Audio.size())
            return std::string("No such audio track");
        return Validate(data, data.Audio[track], sourceSeconds);
    }

    size_t AddTrack(TimelineData& data, const std::string& binding, const std::string& bus)
    {
        TimelineAudioTrack track;
        track.Binding = binding;
        track.Bus     = bus;
        data.Audio.push_back(std::move(track));
        return data.Audio.size() - 1;
    }

    std::variant<size_t, std::string>
    AddClip(TimelineData& data, size_t track, AssetID clip, double start, double seconds)
    {
        if (track >= data.Audio.size())
            return std::string("No such audio track");
        if (!clip.IsValid())
            return std::string("No audio clip");
        if (!std::isfinite(start) || start < 0 || start > data.Length)
            return std::string("The clip would start outside the timeline");
        auto&  clips = data.Audio[track].Clips;
        double end   = start + (std::isfinite(seconds) && seconds > 0 ? seconds : 1.0);
        for (const auto& other : clips)
        {
            if (start >= other.Start && start < other.End)
                return std::string("The clip would start inside another clip");
            if (other.Start > start)
                end = std::min(end, other.Start);
        }
        if (end - start < MinimumClipSeconds)
            return std::string("No room for the clip before the next one");
        TimelineAudioTrack candidate = data.Audio[track];
        TimelineAudioClip  added;
        added.Clip  = clip;
        added.Start = start;
        added.End   = end;
        candidate.Clips.push_back(added);
        size_t index = candidate.Clips.size() - 1;
        Sort(candidate, index);
        TimelineData grown;
        grown.Length   = std::max(data.Length, end);
        grown.Bindings = data.Bindings;
        if (auto error = Validate(grown, candidate, {}))
            return *error;
        data.Length       = grown.Length;
        data.Audio[track] = std::move(candidate);
        return index;
    }

    std::optional<std::string> SetClip(TimelineData& data, size_t track, size_t& index, const TimelineAudioClip& clip)
    {
        if (track >= data.Audio.size() || index >= data.Audio[track].Clips.size())
            return std::string("No such audio clip");
        TimelineAudioTrack candidate = data.Audio[track];
        candidate.Clips[index]       = clip;
        size_t moved                 = index;
        Sort(candidate, moved);
        if (auto error = Validate(data, candidate, {}))
            return error;
        data.Audio[track] = std::move(candidate);
        index             = moved;
        return std::nullopt;
    }

    std::optional<std::string> SetTrack(TimelineData& data, size_t track, const TimelineAudioTrack& settings)
    {
        if (track >= data.Audio.size())
            return std::string("No such audio track");
        TimelineAudioTrack candidate = settings;
        candidate.Clips              = data.Audio[track].Clips;
        if (auto error = Validate(data, candidate, {}))
            return error;
        data.Audio[track] = std::move(candidate);
        return std::nullopt;
    }

    std::optional<std::string> DragClip(TimelineData&            data,
                                        size_t                   track,
                                        size_t&                  index,
                                        const TimelineAudioClip& original,
                                        Edge                     edge,
                                        double                   time,
                                        double                   sourceSeconds)
    {
        if (!std::isfinite(time))
            return std::string("Invalid time");
        TimelineAudioClip clip   = original;
        const double      length = original.End - original.Start;
        const auto        fit    = [&] {
            const double span   = clip.End - clip.Start;
            clip.FadeIn.Length  = std::clamp(clip.FadeIn.Length, 0.0, span);
            clip.FadeOut.Length = std::clamp(clip.FadeOut.Length, 0.0, span - clip.FadeIn.Length);
        };
        switch (edge)
        {
            case Edge::Move:
                clip.Start = std::clamp(time, 0.0, std::max(0.0, data.Length - length));
                clip.End   = clip.Start + length;
                break;
            case Edge::Start: {
                // The audio stays in place: the start cannot reach before the source's first frame.
                const double earliest = std::max(0.0, original.Start - original.ClipIn);
                const double latest   = original.End - MinimumClipSeconds;
                clip.Start            = std::clamp(time, earliest, std::max(earliest, latest));
                clip.ClipIn           = std::max(0.0, original.ClipIn + (clip.Start - original.Start));
                fit();
                break;
            }
            case Edge::End: {
                double latest = data.Length;
                if (sourceSeconds > 0)
                    latest = std::min(latest, original.Start + (sourceSeconds - original.ClipIn));
                const double earliest = original.Start + MinimumClipSeconds;
                clip.End              = std::clamp(time, earliest, std::max(earliest, latest));
                fit();
                break;
            }
            case Edge::FadeIn:
                clip.FadeIn.Length = std::clamp(time - clip.Start, 0.0, length - clip.FadeOut.Length);
                break;
            case Edge::FadeOut:
                clip.FadeOut.Length = std::clamp(clip.End - time, 0.0, length - clip.FadeIn.Length);
                break;
        }
        return SetClip(data, track, index, clip);
    }

    std::vector<uint8_t>
    EnvelopeColumns(const std::vector<uint8_t>& peaks, double clipSeconds, double from, double to, size_t columns)
    {
        std::vector<uint8_t> out(columns, 0);
        if (peaks.empty() || !(clipSeconds > 0) || columns == 0 || !(to > from))
            return out;
        const double buckets = double(peaks.size());
        const double step    = (to - from) / double(columns);
        for (size_t i = 0; i < columns; ++i)
        {
            const double a = from + step * double(i), b = a + step;
            if (a >= clipSeconds || b <= 0)
                continue;
            const auto first =
                size_t(std::clamp(std::floor(std::max(0.0, a) / clipSeconds * buckets), 0.0, buckets - 1));
            auto last =
                size_t(std::clamp(std::ceil(std::min(b, clipSeconds) / clipSeconds * buckets) - 1, 0.0, buckets - 1));
            last   = std::max(last, first);
            out[i] = *std::max_element(peaks.begin() + long(first), peaks.begin() + long(last) + 1);
        }
        return out;
    }

    double OffsetFrames(double seconds, double fps, bool whole)
    {
        if (!(fps > 0) || !std::isfinite(seconds))
            return 0;
        const double frames = seconds * fps;
        return whole ? std::round(frames) : frames;
    }

    double FramesToSeconds(double frames, double fps) { return fps > 0 && std::isfinite(frames) ? frames / fps : 0; }

    TimelineAudioStats CombineStats(const TimelineAudioStats& a, const TimelineAudioStats& b)
    {
        TimelineAudioStats out;
        out.ActiveVoices = a.ActiveVoices + b.ActiveVoices;
        out.DriftSeconds = std::abs(a.DriftSeconds) >= std::abs(b.DriftSeconds) ? a.DriftSeconds : b.DriftSeconds;
        return out;
    }

    TimelineAudioStats SceneStats(const Scene& scene)
    {
        TimelineAudioStats  total;
        std::vector<Entity> stack = scene.GetRootEntities();
        while (!stack.empty())
        {
            Entity entity = stack.back();
            stack.pop_back();
            if (entity.HasComponent<TimelinePlayerComponent>())
                if (const auto& player = entity.GetComponent<TimelinePlayerComponent>().Player)
                    total = CombineStats(total, player->GetAudioStats());
            const auto children = scene.GetChildren(entity);
            stack.insert(stack.end(), children.begin(), children.end());
        }
        return total;
    }

    void History::Reset(const TimelineData& data)
    {
        m_Base = data;
        m_Undo.clear();
        m_Redo.clear();
        m_Pending = false;
    }

    bool History::Settle(const TimelineData& current, bool gestureActive)
    {
        if (!m_Pending || gestureActive)
            return false;
        m_Undo.push_back(std::move(m_Base));
        if (m_Undo.size() > MaxSteps)
            m_Undo.erase(m_Undo.begin());
        m_Redo.clear();
        m_Base    = current;
        m_Pending = false;
        return true;
    }

    bool History::Undo(TimelineData& current)
    {
        Settle(current, false);
        if (m_Undo.empty())
            return false;
        m_Redo.push_back(current);
        current = std::move(m_Undo.back());
        m_Undo.pop_back();
        m_Base = current;
        return true;
    }

    bool History::Redo(TimelineData& current)
    {
        Settle(current, false);
        if (m_Redo.empty())
            return false;
        m_Undo.push_back(current);
        current = std::move(m_Redo.back());
        m_Redo.pop_back();
        m_Base = current;
        return true;
    }
} // namespace Kans::TimelineAudioEditing
