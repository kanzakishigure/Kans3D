#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Kans
{
    // Opening a project in the editor, split into labelled stages that run one per frame on
    // the main thread, so the window keeps drawing (the startup splash) and handling events.
    // A stage that returns an error message, or throws, stops the startup: the remaining
    // stages never run and the splash shows the error instead.
    class EditorStartup
    {
    public:
        // Returns an error message to stop the startup.
        using Step = std::function<std::optional<std::string>()>;

        // `weight` is the stage's share of the progress bar (negative counts as none).
        void Add(std::string label, float weight, Step step);

        // Runs the next stage; false when nothing ran (finished or failed).
        bool RunNext();

        bool Running() const { return !Failed() && m_Next < m_Stages.size(); }
        bool Finished() const { return !Failed() && m_Next == m_Stages.size(); }
        bool Failed() const { return m_Error.has_value(); }
        // Why the startup stopped; empty unless Failed().
        const std::string& Error() const;
        // The stage that runs next, or the one that failed; empty when finished.
        const std::string& CurrentLabel() const;
        // The finished stages' share of the total weight, 0 to 1.
        float  Progress() const;
        size_t StageCount() const { return m_Stages.size(); }

    private:
        struct Stage
        {
            std::string Label;
            float       Weight = 0.0f;
            Step        Run;
        };
        std::vector<Stage>         m_Stages;
        size_t                     m_Next = 0;
        std::optional<std::string> m_Error;
    };
} // namespace Kans
