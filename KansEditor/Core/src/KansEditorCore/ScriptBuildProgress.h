#pragma once
#include <atomic>
#include "Kans3D/Script/ScriptBuild.h"
#include "EditorProgress.h"

namespace Kans
{
    class ScriptBuildProgress
    {
    public:
        // Begin/Poll/Finish: editor thread. One build at a time per adapter.
        void Begin(EditorProgress& progress);
        void Poll(EditorProgress& progress);
        void Finish(EditorProgress& progress, const ScriptBuild::Result& result);
        // Build thread only. A bounded hand-off; no worker accesses the registry.
        void OutputLine(std::string_view line);

    private:
        std::atomic<ScriptBuild::Step>  m_Step {ScriptBuild::Step::Restoring};
        std::optional<EditorProgressID> m_Task;
    };
} // namespace Kans
