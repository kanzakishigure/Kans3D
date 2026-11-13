#include "KansEditorCore/EditorStartup.h"
#include <algorithm>
#include <exception>

namespace Kans
{
    void EditorStartup::Add(std::string label, float weight, Step step)
    {
        m_Stages.push_back({std::move(label), std::max(weight, 0.0f), std::move(step)});
    }

    bool EditorStartup::RunNext()
    {
        if (!Running())
            return false;
        const auto&                stage = m_Stages[m_Next];
        std::optional<std::string> error;
        try
        {
            error = stage.Run ? stage.Run() : std::nullopt;
        }
        catch (const std::exception& exception)
        {
            error = stage.Label + " failed: " + exception.what();
        }
        catch (...)
        {
            error = stage.Label + " failed";
        }
        if (error)
        {
            m_Error = error->empty() ? stage.Label + " failed" : std::move(*error);
            return true; // The failed stage stays current, for its label.
        }
        ++m_Next;
        return true;
    }

    const std::string& EditorStartup::Error() const
    {
        static const std::string none;
        return m_Error ? *m_Error : none;
    }

    const std::string& EditorStartup::CurrentLabel() const
    {
        static const std::string none;
        return m_Next < m_Stages.size() ? m_Stages[m_Next].Label : none;
    }

    float EditorStartup::Progress() const
    {
        float total = 0.0f, done = 0.0f;
        for (size_t i = 0; i < m_Stages.size(); ++i)
        {
            total += m_Stages[i].Weight;
            if (i < m_Next)
                done += m_Stages[i].Weight;
        }
        if (total <= 0.0f)
            return m_Stages.empty() || m_Next == m_Stages.size() ? 1.0f : float(m_Next) / float(m_Stages.size());
        return std::clamp(done / total, 0.0f, 1.0f);
    }
} // namespace Kans
