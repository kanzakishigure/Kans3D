#include "KansEditorCore/Audio/AudioMixerEditing.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include "Kans3D/Audio/AudioBusEffectFields.h"
#include "Kans3D/Audio/AudioEngine.h"

namespace Kans::AudioMixerEditing
{
    namespace
    {
        constexpr float NegativeInfinity = -std::numeric_limits<float>::infinity();

        float Gain(float db) { return std::pow(10.0f, db / 20.0f); }

        template<size_t... I>
        AudioBusEffectParameters DefaultParameters(size_t type, std::index_sequence<I...>)
        {
            AudioBusEffectParameters result;
            ((type == I ? (result = AudioBusEffectParameters(std::in_place_index<I>), 0) : 0), ...);
            return result;
        }
    } // namespace

    // ---------------------------------------------------------------- scales

    float DbToFader(float db)
    {
        if (std::isnan(db) || db == NegativeInfinity)
            return 0.0f;
        db = std::clamp(db, FaderMinDb, FaderMaxDb);
        return std::pow(Gain(db) / Gain(FaderMaxDb), 0.25f);
    }

    float FaderToDb(float position)
    {
        const float floor = DbToFader(FaderMinDb);
        if (!(position >= floor * 0.5f))
            return NegativeInfinity;
        if (position <= floor)
            return FaderMinDb;
        position       = std::min(position, 1.0f);
        const float db = 20.0f * std::log10(std::pow(position, 4.0f) * Gain(FaderMaxDb));
        return std::clamp(db, FaderMinDb, FaderMaxDb);
    }

    float MeterPosition(float db) { return db > FaderMinDb ? DbToFader(db) : 0.0f; }

    float AmplitudeToDb(float amplitude) { return amplitude > 0.0f ? 20.0f * std::log10(amplitude) : NegativeInfinity; }

    // ---------------------------------------------------------------- meters

    void MeterBallistics::Update(const AudioBusMeter& meter, float deltaSeconds)
    {
        deltaSeconds = std::max(0.0f, deltaSeconds);
        m_Count      = std::min(meter.Channels.size(), MaxChannels);
        for (size_t i = 0; i < MaxChannels; ++i)
        {
            auto&       channel = m_Channels[i];
            const float peak    = i < m_Count ? meter.Channels[i].Peak : 0.0f;
            const float db      = std::max(AmplitudeToDb(peak), -1e9f);
            // The bar rises at once and falls at a fixed rate.
            channel.LevelDb = std::max(db, channel.LevelDb - FallDbPerSecond * deltaSeconds);
            // The hold mark keeps the highest level for a while, then falls.
            if (db >= channel.HoldDb)
            {
                channel.HoldDb  = db;
                channel.HoldAge = 0.0f;
            }
            else
            {
                channel.HoldAge += deltaSeconds;
                if (channel.HoldAge > HoldSeconds)
                    channel.HoldDb = std::max(channel.LevelDb, channel.HoldDb - HoldFallDbPerSecond * deltaSeconds);
            }
            if (peak > 1.0f)
            {
                channel.Clip    = true;
                channel.ClipAge = 0.0f;
            }
            else if (channel.Clip && (channel.ClipAge += deltaSeconds) > ClipSeconds)
                channel.Clip = false;
        }
    }

    void MeterBallistics::Reset()
    {
        m_Channels = {};
        m_Count    = 0;
    }

    void MeterBallistics::ClearClip()
    {
        for (auto& channel : m_Channels)
            channel.Clip = false;
    }

    bool MeterBallistics::AnyClip() const
    {
        return std::any_of(m_Channels.begin(), m_Channels.begin() + m_Count, [](const Channel& c) { return c.Clip; });
    }

    // ---------------------------------------------------------------- layout edits

    int FindBus(const AudioBusLayout& layout, const std::string& name)
    {
        for (size_t i = 0; i < layout.Buses.size(); ++i)
            if (layout.Buses[i].Name == name)
                return int(i);
        return -1;
    }

    std::vector<TreeEntry> TreeOrder(const AudioBusLayout& layout)
    {
        std::vector<TreeEntry> order;
        std::vector<bool>      placed(layout.Buses.size());
        const auto             visit = [&](auto& self, size_t index, int depth) -> void {
            if (placed[index])
                return;
            placed[index] = true;
            order.push_back({index, depth});
            for (size_t child = 0; child < layout.Buses.size(); ++child)
                if (!placed[child] && layout.Buses[child].Parent == layout.Buses[index].Name &&
                    layout.Buses[child].Name != MasterAudioBus)
                    self(self, child, depth + 1);
        };
        if (const int master = FindBus(layout, MasterAudioBus); master >= 0)
            visit(visit, size_t(master), 0);
        // Buses whose parent is missing (an invalid file) still get a strip.
        for (size_t i = 0; i < layout.Buses.size(); ++i)
            visit(visit, i, 0);
        return order;
    }

    std::vector<size_t> Subtree(const AudioBusLayout& layout, size_t index)
    {
        std::vector<size_t> result {index};
        for (size_t i = 0; i < result.size(); ++i)
            for (size_t child = 0; child < layout.Buses.size(); ++child)
                if (layout.Buses[child].Parent == layout.Buses[result[i]].Name &&
                    std::find(result.begin(), result.end(), child) == result.end())
                    result.push_back(child);
        return result;
    }

    std::string UniqueBusName(const AudioBusLayout& layout, const std::string& base)
    {
        if (FindBus(layout, base) < 0)
            return base;
        for (int n = 2;; ++n)
            if (auto name = base + " " + std::to_string(n); FindBus(layout, name) < 0)
                return name;
    }

    std::optional<std::string> RenameBus(AudioBusLayout& layout, size_t index, const std::string& name)
    {
        if (index >= layout.Buses.size())
            return "Unknown bus";
        const std::string old = layout.Buses[index].Name;
        if (name == old)
            return std::nullopt;
        if (old == MasterAudioBus)
            return "The Master bus cannot be renamed";
        if (name.empty() || name.find_first_not_of(' ') == std::string::npos)
            return "A bus needs a name";
        if (FindBus(layout, name) >= 0)
            return "Another bus is already named " + name;
        auto next              = layout;
        next.Buses[index].Name = name;
        for (auto& bus : next.Buses)
        {
            if (bus.Parent == old)
                bus.Parent = name;
            for (auto& send : bus.Sends)
                if (send.Target == old)
                    send.Target = name;
            for (auto& effect : bus.Effects)
                if (auto* ducking = std::get_if<AudioDucking>(&effect.Parameters); ducking && ducking->SourceBus == old)
                    ducking->SourceBus = name;
        }
        if (auto error = ValidateAudioBusLayout(next))
            return error->Message;
        layout = std::move(next);
        return std::nullopt;
    }

    std::optional<std::string> SetParent(AudioBusLayout& layout, size_t index, const std::string& parent)
    {
        if (index >= layout.Buses.size())
            return "Unknown bus";
        if (layout.Buses[index].Name == MasterAudioBus)
            return "The Master bus has no parent";
        if (FindBus(layout, parent) < 0)
            return "Unknown parent bus " + parent;
        auto next                = layout;
        next.Buses[index].Parent = parent;
        if (auto error = ValidateAudioBusLayout(next))
            return error->Message;
        layout = std::move(next);
        return std::nullopt;
    }

    size_t AddBus(AudioBusLayout& layout, const std::string& parent)
    {
        AudioBusDefinition bus;
        bus.Name   = UniqueBusName(layout, "Bus");
        bus.Parent = FindBus(layout, parent) >= 0 ? parent : std::string(MasterAudioBus);
        layout.Buses.push_back(std::move(bus));
        return layout.Buses.size() - 1;
    }

    std::optional<std::string> RemoveBus(AudioBusLayout& layout, size_t index)
    {
        if (index >= layout.Buses.size())
            return "Unknown bus";
        const auto removed = layout.Buses[index];
        if (removed.Name == MasterAudioBus)
            return "The Master bus cannot be deleted";
        auto next = layout;
        next.Buses.erase(next.Buses.begin() + std::ptrdiff_t(index));
        for (auto& bus : next.Buses)
        {
            if (bus.Parent == removed.Name)
                bus.Parent = removed.Parent;
            bus.Sends.erase(std::remove_if(bus.Sends.begin(),
                                           bus.Sends.end(),
                                           [&](const AudioBusSend& send) { return send.Target == removed.Name; }),
                            bus.Sends.end());
            for (auto& effect : bus.Effects)
                if (auto* ducking = std::get_if<AudioDucking>(&effect.Parameters);
                    ducking && ducking->SourceBus == removed.Name)
                    ducking->SourceBus = MasterAudioBus;
        }
        if (auto error = ValidateAudioBusLayout(next))
            return error->Message;
        layout = std::move(next);
        return std::nullopt;
    }

    std::optional<std::string> MoveEffect(AudioBusLayout& layout, size_t bus, size_t from, size_t to)
    {
        if (bus >= layout.Buses.size())
            return "Unknown bus";
        auto& effects = layout.Buses[bus].Effects;
        if (from >= effects.size() || to >= effects.size())
            return "Effect index out of range";
        if (from == to)
            return std::nullopt;
        auto effect = std::move(effects[from]);
        effects.erase(effects.begin() + std::ptrdiff_t(from));
        effects.insert(effects.begin() + std::ptrdiff_t(to), std::move(effect));
        return std::nullopt;
    }

    AudioBusEffect MakeEffect(size_t type, const std::string& customName)
    {
        constexpr size_t count = std::variant_size_v<AudioBusEffectParameters>;
        AudioBusEffect   effect;
        effect.Parameters = DefaultParameters(std::min(type, count - 1), std::make_index_sequence<count> {});
        if (auto* ducking = std::get_if<AudioDucking>(&effect.Parameters))
            ducking->SourceBus = MasterAudioBus;
        if (auto* custom = std::get_if<AudioCustomDSP>(&effect.Parameters))
            custom->Name = customName.empty() ? std::string("Custom") : customName;
        return effect;
    }

    // ---------------------------------------------------------------- document

    std::string ToText(const AudioBusLayout& layout)
    {
        auto text = SerializeAudioBusLayout(layout);
        return std::holds_alternative<std::string>(text) ? std::get<std::string>(std::move(text)) : std::string();
    }

    std::string BusPlayStatus(const AudioEngine& engine, const std::string& bus)
    {
        if (engine.IsBusPaused(bus))
            return "Paused";
        const float scale = engine.GetTimeScale();
        for (const auto& definition : engine.GetBuses())
            if (definition.Name == bus && definition.FollowTimeScale && scale != 1.0f)
            {
                char text[48];
                std::snprintf(text, sizeof(text), "Time scale %.2fx", double(scale));
                return text;
            }
        return {};
    }

    void MixerDocument::Reset(AudioBusLayout layout, bool saved)
    {
        m_Undo.clear();
        m_Redo.clear();
        m_GestureOpen = false;
        Set(std::move(layout));
        m_SavedText = saved ? m_Text : std::string();
        (void)ApplyToEngine();
    }

    void MixerDocument::Set(AudioBusLayout layout)
    {
        m_Layout = std::move(layout);
        m_Text   = ToText(m_Layout);
        ++m_Revision;
    }

    std::optional<std::string> MixerDocument::Commit(AudioBusLayout next, bool continuous)
    {
        if (auto error = ValidateAudioBusLayout(next))
            return error->Message;
        auto text = ToText(next);
        if (text == m_Text)
            return std::nullopt;
        if (!continuous || !m_GestureOpen)
        {
            m_Undo.push_back(m_Layout);
            if (m_Undo.size() > MaxHistory)
                m_Undo.erase(m_Undo.begin());
            m_Redo.clear();
        }
        m_GestureOpen = continuous;
        m_Layout      = std::move(next);
        m_Text        = std::move(text);
        ++m_Revision;
        return ApplyToEngine();
    }

    bool MixerDocument::Undo()
    {
        if (m_Undo.empty())
            return false;
        m_GestureOpen = false;
        m_Redo.push_back(std::move(m_Layout));
        Set(std::move(m_Undo.back()));
        m_Undo.pop_back();
        (void)ApplyToEngine();
        return true;
    }

    bool MixerDocument::Redo()
    {
        if (m_Redo.empty())
            return false;
        m_GestureOpen = false;
        m_Undo.push_back(std::move(m_Layout));
        Set(std::move(m_Redo.back()));
        m_Redo.pop_back();
        (void)ApplyToEngine();
        return true;
    }

    std::optional<std::string> MixerDocument::ApplyToEngine() const
    {
        if (!m_Engine || !m_Engine->IsInitialized() || m_Layout.Buses.empty())
            return std::nullopt;
        return m_Engine->ApplyBusLayout(m_Layout);
    }
} // namespace Kans::AudioMixerEditing
