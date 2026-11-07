#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include "Kans3D/Core/WindowGeometry.h"

namespace Kans::Launcher
{
    // Per-user placements, conventionally named "Launcher" and "Editor".
    // A single UI-thread writer; missing/corrupt storage is silently treated as empty.
    class WindowPlacementStore
    {
    public:
        explicit WindowPlacementStore(std::filesystem::path file = DefaultStorageFile());
        void Load();
        // Atomic replacement: failures return false and preserve the previous file.
        [[nodiscard]] bool             Save() const;
        std::optional<WindowPlacement> Get(const std::string& name) const;
        void                           Set(const std::string& name, const WindowPlacement& placement);
        static std::filesystem::path   DefaultStorageFile();

    private:
        std::filesystem::path                  m_File;
        std::map<std::string, WindowPlacement> m_Placements;
    };
} // namespace Kans::Launcher
