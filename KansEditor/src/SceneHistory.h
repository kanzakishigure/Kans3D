#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/Scene/SceneSnapshot.h"

namespace Kans
{
    // Undo/redo for scene edits as a list of whole-scene snapshots. The editor
    // records the scene after each finished interaction; a record that changed
    // nothing is ignored. Steps are named after what changed.
    class SceneHistory
    {
    public:
        static constexpr size_t Limit = 100;

        // Starts over from the scene's current state, which counts as saved.
        void Reset(const Scene& scene);
        // Adds a step when the scene differs from the last recorded state.
        bool Record(const Scene& scene);
        bool Undo(Scene& scene);
        bool Redo(Scene& scene);

        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        // Names of the steps, most recent first.
        std::vector<std::string> UndoLabels() const;
        std::vector<std::string> RedoLabels() const;

        // When the step Undo (Redo) would apply was recorded (NextEditStamp); nothing without one.
        std::optional<uint64_t> UndoStamp() const;
        std::optional<uint64_t> RedoStamp() const;

        void MarkSaved() { m_SavedVersion = m_Version; }
        bool IsModified() const { return m_Version != m_SavedVersion; }

    private:
        struct Step
        {
            SceneSnapshot Before, After;
            uint64_t      BeforeVersion = 0, AfterVersion = 0;
            uint64_t      Stamp = 0;
            std::string   Label;
        };
        std::deque<Step>  m_Undo; // Oldest first.
        std::vector<Step> m_Redo; // Next redo last.
        SceneSnapshot     m_Current;
        uint64_t          m_Version = 0, m_SavedVersion = 0, m_NextVersion = 1;
    };

    // A short name for the change between two snapshots, e.g. "Move Camera".
    std::string DescribeChange(const SceneSnapshot& before, const SceneSnapshot& after);
} // namespace Kans
