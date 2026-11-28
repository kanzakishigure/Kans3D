#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Kans
{
    // Modal browser over the whole file system for choosing a folder, a project (a .kproj
    // file or the folder holding it), or a file with a given extension.
    class FolderBrowser
    {
    public:
        enum class Mode
        {
            Folder,
            Project,
            File
        };

        // `extension` (with its dot) is the kind of file Mode::File chooses.
        void Open(Mode mode, const std::filesystem::path& start, const char* title, std::string extension = {});
        // Call every frame from the window the browser belongs to; returns the choice once.
        std::optional<std::filesystem::path> Draw();

    private:
        struct Entry
        {
            std::string           Name;
            std::filesystem::path Path;
            bool                  Project = false; // A .kproj, or a folder holding one; in File mode a file.
        };
        void Navigate(const std::filesystem::path& directory);

        Mode                  m_Mode = Mode::Folder;
        std::string           m_Title;
        bool                  m_OpenRequested = false;
        std::filesystem::path m_Directory, m_Selected;
        std::vector<Entry>    m_Entries;
        std::string           m_PathText, m_Error, m_Extension;
    };
} // namespace Kans
