#pragma once

#include <random>
#include <set>
#include "MusicSet.h"

namespace Kans
{
    struct MusicSetClip
    {
        AudioSoundID Sound  = 0;
        float        Volume = 1;
    };

    struct MusicSetPosition
    {
        std::string_view State, Segment;
        double           Seconds = 0, Beat = 0;
        uint64_t         Bar       = 0;
        double           BeatInBar = 0;
    };

    // A transition (or the next playlist segment) scheduled but not heard yet: the state and segment that
    // start, and where in the playing segment's clip (seconds) they take over.
    struct MusicSetPendingTransition
    {
        std::string_view State, Segment;
        double           Seconds = 0;
        uint64_t         Frame   = 0;
    };

    enum class MusicSetEventType
    {
        Beat,
        Bar,
        Cue,
        StateChanged
    };
    struct MusicSetEvent
    {
        MusicSetEventType Type;
        uint64_t          Frame;
        std::string_view  Name;
        uint64_t          Bar  = 0;
        uint32_t          Beat = 0;
    };

    // Main-thread transport. The engine and registered clips must outlive the player.
    class MusicSetPlayer
    {
    public:
        using ClipResolver  = std::function<MusicSetClip(AssetID)>;
        using EventCallback = std::function<void(const MusicSetEvent&)>;

        explicit MusicSetPlayer(AudioEngine& engine, uint32_t seed = 0);
        ~MusicSetPlayer();
        MusicSetPlayer(const MusicSetPlayer&)            = delete;
        MusicSetPlayer& operator=(const MusicSetPlayer&) = delete;

        std::optional<AssetError>       Load(Ref<const MusicSetAsset> asset,
                                             const ClipResolver&      resolve,
                                             std::string_view         busOverride = {},
                                             uint64_t                 entity      = 0);
        bool                            Play(std::string_view state = {}, double fadeSeconds = 0);
        void                            Stop(double fadeSeconds = 0);
        bool                            SetState(std::string_view state);
        bool                            SetParameter(std::string_view name, float value);
        bool                            TriggerStinger(std::string_view name);
        bool                            SeekSeconds(double seconds);
        void                            SetPaused(bool paused);
        void                            Update();
        std::optional<MusicSetPosition> GetPosition() const;
        std::optional<MusicSetPendingTransition> GetPendingTransition() const;
        // The value a parameter plays with (its default until set); none for an unknown name.
        std::optional<float>            GetParameter(std::string_view name) const;
        void                            SetEventCallback(EventCallback callback);
        const MusicSetData*             GetData() const { return m_Asset ? &m_Asset->GetData() : nullptr; }

    private:
        struct Layer
        {
            AudioVoiceHandle Voice;
            uint32_t         Rate       = 0;
            size_t           Parameter  = SIZE_MAX;
            float            ClipVolume = 1;
        };
        struct Bank
        {
            std::vector<Layer> Layers;
            bool               Loop = false;
            bool               Busy = false;
        };
        struct Segment
        {
            std::array<Bank, 4> Banks;
            Bank                Tail;
            std::vector<double> Cues;
            double              Entry = 0, Exit = 0, Length = 0;
            std::string         Bus;
        };
        struct Playlist
        {
            std::vector<size_t> Segments, Bag;
            size_t              Cursor = 0, Last = SIZE_MAX;
        };
        struct Stinger
        {
            AudioVoiceHandle Voice;
            std::string      Bus;
            double           Length = 0;
            uint64_t         Start = UINT64_MAX, End = 0, Next = 0;
            bool             Started = false;
        };
        struct Run
        {
            size_t   SegmentIndex, StateIndex;
            Bank*    Voices;
            uint64_t Start, Sync, End = UINT64_MAX, Release = UINT64_MAX;
            uint64_t NextEvent = 0;
            double   Offset = 0, Rate = 1;
            bool     Via = false, StateChange = false, Started = false, Transition = false, Cut = false;
        };

        void       Clear();
        bool       BadName(std::string_view kind, std::string_view name);
        float      Gain(size_t segment, size_t layer, const Layer& prepared) const;
        size_t     Choose(size_t state, size_t previous);
        Bank*      FreeBank(size_t segment, bool loop);
        bool       StartRun(size_t           segment,
                            size_t           state,
                            uint64_t         sync,
                            double           offset,
                            bool             loop,
                            const MusicFade& fade,
                            bool             via         = false,
                            bool             stateChange = false,
                            Bank*            tail        = nullptr);
        void       EndRun(Run& run, uint64_t sync, const MusicFade& fade);
        Run*       At(double clock);
        const Run* At(double clock) const;
        uint64_t   Earliest() const;
        uint64_t   FrameAt(const Run& run, double seconds) const;
        double     SecondsAt(const Run& run, double clock) const;
        double     WrappedAt(const Run& run, double clock) const;
        double     Heard() const;
        uint64_t   FindSync(const Run& run, const MusicSyncPoint& point, uint64_t earliest) const;
        double     EntryPoint(size_t segment, const MusicTransition& rule, double relative) const;
        void       Dispatch(Run& run, uint64_t heard);
        void       Reap(uint64_t heard);
        void       RefreshTransport();

        AudioEngine&                         m_Engine;
        Ref<const MusicSetAsset>             m_Asset;
        std::vector<Segment>                 m_Segments;
        std::vector<float>                   m_Parameters;
        std::vector<Playlist>                m_Playlists;
        std::vector<Stinger>                 m_Stingers;
        std::vector<Run>                     m_Runs;
        std::mt19937                         m_Random;
        std::set<std::string, std::less<>>   m_Warnings;
        std::shared_ptr<const EventCallback> m_Callback;
        std::optional<uint64_t>              m_PauseFrame;
        double                               m_SecondsPerFrame = 0;
        bool                                 m_UserPaused      = false;
        size_t                               m_RequestedState  = SIZE_MAX;
        uint64_t                             m_Revision        = 0;
        bool                                 m_Updating        = false;
    };
} // namespace Kans
