#include "kspch.h"
#include "ErrorBoundary.h"
#include "Kans3D/Core/Log/Log.h"

namespace Kans
{
    bool ConsecutiveFrameFailures::EndFrame(bool failed)
    {
        m_Count = failed ? m_Count + 1 : 0;
        return m_Count >= 3;
    }
    void ErrorBoundary::Failed(const std::string& name, std::exception_ptr exception)
    {
        std::string message;
        try
        {
            std::rethrow_exception(exception);
        }
        catch (const std::exception& error)
        {
            message = error.what();
        }
        catch (...)
        {
            message = "Unknown C++ exception";
        }
        m_Exception = exception;
        if (m_Count && message == m_Message && name == m_Name)
        {
            ++m_Count;
            return;
        }
        if (m_Count > 1)
            CORE_INFO("{}: previous error occurred {} times", m_Name, m_Count);
        m_Name    = name;
        m_Message = std::move(message);
        m_Count   = 1;
        CORE_ERROR("{}: {}", name, m_Message);
    }
    void ErrorBoundary::Recovered(const std::string& name)
    {
        if (m_Count)
            CORE_INFO("{}: recovered after {} failed attempts", name, m_Count);
        m_Count = 0;
        m_Message.clear();
        m_Exception = {};
    }
} // namespace Kans
