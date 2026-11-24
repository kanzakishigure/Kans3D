#include "kspch.h"
#include "VideoPlayer.h"
#include <cmath>
#include <tuple>

namespace Kans
{
    bool VideoPlayerComponent::operator==(const VideoPlayerComponent& other) const
    {
        return std::tie(Clip, Target, Loop, Speed, PlayOnStart, EndBehavior, ClockSource, ClockEntity, Offset) ==
               std::tie(other.Clip,
                        other.Target,
                        other.Loop,
                        other.Speed,
                        other.PlayOnStart,
                        other.EndBehavior,
                        other.ClockSource,
                        other.ClockEntity,
                        other.Offset);
    }

    VideoPlaybackTime MapVideoTime(const VideoPlayerComponent& player, double clockSeconds, double duration)
    {
        const double seconds = clockSeconds + player.Offset;
        if (!std::isfinite(seconds) || !std::isfinite(duration) || duration <= 0)
            return {};
        if (!player.Loop)
            return {std::clamp(seconds, 0.0, duration), seconds >= duration};
        const double wrapped = std::fmod(seconds, duration);
        return {wrapped < 0 ? wrapped + duration : wrapped, false};
    }
} // namespace Kans
