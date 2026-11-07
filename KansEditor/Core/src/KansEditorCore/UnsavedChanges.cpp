#include "KansEditorCore/UnsavedChanges.h"

namespace Kans
{
    void UnsavedChanges::Request(bool modified, std::string what, std::function<void()> action)
    {
        m_AfterSave = {};
        if (!modified)
        {
            m_Asking = false;
            m_Action = {};
            if (action)
                action();
            return;
        }
        m_What   = std::move(what);
        m_Action = std::move(action);
        m_Asking = true;
    }

    void UnsavedChanges::Resolve(Choice choice, const std::function<SaveResult()>& save)
    {
        if (!m_Asking)
            return;
        m_Asking    = false;
        auto action = std::move(m_Action);
        m_Action    = {};
        switch (choice)
        {
            case Choice::Cancel:
                return;
            case Choice::Discard:
                break;
            case Choice::Save:
                switch (save ? save() : SaveResult::Failed)
                {
                    case SaveResult::Saved:
                        break;
                    case SaveResult::Failed:
                        return; // Never lose the changes because saving them failed.
                    case SaveResult::NeedsPath:
                        m_AfterSave = std::move(action);
                        return;
                }
                break;
        }
        if (action)
            action();
    }

    void UnsavedChanges::SavedAs()
    {
        auto action = std::move(m_AfterSave);
        m_AfterSave = {};
        if (action)
            action();
    }

    void UnsavedChanges::SaveCancelled() { m_AfterSave = {}; }
} // namespace Kans
