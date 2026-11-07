#pragma once
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <spdlog/sinks/base_sink.h>

namespace Kans
{
    // Collects log messages from any thread for the console panel.
    class ConsoleSink final : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        enum class Severity
        {
            Trace,
            Info,
            Warning,
            Error,
        };
        struct Message
        {
            Severity    Level;
            std::string Time;
            std::string Logger;
            std::string Text;
        };
        static constexpr size_t Capacity = 5000;

        // Runs `visit(messages, counts)` under the sink lock.
        template<typename Visit>
        void Read(Visit&& visit)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            visit(m_Messages, m_Counts);
        }
        void Clear();

    protected:
        void sink_it_(const spdlog::details::log_msg& message) override;
        void flush_() override {}

    private:
        std::deque<Message> m_Messages;
        size_t              m_Counts[4] = {};
    };

    // Shows the engine and editor logs with severity filters and search.
    class ConsolePanel
    {
    public:
        // Adds the console sink to the engine and client loggers.
        void Attach();
        void Detach();
        void Draw();
        // Errors logged since the console was last cleared.
        size_t ErrorCount();

    private:
        std::shared_ptr<ConsoleSink> m_Sink;
        bool                         m_Show[4]    = {true, true, true, true};
        bool                         m_AutoScroll = true;
        char                         m_Filter[256] {};
        size_t                       m_LastCount = 0;
    };
} // namespace Kans
