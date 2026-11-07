#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Kans
{
    // Modal browser over the whole file system for choosing a folder, or a
    // project (a .kproj file or the folder holding it).
    class FolderBrowser
    {
    public:
        enum class Mode
        {
            Folder,
            Project
        };

        void Open(Mode mode, const std::filesystem::path& start, const char* title);
        // Call every frame from the window the browser belongs to; returns the choice once.
        std::optional<std::filesystem::path> Draw();

    private:
        struct Entry
        {
            std::string           Name;
            std::filesystem::path Path;
            bool                  Project = false; // A .kproj, or a folder holding one.
        };
        void Navigate(const std::filesystem::path& directory);

        Mode                  m_Mode = Mode::Folder;
        std::string           m_Title;
        bool                  m_OpenRequested = false;
        std::filesystem::path m_Directory, m_Selected;
        std::vector<Entry>    m_Entries;
        std::string           m_PathText, m_Error;
    };
} // namespace Kans
