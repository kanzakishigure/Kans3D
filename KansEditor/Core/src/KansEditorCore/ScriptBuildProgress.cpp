#include "kspch.h"
#include "ScriptBuildProgress.h"

namespace Kans
{
    void ScriptBuildProgress::Begin(EditorProgress& progress)
    {
        m_Step.store(ScriptBuild::Step::Restoring);
        m_Task = progress.Begin(EditorProgressCategory::Scripts, "Building scripts");
        Poll(progress);
    }
    void ScriptBuildProgress::OutputLine(std::string_view line)
    {
        m_Step.store(ScriptBuild::StepForLine(line, m_Step.load()));
    }
    void ScriptBuildProgress::Poll(EditorProgress& progress)
    {
        static constexpr const char* names[] = {"Restoring", "Compiling", "Linking/Copying", "Done"};
        if (m_Task)
            progress.Update(*m_Task, names[int(m_Step.load())]);
    }
    void ScriptBuildProgress::Finish(EditorProgress& progress, const ScriptBuild::Result& result)
    {
        if (!m_Task)
            return;
        progress.Update(*m_Task, "Done (" + std::to_string(result.ErrorCount) + " errors)");
        progress.Finish(*m_Task,
                        result.Success ? EditorProgressState::Succeeded : EditorProgressState::Failed,
                        result.Success ? "" : result.Output);
        m_Task.reset();
    }
} // namespace Kans
