#include "kspch.h"
#include "ImportProgress.h"

namespace Kans
{
    void ImportProgressReporter::Poll(EditorProgress&              progress,
                                      EditorAssetImportService&    service,
                                      const SceneAssetImportQueue& queue)
    {
        const auto  result   = service.GetSnapshot();
        const auto* snapshot = std::get_if<EditorAssetImportSnapshot>(&result);
        if (!snapshot || !snapshot->Session)
            return;
        const auto& s = *snapshot;
        if (s.Session != m_Session)
        {
            if (m_Terminal)
            {
                m_Task = progress.Begin(
                    EditorProgressCategory::Import, "Importing assets", [&service] { (void)service.Cancel(); });
                m_Done = m_Total = 0;
            }
            else
                m_Done += m_LastCompleted;
            m_Session  = s.Session;
            m_Terminal = false;
        }
        if (m_Terminal || !m_Task)
            return;
        m_LastCompleted    = s.Completed;
        m_Total            = std::max(m_Total, m_Done + s.Total + queue.PendingCount());
        std::string detail = s.SourcePath;
        if (s.State == EditorAssetImportState::Building)
        {
            static constexpr const char* phases[] = {
                "validating products", "writing products", "committing products", "committed"};
            if (!detail.empty())
                detail += ": ";
            detail += s.Phase ? phases[int(*s.Phase)] : "building products";
        }
        progress.Update(*m_Task, std::move(detail), m_Done + s.Completed, m_Total, s.ItemFraction);
        if (s.State == EditorAssetImportState::Building)
            return;
        EditorProgressState state = EditorProgressState::Succeeded;
        if (s.State == EditorAssetImportState::Failed || s.SessionError)
            state = EditorProgressState::Failed;
        else if (s.State == EditorAssetImportState::Cancelled)
            state = EditorProgressState::Cancelled;
        else if (!queue.Empty())
            return;
        // Queued roots can become current as dependencies of an earlier root;
        // StartNext consumes those without starting another service session.
        if (state == EditorProgressState::Succeeded)
            progress.Update(*m_Task, s.SourcePath, m_Total, m_Total);
        progress.Finish(*m_Task, state, s.Error ? s.Error->Message : s.SessionError ? s.SessionError->Message : "");
        m_Terminal = true;
    }
} // namespace Kans
