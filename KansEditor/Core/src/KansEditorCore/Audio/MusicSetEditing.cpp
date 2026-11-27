#include "MusicSetEditing.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include "Kans3D/Audio/AudioBusLayout.h"

namespace Kans::MusicSetEditing
{
    namespace
    {
        bool Time(double value) { return std::isfinite(value) && value >= 0; }

        template<class List>
        std::vector<std::string> NamesOf(const List& list)
        {
            std::vector<std::string> names;
            names.reserve(list.size());
            for (const auto& item : list)
                names.push_back(item.Name);
            return names;
        }

        template<class List>
        bool HasName(const List& list, const std::string& name)
        {
            return std::any_of(list.begin(), list.end(), [&](const auto& item) { return item.Name == name; });
        }

        template<class List>
        bool Erase(List& list, size_t index)
        {
            if (index >= list.size())
                return false;
            list.erase(list.begin() + std::ptrdiff_t(index));
            return true;
        }

        // A name the set can store for an item of `list` at `index`.
        template<class List>
        bool Renameable(const List& list, size_t index, const std::string& name)
        {
            if (index >= list.size() || name.empty() || name.size() >= 256)
                return false;
            for (size_t i = 0; i < list.size(); ++i)
                if (i != index && list[i].Name == name)
                    return false;
            return true;
        }

        bool SameTime(const MusicTime& a, const MusicTime& b)
        {
            return a.Seconds == b.Seconds && a.Bar == b.Bar && a.Beat == b.Beat;
        }
        bool SameSync(const MusicSyncPoint& a, const MusicSyncPoint& b)
        {
            return a.Sync == b.Sync && a.Bars == b.Bars && a.Cue == b.Cue;
        }
        bool SameFade(const MusicFade& a, const MusicFade& b) { return a.Seconds == b.Seconds && a.Curve == b.Curve; }

        bool SameLayer(const MusicLayer& a, const MusicLayer& b)
        {
            return a.Clip == b.Clip && a.Bus == b.Bus && a.VolumeDb == b.VolumeDb && a.Parameter == b.Parameter &&
                   std::equal(a.Curve.begin(),
                              a.Curve.end(),
                              b.Curve.begin(),
                              b.Curve.end(),
                              [](const MusicVolumeKey& x, const MusicVolumeKey& y) {
                                  return x.Input == y.Input && x.VolumeDb == y.VolumeDb &&
                                         x.Interpolation == y.Interpolation;
                              });
        }
        bool SameSegment(const MusicSegment& a, const MusicSegment& b)
        {
            return a.Name == b.Name && a.BPM == b.BPM && a.FirstBeatSeconds == b.FirstBeatSeconds &&
                   a.BeatsPerBar == b.BeatsPerBar && SameTime(a.Entry, b.Entry) &&
                   a.Exit.has_value() == b.Exit.has_value() && (!a.Exit || SameTime(*a.Exit, *b.Exit)) &&
                   a.Loop == b.Loop && a.PreEntry == b.PreEntry && a.PostExit == b.PostExit &&
                   std::equal(a.Layers.begin(), a.Layers.end(), b.Layers.begin(), b.Layers.end(), SameLayer) &&
                   std::equal(a.Cues.begin(),
                              a.Cues.end(),
                              b.Cues.begin(),
                              b.Cues.end(),
                              [](const MusicCue& x, const MusicCue& y) {
                                  return x.Name == y.Name && SameTime(x.Time, y.Time);
                              });
        }
        bool SameParameter(const MusicParameter& a, const MusicParameter& b)
        {
            return a.Name == b.Name && a.Min == b.Min && a.Max == b.Max && a.Default == b.Default &&
                   a.RampSeconds == b.RampSeconds;
        }
        bool SameState(const MusicState& a, const MusicState& b)
        {
            return a.Name == b.Name && a.Segment == b.Segment && a.Order == b.Order &&
                   a.NoImmediateRepeat == b.NoImmediateRepeat &&
                   std::equal(a.Playlist.begin(),
                              a.Playlist.end(),
                              b.Playlist.begin(),
                              b.Playlist.end(),
                              [](const MusicPlaylistItem& x, const MusicPlaylistItem& y) {
                                  return x.Segment == y.Segment && x.Weight == y.Weight;
                              });
        }
        bool SameTransition(const MusicTransition& a, const MusicTransition& b)
        {
            return a.From == b.From && a.To == b.To && SameSync(a.Sync, b.Sync) && SameFade(a.FadeOut, b.FadeOut) &&
                   SameFade(a.FadeIn, b.FadeIn) && a.Entry == b.Entry && a.Cue == b.Cue && a.Via == b.Via;
        }
        bool SameStinger(const MusicStinger& a, const MusicStinger& b)
        {
            return a.Name == b.Name && a.Clip == b.Clip && a.Bus == b.Bus && SameSync(a.Sync, b.Sync) &&
                   a.MinimumInterval == b.MinimumInterval;
        }

        template<class List, class Same>
        bool SameList(const List& a, const List& b, Same same)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end(), same);
        }

        std::string Number(const char* format, double value)
        {
            char text[48];
            std::snprintf(text, sizeof(text), format, value);
            return text;
        }
    } // namespace

    // ------------------------------------------------------------------ names

    const char* SyncName(MusicSync sync)
    {
        switch (sync)
        {
            case MusicSync::Immediate:
                return "Immediate";
            case MusicSync::NextBeat:
                return "Next beat";
            case MusicSync::NextBar:
                return "Next bar";
            case MusicSync::NextNBar:
                return "Next N bars";
            case MusicSync::NextCue:
                return "Next cue";
            case MusicSync::ExitCue:
                return "Exit cue";
            case MusicSync::EndOfSegment:
                return "End of segment";
        }
        return "?";
    }

    const char* EntryName(MusicEntry entry)
    {
        switch (entry)
        {
            case MusicEntry::EntryCue:
                return "Entry cue";
            case MusicEntry::SameRelativePosition:
                return "Same position";
            case MusicEntry::NamedCue:
                return "Named cue";
        }
        return "?";
    }

    const char* OrderName(MusicPlaylistOrder order)
    {
        switch (order)
        {
            case MusicPlaylistOrder::Ordered:
                return "Ordered";
            case MusicPlaylistOrder::Shuffled:
                return "Shuffled";
            case MusicPlaylistOrder::Weighted:
                return "Weighted";
        }
        return "?";
    }

    const char* FadeCurveName(AudioFadeCurve curve)
    {
        switch (curve)
        {
            case AudioFadeCurve::Linear:
                return "Linear";
            case AudioFadeCurve::EqualPower:
                return "Equal power";
            case AudioFadeCurve::SCurve:
                return "S-curve";
        }
        return "?";
    }

    std::string UniqueName(const std::vector<std::string>& taken, const std::string& base)
    {
        const auto used = [&](const std::string& name) {
            return std::find(taken.begin(), taken.end(), name) != taken.end();
        };
        if (!used(base))
            return base;
        for (int i = 2;; ++i)
            if (auto name = base + " " + std::to_string(i); !used(name))
                return name;
    }

    // ------------------------------------------------------------------ edits

    size_t AddSegment(MusicSetData& data)
    {
        MusicSegment segment;
        segment.Name = UniqueName(NamesOf(data.Segments), "Segment");
        data.Segments.push_back(std::move(segment));
        return data.Segments.size() - 1;
    }

    bool RemoveSegment(MusicSetData& data, size_t index) { return Erase(data.Segments, index); }

    bool RenameSegment(MusicSetData& data, size_t index, const std::string& name)
    {
        if (!Renameable(data.Segments, index, name))
            return false;
        const auto old = data.Segments[index].Name;
        if (old == name)
            return true;
        data.Segments[index].Name = name;
        for (auto& state : data.States)
        {
            if (state.Segment == old)
                state.Segment = name;
            for (auto& item : state.Playlist)
                if (item.Segment == old)
                    item.Segment = name;
        }
        for (auto& rule : data.Transitions)
            if (rule.Via == old)
                rule.Via = name;
        return true;
    }

    size_t AddLayer(MusicSegment& segment, AssetID clip)
    {
        MusicLayer layer;
        layer.Clip = clip;
        segment.Layers.push_back(std::move(layer));
        return segment.Layers.size() - 1;
    }

    bool RemoveLayer(MusicSegment& segment, size_t index) { return Erase(segment.Layers, index); }

    size_t AddCue(MusicSegment& segment, const MusicTime& time)
    {
        segment.Cues.push_back({UniqueName(NamesOf(segment.Cues), "Cue"), time});
        return segment.Cues.size() - 1;
    }

    bool RemoveCue(MusicSegment& segment, size_t index) { return Erase(segment.Cues, index); }

    size_t AddParameter(MusicSetData& data)
    {
        MusicParameter parameter;
        parameter.Name = UniqueName(NamesOf(data.Parameters), "Parameter");
        data.Parameters.push_back(std::move(parameter));
        return data.Parameters.size() - 1;
    }

    bool RemoveParameter(MusicSetData& data, size_t index)
    {
        if (index >= data.Parameters.size())
            return false;
        const auto name = data.Parameters[index].Name;
        data.Parameters.erase(data.Parameters.begin() + std::ptrdiff_t(index));
        for (auto& segment : data.Segments)
            for (auto& layer : segment.Layers)
                if (layer.Parameter == name)
                {
                    layer.Parameter.clear();
                    layer.Curve.clear();
                }
        return true;
    }

    bool RenameParameter(MusicSetData& data, size_t index, const std::string& name)
    {
        if (!Renameable(data.Parameters, index, name))
            return false;
        const auto old              = data.Parameters[index].Name;
        data.Parameters[index].Name = name;
        for (auto& segment : data.Segments)
            for (auto& layer : segment.Layers)
                if (layer.Parameter == old)
                    layer.Parameter = name;
        return true;
    }

    void SetLayerParameter(const MusicSetData& data, MusicLayer& layer, const std::string& parameter)
    {
        if (parameter == layer.Parameter)
            return;
        layer.Parameter = parameter;
        if (parameter.empty())
        {
            layer.Curve.clear();
            return;
        }
        if (!layer.Curve.empty())
            return;
        float lower = 0, upper = 1;
        for (const auto& candidate : data.Parameters)
            if (candidate.Name == parameter)
                lower = candidate.Min, upper = candidate.Max;
        if (!(upper > lower))
            upper = lower + 1;
        layer.Curve = {{lower, 0}, {upper, 0}};
    }

    size_t AddState(MusicSetData& data)
    {
        std::vector<std::string> taken = NamesOf(data.States);
        taken.push_back(AnyState);
        MusicState state;
        state.Name = UniqueName(taken, "State");
        if (!data.Segments.empty())
            state.Segment = data.Segments.front().Name;
        data.States.push_back(std::move(state));
        return data.States.size() - 1;
    }

    bool RemoveState(MusicSetData& data, size_t index)
    {
        if (index >= data.States.size())
            return false;
        const auto name = data.States[index].Name;
        data.States.erase(data.States.begin() + std::ptrdiff_t(index));
        data.Transitions.erase(
            std::remove_if(data.Transitions.begin(),
                           data.Transitions.end(),
                           [&](const MusicTransition& rule) { return rule.From == name || rule.To == name; }),
            data.Transitions.end());
        return true;
    }

    bool RenameState(MusicSetData& data, size_t index, const std::string& name)
    {
        if (name == AnyState || !Renameable(data.States, index, name))
            return false;
        const auto old          = data.States[index].Name;
        data.States[index].Name = name;
        for (auto& rule : data.Transitions)
        {
            if (rule.From == old)
                rule.From = name;
            if (rule.To == old)
                rule.To = name;
        }
        return true;
    }

    std::optional<size_t> AddTransition(MusicSetData& data, const std::string& from, const std::string& to)
    {
        if (RuleAt(data, from, to))
            return std::nullopt;
        MusicTransition rule;
        rule.From = from;
        rule.To   = to;
        data.Transitions.push_back(std::move(rule));
        return data.Transitions.size() - 1;
    }

    bool RemoveTransition(MusicSetData& data, size_t index) { return Erase(data.Transitions, index); }

    size_t AddStinger(MusicSetData& data, AssetID clip)
    {
        MusicStinger stinger;
        stinger.Name = UniqueName(NamesOf(data.Stingers), "Stinger");
        stinger.Clip = clip;
        data.Stingers.push_back(std::move(stinger));
        return data.Stingers.size() - 1;
    }

    bool RemoveStinger(MusicSetData& data, size_t index) { return Erase(data.Stingers, index); }

    // ------------------------------------------------------------------ tempo grid

    double BeatSeconds(const MusicSegment& segment)
    {
        return std::isfinite(segment.BPM) && segment.BPM > 0 ? 60.0 / segment.BPM : 0.0;
    }

    double BarSeconds(const MusicSegment& segment) { return BeatSeconds(segment) * segment.BeatsPerBar; }

    MusicTime SnapTime(const MusicSegment& segment, double seconds, Snap snap)
    {
        const double beat = BeatSeconds(segment);
        if (snap == Snap::Off || beat <= 0 || segment.BeatsPerBar == 0 || !std::isfinite(segment.FirstBeatSeconds))
            return {std::max(0.0, seconds), std::nullopt, 0};
        const double period = snap == Snap::Bar ? beat * segment.BeatsPerBar : beat;
        const double steps  = std::max(0.0, std::round((seconds - segment.FirstBeatSeconds) / period));
        uint64_t     beats  = uint64_t(steps) * (snap == Snap::Bar ? segment.BeatsPerBar : 1u);
        MusicTime    time;
        time.Bar     = uint32_t(std::min<uint64_t>(beats / segment.BeatsPerBar, std::numeric_limits<uint32_t>::max()));
        time.Beat    = double(beats % segment.BeatsPerBar);
        time.Seconds = segment.SecondsAt(time);
        return time;
    }

    std::string TimeLabel(const MusicSegment& segment, const MusicTime& time)
    {
        (void)segment;
        if (!time.Bar)
            return Number("%.3f s", time.Seconds);
        char text[64];
        std::snprintf(text, sizeof(text), "Bar %u, beat %g", unsigned(*time.Bar + 1), time.Beat + 1);
        return text;
    }

    std::string BarBeatLabel(uint64_t bar, double beatInBar)
    {
        char text[48];
        std::snprintf(text,
                      sizeof(text),
                      "%llu.%d",
                      static_cast<unsigned long long>(bar + 1),
                      int(std::floor(std::max(0.0, beatInBar))) + 1);
        return text;
    }

    std::vector<RulerTick>
    RulerTicks(const MusicSegment& segment, double length, double pixelsPerSecond, double minSpacing)
    {
        std::vector<RulerTick> ticks;
        const double           beat = BeatSeconds(segment);
        if (beat <= 0 || segment.BeatsPerBar == 0 || !std::isfinite(length) || length <= 0 ||
            !std::isfinite(pixelsPerSecond) || pixelsPerSecond <= 0 || !std::isfinite(segment.FirstBeatSeconds))
            return ticks;
        const bool beats = beat * pixelsPerSecond >= minSpacing;
        uint64_t   step  = 1; // Bars between ticks when beats are hidden.
        if (!beats)
            while (beat * segment.BeatsPerBar * double(step) * pixelsPerSecond < minSpacing && step < (1u << 20))
                step *= 2;
        const uint64_t   stride   = beats ? 1 : step * segment.BeatsPerBar;
        constexpr size_t MaxTicks = 4096;
        for (uint64_t index = 0; ticks.size() < MaxTicks; index += stride)
        {
            const double seconds = segment.FirstBeatSeconds + double(index) * beat;
            if (seconds > length + 1e-9)
                break;
            if (seconds < 0)
                continue;
            RulerTick tick;
            tick.Seconds = seconds;
            tick.Bar     = uint32_t(index / segment.BeatsPerBar);
            tick.Beat    = uint32_t(index % segment.BeatsPerBar);
            tick.IsBar   = tick.Beat == 0;
            ticks.push_back(tick);
        }
        return ticks;
    }

    // ------------------------------------------------------------------ cues

    double CueSeconds(const MusicSegment& segment, CueRef cue, double length)
    {
        switch (cue.Kind)
        {
            case CueKind::Entry:
                return segment.SecondsAt(segment.Entry);
            case CueKind::Exit:
                return segment.Exit ? segment.SecondsAt(*segment.Exit) : length;
            case CueKind::Custom:
                return cue.Index < segment.Cues.size() ? segment.SecondsAt(segment.Cues[cue.Index].Time) : 0.0;
        }
        return 0.0;
    }

    bool MoveCue(MusicSegment& segment, CueRef cue, double seconds, Snap snap, double length)
    {
        if (cue.Kind == CueKind::Custom && cue.Index >= segment.Cues.size())
            return false;
        if (!std::isfinite(seconds))
            return false;
        const double upper = std::isfinite(length) && length > 0 ? length : std::max(0.0, seconds);
        auto         time  = SnapTime(segment, std::clamp(seconds, 0.0, upper), snap);
        // A snapped line past the end steps back one grid line.
        if (time.Bar && segment.SecondsAt(time) > upper + 1e-9)
        {
            const double back = snap == Snap::Bar ? BarSeconds(segment) : BeatSeconds(segment);
            time              = SnapTime(segment, segment.SecondsAt(time) - back, snap);
            if (segment.SecondsAt(time) > upper + 1e-9)
                time = {upper, std::nullopt, 0};
        }
        switch (cue.Kind)
        {
            case CueKind::Entry:
                segment.Entry = time;
                break;
            case CueKind::Exit:
                segment.Exit = time;
                break;
            case CueKind::Custom:
                segment.Cues[cue.Index].Time = time;
                break;
        }
        return true;
    }

    std::optional<double> SegmentLength(const MusicSegment& segment, const MusicClipDuration& duration)
    {
        if (!duration || segment.Layers.empty())
            return std::nullopt;
        double length = std::numeric_limits<double>::infinity();
        for (const auto& layer : segment.Layers)
        {
            const auto seconds = layer.Clip.IsValid() ? duration(layer.Clip) : std::nullopt;
            if (!seconds || !Time(*seconds) || *seconds == 0)
                return std::nullopt;
            length = std::min(length, *seconds);
        }
        return length;
    }

    // ------------------------------------------------------------------ curves

    float CurveDb(const MusicLayer& layer, float input)
    {
        const auto& curve = layer.Curve;
        if (curve.empty())
            return 0.0f;
        const auto after =
            std::upper_bound(curve.begin(), curve.end(), input, [](float value, const MusicVolumeKey& key) {
                return value < key.Input;
            });
        if (after == curve.begin())
            return after->VolumeDb;
        if (after == curve.end())
            return curve.back().VolumeDb;
        const auto& before   = after[-1];
        float       fraction = (input - before.Input) / (after->Input - before.Input);
        if (before.Interpolation == AudioCurveInterpolation::Smooth)
            fraction *= fraction * (3 - 2 * fraction);
        return before.VolumeDb + (after->VolumeDb - before.VolumeDb) * fraction;
    }

    std::optional<size_t> InsertKey(std::vector<MusicVolumeKey>& keys, MusicVolumeKey key, float gap)
    {
        if (!std::isfinite(key.Input) || !std::isfinite(key.VolumeDb))
            return std::nullopt;
        for (const auto& existing : keys)
            if (std::abs(existing.Input - key.Input) < gap)
                return std::nullopt;
        const auto at = std::upper_bound(
            keys.begin(), keys.end(), key.Input, [](float value, const MusicVolumeKey& k) { return value < k.Input; });
        const auto index = size_t(at - keys.begin());
        keys.insert(at, key);
        return index;
    }

    bool MoveKey(std::vector<MusicVolumeKey>& keys, size_t index, float input, float db, float gap)
    {
        if (index >= keys.size() || !std::isfinite(input) || !std::isfinite(db))
            return false;
        float lower = -std::numeric_limits<float>::max(), upper = std::numeric_limits<float>::max();
        if (index > 0)
            lower = keys[index - 1].Input + gap;
        if (index + 1 < keys.size())
            upper = keys[index + 1].Input - gap;
        keys[index].Input    = upper < lower ? keys[index].Input : std::clamp(input, lower, upper);
        keys[index].VolumeDb = db;
        return true;
    }

    bool RemoveKey(std::vector<MusicVolumeKey>& keys, size_t index) { return Erase(keys, index); }

    // ------------------------------------------------------------------ matrix

    std::vector<std::string> MatrixStates(const MusicSetData& data)
    {
        std::vector<std::string> states {AnyState};
        for (const auto& state : data.States)
            states.push_back(state.Name);
        return states;
    }

    std::optional<size_t> RuleAt(const MusicSetData& data, const std::string& from, const std::string& to)
    {
        for (size_t i = 0; i < data.Transitions.size(); ++i)
            if (data.Transitions[i].From == from && data.Transitions[i].To == to)
                return i;
        return std::nullopt;
    }

    std::optional<size_t> WinningRule(const MusicSetData& data, const std::string& from, const std::string& to)
    {
        const auto* rule = data.FindTransition(from, to);
        if (!rule)
            return std::nullopt;
        return size_t(rule - data.Transitions.data());
    }

    std::string RuleSummary(const MusicTransition& rule)
    {
        std::string text = SyncName(rule.Sync.Sync);
        if (rule.Sync.Sync == MusicSync::NextNBar)
            text = rule.Sync.Bars == 1 ? std::string("Next 1 bar") : "Next " + std::to_string(rule.Sync.Bars) + " bars";
        else if (rule.Sync.Sync == MusicSync::NextCue && !rule.Sync.Cue.empty())
            text += " " + rule.Sync.Cue;
        if (rule.FadeOut.Seconds > 0)
            text += Number(" \xC2\xB7 out %.2f s", rule.FadeOut.Seconds);
        if (rule.FadeIn.Seconds > 0)
            text += Number(" \xC2\xB7 in %.2f s", rule.FadeIn.Seconds);
        if (!rule.Via.empty())
            text += " \xC2\xB7 via " + rule.Via;
        return text;
    }

    // ------------------------------------------------------------------ validation

    std::vector<Issue> CollectIssues(const MusicSetData& data, const MusicClipDuration& duration)
    {
        std::vector<Issue> issues;
        const auto         add = [&](Where kind, size_t index, size_t item, std::string message) {
            issues.push_back({kind, index, item, std::move(message)});
        };
        const bool empty = data.Segments.empty() && data.States.empty() && data.Parameters.empty() &&
                           data.Transitions.empty() && data.Stingers.empty();
        if (!empty && (data.Segments.empty() || data.States.empty()))
            add(Where::Set, 0, 0, "Music set needs segments and states");

        // Names: empty, duplicated, oversized, or the reserved Any.
        const auto names = [&](const auto& list, Where kind, bool state) {
            std::set<std::string> seen;
            for (size_t i = 0; i < list.size(); ++i)
            {
                const auto& name = list[i].Name;
                if (name.empty())
                    add(kind, i, 0, "Empty name");
                else if (name.size() >= 256)
                    add(kind, i, 0, "Name longer than 255 bytes");
                else if (state && name == AnyState)
                    add(kind, i, 0, "\"Any\" is reserved for transition rules");
                else if (!seen.insert(name).second)
                    add(kind, i, 0, "Duplicate name " + name);
            }
        };
        names(data.Segments, Where::Segment, false);
        names(data.Parameters, Where::Parameter, false);
        names(data.States, Where::State, true);
        names(data.Stingers, Where::Stinger, false);

        for (size_t i = 0; i < data.Parameters.size(); ++i)
        {
            const auto& parameter = data.Parameters[i];
            if (!std::isfinite(parameter.Min) || !std::isfinite(parameter.Max) || !std::isfinite(parameter.Default) ||
                parameter.Min >= parameter.Max || parameter.Default < parameter.Min ||
                parameter.Default > parameter.Max || !Time(parameter.RampSeconds) || parameter.RampSeconds < .001f ||
                parameter.RampSeconds > 1)
                add(Where::Parameter, i, 0, "Invalid parameter range, default or ramp for " + parameter.Name);
        }

        for (size_t s = 0; s < data.Segments.size(); ++s)
        {
            const auto& segment = data.Segments[s];
            if (!Time(segment.BPM) || segment.BPM < 1 || segment.BPM > 1000 || segment.BeatsPerBar == 0 ||
                segment.BeatsPerBar > 64 || !std::isfinite(segment.FirstBeatSeconds))
                add(Where::Segment, s, 0, "Invalid BPM, meter or first beat for " + segment.Name);
            if (segment.Layers.empty())
                add(Where::Segment, s, 0, "Missing layers for " + segment.Name);
            double length = std::numeric_limits<double>::infinity();
            bool   known  = true; // Every stem's length is known (else cue bounds are not checked against it).
            for (size_t l = 0; l < segment.Layers.size(); ++l)
            {
                const auto& layer   = segment.Layers[l];
                const auto  seconds = duration && layer.Clip.IsValid() ? duration(layer.Clip) : std::nullopt;
                if (!layer.Clip.IsValid() || (duration && (!seconds || !Time(*seconds) || *seconds == 0)))
                {
                    add(Where::Layer,
                        s,
                        l,
                        "Missing clip for layer in " + segment.Name + " (" + layer.Clip.ToString() + ")");
                    known = false;
                }
                else if (seconds)
                    length = std::min(length, *seconds);
                if (layer.Bus.empty() || !IsValidAudioBusLevel(layer.VolumeDb) ||
                    (layer.Parameter.empty() != layer.Curve.empty()) ||
                    (!layer.Parameter.empty() && !HasName(data.Parameters, layer.Parameter)) || layer.Curve.size() > 32)
                    add(Where::Layer, s, l, "Invalid bus, gain or parameter curve in " + segment.Name);
                float previous = -std::numeric_limits<float>::infinity();
                for (const auto& key : layer.Curve)
                {
                    if (!std::isfinite(key.Input) || key.Input <= previous || !std::isfinite(key.VolumeDb) ||
                        !IsValidAudioBusLevel(key.VolumeDb))
                    {
                        add(Where::Layer, s, l, "Invalid or unordered volume key in " + segment.Name);
                        break;
                    }
                    previous = key.Input;
                }
            }
            if (!known)
                length = std::numeric_limits<double>::infinity();
            const auto entry = segment.SecondsAt(segment.Entry);
            const auto exit  = segment.Exit ? segment.SecondsAt(*segment.Exit) : length;
            if (!Time(entry) || (segment.Exit && !Time(exit)) || entry >= exit || exit > length)
                add(Where::Segment, s, 0, "Entry/exit cue out of order or past the end of " + segment.Name);
            std::set<std::string> cues;
            for (size_t c = 0; c < segment.Cues.size(); ++c)
            {
                const auto& cue = segment.Cues[c];
                if (cue.Name.empty() || cue.Name.size() >= 256 || !cues.insert(cue.Name).second)
                    add(Where::Cue, s, c, "Invalid custom cue name in " + segment.Name);
                const auto seconds = segment.SecondsAt(cue.Time);
                if (!Time(seconds) || seconds > length)
                    add(Where::Cue, s, c, "Cue " + cue.Name + " past the end of " + segment.Name);
            }
        }

        for (size_t i = 0; i < data.States.size(); ++i)
        {
            const auto& state = data.States[i];
            if (state.Segment.empty() == state.Playlist.empty() ||
                (!state.Segment.empty() && !HasName(data.Segments, state.Segment)))
                add(Where::State, i, 0, "Invalid segment or playlist for state " + state.Name);
            for (size_t p = 0; p < state.Playlist.size(); ++p)
            {
                const auto& item = state.Playlist[p];
                if (!HasName(data.Segments, item.Segment) || !Time(item.Weight) || item.Weight == 0)
                    add(Where::PlaylistItem, i, p, "Invalid playlist item in state " + state.Name);
            }
        }

        std::set<std::pair<std::string, std::string>> pairs;
        for (size_t i = 0; i < data.Transitions.size(); ++i)
        {
            const auto& rule = data.Transitions[i];
            if ((rule.From != AnyState && !HasName(data.States, rule.From)) ||
                (rule.To != AnyState && !HasName(data.States, rule.To)))
                add(Where::Transition,
                    i,
                    0,
                    "Transition rule references unknown state " + rule.From + " -> " + rule.To);
            if (!pairs.emplace(rule.From, rule.To).second || rule.Sync.Bars == 0 || !Time(rule.FadeOut.Seconds) ||
                !Time(rule.FadeIn.Seconds) || (rule.Entry == MusicEntry::NamedCue && rule.Cue.empty()) ||
                (!rule.Via.empty() && !HasName(data.Segments, rule.Via)))
                add(Where::Transition, i, 0, "Invalid or duplicate transition " + rule.From + " -> " + rule.To);
        }

        for (size_t i = 0; i < data.Stingers.size(); ++i)
        {
            const auto& stinger = data.Stingers[i];
            if (!stinger.Clip.IsValid() || (duration && !duration(stinger.Clip)) || stinger.Bus.empty() ||
                stinger.Sync.Bars == 0 || !Time(stinger.MinimumInterval))
                add(Where::Stinger, i, 0, "Invalid stinger or missing clip for " + stinger.Name);
        }
        return issues;
    }

    const Issue* FindIssue(const std::vector<Issue>& issues, Where kind, size_t index, size_t item)
    {
        const bool items = kind == Where::Layer || kind == Where::Cue || kind == Where::PlaylistItem;
        for (const auto& issue : issues)
            if (issue.Kind == kind && issue.Index == index && (!items || issue.Item == item))
                return &issue;
        return nullptr;
    }

    // ------------------------------------------------------------------ document

    bool Equal(const MusicSetData& a, const MusicSetData& b)
    {
        return SameList(a.Segments, b.Segments, SameSegment) && SameList(a.Parameters, b.Parameters, SameParameter) &&
               SameList(a.States, b.States, SameState) && SameList(a.Transitions, b.Transitions, SameTransition) &&
               SameList(a.Stingers, b.Stingers, SameStinger);
    }

    void MusicSetDocument::Reset(MusicSetData data, bool saved)
    {
        m_Undo.clear();
        m_Redo.clear();
        m_GestureOpen = false;
        m_Saved.reset();
        Set(std::move(data));
        if (saved)
            m_Saved = m_Data;
    }

    bool MusicSetDocument::Commit(MusicSetData next, bool continuous)
    {
        if (Equal(next, m_Data))
            return false;
        // A gesture already in progress has its starting state on the undo stack.
        if (!(continuous && m_GestureOpen))
        {
            m_Undo.push_back(m_Data);
            if (m_Undo.size() > MaxHistory)
                m_Undo.erase(m_Undo.begin());
        }
        m_GestureOpen = continuous;
        m_Redo.clear();
        Set(std::move(next));
        return true;
    }

    bool MusicSetDocument::Undo()
    {
        if (m_Undo.empty())
            return false;
        m_GestureOpen = false;
        m_Redo.push_back(m_Data);
        auto previous = std::move(m_Undo.back());
        m_Undo.pop_back();
        Set(std::move(previous));
        return true;
    }

    bool MusicSetDocument::Redo()
    {
        if (m_Redo.empty())
            return false;
        m_GestureOpen = false;
        m_Undo.push_back(m_Data);
        auto next = std::move(m_Redo.back());
        m_Redo.pop_back();
        Set(std::move(next));
        return true;
    }

    void MusicSetDocument::Set(MusicSetData data)
    {
        m_Data = std::move(data);
        ++m_Revision;
    }

    // ------------------------------------------------------------------ files

    std::filesystem::path UniqueMusicSetFile(const std::filesystem::path& directory, const std::string& stem)
    {
        const std::string base = stem.empty() ? std::string("NewMusicSet") : stem;
        auto              file = directory / std::filesystem::u8path(base + ".musicset");
        std::error_code   error;
        for (int i = 2; std::filesystem::exists(file, error); ++i)
            file = directory / std::filesystem::u8path(base + " " + std::to_string(i) + ".musicset");
        return file;
    }

    std::optional<AssetError> SaveMusicSetFile(const std::filesystem::path& file, const MusicSetData& data)
    {
        auto text = SerializeMusicSet(data);
        if (const auto* error = std::get_if<AssetError>(&text))
            return *error;
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (!(stream << std::get<std::string>(text)) || !stream.flush())
            return AssetError {AssetErrorCode::IoError, "Cannot write " + file.u8string()};
        return std::nullopt;
    }

    std::variant<MusicSetData, AssetError> LoadMusicSetFile(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            return AssetError {AssetErrorCode::IoError, "Cannot read " + file.filename().u8string()};
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return ParseMusicSet(text);
    }

    MusicSetData FromClips(const std::vector<AssetID>& clips, const std::string& name)
    {
        MusicSetData data;
        if (clips.empty())
            return data;
        MusicSegment segment;
        segment.Name = name.empty() ? std::string("Main") : name;
        segment.Loop = true;
        for (const auto& clip : clips)
            AddLayer(segment, clip);
        data.States.push_back({segment.Name, segment.Name});
        data.Segments.push_back(std::move(segment));
        return data;
    }

    std::variant<std::filesystem::path, AssetError>
    CreateMusicSetFile(const std::filesystem::path& directory, const std::string& stem, const MusicSetData& data)
    {
        const auto file = UniqueMusicSetFile(directory, stem);
        if (auto error = SaveMusicSetFile(file, data))
            return *error;
        return file;
    }
} // namespace Kans::MusicSetEditing
