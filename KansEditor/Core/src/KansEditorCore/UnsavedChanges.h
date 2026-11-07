#pragma once
#include <functional>
#include <string>

namespace Kans
{
    // Holds an action that would lose unsaved scene changes (new scene, open, exit...)
    // until the user chooses to save, discard or cancel.
    class UnsavedChanges
    {
    public:
        enum class Choice
        {
            Save,
            Discard,
            Cancel
        };
        enum class SaveResult
        {
            Saved,
            Failed,
            NeedsPath // A Save As dialog has opened; call SavedAs() or SaveCancelled() later.
        };

        // Runs `action` now when nothing is unsaved; otherwise keeps it for Resolve().
        // A newer request replaces one still waiting.
        void Request(bool modified, std::string what, std::function<void()> action);
        // A request waits for the user's choice.
        bool Asking() const { return m_Asking; }
        // What the action does ("creating a new scene"), for the prompt.
        const std::string& What() const { return m_What; }

        void Resolve(Choice choice, const std::function<SaveResult()>& save);
        // The Save As dialog finished: continue the action, or drop it.
        void SavedAs();
        void SaveCancelled();
        bool WaitingForSaveAs() const { return bool(m_AfterSave); }

    private:
        std::function<void()> m_Action, m_AfterSave;
        std::string           m_What;
        bool                  m_Asking = false;
    };
} // namespace Kans
