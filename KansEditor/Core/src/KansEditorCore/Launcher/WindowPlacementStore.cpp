#include "WindowPlacementStore.h"
#include <fstream>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Project/RecentProjects.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Kans::Launcher
{
    namespace fs = std::filesystem;
    WindowPlacementStore::WindowPlacementStore(fs::path file) : m_File(std::move(file)) {}
    fs::path WindowPlacementStore::DefaultStorageFile()
    {
        return RecentProjects::DefaultStorageFile().parent_path() / "window-placements.yaml";
    }
    void WindowPlacementStore::Load()
    {
        m_Placements.clear();
        try
        {
            std::ifstream input(m_File);
            if (!input)
                return;
            const auto root = YAML::Load(input);
            if (input.bad() || !root.IsMap() || root["Version"].as<int>() != 1 || !root["Placements"].IsMap())
                return;
            std::map<std::string, WindowPlacement> loaded;
            for (const auto& item : root["Placements"])
            {
                const auto      name = item.first.as<std::string>();
                const auto      node = item.second;
                WindowPlacement p {node["X"].as<int>(),
                                   node["Y"].as<int>(),
                                   node["Width"].as<int>(),
                                   node["Height"].as<int>(),
                                   node["Maximized"].as<bool>()};
                if (name.empty() || p.Width <= 0 || p.Height <= 0)
                    return;
                loaded.emplace(name, p);
            }
            m_Placements = std::move(loaded);
        }
        catch (const std::exception&)
        {} // Preferences must never prevent startup.
    }
    std::optional<WindowPlacement> WindowPlacementStore::Get(const std::string& name) const
    {
        const auto found = m_Placements.find(name);
        return found == m_Placements.end() ? std::nullopt : std::optional<WindowPlacement>(found->second);
    }
    void WindowPlacementStore::Set(const std::string& name, const WindowPlacement& placement)
    {
        m_Placements[name] = placement;
    }
    bool WindowPlacementStore::Save() const
    {
        auto temporary = m_File;
        temporary += ".tmp"; // Same directory/filesystem as the destination for atomic rename.
        bool opened = false;
        try
        {
            YAML::Emitter out;
            out << YAML::BeginMap << YAML::Key << "Version" << YAML::Value << 1 << YAML::Key << "Placements"
                << YAML::Value << YAML::BeginMap;
            for (const auto& [name, p] : m_Placements)
            {
                if (name.empty() || p.Width <= 0 || p.Height <= 0)
                    return false;
                out << YAML::Key << name << YAML::Value << YAML::BeginMap << YAML::Key << "X" << YAML::Value << p.X
                    << YAML::Key << "Y" << YAML::Value << p.Y << YAML::Key << "Width" << YAML::Value << p.Width
                    << YAML::Key << "Height" << YAML::Value << p.Height << YAML::Key << "Maximized" << YAML::Value
                    << p.Maximized << YAML::EndMap;
            }
            out << YAML::EndMap << YAML::EndMap;
            if (!out.good())
                return false;
            if (!m_File.parent_path().empty())
                fs::create_directories(m_File.parent_path());
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
                return false;
            opened = true;
            output << out.c_str();
            output.flush();
            const bool written = output.good();
            output.close();
            if (written && !output.fail())
            {
#ifdef _WIN32
                // std::filesystem::rename does not replace existing files on Windows.
                if (MoveFileExW(temporary.c_str(), m_File.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return true;
#else
                std::error_code ec;
                fs::rename(temporary, m_File, ec);
                if (!ec)
                    return true;
#endif
            }
        }
        catch (const std::exception&)
        {}
        if (opened)
        {
            std::error_code ec;
            fs::remove(temporary, ec);
        }
        return false;
    }
} // namespace Kans::Launcher
