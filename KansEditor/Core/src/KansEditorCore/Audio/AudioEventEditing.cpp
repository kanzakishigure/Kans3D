#include "AudioEventEditing.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace Kans::AudioEventEditing
{
    namespace
    {
        bool SameRange(AudioEventRange a, AudioEventRange b) { return a.Min == b.Min && a.Max == b.Max; }

        bool SameSpatial(const AudioSpatialParams& a, const AudioSpatialParams& b)
        {
            return a.Attenuation == b.Attenuation && a.MinDistance == b.MinDistance && a.MaxDistance == b.MaxDistance &&
                   a.Rolloff == b.Rolloff && a.DopplerFactor == b.DopplerFactor &&
                   a.ConeInnerAngle == b.ConeInnerAngle && a.ConeOuterAngle == b.ConeOuterAngle &&
                   a.ConeOuterGain == b.ConeOuterGain;
        }
    } // namespace

    // ------------------------------------------------------------------ entries

    std::vector<float> SelectionShares(const AudioEventData& data)
    {
        const auto usable = [](float weight) { return std::isfinite(weight) && weight > 0.0f; };
        double     total  = 0.0;
        for (const auto& entry : data.Entries)
            if (usable(entry.Weight))
                total += entry.Weight;
        std::vector<float> shares(data.Entries.size(), 0.0f);
        if (total > 0.0)
            for (size_t i = 0; i < shares.size(); ++i)
                if (usable(data.Entries[i].Weight))
                    shares[i] = float(100.0 * data.Entries[i].Weight / total);
        return shares;
    }

    size_t AddEntry(AudioEventData& data, AssetID clip)
    {
        AudioEventEntry entry;
        entry.Clip = clip;
        data.Entries.push_back(entry);
        return data.Entries.size() - 1;
    }

    bool RemoveEntry(AudioEventData& data, size_t index)
    {
        if (index >= data.Entries.size())
            return false;
        data.Entries.erase(data.Entries.begin() + std::ptrdiff_t(index));
        return true;
    }

    bool MoveEntry(AudioEventData& data, size_t from, size_t to)
    {
        auto& entries = data.Entries;
        if (from >= entries.size() || to >= entries.size())
            return false;
        if (from == to)
            return true;
        const auto moved = entries[from];
        entries.erase(entries.begin() + std::ptrdiff_t(from));
        entries.insert(entries.begin() + std::ptrdiff_t(to), moved);
        return true;
    }

    uint32_t MaxAvoidRepeatingLast(const AudioEventData& data)
    {
        return data.Entries.size() > 1 ? uint32_t(data.Entries.size() - 1) : 0u;
    }

    // ------------------------------------------------------------------ ranges

    std::optional<std::string> CheckRange(AudioEventRange range, RangeKind kind)
    {
        if (!std::isfinite(range.Min) || !std::isfinite(range.Max))
            return "Both ends must be numbers";
        if (kind == RangeKind::Nonnegative && range.Min < 0.0f)
            return "The start offset cannot be negative";
        if (range.Min > range.Max)
            return "Min must not be greater than max";
        return std::nullopt;
    }

    void RangeField::Sync(AudioEventRange stored, bool editing)
    {
        if (editing)
            return;
        if (!SameRange(stored, m_Stored))
            m_Error.clear(); // Changed elsewhere (undo, another file): the old refusal no longer applies.
        m_Stored = stored;
        m_Shown  = stored;
    }

    std::optional<AudioEventRange> RangeField::Edit(bool upper, float value, RangeKind kind)
    {
        (upper ? m_Shown.Max : m_Shown.Min) = value;
        if (auto error = CheckRange(m_Shown, kind))
        {
            m_Error = *error;
            return std::nullopt;
        }
        m_Error.clear();
        m_Stored = m_Shown;
        return m_Shown;
    }

    // ------------------------------------------------------------------ comparison and text

    bool Equal(const AudioEventData& a, const AudioEventData& b)
    {
        return a.Entries == b.Entries && a.Selection == b.Selection && a.AvoidRepeatingLast == b.AvoidRepeatingLast &&
               a.VolumeDb == b.VolumeDb && a.PitchSemitones == b.PitchSemitones &&
               SameRange(a.VolumeRangeDb, b.VolumeRangeDb) && SameRange(a.PitchRangeSemitones, b.PitchRangeSemitones) &&
               SameRange(a.StartOffsetSeconds, b.StartOffsetSeconds) && a.CooldownSeconds == b.CooldownSeconds &&
               a.InstanceLimit.MaxCount == b.InstanceLimit.MaxCount && a.InstanceLimit.Rule == b.InstanceLimit.Rule &&
               a.InstanceLimit.ReleaseSeconds == b.InstanceLimit.ReleaseSeconds &&
               a.ConcurrencyGroup == b.ConcurrencyGroup && a.Bus == b.Bus && a.Priority == b.Priority &&
               a.Loop == b.Loop && a.Spatial == b.Spatial && a.Spatializer == b.Spatializer &&
               a.SpatialBlend == b.SpatialBlend && a.Occlusion == b.Occlusion &&
               a.AttenuationAsset == b.AttenuationAsset && a.ReverbZoneSend == b.ReverbZoneSend &&
               a.LfeSendDb == b.LfeSendDb && SameSpatial(a.Attenuation, b.Attenuation);
    }

    const char* SelectionName(AudioEventSelection selection)
    {
        switch (selection)
        {
            case AudioEventSelection::Random:
                return "Random";
            case AudioEventSelection::Shuffle:
                return "Shuffle";
            case AudioEventSelection::Sequential:
                return "Sequential";
        }
        return "Unknown";
    }

    const char* RuleName(AudioConcurrencyRule rule)
    {
        switch (rule)
        {
            case AudioConcurrencyRule::StopOldest:
                return "Stop oldest";
            case AudioConcurrencyRule::StopQuietest:
                return "Stop quietest";
            case AudioConcurrencyRule::StopLowestPriority:
                return "Stop lowest priority";
            case AudioConcurrencyRule::RejectNew:
                return "Reject new";
        }
        return "Unknown";
    }

    const char* StatusName(AudioEventTriggerStatus status)
    {
        switch (status)
        {
            case AudioEventTriggerStatus::Started:
                return "Started";
            case AudioEventTriggerStatus::Cooldown:
                return "Cooldown";
            case AudioEventTriggerStatus::InstanceLimit:
                return "Limit";
            case AudioEventTriggerStatus::MissingClip:
                return "Missing clip";
            case AudioEventTriggerStatus::Invalid:
                return "Invalid";
        }
        return "Unknown";
    }

    // ------------------------------------------------------------------ document

    void EventDocument::Reset(AudioEventData data, bool saved)
    {
        m_Undo.clear();
        m_Redo.clear();
        m_GestureOpen = false;
        m_Saved.reset();
        Set(std::move(data));
        if (saved)
            m_Saved = m_Data;
    }

    bool EventDocument::Commit(AudioEventData next, bool continuous)
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

    bool EventDocument::Undo()
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

    bool EventDocument::Redo()
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

    std::optional<std::string> EventDocument::GetError() const
    {
        if (auto error = ValidateAudioEvent(m_Data))
            return error->Message;
        return std::nullopt;
    }

    void EventDocument::Set(AudioEventData data)
    {
        m_Data = std::move(data);
        ++m_Revision;
    }

    // ------------------------------------------------------------------ files

    AudioEventData FromClips(const std::vector<AssetID>& clips)
    {
        AudioEventData data;
        for (const auto& clip : clips)
            AddEntry(data, clip);
        return data;
    }

    std::filesystem::path UniqueEventFile(const std::filesystem::path& directory, const std::string& stem)
    {
        const std::string base = stem.empty() ? std::string("NewAudioEvent") : stem;
        auto              file = directory / std::filesystem::u8path(base + ".audioevent");
        std::error_code   error;
        for (int i = 2; std::filesystem::exists(file, error); ++i)
            file = directory / std::filesystem::u8path(base + " " + std::to_string(i) + ".audioevent");
        return file;
    }

    std::optional<AssetError> SaveEventFile(const std::filesystem::path& file, const AudioEventData& data)
    {
        auto text = SerializeAudioEvent(data);
        if (const auto* error = std::get_if<AssetError>(&text))
            return *error;
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (!(stream << std::get<std::string>(text)) || !stream.flush())
            return AssetError {AssetErrorCode::IoError, "Cannot write " + file.u8string()};
        return std::nullopt;
    }

    std::variant<std::filesystem::path, AssetError>
    CreateEventFile(const std::filesystem::path& directory, const std::string& stem, const AudioEventData& data)
    {
        const auto file = UniqueEventFile(directory, stem);
        if (auto error = SaveEventFile(file, data))
            return *error;
        return file;
    }

    std::variant<AudioEventData, AssetError> LoadEventFile(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            return AssetError {AssetErrorCode::IoError, "Cannot read " + file.filename().u8string()};
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return ParseAudioEvent(text);
    }

    // ------------------------------------------------------------------ statistics

    StatsRows VoiceStatsRows(const AudioVoiceStats& stats)
    {
        StatsRows rows;
        rows.reserve(5 + stats.GroupActiveCounts.size());
        rows.emplace_back("Real voices", std::to_string(stats.RealVoices));
        rows.emplace_back("Virtual voices", std::to_string(stats.VirtualVoices));
        rows.emplace_back("Allocated voices", std::to_string(stats.AllocatedVoices));
        rows.emplace_back("Steals", std::to_string(stats.StolenThisFrame) + " / " + std::to_string(stats.TotalStolen));
        rows.emplace_back("Rejected starts", std::to_string(stats.RejectedStarts));
        for (const auto& [group, count] : stats.GroupActiveCounts)
            rows.emplace_back("Group " + group, std::to_string(count));
        return rows;
    }

    std::string EventStatsText(const AudioEventStats& stats)
    {
        char text[128];
        std::snprintf(text,
                      sizeof(text),
                      "%u active, %llu triggers, %llu cooldown, %llu limit",
                      stats.ActiveInstances,
                      static_cast<unsigned long long>(stats.TotalTriggers),
                      static_cast<unsigned long long>(stats.CooldownRejections),
                      static_cast<unsigned long long>(stats.LimitRejections));
        return text;
    }
} // namespace Kans::AudioEventEditing
