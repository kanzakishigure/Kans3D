#include "kspch.h"
#include "MusicSetPlayer.h"

#include <cmath>
#include <numeric>

namespace Kans
{
    namespace
    {
        constexpr uint64_t NoFrame = UINT64_MAX;
        uint64_t           Frames(double value)
        {
            if (value <= 0)
                return 0;
            if (!std::isfinite(value) || value >= double(NoFrame))
                return NoFrame;
            return static_cast<uint64_t>(std::round(value));
        }
        uint64_t Add(uint64_t frame, uint64_t duration) { return frame + std::min(NoFrame - frame, duration); }

        template<class List>
        size_t Named(const List& list, std::string_view name)
        {
            const auto item =
                std::find_if(list.begin(), list.end(), [&](const auto& entry) { return entry.Name == name; });
            return item == list.end() ? SIZE_MAX : size_t(item - list.begin());
        }
    } // namespace

    MusicSetPlayer::MusicSetPlayer(AudioEngine& engine, uint32_t seed) : m_Engine(engine), m_Random(seed) {}
    MusicSetPlayer::~MusicSetPlayer() { Clear(); }

    void MusicSetPlayer::Clear()
    {
        for (const auto& segment : m_Segments)
        {
            for (const auto& bank : segment.Banks)
                for (const auto& layer : bank.Layers)
                    m_Engine.DestroyVoice(layer.Voice);
            for (const auto& layer : segment.Tail.Layers)
                m_Engine.DestroyVoice(layer.Voice);
        }
        for (const auto& stinger : m_Stingers)
            m_Engine.DestroyVoice(stinger.Voice);
        m_Segments.clear();
        m_Stingers.clear();
        m_Parameters.clear();
        m_Playlists.clear();
        m_Runs.clear();
        m_Warnings.clear();
        m_Asset.reset();
        m_PauseFrame.reset();
        m_UserPaused     = false;
        m_RequestedState = SIZE_MAX;
        ++m_Revision;
    }

    std::optional<AssetError> MusicSetPlayer::Load(Ref<const MusicSetAsset> asset,
                                                   const ClipResolver&      resolve,
                                                   std::string_view         busOverride,
                                                   uint64_t                 entity)
    {
        if (!asset || !resolve || !m_Engine.IsInitialized())
            return AssetError {AssetErrorCode::InvalidArgument,
                               "Music set requires an initialized engine, asset and clip resolver"};
        const auto&                     data = asset->GetData();
        std::map<AssetID, MusicSetClip> clips;
        const auto                      duration = [&](AssetID id) -> std::optional<double> {
            auto found = clips.find(id);
            if (found == clips.end())
                found = clips.emplace(id, resolve(id)).first;
            if (const auto info = m_Engine.GetSoundInfo(found->second.Sound))
                return double(info->LengthFrames) / info->SampleRate;
            return std::nullopt;
        };
        if (auto error = ValidateMusicSet(data, duration))
            return error;
        if (data.Segments.empty())
            return AssetError {AssetErrorCode::InvalidData, "Music set is empty: add a segment and a state"};
        const auto bus = [&](const MusicLayer& layer) {
            return busOverride.empty() ? layer.Bus : std::string(busOverride);
        };
        const auto transportBus = bus(data.Segments.front().Layers.front());
        for (const auto& segment : data.Segments)
        {
            const auto rate = m_Engine.GetSoundInfo(clips.at(segment.Layers.front().Clip).Sound)->SampleRate;
            for (const auto& layer : segment.Layers)
            {
                if (m_Engine.GetSoundInfo(clips.at(layer.Clip).Sound)->SampleRate != rate)
                    return AssetError {AssetErrorCode::InvalidData, "Stem sample rates differ in " + segment.Name};
                if (!m_Engine.MusicBusPoliciesMatch(transportBus, bus(layer)))
                    return AssetError {AssetErrorCode::InvalidData,
                                       "Music set stem buses must share pause and time-scale rules"};
            }
            if (segment.Loop && segment.Exit &&
                Frames(segment.SecondsAt(segment.Entry) * rate) >= Frames(segment.SecondsAt(*segment.Exit) * rate))
                return AssetError {AssetErrorCode::InvalidData,
                                   "Loop cues must be at least one sample apart in " + segment.Name};
        }
        Clear();
        m_Asset = std::move(asset);
        m_Segments.resize(data.Segments.size());
        m_Stingers.resize(data.Stingers.size());
        m_Playlists.resize(data.States.size());
        m_SecondsPerFrame = 1.0 / m_Engine.GetSampleRate();
        for (const auto& parameter : data.Parameters)
            m_Parameters.push_back(parameter.Default);
        size_t     voiceCount  = 0;
        const auto createVoice = [&](AssetID id, const std::string& bus, uint64_t begin, uint64_t end) {
            AudioVoiceDesc desc;
            desc.Clip            = id;
            desc.Owner           = {AudioVoiceOwnerKind::Music, entity, m_Asset->assetID};
            desc.Sound           = clips.at(id).Sound;
            desc.LoadMode        = AudioLoadMode::Stream;
            desc.Loop            = end > begin;
            desc.NeverVirtualize = true;
            desc.Bus             = bus;
            ++voiceCount;
            return m_Engine.CreateVoice(desc, begin, end);
        };
        const auto failed = [&] {
            Clear();
            return AssetError {AssetErrorCode::InvalidData, "Could not prepare music set voices"};
        };
        for (size_t index = 0; index < data.Segments.size(); ++index)
        {
            const auto& spec    = data.Segments[index];
            auto&       segment = m_Segments[index];
            segment.Bus         = busOverride.empty() ? spec.Layers.front().Bus : std::string(busOverride);
            segment.Length      = std::numeric_limits<double>::infinity();
            for (const auto& layer : spec.Layers)
                segment.Length = std::min(segment.Length, *duration(layer.Clip));
            const auto sourceRate = m_Engine.GetSoundInfo(clips.at(spec.Layers.front().Clip).Sound)->SampleRate;
            segment.Entry         = double(Frames(spec.SecondsAt(spec.Entry) * sourceRate)) / sourceRate;
            segment.Exit =
                spec.Exit ? double(Frames(spec.SecondsAt(*spec.Exit) * sourceRate)) / sourceRate : segment.Length;
            for (const auto& cue : spec.Cues)
                segment.Cues.push_back(spec.SecondsAt(cue.Time));
            const auto prepare = [&](Bank& bank, bool loop) {
                bank.Loop = loop;
                for (const auto& layer : spec.Layers)
                {
                    const auto clip  = clips.at(layer.Clip);
                    const auto rate  = m_Engine.GetSoundInfo(clip.Sound)->SampleRate;
                    const auto begin = loop ? Frames(segment.Entry * rate) : 0;
                    const auto end   = loop ? Frames(segment.Exit * rate) : 0;
                    const auto voice =
                        createVoice(layer.Clip, busOverride.empty() ? layer.Bus : std::string(busOverride), begin, end);
                    if (!voice.IsValid())
                        return false;
                    bank.Layers.push_back({voice, rate, Named(data.Parameters, layer.Parameter), clip.Volume});
                }
                return true;
            };
            if (!prepare(segment.Banks[0], spec.Loop) || !prepare(segment.Banks[1], spec.Loop) ||
                !prepare(segment.Banks[2], false) || !prepare(segment.Banks[3], false))
                return failed();
            if (spec.Loop && spec.PostExit && segment.Exit < segment.Length && !prepare(segment.Tail, false))
                return failed();
        }
        for (size_t index = 0; index < data.States.size(); ++index)
        {
            const auto& state    = data.States[index];
            auto&       playlist = m_Playlists[index];
            for (const auto& item : state.Playlist)
                playlist.Segments.push_back(Named(data.Segments, item.Segment));
            playlist.Bag.resize(playlist.Segments.size());
            playlist.Cursor = playlist.Bag.size();
        }
        for (size_t index = 0; index < data.Stingers.size(); ++index)
        {
            const auto& spec    = data.Stingers[index];
            auto&       stinger = m_Stingers[index];
            stinger.Bus         = busOverride.empty() ? spec.Bus : std::string(busOverride);
            stinger.Length      = *duration(spec.Clip);
            stinger.Voice       = createVoice(spec.Clip, stinger.Bus, 0, 0);
            if (!stinger.Voice.IsValid())
                return failed();
            m_Engine.SetVolume(stinger.Voice, clips.at(spec.Clip).Volume);
        }
        m_Runs.reserve(m_Segments.size() * 5);
        m_Engine.ReserveMusicScheduling(voiceCount * 2);
        return {};
    }

    bool MusicSetPlayer::BadName(std::string_view kind, std::string_view name)
    {
        const auto prefix  = std::string("Unknown music ") + std::string(kind) + " ";
        auto       message = prefix + std::string(name);
        if (m_Warnings.insert(message).second)
        {
            if (Log::GetCoreLogger())
                CORE_WARN("Audio: {}", message);
            else
                std::fprintf(stderr, "Audio: %s\n", message.c_str());
        }
        return false;
    }

    float MusicSetPlayer::Gain(size_t segment, size_t layer, const Layer& prepared) const
    {
        const auto& spec = GetData()->Segments[segment].Layers[layer];
        float       db   = spec.VolumeDb;
        if (prepared.Parameter != SIZE_MAX)
        {
            const float value = m_Parameters[prepared.Parameter];
            const auto& curve = spec.Curve;
            auto        after =
                std::upper_bound(curve.begin(), curve.end(), value, [](float input, const MusicVolumeKey& key) {
                    return input < key.Input;
                });
            if (after == curve.begin())
                db += after->VolumeDb;
            else if (after == curve.end())
                db += curve.back().VolumeDb;
            else
            {
                const auto& before   = after[-1];
                float       fraction = (value - before.Input) / (after->Input - before.Input);
                if (before.Interpolation == AudioCurveInterpolation::Smooth)
                    fraction *= fraction * (3 - 2 * fraction);
                db += before.VolumeDb + (after->VolumeDb - before.VolumeDb) * fraction;
            }
        }
        return prepared.ClipVolume * std::pow(10.f, db / 20);
    }

    size_t MusicSetPlayer::Choose(size_t stateIndex, size_t previous)
    {
        const auto& state    = GetData()->States[stateIndex];
        auto&       playlist = m_Playlists[stateIndex];
        if (playlist.Segments.empty())
            return Named(GetData()->Segments, state.Segment);
        const size_t count    = playlist.Segments.size();
        const bool   avoid    = state.NoImmediateRepeat && std::any_of(playlist.Segments.begin(),
                                                                       playlist.Segments.end(),
                                                                       [&](size_t item) { return item != previous; });
        size_t       selected = 0;
        switch (state.Order)
        {
            case MusicPlaylistOrder::Ordered:
                selected = playlist.Last == SIZE_MAX ? 0 : (playlist.Last + 1) % count;
                while (avoid && playlist.Segments[selected] == previous)
                    selected = (selected + 1) % count;
                break;
            case MusicPlaylistOrder::Shuffled: {
                const auto shuffle = [&] {
                    std::iota(playlist.Bag.begin(), playlist.Bag.end(), size_t(0));
                    std::shuffle(playlist.Bag.begin(), playlist.Bag.end(), m_Random);
                    playlist.Cursor = 0;
                };
                if (playlist.Cursor == count)
                    shuffle();
                if (avoid && playlist.Segments[playlist.Bag[playlist.Cursor]] == previous)
                {
                    const auto different = [&](size_t item) { return playlist.Segments[item] != previous; };
                    auto       first     = playlist.Bag.begin() + playlist.Cursor;
                    auto       other     = std::find_if(first, playlist.Bag.end(), different);
                    if (other == playlist.Bag.end())
                    {
                        shuffle();
                        first = playlist.Bag.begin();
                        other = std::find_if(first, playlist.Bag.end(), different);
                    }
                    std::iter_swap(first, other);
                }
                selected = playlist.Bag[playlist.Cursor++];
                break;
            }
            case MusicPlaylistOrder::Weighted: {
                double total = 0;
                for (size_t index = 0; index < count; ++index)
                    if (!avoid || playlist.Segments[index] != previous)
                        total += state.Playlist[index].Weight;
                double draw = std::uniform_real_distribution<double>(0, total)(m_Random);
                for (size_t index = 0; index < count; ++index)
                    if (!avoid || playlist.Segments[index] != previous)
                    {
                        selected = index;
                        draw -= state.Playlist[index].Weight;
                        if (draw < 0)
                            break;
                    }
                break;
            }
        }
        playlist.Last = selected;
        return playlist.Segments[selected];
    }

    MusicSetPlayer::Bank* MusicSetPlayer::FreeBank(size_t segment, bool loop)
    {
        for (auto& bank : m_Segments[segment].Banks)
            if (bank.Loop == loop && !bank.Busy)
                return &bank;
        return nullptr;
    }

    double MusicSetPlayer::Heard() const
    {
        const auto heard = m_Engine.GetPlaybackClockFrames();
        return m_PauseFrame ? std::min(heard, double(*m_PauseFrame)) : heard;
    }
    uint64_t MusicSetPlayer::Earliest() const
    {
        return Add(m_Engine.GetSchedulingClockFrames(), m_Engine.MusicSchedulingLead());
    }
    uint64_t MusicSetPlayer::FrameAt(const Run& run, double seconds) const
    {
        const auto delta = (seconds - run.Offset) / (run.Rate * m_SecondsPerFrame);
        return delta < 0 ? run.Start - std::min(run.Start, Frames(-delta)) : Add(run.Start, Frames(delta));
    }
    double MusicSetPlayer::SecondsAt(const Run& run, double clock) const
    {
        return run.Offset + (clock - double(run.Start)) * run.Rate * m_SecondsPerFrame;
    }
    double MusicSetPlayer::WrappedAt(const Run& run, double clock) const
    {
        const auto&  segment = m_Segments[run.SegmentIndex];
        const double seconds = SecondsAt(run, clock);
        return run.Voices->Loop && seconds >= segment.Exit ?
                   segment.Entry + std::fmod(seconds - segment.Entry, segment.Exit - segment.Entry) :
                   seconds;
    }

    const MusicSetPlayer::Run* MusicSetPlayer::At(double clock) const
    {
        for (auto it = m_Runs.rbegin(); it != m_Runs.rend(); ++it)
            if (!it->Voices->Layers.empty() && it->Voices != &m_Segments[it->SegmentIndex].Tail && clock >= it->Sync &&
                clock < it->End)
                return &*it;
        return nullptr;
    }
    MusicSetPlayer::Run* MusicSetPlayer::At(double clock) { return const_cast<Run*>(std::as_const(*this).At(clock)); }

    uint64_t MusicSetPlayer::FindSync(const Run& run, const MusicSyncPoint& point, uint64_t earliest) const
    {
        if (point.Sync == MusicSync::Immediate)
            return earliest;
        const auto& segment    = m_Segments[run.SegmentIndex];
        const auto& spec       = GetData()->Segments[run.SegmentIndex];
        const auto  request    = SecondsAt(run, double(earliest));
        const auto  loopLength = segment.Exit - segment.Entry;
        const auto  end        = run.Voices->Loop ? NoFrame : run.End;
        const auto  next       = [&](double origin, double period) {
            const auto sequence = period > 0 ? std::max(0.0, std::floor((request - origin) / period)) : 0.0;
            auto       frame    = FrameAt(run, origin + period * sequence);
            if (frame < earliest && period > 0)
                frame = FrameAt(run, origin + period * (sequence + 1));
            return frame >= earliest && frame <= end ? frame : NoFrame;
        };
        switch (point.Sync)
        {
            case MusicSync::NextBeat:
            case MusicSync::NextBar:
            case MusicSync::NextNBar: {
                const double beats = point.Sync == MusicSync::NextBeat ?
                                         1 :
                                         double(spec.BeatsPerBar) * (point.Sync == MusicSync::NextBar ? 1 : point.Bars);
                return next(spec.FirstBeatSeconds, 60 * beats / spec.BPM);
            }
            case MusicSync::NextCue: {
                uint64_t first = NoFrame;
                for (size_t index = 0; index < spec.Cues.size(); ++index)
                {
                    if (!point.Cue.empty() && spec.Cues[index].Name != point.Cue)
                        continue;
                    const auto cue = segment.Cues[index];
                    if (cue <= segment.Exit)
                        first = std::min(first, next(cue, run.Voices->Loop && cue >= segment.Entry ? loopLength : 0));
                }
                return first;
            }
            case MusicSync::ExitCue:
                return next(segment.Exit, run.Voices->Loop ? loopLength : 0);
            case MusicSync::EndOfSegment:
                return run.Voices->Loop ? next(segment.Exit, loopLength) :
                                          (run.Release >= earliest ? run.Release : NoFrame);
            case MusicSync::Immediate:
                break;
        }
        return NoFrame;
    }

    bool MusicSetPlayer::StartRun(size_t           index,
                                  size_t           state,
                                  uint64_t         sync,
                                  double           offset,
                                  bool             loop,
                                  const MusicFade& fade,
                                  bool             via,
                                  bool             stateChange,
                                  Bank*            tail)
    {
        auto& segment = m_Segments[index];
        auto* bank    = tail ? tail : FreeBank(index, loop);
        if (!bank || bank->Busy || sync == NoFrame)
            return false;
        const auto& spec     = GetData()->Segments[index];
        const auto  rate     = m_Engine.MusicBusRate(segment.Bus);
        const auto  earliest = Earliest();
        const auto  preEntry =
            spec.PreEntry && offset == segment.Entry && !tail ?
                std::min(sync - std::min(sync, earliest), Frames(offset / (rate * m_SecondsPerFrame))) :
                0;
        const auto source = std::max(0.0, offset - double(preEntry) * rate * m_SecondsPerFrame);
        Run        run {index, state, bank, sync - preEntry, sync};
        run.NextEvent   = sync;
        run.Offset      = source;
        run.Rate        = rate;
        run.Via         = via;
        run.StateChange = stateChange;
        if (!loop)
        {
            run.End     = FrameAt(run, segment.Exit);
            run.Release = FrameAt(run, spec.PostExit ? segment.Length : segment.Exit);
        }
        AudioVoiceWindow window;
        window.Duration = run.Release == NoFrame ? NoFrame : run.Release - run.Start;
        window.FadeIn   = Frames(fade.Seconds / m_SecondsPerFrame);
        window.InCurve  = fade.Curve;
        for (size_t layer = 0; layer < bank->Layers.size(); ++layer)
        {
            const auto& voice = bank->Layers[layer];
            if (m_Engine.StartVoice(voice.Voice,
                                    run.Start,
                                    0,
                                    fade.Curve,
                                    &window,
                                    Frames(source * voice.Rate),
                                    Gain(index, layer, voice)) == NoFrame)
            {
                for (const auto& cleanup : bank->Layers)
                    m_Engine.Stop(cleanup.Voice);
                return false;
            }
        }
        bank->Busy = true;
        m_Runs.push_back(run);
        return true;
    }

    void MusicSetPlayer::EndRun(Run& run, uint64_t sync, const MusicFade& fade)
    {
        const auto fadeFrames = Frames(fade.Seconds / m_SecondsPerFrame);
        const auto end        = std::min(run.Release, Add(sync, fadeFrames));
        if (end != run.Release)
            for (const auto& layer : run.Voices->Layers)
                m_Engine.ScheduleStop(layer.Voice, end, end > sync ? end - sync : 0, fade.Curve);
        run.End     = std::min(run.End, sync);
        run.Release = end;
        run.Cut     = true;
    }

    double MusicSetPlayer::EntryPoint(size_t index, const MusicTransition& rule, double relative) const
    {
        const auto& segment = m_Segments[index];
        if (rule.Entry == MusicEntry::SameRelativePosition)
            return segment.Entry + std::fmod(std::max(0.0, relative), segment.Exit - segment.Entry);
        if (rule.Entry == MusicEntry::NamedCue)
        {
            const auto cue = Named(GetData()->Segments[index].Cues, rule.Cue);
            if (cue == SIZE_MAX || segment.Cues[cue] >= segment.Exit)
                return -1;
            return segment.Cues[cue];
        }
        return segment.Entry;
    }

    bool MusicSetPlayer::Play(std::string_view state, double fadeSeconds)
    {
        if (!GetData() || !std::isfinite(fadeSeconds) || fadeSeconds < 0)
            return false;
        const auto index = state.empty() ? 0 : Named(GetData()->States, state);
        if (index == SIZE_MAX)
            return BadName("state", state);
        Stop();
        Reap(m_Engine.GetSchedulingClockFrames());
        const auto  segment  = Choose(index, SIZE_MAX);
        const auto& spec     = GetData()->Segments[segment];
        const auto  first    = Earliest();
        const auto  preEntry = spec.PreEntry ?
                                   Frames(m_Segments[segment].Entry /
                                          (m_SecondsPerFrame * m_Engine.MusicBusRate(m_Segments[segment].Bus))) :
                                   0;
        m_Engine.BeginMusicSchedule();
        const bool played = StartRun(segment,
                                     index,
                                     Add(first, preEntry),
                                     m_Segments[segment].Entry,
                                     spec.Loop && m_Playlists[index].Segments.empty(),
                                     {fadeSeconds},
                                     false,
                                     true);
        m_Engine.EndMusicSchedule();
        RefreshTransport();
        return played;
    }

    void MusicSetPlayer::Stop(double fadeSeconds)
    {
        if (!std::isfinite(fadeSeconds) || fadeSeconds < 0)
            return;
        ++m_Revision;
        m_RequestedState = SIZE_MAX;
        const auto now   = m_Engine.GetSchedulingClockFrames();
        m_Engine.BeginMusicSchedule();
        for (auto& run : m_Runs)
        {
            if (run.Start >= now || m_PauseFrame || fadeSeconds == 0)
            {
                for (const auto& layer : run.Voices->Layers)
                    m_Engine.Stop(layer.Voice);
                run.Release = run.End = now;
            }
            else
                EndRun(run, now, {fadeSeconds});
        }
        for (auto& stinger : m_Stingers)
        {
            m_Engine.Stop(stinger.Voice);
            stinger.Start = NoFrame;
            stinger.End   = 0;
        }
        m_Engine.EndMusicSchedule();
        m_PauseFrame.reset();
        Reap(now);
    }

    bool MusicSetPlayer::SetState(std::string_view state)
    {
        if (!GetData())
            return false;
        const auto index = Named(GetData()->States, state);
        if (index == SIZE_MAX)
            return BadName("state", state);
        RefreshTransport();
        const auto clock    = Heard();
        auto*      outgoing = At(clock);
        if (m_PauseFrame || std::any_of(m_Runs.begin(), m_Runs.end(), [&](const Run& run) {
                return run.Sync > clock && run.Sync < run.End;
            }))
        {
            m_RequestedState = index;
            return true;
        }
        if (!outgoing)
            return Play(state);
        if (outgoing->StateIndex == index)
            return true;
        const MusicTransition fallback;
        const auto*           selected = GetData()->FindTransition(GetData()->States[outgoing->StateIndex].Name, state);
        const auto&           rule     = selected ? *selected : fallback;
        const auto            segment  = Choose(index, outgoing->SegmentIndex);
        const auto&           spec     = GetData()->Segments[segment];
        const auto            via      = rule.Via.empty() ? SIZE_MAX : Named(GetData()->Segments, rule.Via);
        const auto&           incoming = m_Segments[via == SIZE_MAX ? segment : via];
        const auto&           incomingSpec = GetData()->Segments[via == SIZE_MAX ? segment : via];
        const auto preEntry = incomingSpec.PreEntry ?
                                  Frames(incoming.Entry / (m_SecondsPerFrame * m_Engine.MusicBusRate(incoming.Bus))) :
                                  0;
        const auto reached  = Add(Frames(std::floor(clock)), rule.Sync.Sync == MusicSync::Immediate ? 0 : 1);
        const auto earliest = std::max(Add(Earliest(), preEntry), reached);
        const auto sync     = FindSync(*outgoing, rule.Sync, earliest);
        if (sync == NoFrame)
            return false;
        const auto relative = WrappedAt(*outgoing, double(sync)) - m_Segments[outgoing->SegmentIndex].Entry;
        const auto entry    = EntryPoint(segment, rule, relative);
        if (entry < 0)
            return BadName("entry cue", rule.Cue);
        const bool loop = spec.Loop && m_Playlists[index].Segments.empty();
        if (!FreeBank(segment, loop) || (via != SIZE_MAX && !FreeBank(via, false)))
            return false;
        const auto original = m_Runs.size();
        m_Engine.BeginMusicSchedule();
        bool success         = true;
        auto destinationSync = sync;
        if (via != SIZE_MAX)
        {
            success = StartRun(via, outgoing->StateIndex, sync, m_Segments[via].Entry, false, rule.FadeIn, true);
            if (success)
                destinationSync = m_Runs.back().End;
        }
        success = success && StartRun(segment, index, destinationSync, entry, loop, rule.FadeIn, false, true);
        if (success)
        {
            auto&      current = m_Segments[outgoing->SegmentIndex];
            const bool atExit  = FindSync(*outgoing, {MusicSync::ExitCue}, sync) == sync;
            if (atExit && outgoing->Voices->Loop && !current.Tail.Layers.empty() &&
                StartRun(outgoing->SegmentIndex,
                         outgoing->StateIndex,
                         sync,
                         current.Exit,
                         false,
                         {},
                         true,
                         false,
                         &current.Tail))
            {
                EndRun(*outgoing, sync, {});
                if (rule.FadeOut.Seconds > 0)
                    EndRun(m_Runs.back(), sync, rule.FadeOut);
            }
            else if (atExit && !outgoing->Voices->Loop && GetData()->Segments[outgoing->SegmentIndex].PostExit &&
                     rule.FadeOut.Seconds == 0)
            {
                outgoing->End = std::min(outgoing->End, sync);
                outgoing->Cut = true;
            }
            else
                EndRun(*outgoing, sync, rule.FadeOut);
            m_Runs[original].Transition = true;
            ++m_Revision;
            m_Engine.RecordMusicEvent(
                AudioProfileEventType::TransitionScheduled, m_Runs[original].Voices->Layers.front().Voice, sync);
        }
        else
        {
            while (m_Runs.size() > original)
            {
                auto& bank = *m_Runs.back().Voices;
                for (const auto& layer : bank.Layers)
                    m_Engine.Stop(layer.Voice);
                bank.Busy = false;
                m_Runs.pop_back();
            }
        }
        m_Engine.EndMusicSchedule();
        return success;
    }

    bool MusicSetPlayer::SetParameter(std::string_view name, float value)
    {
        if (!GetData() || !std::isfinite(value))
            return false;
        const auto index = Named(GetData()->Parameters, name);
        if (index == SIZE_MAX)
            return BadName("parameter", name);
        const auto& parameter = GetData()->Parameters[index];
        value                 = std::clamp(value, parameter.Min, parameter.Max);
        if (m_Parameters[index] == value)
            return true;
        m_Parameters[index] = value;
        m_Engine.BeginMusicSchedule();
        for (auto& run : m_Runs)
            for (size_t layer = 0; layer < run.Voices->Layers.size(); ++layer)
            {
                const auto& voice = run.Voices->Layers[layer];
                if (voice.Parameter == index)
                    m_Engine.FadeTo(voice.Voice, Gain(run.SegmentIndex, layer, voice), parameter.RampSeconds);
            }
        m_Engine.EndMusicSchedule();
        return true;
    }

    bool MusicSetPlayer::TriggerStinger(std::string_view name)
    {
        if (!GetData())
            return false;
        const auto index = Named(GetData()->Stingers, name);
        if (index == SIZE_MAX)
            return BadName("stinger", name);
        RefreshTransport();
        auto&       stinger  = m_Stingers[index];
        const auto& spec     = GetData()->Stingers[index];
        const auto  earliest = Earliest();
        if (earliest < stinger.End || m_Engine.IsPlaying(stinger.Voice) || m_Engine.IsBusPaused(stinger.Bus))
            return false;
        const auto* run = At(Heard());
        const auto  sync =
            spec.Sync.Sync == MusicSync::Immediate ? earliest :
            run && !m_PauseFrame ? FindSync(*run, spec.Sync, std::max(earliest, Add(Frames(std::floor(Heard())), 1))) :
                                   NoFrame;
        if (sync == NoFrame || sync < stinger.Next)
            return false;
        m_Engine.BeginMusicSchedule();
        const auto start = m_Engine.StartVoice(stinger.Voice, sync, 0, AudioFadeCurve::Linear, nullptr, 0);
        m_Engine.EndMusicSchedule();
        if (start == NoFrame)
            return false;
        stinger.Start   = start;
        stinger.End     = Add(start, Frames(stinger.Length / (m_SecondsPerFrame * m_Engine.MusicBusRate(stinger.Bus))));
        stinger.Next    = Add(start, Frames(spec.MinimumInterval / m_SecondsPerFrame));
        stinger.Started = false;
        m_Engine.RecordMusicEvent(AudioProfileEventType::StingerScheduled, stinger.Voice, start);
        return true;
    }

    void MusicSetPlayer::SetEventCallback(EventCallback callback)
    {
        if (callback && !m_Callback)
            for (auto& run : m_Runs)
                run.NextEvent = std::max(run.NextEvent, Frames(std::floor(Heard())));
        m_Callback = callback ? std::make_shared<const EventCallback>(std::move(callback)) : nullptr;
        ++m_Revision;
    }

    void MusicSetPlayer::Dispatch(Run& run, uint64_t heard)
    {
        if (run.Voices == &m_Segments[run.SegmentIndex].Tail || heard < run.Sync || run.End <= run.Sync)
            return;
        const auto  revision = m_Revision;
        const auto  callback = m_Callback;
        const auto& data     = *GetData();
        const auto& spec     = data.Segments[run.SegmentIndex];
        const auto& segment  = m_Segments[run.SegmentIndex];
        if (!run.Started)
        {
            run.Started = true;
            if (run.Transition)
                m_Engine.RecordMusicEvent(
                    AudioProfileEventType::TransitionStarted, run.Voices->Layers.front().Voice, run.Sync);
            if (run.StateChange && callback)
                (*callback)({MusicSetEventType::StateChanged, run.Sync, data.States[run.StateIndex].Name});
        }
        if (revision != m_Revision || !callback)
            return;
        const auto period = 60 / spec.BPM;
        const auto last   = std::min(heard, run.End);
        while (run.NextEvent <= last)
        {
            const auto first     = run.NextEvent;
            auto       beat      = Frames(std::floor((SecondsAt(run, double(first)) - spec.FirstBeatSeconds) / period));
            auto       beatFrame = FrameAt(run, spec.FirstBeatSeconds + double(beat) * period);
            if (beatFrame < first)
            {
                ++beat;
                beatFrame = FrameAt(run, spec.FirstBeatSeconds + double(beat) * period);
            }
            auto       next     = beatFrame;
            const auto cueFrame = [&](size_t index) {
                const auto cue        = segment.Cues[index];
                const auto loopLength = segment.Exit - segment.Entry;
                const auto repeats    = run.Voices->Loop && cue >= segment.Entry;
                if (cue > segment.Exit || (!repeats && FrameAt(run, cue) < run.Sync))
                    return NoFrame;
                const auto cycle =
                    repeats ? std::max(0.0, std::floor((SecondsAt(run, double(first)) - cue) / loopLength)) : 0.0;
                auto frame = FrameAt(run, cue + cycle * loopLength);
                if (frame < first && repeats)
                    frame = FrameAt(run, cue + (cycle + 1) * loopLength);
                return frame < first ? NoFrame : frame;
            };
            for (size_t index = 0; index < spec.Cues.size(); ++index)
                next = std::min(next, cueFrame(index));
            if (next > last)
                break;
            run.NextEvent        = Add(next, 1);
            const auto bar       = beat / spec.BeatsPerBar;
            const auto beatInBar = uint32_t(beat % spec.BeatsPerBar);
            if (next == beatFrame && next < run.End)
            {
                (*callback)({MusicSetEventType::Beat, next, {}, bar, beatInBar});
                if (revision != m_Revision)
                    return;
                if (beatInBar == 0)
                {
                    (*callback)({MusicSetEventType::Bar, next, {}, bar, beatInBar});
                    if (revision != m_Revision)
                        return;
                }
            }
            for (size_t index = 0; index < spec.Cues.size(); ++index)
                if (cueFrame(index) == next)
                {
                    (*callback)({MusicSetEventType::Cue, next, spec.Cues[index].Name});
                    if (revision != m_Revision)
                        return;
                }
        }
    }

    void MusicSetPlayer::Reap(uint64_t heard)
    {
        m_Runs.erase(std::remove_if(m_Runs.begin(),
                                    m_Runs.end(),
                                    [&](const Run& run) {
                                        if (heard < run.Release)
                                            return false;
                                        run.Voices->Busy = false;
                                        return true;
                                    }),
                     m_Runs.end());
    }

    void MusicSetPlayer::RefreshTransport()
    {
        if (m_Runs.empty())
            return;
        const auto  now       = m_Engine.GetSchedulingClockFrames();
        const auto* current   = At(Heard());
        const auto& reference = current ? *current : m_Runs.front();
        const auto& bus       = m_Segments[reference.SegmentIndex].Bus;
        const bool  busPaused = m_Engine.IsBusPaused(bus);
        const bool  paused    = m_UserPaused || busPaused;
        const auto  shift     = [](uint64_t frame, uint64_t delay) { return Add(frame, delay); };
        if (paused && !m_PauseFrame)
        {
            m_PauseFrame = m_Engine.GetClockFrames();
            if (m_UserPaused && !busPaused)
                for (auto& run : m_Runs)
                    if (run.Release > *m_PauseFrame)
                        for (const auto& layer : run.Voices->Layers)
                            m_Engine.Pause(layer.Voice);
        }
        if (!paused && m_PauseFrame)
        {
            const auto delay = now - std::min(now, *m_PauseFrame);
            for (auto& run : m_Runs)
            {
                if (run.Release <= *m_PauseFrame)
                    continue;
                run.Start = shift(run.Start, delay);
                if (!run.Started)
                    run.Sync = shift(run.Sync, delay);
                run.End       = shift(run.End, delay);
                run.Release   = shift(run.Release, delay);
                run.NextEvent = shift(run.NextEvent, delay);
                for (const auto& layer : run.Voices->Layers)
                    m_Engine.UnPause(layer.Voice);
            }
            m_PauseFrame.reset();
        }
        if (paused)
            return;
        for (auto& run : m_Runs)
        {
            const auto rate = m_Engine.MusicBusRate(m_Segments[run.SegmentIndex].Bus);
            if (run.Rate == rate || run.Release <= now)
                continue;
            const auto remap = [&](uint64_t frame) {
                return frame == NoFrame || frame <= now ? frame :
                                                          Add(now, Frames(double(frame - now) * run.Rate / rate));
            };
            // Scheduled starts use the existing output clock. Re-anchor the running grid at the
            // current position, then recompute its remaining content duration at the new rate.
            if (run.Start <= now)
            {
                const auto& segment  = m_Segments[run.SegmentIndex];
                const auto  logical  = run.Cut ? run.End : remap(run.End);
                const auto  release  = run.Cut ? run.Release : remap(run.Release);
                const auto  old      = SecondsAt(run, double(now));
                const auto  position = m_Engine.GetVoicePositionSeconds(run.Voices->Layers.front().Voice);
                const auto  period   = segment.Exit - segment.Entry;
                const auto  cycles   = run.Voices->Loop ? std::round((old - position) / period) : 0;
                run.Offset           = position + std::max(0.0, cycles) * period;
                run.Start            = now;
                run.End              = logical;
                if (release != run.Release)
                    for (const auto& layer : run.Voices->Layers)
                        m_Engine.ScheduleStop(layer.Voice, release);
                run.Release = release;
            }
            run.Rate = rate;
        }
    }

    void MusicSetPlayer::SetPaused(bool paused)
    {
        m_UserPaused = paused;
        RefreshTransport();
    }

    bool MusicSetPlayer::SeekSeconds(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0)
            return false;
        auto* run = At(Heard());
        if (!run)
            return false;
        const auto segment = run->SegmentIndex;
        const auto state   = run->StateIndex;
        const bool loop    = run->Voices->Loop;
        if (seconds >= m_Segments[segment].Exit)
            return false;
        Stop();
        m_Engine.BeginMusicSchedule();
        const bool result = StartRun(segment, state, Earliest(), seconds, loop, {});
        m_Engine.EndMusicSchedule();
        RefreshTransport();
        return result;
    }

    void MusicSetPlayer::Update()
    {
        if (!GetData() || m_Updating)
            return;
        struct UpdateScope
        {
            bool& Updating;
            ~UpdateScope() { Updating = false; }
        } scope {m_Updating};
        m_Updating = true;
        RefreshTransport();
        const auto heard    = Frames(std::floor(Heard()));
        const auto revision = m_Revision;
        for (size_t index = 0; index < m_Runs.size(); ++index)
        {
            Dispatch(m_Runs[index], heard);
            if (m_Revision != revision)
                return;
        }
        if (!m_PauseFrame)
            Reap(heard);
        for (auto& stinger : m_Stingers)
        {
            if (stinger.Started || stinger.Start == NoFrame)
                continue;
            const auto start = m_Engine.GetMusicVoiceStart(stinger.Voice);
            if (!start)
                continue;
            if (*start > stinger.Start)
            {
                const auto delay = *start - stinger.Start;
                stinger.Start    = *start;
                stinger.End      = Add(stinger.End, delay);
                stinger.Next     = Add(stinger.Next, delay);
            }
            if (heard >= stinger.Start)
            {
                stinger.Started = true;
                m_Engine.RecordMusicEvent(AudioProfileEventType::StingerStarted, stinger.Voice, stinger.Start);
            }
        }
        if (m_PauseFrame || m_Runs.empty())
            return;
        auto* current = At(double(heard));
        if (!current || std::any_of(m_Runs.begin(), m_Runs.end(), [&](const Run& run) {
                return run.Sync > heard && run.Sync < run.End;
            }))
            return;
        if (m_RequestedState != SIZE_MAX)
        {
            const auto requested = std::exchange(m_RequestedState, SIZE_MAX);
            if (SetState(GetData()->States[requested].Name) && requested != current->StateIndex)
                return;
        }
        if (current->Via || m_Playlists[current->StateIndex].Segments.empty())
            return;
        const auto oldest = Earliest();
        const auto ahead  = Add(oldest, m_Engine.GetSampleRate() / 4);
        if (current->End < oldest || current->End > ahead)
            return;
        const auto next = Choose(current->StateIndex, current->SegmentIndex);
        m_Engine.BeginMusicSchedule();
        if (StartRun(next, current->StateIndex, current->End, m_Segments[next].Entry, false, {}))
        {
            m_Runs.back().Transition = true;
            m_Engine.RecordMusicEvent(
                AudioProfileEventType::TransitionScheduled, m_Runs.back().Voices->Layers.front().Voice, current->End);
        }
        m_Engine.EndMusicSchedule();
    }

    std::optional<MusicSetPosition> MusicSetPlayer::GetPosition() const
    {
        const auto  clock = Heard();
        const auto* run   = At(clock);
        if (!run)
            return std::nullopt;
        const auto& data    = *GetData();
        const auto& segment = data.Segments[run->SegmentIndex];
        const auto  seconds = m_Engine.GetVoicePositionSeconds(run->Voices->Layers.front().Voice);
        const auto  beat    = std::max(0.0, (seconds - segment.FirstBeatSeconds) * segment.BPM / 60);
        return MusicSetPosition {data.States[run->StateIndex].Name,
                                 segment.Name,
                                 seconds,
                                 beat,
                                 Frames(std::floor(beat / segment.BeatsPerBar)),
                                 std::fmod(beat, segment.BeatsPerBar)};
    }

    std::optional<MusicSetPendingTransition> MusicSetPlayer::GetPendingTransition() const
    {
        if (!GetData())
            return std::nullopt;
        const auto  clock   = Heard();
        const auto* current = At(clock);
        const Run*  first   = nullptr;
        const Run*  target  = nullptr;
        for (const auto& run : m_Runs)
        {
            if (run.Voices == &m_Segments[run.SegmentIndex].Tail || double(run.Sync) <= clock || run.Sync >= run.End)
                continue;
            if (!first || run.Sync < first->Sync)
                first = &run;
            if (run.StateChange && (!target || run.Sync < target->Sync))
                target = &run;
        }
        if (!first)
            return std::nullopt;
        const auto& data = *GetData();
        return MusicSetPendingTransition {data.States[(target ? target : first)->StateIndex].Name,
                                          data.Segments[first->SegmentIndex].Name,
                                          current ? WrappedAt(*current, double(first->Sync)) : first->Offset,
                                          first->Sync};
    }

    std::optional<float> MusicSetPlayer::GetParameter(std::string_view name) const
    {
        if (!GetData())
            return std::nullopt;
        const auto index = Named(GetData()->Parameters, name);
        return index == SIZE_MAX ? std::nullopt : std::optional<float>(m_Parameters[index]);
    }
} // namespace Kans
