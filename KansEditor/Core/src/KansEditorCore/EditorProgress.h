#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace Kans
{
    using EditorProgressID = uint64_t;
    enum class EditorProgressCategory
    {
        Import,
        Scripts
    };
    enum class EditorProgressState
    {
        Running,
        Succeeded,
        Failed,
        Cancelled
    };
    struct EditorProgressTask
    {
        EditorProgressID                                     ID;
        EditorProgressCategory                               Category;
        std::string                                          Title, Detail;
        uint64_t                                             Completed = 0, Total = 0; // Total == 0: indeterminate.
        std::optional<double>                                ItemFraction;
        EditorProgressState                                  State = EditorProgressState::Running;
        std::string                                          Error;
        bool                                                 CanCancel = false;
        std::chrono::steady_clock::time_point                Started;
        std::optional<std::chrono::steady_clock::time_point> Finished;
    };
    // Editor-thread only. IDs are never reused, including after dismissal.
    class EditorProgress
    {
    public:
        using Clock = std::chrono::steady_clock;
        explicit EditorProgress(Clock::duration retention = std::chrono::seconds(5)) : m_Retention(retention) {}
        EditorProgressID                                      Begin(EditorProgressCategory category,
                                                                    std::string            title,
                                                                    std::function<void()>  cancel = {},
                                                                    Clock::time_point      now    = Clock::now());
        void                                                  Update(EditorProgressID      id,
                                                                     std::string           detail,
                                                                     uint64_t              completed    = 0,
                                                                     uint64_t              total        = 0,
                                                                     std::optional<double> itemFraction = {});
        void                                                  Finish(EditorProgressID    id,
                                                                     EditorProgressState state,
                                                                     std::string         error = {},
                                                                     Clock::time_point   now   = Clock::now());
        bool                                                  Cancel(EditorProgressID id);
        void                                                  Dismiss(EditorProgressID id);
        void                                                  Prune(Clock::time_point now = Clock::now());
        const std::map<EditorProgressID, EditorProgressTask>& Tasks() const { return m_Tasks; }

    private:
        EditorProgressID                                  m_NextID = 1;
        Clock::duration                                   m_Retention;
        std::map<EditorProgressID, EditorProgressTask>    m_Tasks;
        std::map<EditorProgressID, std::function<void()>> m_Cancel;
    };
} // namespace Kans
