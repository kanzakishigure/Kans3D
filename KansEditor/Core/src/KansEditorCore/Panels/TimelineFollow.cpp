#include "kspch.h"
#include "TimelineFollow.h"
#include <algorithm>
#include <limits>
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans::TimelineFollow
{
    std::vector<Target> FindTargets(const Scene& scene, AssetID timeline)
    {
        std::vector<Target> targets;
        if (!timeline.IsValid())
            return targets;
        std::vector<Entity> stack = scene.GetRootEntities();
        std::reverse(stack.begin(), stack.end());
        while (!stack.empty())
        {
            Entity entity = stack.back();
            stack.pop_back();
            if (entity.HasComponent<TimelinePlayerComponent>())
                if (const auto& c = entity.GetComponent<TimelinePlayerComponent>(); c.Timeline == timeline && c.Player)
                    targets.push_back({entity.GetUUID(), entity.GetComponent<TagComponent>().Tag});
            auto children = scene.GetChildren(entity);
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
        return targets;
    }

    Marker EventMarker(size_t index, const std::set<size_t>& fired, const std::map<size_t, double>& firedAt, double now)
    {
        if (!fired.count(index))
            return Marker::Pending;
        const auto at = firedAt.find(index);
        return at != firedAt.end() && now >= at->second && now - at->second < FlashSeconds ? Marker::Flash :
                                                                                             Marker::Fired;
    }

    double FollowScroll(double scroll, double viewSeconds, double time)
    {
        if (viewSeconds <= 0)
            return scroll;
        if (time >= scroll && time <= scroll + viewSeconds * 0.95)
            return scroll;
        return time - viewSeconds * 0.1;
    }

    const std::string& Model::FollowedName() const
    {
        static const std::string none;
        for (const auto& target : m_Targets)
            if (target.Entity == m_Followed)
                return target.Name;
        return none;
    }

    void Model::Update(const Scene* scene, AssetID timeline, bool playing, double now)
    {
        m_Now     = now;
        m_Targets = playing && scene ? FindTargets(*scene, timeline) : std::vector<Target> {};
        // The chosen entity while it still plays, otherwise the first one found.
        UUID next = 0;
        for (UUID wanted : {m_Choice, m_Followed})
            if (!next && wanted &&
                std::any_of(m_Targets.begin(), m_Targets.end(), [&](const Target& t) { return t.Entity == wanted; }))
                next = wanted;
        if (!next && !m_Targets.empty())
            next = m_Targets.front().Entity;
        m_Choice = 0;
        if (!next)
        {
            m_Mode     = playing ? Mode::NotPlaying : Mode::Editing;
            m_Followed = 0;
            m_Time     = 0;
            m_Loop     = false;
            m_Fired.clear();
            m_FiredAt.clear();
            return;
        }
        const auto& component = scene->GetEntityByUUID(next).GetComponent<TimelinePlayerComponent>();
        const auto& player    = *component.Player;
        const auto& fired     = player.FiredEventIndices();
        if (m_Followed && m_Followed != next)
        {
            // Another entity: what it fired before is not news.
            m_FiredAt.clear();
            for (size_t i : fired)
                m_FiredAt[i] = -std::numeric_limits<double>::infinity();
        }
        else
        {
            // Newly fired, including events rearmed by a loop that fire again.
            for (size_t i : fired)
                if (!m_Fired.count(i))
                    m_FiredAt[i] = now;
        }
        m_Mode     = Mode::Following;
        m_Followed = next;
        m_Fired    = fired;
        m_Time     = player.Time();
        m_Loop     = component.Loop;
    }
} // namespace Kans::TimelineFollow
