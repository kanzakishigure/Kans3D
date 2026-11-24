#pragma once
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Core/UUID.h"

namespace Kans
{
    enum class VideoClockSource : uint8_t
    {
        Game,
        Timeline,
        AudioSource
    };
    enum class VideoEndBehavior : uint8_t
    {
        Hold,
        Clear
    };

    struct VideoPlayerComponent
    {
        AssetID          Clip, Target;
        bool             Loop        = false;
        double           Speed       = 1;
        bool             PlayOnStart = true;
        VideoEndBehavior EndBehavior = VideoEndBehavior::Hold;
        VideoClockSource ClockSource = VideoClockSource::Game;
        UUID             ClockEntity = 0; // Zero selects this entity.
        double           Offset      = 0;

        bool operator==(const VideoPlayerComponent& other) const;
    };

    struct VideoPlaybackTime
    {
        double Seconds = 0;
        bool   Ended   = false;
    };

    // Game time is already scaled by Speed. External sources supply absolute seconds.
    VideoPlaybackTime MapVideoTime(const VideoPlayerComponent&, double clockSeconds, double duration);

    struct VideoPlayerStats
    {
        unsigned ActivePlayers = 0, RingFill = 0, RingCapacity = 0;
        uint64_t DroppedFrames = 0;
        double   DecodedFPS = 0, UploadMilliseconds = 0;
    };
} // namespace Kans
