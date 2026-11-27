#include "AudioProfilerModel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Kans3D/Audio/AudioEngine.h"

namespace Kans::AudioProfiler
{
    double WindowSeconds(TimeWindow window) { return window == TimeWindow::OneSecond ? 1.0 : 10.0; }

    const AudioProfileWindow& Pick(const AudioProfileStats& stats, TimeWindow window)
    {
        return window == TimeWindow::OneSecond ? stats.LastSecond : stats.LastTenSeconds;
    }

    const AudioProfileWindow& Pick(const AudioProfileNode& node, TimeWindow window)
    {
        return window == TimeWindow::OneSecond ? node.LastSecond : node.LastTenSeconds;
    }

    // ------------------------------------------------------------------ record

    void Recorder::Start(AudioEngine& engine)
    {
        if (m_Recording)
            return;
        m_WasEnabled = engine.IsProfilingEnabled();
        m_Recording  = true;
        engine.SetProfilingEnabled(true);
    }

    void Recorder::Stop(AudioEngine& engine)
    {
        if (!m_Recording)
            return;
        m_Recording = false;
        engine.SetProfilingEnabled(m_WasEnabled);
    }

    void Recorder::Rebind(AudioEngine& engine)
    {
        if (m_Recording)
            engine.SetProfilingEnabled(true);
    }

    // ------------------------------------------------------------------ load history

    void LoadHistory::Add(double time, float load)
    {
        if (m_Count == Capacity)
        {
            m_Samples[m_Start] = {time, load};
            m_Start            = (m_Start + 1) % Capacity;
        }
        else
            m_Samples[(m_Start + m_Count++) % Capacity] = {time, load};
    }

    size_t LoadHistory::FirstAtOrAfter(double time) const
    {
        // Samples are in time order: binary search over the ring.
        size_t lo = 0, hi = m_Count;
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2;
            if (At(mid).Time < time)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    // ------------------------------------------------------------------ buses

    namespace
    {
        // Sort key of a row: names ascend, times descend unless asked otherwise.
        bool Before(const BusRow&                          a,
                    const BusRow&                          b,
                    const std::vector<AudioBusDefinition>& buses,
                    BusSort                                sort,
                    bool                                   ascending)
        {
            const auto byName = [&] {
                const auto& an = buses[a.Bus].Name;
                const auto& bn = buses[b.Bus].Name;
                if (a.IsEffect() && b.IsEffect())
                    return a.Slot < b.Slot;
                const int c = an.compare(bn);
                return ascending ? c < 0 : c > 0;
            };
            const auto byTime = [&](double ta, double tb) {
                if (ta == tb)
                    return a.IsEffect() ? a.Slot < b.Slot : a.Bus < b.Bus;
                return ascending ? ta < tb : ta > tb;
            };
            switch (sort)
            {
                case BusSort::Tree:
                    return a.IsEffect() ? a.Slot < b.Slot : a.Bus < b.Bus;
                case BusSort::Name:
                    return byName();
                case BusSort::Current:
                    return byTime(double(a.CurrentNanoseconds), double(b.CurrentNanoseconds));
                case BusSort::Average:
                    return byTime(a.Window.Average, b.Window.Average);
                case BusSort::Peak:
                    return byTime(a.Window.Peak, b.Window.Peak);
            }
            return false;
        }

        BusRow
        MakeRow(size_t bus, size_t slot, int depth, const std::vector<AudioProfileNode>& nodes, TimeWindow window)
        {
            BusRow row;
            row.Bus   = bus;
            row.Slot  = slot;
            row.Depth = depth;
            for (const auto& node : nodes)
                if (node.BusIndex == bus && (slot == SIZE_MAX ? !node.EffectSlot : node.EffectSlot == slot))
                {
                    row.CurrentNanoseconds = node.CurrentNanoseconds;
                    row.Window             = Pick(node, window);
                    row.Timed              = true;
                    break;
                }
            return row;
        }

        void AddSubtree(size_t                                 bus,
                        int                                    depth,
                        const std::vector<AudioBusDefinition>& buses,
                        const std::vector<AudioProfileNode>&   nodes,
                        TimeWindow                             window,
                        BusSort                                sort,
                        bool                                   ascending,
                        std::vector<BusRow>&                   out,
                        std::vector<size_t>&                   scratch)
        {
            if (depth > int(buses.size())) // A parent cycle (never in a validated layout).
                return;
            out.push_back(MakeRow(bus, SIZE_MAX, depth, nodes, window));

            // Effect slots, then the children: both sorted in place inside `out` / `scratch` ranges.
            const size_t effects = out.size();
            for (size_t slot = 0; slot < buses[bus].Effects.size(); ++slot)
                out.push_back(MakeRow(bus, slot, depth + 1, nodes, window));
            std::sort(out.begin() + effects, out.end(), [&](const BusRow& a, const BusRow& b) {
                return Before(a, b, buses, sort == BusSort::Name ? BusSort::Tree : sort, ascending);
            });

            const size_t begin = scratch.size();
            for (size_t child = 0; child < buses.size(); ++child)
                if (child != bus && buses[child].Parent == buses[bus].Name)
                    scratch.push_back(child);
            const size_t end = scratch.size();
            // Children compare by their own bus rows.
            std::sort(scratch.begin() + begin, scratch.begin() + end, [&](size_t a, size_t b) {
                return Before(MakeRow(a, SIZE_MAX, 0, nodes, window),
                              MakeRow(b, SIZE_MAX, 0, nodes, window),
                              buses,
                              sort,
                              ascending);
            });
            for (size_t i = begin; i < end; ++i)
                AddSubtree(scratch[i], depth + 1, buses, nodes, window, sort, ascending, out, scratch);
            scratch.resize(begin);
        }
    } // namespace

    void BuildBusRows(const std::vector<AudioBusDefinition>& buses,
                      const std::vector<AudioProfileNode>&   nodes,
                      TimeWindow                             window,
                      BusSort                                sort,
                      bool                                   ascending,
                      std::vector<BusRow>&                   out,
                      std::vector<size_t>&                   scratch)
    {
        out.clear();
        scratch.clear();
        // Roots: buses without a parent in the layout (Master in a valid layout).
        for (size_t bus = 0; bus < buses.size(); ++bus)
        {
            const auto& parent = buses[bus].Parent;
            const bool  root   = parent.empty() || std::none_of(buses.begin(), buses.end(), [&](const auto& other) {
                                  return other.Name == parent;
                                 });
            if (root)
                AddSubtree(bus, 0, buses, nodes, window, sort, ascending, out, scratch);
        }
    }

    // ------------------------------------------------------------------ voices

    StateClass ClassOf(AudioVoiceState state)
    {
        switch (state)
        {
            case AudioVoiceState::Virtual:
                return StateClass::Virtual;
            case AudioVoiceState::Stopping:
                return StateClass::Stopping;
            case AudioVoiceState::Stopped:
                return StateClass::Stopped;
            default:
                return StateClass::Real;
        }
    }

    namespace
    {
        void CopyLabel(std::string_view text, char* out, size_t size)
        {
            const size_t n = std::min(text.size(), size - 1);
            std::memcpy(out, text.data(), n);
            out[n] = '\0';
        }
    } // namespace

    void CopyVoices(const std::vector<AudioVoiceInfo>& in, std::vector<VoiceRow>& out)
    {
        out.resize(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            auto& row = out[i];
            row.Info  = in[i];
            CopyLabel(in[i].Bus, row.Bus, sizeof(row.Bus));
            CopyLabel(in[i].ConcurrencyGroup, row.Group, sizeof(row.Group));
            row.Info.Bus              = {};
            row.Info.ConcurrencyGroup = {};
            row.Owner[0]              = '\0';
            row.Asset[0]              = '\0';
        }
    }

    bool ContainsNoCase(std::string_view text, std::string_view part)
    {
        if (part.empty())
            return true;
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        for (size_t i = 0; i + part.size() <= text.size(); ++i)
        {
            size_t j = 0;
            while (j < part.size() && lower(text[i + j]) == lower(part[j]))
                ++j;
            if (j == part.size())
                return true;
        }
        return false;
    }

    namespace
    {
        int Compare(const VoiceRow& a, const VoiceRow& b, VoiceColumn column)
        {
            const auto  num = [](double x, double y) { return x < y ? -1 : x > y ? 1 : 0; };
            const auto& x   = a.Info;
            const auto& y   = b.Info;
            switch (column)
            {
                case VoiceColumn::Owner:
                    return std::strcmp(a.Owner, b.Owner);
                case VoiceColumn::Asset:
                    return std::strcmp(a.Asset, b.Asset);
                case VoiceColumn::Bus:
                    return std::strcmp(a.Bus, b.Bus);
                case VoiceColumn::State:
                    return num(double(x.State), double(y.State));
                case VoiceColumn::Priority:
                    return num(x.Priority, y.Priority);
                case VoiceColumn::Group:
                    return std::strcmp(a.Group, b.Group);
                case VoiceColumn::Audibility:
                    return num(x.Audibility, y.Audibility);
                case VoiceColumn::Gain:
                    return num(x.FinalGainDb, y.FinalGainDb);
                case VoiceColumn::Distance:
                    // Voices heard by no listener sort after every distance.
                    return num(x.Listener < 0 ? HUGE_VAL : x.Distance, y.Listener < 0 ? HUGE_VAL : y.Distance);
                case VoiceColumn::Listener:
                    return num(x.Listener, y.Listener);
                case VoiceColumn::Storage:
                    return num(double(x.Storage), double(y.Storage));
                case VoiceColumn::Reason:
                    return std::strcmp(ReasonLabel(x.Reason), ReasonLabel(y.Reason));
                case VoiceColumn::Spatializer:
                    // Panning first, then HRTF by blend.
                    return num(double(x.Spatializer) * 2 + x.SpatialBlend, double(y.Spatializer) * 2 + y.SpatialBlend);
                case VoiceColumn::Fallback:
                    return std::strcmp(FallbackLabel(x.HRTFFallback), FallbackLabel(y.HRTFFallback));
                case VoiceColumn::Occlusion:
                    return num(x.Acoustics.Occlusion, y.Acoustics.Occlusion);
                case VoiceColumn::Transmission:
                    // The mean band gain.
                    return num(double(x.Acoustics.Transmission[0]) + x.Acoustics.Transmission[1] +
                                   x.Acoustics.Transmission[2],
                               double(y.Acoustics.Transmission[0]) + y.Acoustics.Transmission[1] +
                                   y.Acoustics.Transmission[2]);
                case VoiceColumn::Count:
                    break;
            }
            return 0;
        }
    } // namespace

    void FilterAndSort(const std::vector<VoiceRow>& rows,
                       std::string_view             text,
                       StateMask                    states,
                       VoiceColumn                  column,
                       bool                         ascending,
                       std::vector<uint32_t>&       order)
    {
        order.clear();
        for (uint32_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            if (!(states & Bit(ClassOf(row.Info.State))))
                continue;
            if (!text.empty() && !ContainsNoCase(row.Owner, text) && !ContainsNoCase(row.Asset, text) &&
                !ContainsNoCase(row.Bus, text) && !ContainsNoCase(row.Group, text))
                continue;
            order.push_back(i);
        }
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const int c = Compare(rows[a], rows[b], column);
            if (c != 0)
                return ascending ? c < 0 : c > 0;
            return rows[a].Info.Voice.Index < rows[b].Info.Voice.Index;
        });
    }

    const char* StateLabel(AudioVoiceState state)
    {
        switch (state)
        {
            case AudioVoiceState::Stopped:
                return "Stopped";
            case AudioVoiceState::Scheduled:
                return "Scheduled";
            case AudioVoiceState::Playing:
                return "Playing";
            case AudioVoiceState::Paused:
                return "Paused";
            case AudioVoiceState::HeldByGamePause:
                return "Held (game pause)";
            case AudioVoiceState::Virtual:
                return "Virtual";
            case AudioVoiceState::Stopping:
                return "Stopping";
        }
        return "?";
    }

    const char* FallbackLabel(AudioHRTFFallback fallback)
    {
        switch (fallback)
        {
            case AudioHRTFFallback::None:
                return "";
            case AudioHRTFFallback::NonSpatial:
                return "2D sound";
            case AudioHRTFFallback::NonStereoOutput:
                return "Not stereo";
            case AudioHRTFFallback::SteamAudioUnavailable:
                return "No Steam Audio";
            case AudioHRTFFallback::EffectCreationFailed:
                return "Effect failed";
        }
        return "?";
    }

    void FormatSpatializer(const AudioVoiceInfo& voice, char* out, size_t size)
    {
        if (voice.Spatializer == AudioSpatializer::HRTF)
            std::snprintf(out, size, "HRTF %.2f", double(voice.SpatialBlend));
        else
            std::snprintf(out, size, "Panning");
    }

    const char* ReasonLabel(AudioVoiceReason reason)
    {
        switch (reason)
        {
            case AudioVoiceReason::None:
                return "";
            case AudioVoiceReason::Priority:
                return "Priority";
            case AudioVoiceReason::ConcurrencyLimit:
                return "Concurrency limit";
            case AudioVoiceReason::InstanceLimit:
                return "Instance limit";
            case AudioVoiceReason::Cooldown:
                return "Cooldown";
            case AudioVoiceReason::Inaudible:
                return "Inaudible";
            case AudioVoiceReason::PausedBus:
                return "Paused bus";
            case AudioVoiceReason::MissingClip:
                return "Missing clip";
            case AudioVoiceReason::InvalidSource:
                return "Invalid source";
        }
        return "?";
    }

    const char* ReasonDetail(AudioVoiceState state, AudioVoiceReason reason)
    {
        switch (reason)
        {
            case AudioVoiceReason::None:
                return "";
            case AudioVoiceReason::Priority:
                return state == AudioVoiceState::Virtual ?
                           "Virtualised: more voices wanted to play than the real-voice limit allows, and this one "
                           "ranked lowest (priority, then audibility)." :
                           "Stolen: a higher-priority voice took its place in the concurrency group.";
            case AudioVoiceReason::ConcurrencyLimit:
                return "Rejected: its concurrency group was full and the group's rule refuses new voices.";
            case AudioVoiceReason::InstanceLimit:
                return "Rejected: the event already plays as many instances as it allows.";
            case AudioVoiceReason::Cooldown:
                return "Rejected: the event was triggered again inside its cooldown.";
            case AudioVoiceReason::Inaudible:
                return "Virtualised: quieter than the virtualisation threshold, so it is tracked but not mixed.";
            case AudioVoiceReason::PausedBus:
                return "Held: its bus pauses with the game.";
            case AudioVoiceReason::MissingClip:
                return "Rejected: the clip is not loaded or does not exist.";
            case AudioVoiceReason::InvalidSource:
                return "Rejected: the source could not be played (no sound, format or generator).";
        }
        return "";
    }

    const char* OwnerKindLabel(AudioVoiceOwnerKind kind)
    {
        switch (kind)
        {
            case AudioVoiceOwnerKind::Direct:
                return "Direct";
            case AudioVoiceOwnerKind::Entity:
                return "Entity";
            case AudioVoiceOwnerKind::Event:
                return "Event";
            case AudioVoiceOwnerKind::OneShot:
                return "One-shot";
            case AudioVoiceOwnerKind::Music:
                return "Music";
            case AudioVoiceOwnerKind::Timeline:
                return "Timeline";
            case AudioVoiceOwnerKind::Generator:
                return "Generator";
        }
        return "?";
    }

    const char* StorageLabel(AudioVoiceStorage storage)
    {
        switch (storage)
        {
            case AudioVoiceStorage::Decoded:
                return "Decoded";
            case AudioVoiceStorage::Streaming:
                return "Streaming";
            case AudioVoiceStorage::Generator:
                return "Generator";
        }
        return "?";
    }

    const char* EventLabel(AudioProfileEventType type)
    {
        switch (type)
        {
            case AudioProfileEventType::Started:
                return "Started";
            case AudioProfileEventType::Stopped:
                return "Stopped";
            case AudioProfileEventType::Stolen:
                return "Stolen";
            case AudioProfileEventType::Rejected:
                return "Rejected";
            case AudioProfileEventType::Virtualised:
                return "Virtualised";
            case AudioProfileEventType::Revived:
                return "Revived";
            case AudioProfileEventType::Underrun:
                return "Underrun";
            case AudioProfileEventType::DeviceChanged:
                return "Device changed";
        }
        return "?";
    }

    // ------------------------------------------------------------------ event log

    void EventLog::Append(const LogEntry& entry)
    {
        if (m_Count == Capacity)
        {
            m_Entries[m_Start] = entry;
            m_Start            = (m_Start + 1) % Capacity;
        }
        else
            m_Entries[(m_Start + m_Count++) % Capacity] = entry;
    }

    void EventLog::Reset()
    {
        Clear();
        m_Cursor   = {};
        m_Instance = 0;
    }

    size_t EventLog::Poll(AudioEngine& engine)
    {
        const uint64_t instance = engine.GetInstanceID();
        if (instance == 0)
            return 0;
        size_t added = 0;
        if (instance != m_Instance)
        {
            // A rebuilt engine numbers its log from zero: the old cursor would skip or repeat entries.
            if (m_Instance != 0 && m_Count > 0)
            {
                LogEntry restart;
                restart.Kind = LogKind::Restart;
                Append(restart);
                ++added;
            }
            m_Cursor   = {};
            m_Instance = instance;
        }
        for (;;)
        {
            const uint64_t dropped = m_Cursor.Dropped;
            const size_t   count   = engine.ReadProfileEvents(m_Cursor, m_Read.data(), m_Read.size());
            if (m_Cursor.Dropped != dropped)
            {
                LogEntry gap;
                gap.Kind    = LogKind::Dropped;
                gap.Dropped = m_Cursor.Dropped - dropped;
                Append(gap);
                ++added;
            }
            for (size_t i = 0; i < count; ++i)
            {
                LogEntry entry;
                entry.Event = m_Read[i];
                Append(entry);
            }
            added += count;
            if (count < m_Read.size())
                break;
        }
        return added;
    }

    // ------------------------------------------------------------------ text

    void FormatBytes(uint64_t bytes, char* out, size_t size)
    {
        if (bytes < 1024)
            std::snprintf(out, size, "%llu B", static_cast<unsigned long long>(bytes));
        else if (bytes < 1024ull * 1024)
            std::snprintf(out, size, "%.1f KiB", double(bytes) / 1024.0);
        else if (bytes < 1024ull * 1024 * 1024)
            std::snprintf(out, size, "%.1f MiB", double(bytes) / (1024.0 * 1024.0));
        else
            std::snprintf(out, size, "%.2f GiB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    }

    void FormatDb(float db, char* out, size_t size)
    {
        if (!std::isfinite(db))
            std::snprintf(out, size, "-inf dB");
        else
            std::snprintf(out, size, "%.1f dB", double(db));
    }

    void FormatNanoseconds(double nanoseconds, char* out, size_t size)
    {
        if (nanoseconds < 1e6)
            std::snprintf(out, size, "%.0f us", nanoseconds / 1e3);
        else
            std::snprintf(out, size, "%.2f ms", nanoseconds / 1e6);
    }
} // namespace Kans::AudioProfiler
