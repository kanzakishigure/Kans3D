#include "kspch.h"
#include "MusicSet.h"

#include <cmath>
#include <set>
#include <yaml-cpp/yaml.h>

namespace Kans
{
    namespace
    {
        constexpr const char* SyncNames[] =
            {"Immediate", "NextBeat", "NextBar", "NextNBar", "NextCue", "ExitCue", "EndOfSegment"};
        constexpr const char* EntryNames[] = {"EntryCue", "SameRelativePosition", "NamedCue"};
        constexpr const char* OrderNames[] = {"Ordered", "Shuffled", "Weighted"};
        constexpr const char* FadeNames[]  = {"Linear", "EqualPower", "SCurve"};
        constexpr const char* CurveNames[] = {"Linear", "Smooth"};

        AssetError Invalid(std::string reason) { return {AssetErrorCode::InvalidData, std::move(reason)}; }
        bool       Time(double value) { return std::isfinite(value) && value >= 0; }

        template<class T, size_t N>
        bool InRange(T value, const char* const (&)[N])
        {
            return static_cast<size_t>(value) < N;
        }

        template<class List>
        bool HasName(const List& list, std::string_view name)
        {
            return std::any_of(list.begin(), list.end(), [&](const auto& item) { return item.Name == name; });
        }

        template<class List>
        bool UniqueNames(const List& list)
        {
            std::set<std::string_view> seen;
            return std::all_of(list.begin(), list.end(), [&](const auto& item) {
                return !item.Name.empty() && item.Name.size() < 256 && seen.insert(item.Name).second;
            });
        }

        bool ValidSync(const MusicSyncPoint& point) { return InRange(point.Sync, SyncNames) && point.Bars > 0; }

        // Keep YAML names in one place for both the reader and the writer.
        struct Reader
        {
            const YAML::Node Node;
            template<class T>
            void operator()(const char* key, T& value) const
            {
                if (const auto field = Node[key])
                    value = field.as<T>();
            }
            void operator()(const char* key, AssetID& value) const
            {
                if (const auto field = Node[key])
                    value = AssetID::FromString(field.as<std::string>());
            }
            template<class T, size_t N>
            void operator()(const char* key, T& value, const char* const (&names)[N]) const
            {
                if (const auto field = Node[key])
                {
                    const auto name = field.as<std::string>();
                    for (size_t index = 0; index < N; ++index)
                        if (name == names[index])
                        {
                            value = static_cast<T>(index);
                            return;
                        }
                    throw std::runtime_error(std::string("Unknown ") + key + " value " + name);
                }
            }
        };
        struct Writer
        {
            YAML::Node Node {YAML::NodeType::Map};
            template<class T>
            void operator()(const char* key, const T& value)
            {
                Node[key] = value;
            }
            void operator()(const char* key, AssetID value) { Node[key] = value.ToString(); }
            template<class T, size_t N>
            void operator()(const char* key, T value, const char* const (&names)[N])
            {
                Node[key] = names[static_cast<size_t>(value)];
            }
        };

        template<class T, class V>
        void Scalars(T& value, V& visit)
        {
            using Type = std::remove_cv_t<T>;
            if constexpr (std::is_same_v<Type, MusicCue> || std::is_same_v<Type, MusicParameter> ||
                          std::is_same_v<Type, MusicSegment> || std::is_same_v<Type, MusicState> ||
                          std::is_same_v<Type, MusicStinger>)
                visit("Name", value.Name);
            if constexpr (std::is_same_v<Type, MusicLayer> || std::is_same_v<Type, MusicStinger>)
                visit("Clip", value.Clip);
            if constexpr (std::is_same_v<Type, MusicLayer> || std::is_same_v<Type, MusicStinger>)
                visit("Bus", value.Bus);
            if constexpr (std::is_same_v<Type, MusicLayer> || std::is_same_v<Type, MusicVolumeKey>)
                visit("VolumeDb", value.VolumeDb);
            if constexpr (std::is_same_v<Type, MusicParameter>)
            {
                visit("Min", value.Min);
                visit("Max", value.Max);
                visit("Default", value.Default);
                visit("RampSeconds", value.RampSeconds);
            }
            if constexpr (std::is_same_v<Type, MusicLayer>)
                visit("Parameter", value.Parameter);
            if constexpr (std::is_same_v<Type, MusicVolumeKey>)
            {
                visit("Input", value.Input);
                visit("Interpolation", value.Interpolation, CurveNames);
            }
            if constexpr (std::is_same_v<Type, MusicSegment>)
            {
                visit("BPM", value.BPM);
                visit("BeatsPerBar", value.BeatsPerBar);
                visit("FirstBeatSeconds", value.FirstBeatSeconds);
                visit("Loop", value.Loop);
                visit("PreEntry", value.PreEntry);
                visit("PostExit", value.PostExit);
            }
            if constexpr (std::is_same_v<Type, MusicState> || std::is_same_v<Type, MusicPlaylistItem>)
                visit("Segment", value.Segment);
            if constexpr (std::is_same_v<Type, MusicState>)
            {
                visit("Order", value.Order, OrderNames);
                visit("NoImmediateRepeat", value.NoImmediateRepeat);
            }
            if constexpr (std::is_same_v<Type, MusicPlaylistItem>)
                visit("Weight", value.Weight);
            if constexpr (std::is_same_v<Type, MusicSyncPoint>)
            {
                visit("Point", value.Sync, SyncNames);
                visit("Bars", value.Bars);
                visit("Cue", value.Cue);
            }
            if constexpr (std::is_same_v<Type, MusicTransition>)
            {
                visit("From", value.From);
                visit("To", value.To);
                visit("Entry", value.Entry, EntryNames);
                visit("Cue", value.Cue);
                visit("Via", value.Via);
            }
            if constexpr (std::is_same_v<Type, MusicFade>)
            {
                visit("Seconds", value.Seconds);
                visit("Curve", value.Curve, FadeNames);
            }
            if constexpr (std::is_same_v<Type, MusicStinger>)
                visit("MinimumInterval", value.MinimumInterval);
            if constexpr (std::is_same_v<Type, MusicTime>)
            {
                if constexpr (std::is_same_v<V, Reader>)
                {
                    if (visit.Node["Bar"])
                    {
                        value.Bar = 0;
                        visit("Bar", *value.Bar);
                        visit("Beat", value.Beat);
                    }
                    else
                        visit("Seconds", value.Seconds);
                }
                else if (value.Bar)
                {
                    visit("Bar", *value.Bar);
                    visit("Beat", value.Beat);
                }
                else
                    visit("Seconds", value.Seconds);
            }
        }

        template<class T>
        T Decode(const YAML::Node& node, T value = {});
        template<class T>
        YAML::Node Encode(const T& value);

        template<class T>
        void Field(Reader& visit, const char* key, T& value)
        {
            if (const auto field = visit.Node[key])
                value = Decode(field, value);
        }
        template<class T>
        void Field(Writer& visit, const char* key, const T& value)
        {
            visit.Node[key] = Encode(value);
        }
        template<class T>
        void Field(Reader& visit, const char* key, std::optional<T>& value)
        {
            if (const auto field = visit.Node[key])
                value = Decode<T>(field);
        }
        template<class T>
        void Field(Writer& visit, const char* key, const std::optional<T>& value)
        {
            if (value)
                visit.Node[key] = Encode(*value);
        }
        template<class T>
        void Field(Reader& visit, const char* key, std::vector<T>& values)
        {
            if (const auto field = visit.Node[key])
            {
                if (!field.IsSequence())
                    throw std::runtime_error(std::string(key) + " must be a sequence");
                for (const auto& item : field)
                    values.push_back(Decode<T>(item));
            }
        }
        template<class T>
        void Field(Writer& visit, const char* key, const std::vector<T>& values)
        {
            YAML::Node sequence(YAML::NodeType::Sequence);
            for (const auto& item : values)
                sequence.push_back(Encode(item));
            visit.Node[key] = sequence;
        }

        template<class T, class V>
        void Fields(T& value, V& visit)
        {
            Scalars(value, visit);
            using Type = std::remove_cv_t<T>;
            if constexpr (std::is_same_v<Type, MusicSetData>)
            {
                Field(visit, "Segments", value.Segments);
                Field(visit, "Parameters", value.Parameters);
                Field(visit, "States", value.States);
                Field(visit, "Transitions", value.Transitions);
                Field(visit, "Stingers", value.Stingers);
            }
            if constexpr (std::is_same_v<Type, MusicSegment>)
            {
                Field(visit, "Layers", value.Layers);
                Field(visit, "EntryCue", value.Entry);
                Field(visit, "ExitCue", value.Exit);
                Field(visit, "Cues", value.Cues);
            }
            if constexpr (std::is_same_v<Type, MusicLayer>)
                Field(visit, "Curve", value.Curve);
            if constexpr (std::is_same_v<Type, MusicCue>)
                Field(visit, "Time", value.Time);
            if constexpr (std::is_same_v<Type, MusicState>)
                Field(visit, "Playlist", value.Playlist);
            if constexpr (std::is_same_v<Type, MusicTransition> || std::is_same_v<Type, MusicStinger>)
                Field(visit, "Sync", value.Sync);
            if constexpr (std::is_same_v<Type, MusicTransition>)
            {
                Field(visit, "FadeOut", value.FadeOut);
                Field(visit, "FadeIn", value.FadeIn);
            }
        }

        template<class T>
        T Decode(const YAML::Node& node, T value)
        {
            if (!node.IsMap())
                throw std::runtime_error("Music set record must be a map");
            Reader reader {node};
            Fields(value, reader);
            return value;
        }
        template<class T>
        YAML::Node Encode(const T& value)
        {
            Writer writer;
            Fields(value, writer);
            return writer.Node;
        }
    } // namespace

    double MusicSegment::SecondsAt(const MusicTime& time) const
    {
        return time.Bar ? FirstBeatSeconds + (double(*time.Bar) * BeatsPerBar + time.Beat) * 60 / BPM : time.Seconds;
    }

    const MusicTransition* MusicSetData::FindTransition(std::string_view from, std::string_view to) const
    {
        const MusicTransition* best        = nullptr;
        int                    specificity = -1;
        for (const auto& rule : Transitions)
            if ((rule.From == from || rule.From == "Any") && (rule.To == to || rule.To == "Any"))
            {
                const int score = 2 * (rule.From == from) + (rule.To == to);
                if (score > specificity)
                {
                    best        = &rule;
                    specificity = score;
                }
            }
        return best;
    }

    std::optional<AssetError> ValidateMusicSet(const MusicSetData& data, const MusicClipDuration& duration)
    {
        // A new, empty set is valid (nothing to play yet); once it has content it needs both.
        const bool empty = data.Segments.empty() && data.States.empty() && data.Parameters.empty() &&
                           data.Transitions.empty() && data.Stingers.empty();
        if (!empty && (data.Segments.empty() || data.States.empty()))
            return Invalid("Music set needs segments and states");
        if (!UniqueNames(data.Segments) || !UniqueNames(data.Parameters) || !UniqueNames(data.States) ||
            !UniqueNames(data.Stingers) || HasName(data.States, "Any"))
            return Invalid("Music set contains an empty, duplicate, reserved or oversized name");
        for (const auto& parameter : data.Parameters)
            if (!std::isfinite(parameter.Min) || !std::isfinite(parameter.Max) || !std::isfinite(parameter.Default) ||
                parameter.Min >= parameter.Max || parameter.Default < parameter.Min ||
                parameter.Default > parameter.Max || !Time(parameter.RampSeconds) || parameter.RampSeconds < .001f ||
                parameter.RampSeconds > 1)
                return Invalid("Invalid parameter range, default or ramp for " + parameter.Name);
        for (const auto& segment : data.Segments)
        {
            if (!Time(segment.BPM) || segment.BPM < 1 || segment.BPM > 1000 || segment.BeatsPerBar == 0 ||
                segment.BeatsPerBar > 64 || !std::isfinite(segment.FirstBeatSeconds))
                return Invalid("Invalid BPM, meter or first beat for " + segment.Name);
            if (segment.Layers.empty())
                return Invalid("Missing layers for " + segment.Name);
            double length = std::numeric_limits<double>::infinity();
            for (const auto& layer : segment.Layers)
            {
                const auto seconds = duration && layer.Clip.IsValid() ? duration(layer.Clip) : std::optional<double>();
                if (!layer.Clip.IsValid() || (duration && (!seconds || !Time(*seconds) || *seconds == 0)))
                    return Invalid("Missing clip for layer in " + segment.Name + " (" + layer.Clip.ToString() + ")");
                if (seconds)
                    length = std::min(length, *seconds);
                if (layer.Bus.empty() || !IsValidAudioBusLevel(layer.VolumeDb) ||
                    (layer.Parameter.empty() != layer.Curve.empty()) ||
                    (!layer.Parameter.empty() && !HasName(data.Parameters, layer.Parameter)) || layer.Curve.size() > 32)
                    return Invalid("Invalid bus, gain or parameter curve in " + segment.Name);
                float previous = -std::numeric_limits<float>::infinity();
                for (const auto& key : layer.Curve)
                {
                    if (!std::isfinite(key.Input) || key.Input <= previous || !std::isfinite(key.VolumeDb) ||
                        !IsValidAudioBusLevel(key.VolumeDb) || !InRange(key.Interpolation, CurveNames))
                        return Invalid("Invalid or unordered volume key in " + segment.Name);
                    previous = key.Input;
                }
            }
            const auto entry = segment.SecondsAt(segment.Entry);
            const auto exit  = segment.Exit ? segment.SecondsAt(*segment.Exit) : length;
            if (!Time(entry) || (segment.Exit && !Time(exit)) || entry >= exit || exit > length)
                return Invalid("Entry/exit cue out of order or past the end of " + segment.Name);
            if (!UniqueNames(segment.Cues))
                return Invalid("Invalid custom cue name in " + segment.Name);
            for (const auto& cue : segment.Cues)
            {
                const auto seconds = segment.SecondsAt(cue.Time);
                if (!Time(seconds) || seconds > length)
                    return Invalid("Cue " + cue.Name + " past the end of " + segment.Name);
            }
        }
        for (const auto& state : data.States)
        {
            if (state.Segment.empty() == state.Playlist.empty() || !InRange(state.Order, OrderNames) ||
                (!state.Segment.empty() && !HasName(data.Segments, state.Segment)))
                return Invalid("Invalid segment or playlist for state " + state.Name);
            for (const auto& item : state.Playlist)
                if (!HasName(data.Segments, item.Segment) || !Time(item.Weight) || item.Weight == 0)
                    return Invalid("Invalid playlist item in state " + state.Name);
        }
        std::set<std::pair<std::string_view, std::string_view>> pairs;
        for (const auto& rule : data.Transitions)
        {
            if ((rule.From != "Any" && !HasName(data.States, rule.From)) ||
                (rule.To != "Any" && !HasName(data.States, rule.To)))
                return Invalid("Transition rule references unknown state " + rule.From + " -> " + rule.To);
            if (!pairs.emplace(rule.From, rule.To).second || !ValidSync(rule.Sync) || !Time(rule.FadeOut.Seconds) ||
                !Time(rule.FadeIn.Seconds) || !InRange(rule.FadeOut.Curve, FadeNames) ||
                !InRange(rule.FadeIn.Curve, FadeNames) || !InRange(rule.Entry, EntryNames) ||
                (rule.Entry == MusicEntry::NamedCue && rule.Cue.empty()) ||
                (!rule.Via.empty() && !HasName(data.Segments, rule.Via)))
                return Invalid("Invalid or duplicate transition " + rule.From + " -> " + rule.To);
        }
        for (const auto& stinger : data.Stingers)
            if (!stinger.Clip.IsValid() || (duration && !duration(stinger.Clip)) || stinger.Bus.empty() ||
                !ValidSync(stinger.Sync) || !Time(stinger.MinimumInterval))
                return Invalid("Invalid stinger or missing clip for " + stinger.Name);
        return std::nullopt;
    }

    std::variant<MusicSetData, AssetError> ParseMusicSet(const std::string& text)
    {
        try
        {
            const auto root = YAML::Load(text);
            if (!root.IsMap() || (root["Version"] && root["Version"].as<uint32_t>() != 1))
                return Invalid("Unsupported music set version or invalid root");
            auto data = Decode<MusicSetData>(root);
            if (auto error = ValidateMusicSet(data))
                return *error;
            return data;
        }
        catch (const std::exception& error)
        {
            return Invalid(error.what());
        }
    }

    std::variant<std::string, AssetError> SerializeMusicSet(const MusicSetData& data)
    {
        if (auto error = ValidateMusicSet(data))
            return *error;
        auto root       = Encode(data);
        root["Version"] = 1;
        return YAML::Dump(root);
    }
} // namespace Kans
