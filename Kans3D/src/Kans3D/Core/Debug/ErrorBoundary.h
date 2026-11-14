#pragma once
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace Kans
{
    // Native debuggers see the original throw before these catches. Visual Studio does
    // not break on handled C++ exceptions by default: Debug > Windows > Exception
    // Settings > C++ Exceptions > Break When Thrown enables first-chance inspection.
    class ErrorBoundary
    {
    public:
        template<class F>
        bool Try(const std::string& name, F&& work)
        {
            try
            {
                std::forward<F>(work)();
                Recovered(name);
                return true;
            }
            catch (...)
            {
                Failed(name, std::current_exception());
                return false;
            }
        }
        void               Failed(const std::string& name, std::exception_ptr exception);
        void               Recovered(const std::string& name);
        const std::string& Message() const { return m_Message; }
        uint64_t           Count() const { return m_Count; }
        std::exception_ptr Exception() const { return m_Exception; }

    private:
        std::string        m_Message, m_Name;
        uint64_t           m_Count = 0;
        std::exception_ptr m_Exception;
    };

    // Call once per application frame, combining update and UI failures for a layer.
    class ConsecutiveFrameFailures
    {
    public:
        bool EndFrame(bool failed);

    private:
        unsigned m_Count = 0;
    };
} // namespace Kans
