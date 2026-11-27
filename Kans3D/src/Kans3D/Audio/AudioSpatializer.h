#pragma once

namespace Kans
{
    enum class AudioSpatializer
    {
        Panning,
        HRTF
    };

    enum class AudioHRTFFallback
    {
        None,
        NonSpatial,
        NonStereoOutput,
        SteamAudioUnavailable,
        EffectCreationFailed
    };
} // namespace Kans
