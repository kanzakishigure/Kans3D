#pragma once

#include <functional>
#include "AudioAttenuation.h"
#include "AudioEngine.h"

namespace Kans
{
    enum class MusicSync
    {
        Immediate,
        NextBeat,
        NextBar,
        NextNBar,
        NextCue,
        ExitCue,
        EndOfSegment
    };
    enum class MusicEntry
    {
        EntryCue,
        SameRelativePosition,
        NamedCue
    };
    enum class MusicPlaylistOrder
    {
        Ordered,
        Shuffled,
        Weighted
    };

    // Clip-relative coordinates. Bar and beat indices are zero-based from FirstBeatSeconds.
    struct MusicTime
    {
        double                  Seconds = 0;
        std::optional<uint32_t> Bar;
        double                  Beat = 0;
    };

    struct MusicCue
    {
        std::string Name;
        MusicTime   Time;
    };

    struct MusicParameter
    {
        std::string Name;
        float       Min = 0, Max = 1, Default = 0, RampSeconds = .02f;
    };

    struct MusicVolumeKey
    {
        float                   Input = 0, VolumeDb = 0;
        AudioCurveInterpolation Interpolation = AudioCurveInterpolation::Linear;
    };

    struct MusicLayer
    {
        AssetID                     Clip;
        std::string                 Bus      = "Music";
        float                       VolumeDb = 0;
        std::string                 Parameter;
        std::vector<MusicVolumeKey> Curve;
    };

    struct MusicSegment
    {
        std::string              Name;
        std::vector<MusicLayer>  Layers;
        double                   BPM = 120, FirstBeatSeconds = 0;
        uint32_t                 BeatsPerBar = 4;
        MusicTime                Entry;
        std::optional<MusicTime> Exit; // The shortest stem when omitted.
        bool                     Loop = false, PreEntry = false, PostExit = false;
        std::vector<MusicCue>    Cues;

        double SecondsAt(const MusicTime& time) const;
    };

    struct MusicPlaylistItem
    {
        std::string Segment;
        float       Weight = 1;
    };

    struct MusicState
    {
        std::string                    Name, Segment;
        std::vector<MusicPlaylistItem> Playlist;
        MusicPlaylistOrder             Order             = MusicPlaylistOrder::Ordered;
        bool                           NoImmediateRepeat = false;
    };

    struct MusicSyncPoint
    {
        MusicSync   Sync = MusicSync::Immediate;
        uint32_t    Bars = 1;
        std::string Cue; // Empty NextCue matches every custom cue.
    };

    struct MusicFade
    {
        double         Seconds = 0;
        AudioFadeCurve Curve   = AudioFadeCurve::Linear;
    };

    struct MusicTransition
    {
        std::string    From = "Any", To = "Any";
        MusicSyncPoint Sync;
        MusicFade      FadeOut, FadeIn;
        MusicEntry     Entry = MusicEntry::EntryCue;
        std::string    Cue, Via;
    };

    struct MusicStinger
    {
        std::string    Name;
        AssetID        Clip;
        std::string    Bus = "Music";
        MusicSyncPoint Sync {MusicSync::NextBeat, 1, {}};
        double         MinimumInterval = 0;
    };

    struct MusicSetData
    {
        std::vector<MusicSegment>    Segments;
        std::vector<MusicParameter>  Parameters;
        std::vector<MusicState>      States;
        std::vector<MusicTransition> Transitions;
        std::vector<MusicStinger>    Stingers;

        const MusicTransition* FindTransition(std::string_view from, std::string_view to) const;
    };

    class MusicSetAsset final : public Asset
    {
    public:
        explicit MusicSetAsset(MusicSetData data) : m_Data(std::move(data)) {}
        static AssetType    GetStaticType() { return AssetType::MusicSet; }
        AssetType           GetAssetType() const override { return GetStaticType(); }
        const MusicSetData& GetData() const { return m_Data; }

    private:
        MusicSetData m_Data;
    };

    // The optional resolver checks clip existence and cue bounds against actual source durations.
    using MusicClipDuration = std::function<std::optional<double>(AssetID)>;
    std::optional<AssetError> ValidateMusicSet(const MusicSetData& data, const MusicClipDuration& duration = {});
    std::variant<MusicSetData, AssetError> ParseMusicSet(const std::string& text);
    std::variant<std::string, AssetError>  SerializeMusicSet(const MusicSetData& data);
} // namespace Kans
