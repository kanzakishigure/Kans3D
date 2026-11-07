#pragma once
#include <chrono>
#include <optional>
#include <string>
#include "KansEditorCore/EditorProgress.h"

// What the progress popup shows and when, without UI: the text of a task row, the short
// status-bar line, when the popup is open, and where it sits.
namespace Kans::ProgressUI
{
    using Clock = std::chrono::steady_clock;
    using Tasks = std::map<EditorProgressID, EditorProgressTask>;

    // The popup closes this long after the last task finished, unless one failed.
    inline constexpr Clock::duration AutoCloseDelay = std::chrono::seconds(3);

    struct RowText
    {
        std::string          Count;    // "3 of 12"; empty while the total is unknown.
        std::optional<float> Fraction; // 0..1; empty for an indeterminate bar.
        std::string          Elapsed;  // "0:07", "1:02:03".
    };
    // Running tasks count their elapsed time up to `now`, finished ones up to when they ended.
    RowText     FormatRow(const EditorProgressTask& task, Clock::time_point now);
    std::string FormatElapsed(Clock::duration elapsed);

    // One line for a status bar: the first running task's title, its detail and count, and how
    // many others run. Empty when nothing runs.
    std::string StatusText(const Tasks& tasks);

    class Model
    {
    public:
        // Call once a frame with the registry's tasks.
        void Update(const Tasks& tasks, Clock::time_point now);
        // Open while a task runs or a failure is undismissed, and for AutoCloseDelay after the
        // last task finished.
        bool   Visible() const { return m_Visible; }
        bool   Collapsed() const { return m_Collapsed; }
        void   ToggleCollapsed() { m_Collapsed = !m_Collapsed; }
        size_t Running() const { return m_Running; }
        size_t Failed() const { return m_Failed; }
        // The collapsed pill's text: "2 tasks running", "1 task failed", "Done".
        std::string PillText() const;

    private:
        bool   m_Visible = false, m_Collapsed = false;
        size_t m_Running = 0, m_Failed = 0;
    };

    struct Rect
    {
        float X = 0, Y = 0, Width = 0, Height = 0;
        float Right() const { return X + Width; }
        float Bottom() const { return Y + Height; }
        bool  operator==(const Rect& o) const
        {
            return X == o.X && Y == o.Y && Width == o.Width && Height == o.Height;
        }
    };
    // The popup's rectangle: at the bottom right of `panel` (the Content Browser above its status
    // bar) when it is shown, otherwise of `viewport`, `margin` in from both edges. It never leaves
    // its host: a host narrower or shorter than the popup shrinks it.
    Rect Anchor(const std::optional<Rect>& panel, const Rect& viewport, float width, float height, float margin);
} // namespace Kans::ProgressUI
