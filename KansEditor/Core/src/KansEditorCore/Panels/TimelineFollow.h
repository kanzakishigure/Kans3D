#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Kans3D/Core/UUID.h"
#include "Kans3D/Timeline/Timeline.h"

// What the Timeline window shows during Play or Simulate: which entity's running player it
// follows, that player's time and fired events, and how the event markers look. No UI.
namespace Kans::TimelineFollow
{
    struct Target
    {
        UUID        Entity = 0;
        std::string Name;
    };
    // Entities of `scene`, in hierarchy order (depth first), whose Timeline Player plays
    // `timeline` with a live player.
    std::vector<Target> FindTargets(const Scene& scene, AssetID timeline);

    enum class Mode
    {
        Editing,    // Not in Play: the window edits.
        NotPlaying, // In Play, but no entity plays this timeline: read-only.
        Following   // In Play, tracking a running player: read-only.
    };
    enum class Marker
    {
        Pending, // Not fired in this pass: hollow.
        Fired,   // Fired: filled.
        Flash    // Fired within FlashSeconds: filled, with a flash.
    };
    inline constexpr double FlashSeconds = 0.4;

    // The marker of event `index`: fired when it is in `fired`, flashing while `now` is within
    // FlashSeconds of `firedAt[index]`.
    Marker EventMarker(size_t index, const std::set<size_t>& fired, const std::map<size_t, double>& firedAt, double now);

    // The scroll (the time at the left edge) that keeps `time` in a view `viewSeconds` wide: it is
    // kept while the time is in view, otherwise the time is placed a tenth in from the left.
    double FollowScroll(double scroll, double viewSeconds, double time);

    class Model
    {
    public:
        // Call once a frame. `now` is a clock in seconds, for the event flashes.
        void Update(const Scene* scene, AssetID timeline, bool playing, double now);
        Mode                       GetMode() const { return m_Mode; }
        bool                       ReadOnly() const { return m_Mode != Mode::Editing; }
        const std::vector<Target>& Targets() const { return m_Targets; }
        // The followed entity (0 unless Following) and its name.
        UUID               Followed() const { return m_Followed; }
        const std::string& FollowedName() const;
        // Picks another of Targets() to follow from the next Update on.
        void Follow(UUID entity) { m_Choice = entity; }
        // The followed player's time and fired events at the last Update.
        double                  Time() const { return m_Time; }
        bool                    Looping() const { return m_Loop; }
        const std::set<size_t>& Fired() const { return m_Fired; }
        Marker                  EventState(size_t index) const { return EventMarker(index, m_Fired, m_FiredAt, m_Now); }

    private:
        Mode                     m_Mode = Mode::Editing;
        std::vector<Target>      m_Targets;
        UUID                     m_Followed = 0, m_Choice = 0;
        double                   m_Time = 0, m_Now = 0;
        bool                     m_Loop = false;
        std::set<size_t>         m_Fired;
        std::map<size_t, double> m_FiredAt;
    };
} // namespace Kans::TimelineFollow
