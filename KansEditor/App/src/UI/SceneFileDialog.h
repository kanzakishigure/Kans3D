#pragma once
#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Kans
{
    // Modal browser for opening and saving .kans scenes inside the project
    // folder: a breadcrumb bar, quick places, a sortable list of folders and
    // scenes, and a file name field. Saving can create folders and asks before
    // replacing a scene.
    class SceneFileDialog
    {
    public:
        enum class Mode
        {
            Open,
            Save
        };
        // Handles the chosen file; returns an error to show, or nothing to close.
        using Submit = std::function<std::optional<std::string>(const std::filesystem::path&)>;

        // Opens the dialog in `root` (navigation stays inside it), showing the
        // folder of `current` and selecting it when it is a file.
        void Open(Mode mode, const std::filesystem::path& root, const std::filesystem::path& current);
        // Call every frame from the window the dialog belongs to.
        void Draw(const Submit& submit);
        Mode GetMode() const { return m_Mode; }
        // Requested or on screen (false again once it is closed or cancelled).
        bool IsOpen() const { return m_OpenRequested || m_Showing; }

    private:
        struct Entry
        {
            std::string           Name;
            std::filesystem::path Path;
            bool                  Directory = false;
            std::uintmax_t        Size      = 0;
            std::time_t           Modified  = 0;
        };

        void Navigate(const std::filesystem::path& directory);
        void Refresh();
        void DrawPathBar();
        void DrawPlaces();
        void DrawEntries(const Submit& submit);
        void DrawFooter(const Submit& submit);
        // Submits the file name field; a folder name opens that folder.
        void Confirm(const Submit& submit);
        void Finish(const Submit& submit, const std::filesystem::path& file);
        void SetName(const std::string& name);

        Mode                  m_Mode = Mode::Open;
        bool                  m_OpenRequested = false;
        bool                  m_Showing       = false;
        std::filesystem::path m_Root, m_Directory;
        std::vector<Entry>    m_Entries;
        std::string           m_Error;
        // Save: the existing scene waiting for the replace confirmation.
        std::filesystem::path m_Replace;
        bool                  m_NewFolder = false, m_FocusFolder = false;
        // Set when the entries changed and need sorting by the table's order.
        bool m_Unsorted = true;
        // Closing is deferred to the dialog's own scope (Finish can run in a nested popup).
        bool m_Close = false;
        // Bytes of the file name to select when the field activates (the name
        // without its extension), or -1.
        int m_NameSelection = -1;

        std::array<char, 256> m_Name {};
        std::array<char, 128> m_Search {};
        std::array<char, 128> m_FolderName {};
    };
} // namespace Kans
