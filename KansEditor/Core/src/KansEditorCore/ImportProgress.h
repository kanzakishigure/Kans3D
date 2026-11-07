#pragma once
#include "EditorProgress.h"
#include "SceneAssetImportQueue.h"

namespace Kans
{
    // Keep one reporter beside the owning service. Call each frame after service.Poll(),
    // before queue.StartNext(), including the frame consuming a terminal snapshot.
    // The service must outlive any cancellable task (or dismiss it before destruction).
    class ImportProgressReporter
    {
    public:
        void Poll(EditorProgress& progress, EditorAssetImportService& service, const SceneAssetImportQueue& queue);
        std::optional<EditorProgressID> Task() const { return m_Task; }

    private:
        std::optional<EditorProgressID> m_Task;
        uint64_t                        m_Session = 0, m_Done = 0, m_LastCompleted = 0, m_Total = 0;
        bool                            m_Terminal = true;
    };
} // namespace Kans
