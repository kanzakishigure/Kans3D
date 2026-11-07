#include "kspch.h"
#include "EditorProgress.h"
#include <cmath>

namespace Kans
{
    EditorProgressID EditorProgress::Begin(EditorProgressCategory category,
                                           std::string            title,
                                           std::function<void()>  cancel,
                                           Clock::time_point      now)
    {
        const auto         id = m_NextID++;
        EditorProgressTask task;
        task.ID        = id;
        task.Category  = category;
        task.Title     = std::move(title);
        task.CanCancel = bool(cancel);
        task.Started   = now;
        m_Tasks.emplace(id, std::move(task));
        if (cancel)
            m_Cancel.emplace(id, std::move(cancel));
        return id;
    }
    void EditorProgress::Update(EditorProgressID      id,
                                std::string           detail,
                                uint64_t              completed,
                                uint64_t              total,
                                std::optional<double> fraction)
    {
        auto found = m_Tasks.find(id);
        if (found == m_Tasks.end() || found->second.State != EditorProgressState::Running)
            return;
        auto& task        = found->second;
        task.Detail       = std::move(detail);
        task.Completed    = total ? std::min(completed, total) : completed;
        task.Total        = total;
        task.ItemFraction = fraction && std::isfinite(*fraction) ?
                                std::optional<double>(std::clamp(*fraction, 0.0, 1.0)) :
                                std::nullopt;
    }
    void
    EditorProgress::Finish(EditorProgressID id, EditorProgressState state, std::string error, Clock::time_point now)
    {
        auto found = m_Tasks.find(id);
        if (found == m_Tasks.end() || found->second.State != EditorProgressState::Running ||
            state == EditorProgressState::Running)
            return;
        auto& task     = found->second;
        task.State     = state;
        task.Error     = std::move(error);
        task.Finished  = now;
        task.CanCancel = false;
        m_Cancel.erase(id);
    }
    bool EditorProgress::Cancel(EditorProgressID id)
    {
        auto found = m_Cancel.find(id);
        if (found == m_Cancel.end())
            return false;
        auto cancel = found->second;
        m_Cancel.erase(found);
        m_Tasks.at(id).CanCancel = false;
        cancel(); // The owning service reports the eventual result through its poll.
        return true;
    }
    void EditorProgress::Dismiss(EditorProgressID id)
    {
        m_Cancel.erase(id);
        m_Tasks.erase(id);
    }
    void EditorProgress::Prune(Clock::time_point now)
    {
        for (auto it = m_Tasks.begin(); it != m_Tasks.end();)
            if (it->second.Finished && it->second.State != EditorProgressState::Failed &&
                now - *it->second.Finished >= m_Retention)
                it = m_Tasks.erase(it);
            else
                ++it;
    }
} // namespace Kans
