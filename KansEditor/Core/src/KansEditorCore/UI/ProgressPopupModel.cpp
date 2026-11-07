#include "kspch.h"
#include "ProgressPopupModel.h"
#include <algorithm>
#include <cstdio>

namespace Kans::ProgressUI
{
    std::string FormatElapsed(Clock::duration elapsed)
    {
        const long long total = std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        char            text[32];
        if (total >= 3600)
            std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", total / 3600, total / 60 % 60, total % 60);
        else
            std::snprintf(text, sizeof(text), "%lld:%02lld", total / 60, total % 60);
        return text;
    }

    RowText FormatRow(const EditorProgressTask& task, Clock::time_point now)
    {
        RowText row;
        if (task.Total > 0)
        {
            row.Count = std::to_string(task.Completed) + " of " + std::to_string(task.Total);
            // The current item's own progress fills the part of the bar it stands for.
            double done = double(task.Completed);
            if (task.Completed < task.Total && task.ItemFraction)
                done += *task.ItemFraction;
            row.Fraction = float(std::clamp(done / double(task.Total), 0.0, 1.0));
        }
        else if (task.ItemFraction)
            row.Fraction = float(*task.ItemFraction);
        if (task.State == EditorProgressState::Succeeded)
            row.Fraction = 1.0f;
        row.Elapsed = FormatElapsed((task.Finished ? *task.Finished : now) - task.Started);
        return row;
    }

    std::string StatusText(const Tasks& tasks)
    {
        const EditorProgressTask* first  = nullptr;
        size_t                    others = 0;
        for (const auto& [id, task] : tasks)
            if (task.State == EditorProgressState::Running)
            {
                if (first)
                    ++others;
                else
                    first = &task;
            }
        if (!first)
            return {};
        std::string text = first->Title;
        if (!first->Detail.empty())
            text += ": " + first->Detail;
        if (first->Total > 0)
            text += " (" + std::to_string(first->Completed) + " of " + std::to_string(first->Total) + ")";
        if (others)
            text += "  +" + std::to_string(others) + " more";
        return text;
    }

    void Model::Update(const Tasks& tasks, Clock::time_point now)
    {
        m_Running = m_Failed = 0;
        std::optional<Clock::time_point> lastFinished;
        for (const auto& [id, task] : tasks)
        {
            if (task.State == EditorProgressState::Running)
                ++m_Running;
            else if (task.State == EditorProgressState::Failed)
                ++m_Failed;
            if (task.Finished && (!lastFinished || *task.Finished > *lastFinished))
                lastFinished = task.Finished;
        }
        m_Visible = m_Running || m_Failed || (lastFinished && now - *lastFinished < AutoCloseDelay);
        if (!m_Visible)
            m_Collapsed = false; // The next task opens the popup in full.
    }

    std::string Model::PillText() const
    {
        if (m_Running)
            return std::to_string(m_Running) + (m_Running == 1 ? " task running" : " tasks running");
        if (m_Failed)
            return std::to_string(m_Failed) + (m_Failed == 1 ? " task failed" : " tasks failed");
        return "Done";
    }

    Rect Anchor(const std::optional<Rect>& panel, const Rect& viewport, float width, float height, float margin)
    {
        const Rect& host = panel && panel->Width > 0 && panel->Height > 0 ? *panel : viewport;
        Rect        r;
        r.Width  = std::max(0.0f, std::min(width, host.Width - margin * 2.0f));
        r.Height = std::max(0.0f, std::min(height, host.Height - margin * 2.0f));
        r.X      = host.Right() - margin - r.Width;
        r.Y      = host.Bottom() - margin - r.Height;
        return r;
    }
} // namespace Kans::ProgressUI
