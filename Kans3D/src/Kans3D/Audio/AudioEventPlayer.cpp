#include "kspch.h"
#include "AudioEventPlayer.h"

#include <cmath>
#include <numeric>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "AudioClipRegistry.h"

namespace Kans
{
    namespace
    {
        double Unit(std::mt19937_64& rng)
        {
            // A fixed conversion to [0, 1) gives the same draws on different standard libraries.
            return double(rng() >> 11) * (1.0 / 9007199254740992.0);
        }

        size_t Index(std::mt19937_64& rng, size_t count)
        {
            const uint64_t bound     = count;
            const uint64_t threshold = (uint64_t(0) - bound) % bound;
            uint64_t       draw;
            do
                draw = rng();
            while (draw < threshold);
            return size_t(draw % bound);
        }

        double Sample(std::mt19937_64& rng, AudioEventRange range)
        {
            return range.Min == range.Max ? range.Min : range.Min + (double(range.Max) - range.Min) * Unit(rng);
        }
    } // namespace

    AudioEventPlayer::AudioEventPlayer(AudioEngine& engine) : m_Engine(engine)
    {
        std::random_device random;
        m_Seed = (uint64_t(random()) << 32) ^ random();
    }

    void AudioEventPlayer::SetSeed(uint64_t seed)
    {
        m_Seed = seed;
        for (auto& [id, state] : m_Events)
            state.Rng.seed(seed);
    }

    void AudioEventPlayer::Clear() { m_Events.clear(); }

    void AudioEventPlayer::State::Update(Ref<const AudioEventAsset> event)
    {
        const auto& data     = event->GetData();
        const auto* previous = Event ? &Event->GetData() : nullptr;
        const bool  changed  = !previous || previous->Entries != data.Entries || previous->Selection != data.Selection;
        const bool  historyChanged = changed || previous->AvoidRepeatingLast != data.AvoidRepeatingLast;
        Valid                      = !ValidateAudioEvent(data);
        if (changed)
        {
            Next = Remaining = 0;
            LastShuffled.reset();
            Bag.resize(data.Selection == AudioEventSelection::Shuffle ? data.Entries.size() : 0);
        }
        if (historyChanged)
        {
            const auto avoid = Valid && data.Selection == AudioEventSelection::Random ?
                                   std::min(size_t(data.AvoidRepeatingLast), data.Entries.size() - 1) :
                                   0;
            History.resize(avoid);
            Excluded.assign(avoid ? data.Entries.size() : 0, 0);
            HistorySize = HistoryHead = 0;
        }
        Event = std::move(event);
    }

    size_t AudioEventPlayer::State::Pick()
    {
        const auto& data  = Event->GetData();
        const auto  count = data.Entries.size();
        if (data.Selection == AudioEventSelection::Sequential)
            return Next;
        if (data.Selection == AudioEventSelection::Shuffle)
        {
            if (!Remaining)
            {
                std::iota(Bag.begin(), Bag.end(), size_t(0));
                for (size_t i = count; i > 1; --i)
                    std::swap(Bag[i - 1], Bag[Index(Rng, i)]);
                if (count > 1 && LastShuffled == Bag.back())
                    std::swap(Bag.back(), Bag[Index(Rng, count - 1)]);
                Remaining = count;
            }
            return Bag[Remaining - 1];
        }

        const auto eligible = [&](size_t i) { return Excluded.empty() || !Excluded[i]; };
        double     total    = 0;
        for (size_t i = 0; i < count; ++i)
            if (eligible(i))
                total += data.Entries[i].Weight;
        double draw         = Unit(Rng) * total;
        size_t lastEligible = 0;
        for (size_t i = 0; i < count; ++i)
        {
            if (!eligible(i))
                continue;
            lastEligible = i;
            if (draw < data.Entries[i].Weight)
                return i;
            draw -= data.Entries[i].Weight;
        }
        return lastEligible; // Floating-point rounding at the upper boundary.
    }

    void AudioEventPlayer::State::Accept(size_t index)
    {
        const auto& data = Event->GetData();
        if (data.Selection == AudioEventSelection::Sequential)
            Next = (Next + 1) % data.Entries.size();
        else if (data.Selection == AudioEventSelection::Shuffle)
        {
            --Remaining;
            LastShuffled = index;
        }
        else if (!History.empty())
        {
            if (HistorySize == History.size())
                Excluded[History[HistoryHead]] = false;
            else
                ++HistorySize;
            History[HistoryHead] = index;
            Excluded[index]      = true;
            HistoryHead          = (HistoryHead + 1) % History.size();
        }
    }

    AudioEventPlayer::State& AudioEventPlayer::StateOf(AssetID event)
    {
        auto [it, inserted] = m_Events.try_emplace(event);
        if (inserted)
            it->second.Rng.seed(m_Seed);
        return it->second;
    }

    namespace
    {
        bool ValidParams(const AudioEventParams& params)
        {
            return std::isfinite(params.Volume) && params.Volume >= 0 && std::isfinite(params.Pitch) &&
                   params.Pitch > 0;
        }
    } // namespace

    AudioEventTriggerResult AudioEventPlayer::Trigger(AssetManager&           assets,
                                                      AudioClipRegistry&      clips,
                                                      AssetID                 event,
                                                      const AudioEventParams& params)
    {
        if (!m_Engine.IsInitialized() || !event.IsValid())
            return {};
        auto& state = StateOf(event);
        ++state.Stats.TotalTriggers;
        if (!ValidParams(params))
            return {};
        // Load by ID each time so a newly published product takes effect on this trigger.
        auto  loadedEvent = assets.Load<AudioEventAsset>(event);
        auto* asset       = std::get_if<Ref<const AudioEventAsset>>(&loadedEvent);
        if (!asset)
            return {};
        return Play(state, assets, clips, event, *asset, params);
    }

    AudioEventTriggerResult AudioEventPlayer::Trigger(AssetManager&                     assets,
                                                      AudioClipRegistry&                clips,
                                                      AssetID                           event,
                                                      const Ref<const AudioEventAsset>& asset,
                                                      const AudioEventParams&           params)
    {
        if (!m_Engine.IsInitialized() || !event.IsValid() || !asset)
            return {};
        auto& state = StateOf(event);
        ++state.Stats.TotalTriggers;
        if (!ValidParams(params))
            return {};
        return Play(state, assets, clips, event, asset, params);
    }

    AudioEventTriggerResult AudioEventPlayer::Play(State&                            state,
                                                   AssetManager&                     assets,
                                                   AudioClipRegistry&                clips,
                                                   AssetID                           event,
                                                   const Ref<const AudioEventAsset>& asset,
                                                   const AudioEventParams&           params)
    {
        if (state.Event != asset)
        {
            state.Update(asset);
            if (state.Valid)
                m_Engine.DefineEventGroup(event, asset->GetData().InstanceLimit);
        }
        if (!state.Valid)
            return {};

        const auto& data = state.Event->GetData();
        const auto  now  = m_Engine.GetClockFrames();
        if (state.LastTrigger && double(now - *state.LastTrigger) < (data.CooldownSeconds * m_Engine.GetSampleRate()))
        {
            ++state.Stats.CooldownRejections;
            return {{}, AudioEventTriggerStatus::Cooldown};
        }
        const auto  index      = state.Pick();
        const auto& entry      = data.Entries[index];
        auto        loadedClip = assets.Load<AudioClipAsset>(entry.Clip);
        auto*       clip       = std::get_if<Ref<const AudioClipAsset>>(&loadedClip);
        if (!clip || !(*clip)->IsValid())
            return {{}, AudioEventTriggerStatus::MissingClip};

        const auto volumeDb = double(data.VolumeDb) + entry.VolumeDb + Sample(state.Rng, data.VolumeRangeDb);
        const auto pitchSemitones =
            double(data.PitchSemitones) + entry.PitchSemitones + Sample(state.Rng, data.PitchRangeSemitones);
        auto& desc  = state.VoiceDesc;
        desc.Volume = float(params.Volume * std::pow(10.0, volumeDb / 20));
        desc.Pitch  = float(params.Pitch * std::exp2(pitchSemitones / 12));
        if (!std::isfinite(desc.Volume * (*clip)->GetData().Volume) || !std::isfinite(desc.Pitch) || desc.Pitch <= 0)
            return {};
        desc.Priority         = params.Priority.value_or(data.Priority);
        desc.Bus              = params.Bus ? *params.Bus : data.Bus;
        desc.Loop             = params.Loop.value_or(data.Loop);
        desc.ReverbZoneSend   = params.ReverbZoneSend && data.ReverbZoneSend;
        desc.LfeSendDb        = params.LfeSendDb.value_or(data.LfeSendDb);
        desc.PinToCenter      = params.PinToCenter;
        desc.ConcurrencyGroup = data.ConcurrencyGroup.empty() ? params.FallbackConcurrencyGroup : data.ConcurrencyGroup;
        const bool spatial    = params.Spatial.value_or(data.Spatial);
        desc.Pan              = spatial ? 0 : params.Pan;
        auto emitter          = params.Emitter.value_or(data.Attenuation);
        if (params.Position)
            emitter.Position = *params.Position;
        if (params.EntityTransform)
        {
            emitter.Shape          = params.EntityShape;
            emitter.ShapeTransform = *params.EntityTransform;
            if (!params.Position)
                emitter.Position = glm::vec3((*params.EntityTransform)[3]);
            if (emitter.Shape.Type != AudioSourceShape::Point || params.AttenuationAsset.IsValid() ||
                data.AttenuationAsset.IsValid())
                emitter.Direction = -glm::vec3((*params.EntityTransform)[2]);
        }

        desc.ReverbPosition = params.Position;
        if (!desc.ReverbPosition && (params.Emitter || params.EntityTransform))
            desc.ReverbPosition = emitter.Position;

        bool         rejected = false;
        const double offset   = Sample(state.Rng, data.StartOffsetSeconds);
        auto         voice    = m_Engine.PlayRegisteredOneShot(clips,
                                                               *clip,
                                                               desc,
                                                               spatial ? &emitter : nullptr,
                                                               offset,
                                                               &rejected,
                                                               event,
                                                               &assets,
                                                               data.AttenuationAsset,
                                                               params.AttenuationAsset);
        if (rejected)
        {
            ++state.Stats.LimitRejections;
            return {{}, AudioEventTriggerStatus::InstanceLimit};
        }
        if (!voice.IsValid())
            return {};
        state.Accept(index);
        state.LastTrigger = m_Engine.GetClockFrames();
        AudioEventTriggerResult result {voice, AudioEventTriggerStatus::Started};
        result.Entry              = index;
        result.VolumeDb           = float(volumeDb);
        result.PitchSemitones     = float(pitchSemitones);
        result.StartOffsetSeconds = offset;
        return result;
    }

    std::vector<AssetID> AudioEventPlayer::GetTriggeredEvents() const
    {
        std::vector<AssetID> events;
        events.reserve(m_Events.size());
        for (const auto& entry : m_Events)
            events.push_back(entry.first);
        return events;
    }

    void AudioEventPlayer::Forget(AssetID event) { m_Events.erase(event); }

    AudioEventStats AudioEventPlayer::GetStats(AssetID event) const
    {
        const auto it = m_Events.find(event);
        if (it == m_Events.end())
            return {};
        auto stats            = it->second.Stats;
        stats.ActiveInstances = m_Engine.CountEventInstances(event);
        return stats;
    }
} // namespace Kans
