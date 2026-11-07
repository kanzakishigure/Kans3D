#include "kspch.h"
#include <cmath>
#include "Timeline.h"

namespace Kans
{
    TimelineActiveSections TimelineSectionsAt(const TimelineData& data, double time, bool loop)
    {
        TimelineActiveSections result;
        if (!std::isfinite(time) || !std::isfinite(data.Length) || data.Length < 0)
            return result;
        time               = std::max(0.0, time);
        result.Time        = loop && data.Length > 0 ? std::fmod(time, data.Length) : std::min(time, data.Length);
        const auto collect = [&](const auto& sections) {
            std::vector<size_t> active;
            for (size_t i = 0; i < sections.size(); ++i)
                if (TimelineRange {sections[i].Start, sections[i].End}.Contains(result.Time))
                    active.push_back(i);
            return active;
        };
        for (const auto& track : data.Activations)
            result.Activations.push_back(collect(track.Ranges));
        for (const auto& track : data.Animations)
            result.Animations.push_back(collect(track.Clips));
        for (const auto& track : data.Particles)
            result.Particles.push_back(collect(track.Sections));
        result.CameraCuts = collect(data.CameraCuts);
        return result;
    }
} // namespace Kans
