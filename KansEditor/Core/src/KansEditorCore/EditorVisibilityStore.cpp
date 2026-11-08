#include "EditorVisibilityStore.h"
#include <fstream>
#include <set>
#include <vector>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Scene/Entity.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Kans
{
    EditorVisibilityStore::EditorVisibilityStore(std::filesystem::path file) : m_File(std::move(file)) {}
    std::filesystem::path EditorVisibilityStore::File() const
    {
        if (!m_File.empty())
            return m_File;
        const auto& root = KansFileSystem::GetProjectRoot();
        return root.empty() ? std::filesystem::path {} : root / "Cache/Editor/Visibility.yaml";
    }
    const std::unordered_set<UUID>& EditorVisibilityStore::Hidden() const
    {
        static const std::unordered_set<UUID> empty;
        auto                                  found = m_Scenes.find(m_Key);
        return found == m_Scenes.end() ? empty : found->second;
    }
    bool EditorVisibilityStore::IsHidden(UUID entity) const { return Hidden().count(entity) != 0; }
    void EditorVisibilityStore::SetHidden(UUID entity, bool hidden)
    {
        if (!m_Scene || !m_Scene->GetEntityByUUID(entity))
            return;
        if (hidden)
            m_Scenes[m_Key].insert(entity);
        else
            m_Scenes[m_Key].erase(entity);
    }
    void EditorVisibilityStore::ShowAll() { m_Scenes[m_Key].clear(); }
    void EditorVisibilityStore::Isolate(const std::unordered_set<UUID>& entities)
    {
        if (!m_Scene)
            return;
        std::unordered_set<UUID> visible;
        std::vector<Entity>      pending;
        for (UUID id : entities)
            if (auto e = m_Scene->GetEntityByUUID(id))
            {
                pending.push_back(e);
                for (auto parent = m_Scene->GetParent(e); parent; parent = m_Scene->GetParent(parent))
                    visible.insert(parent.GetUUID());
            }
        while (!pending.empty())
        {
            auto e = pending.back();
            pending.pop_back();
            visible.insert(e.GetUUID());
            for (auto child : m_Scene->GetChildren(e))
                pending.push_back(child);
        }
        auto& hidden = m_Scenes[m_Key];
        hidden.clear();
        pending = m_Scene->GetRootEntities();
        while (!pending.empty())
        {
            auto e = pending.back();
            pending.pop_back();
            if (!visible.count(e.GetUUID()))
                hidden.insert(e.GetUUID());
            for (auto child : m_Scene->GetChildren(e))
                pending.push_back(child);
        }
    }
    void EditorVisibilityStore::Unload()
    {
        m_Scene = nullptr;
        m_Key.clear();
        m_Scenes.clear();
    }
    void EditorVisibilityStore::Load(const Scene& scene, std::string key)
    {
        m_Scene = &scene;
        m_Key   = key.empty() && scene.assetID.IsValid() ? scene.assetID.ToString() : std::move(key);
        if (!m_Key.empty())
            m_Key = std::filesystem::path(m_Key).lexically_normal().generic_u8string();
        m_Scenes.clear();
        try
        {
            std::ifstream input(File());
            if (input)
            {
                auto root = YAML::Load(input);
                if (input.bad() || !root.IsMap() || root["Version"].as<int>() != 1 || !root["Scenes"].IsMap())
                    return;
                std::map<std::string, std::unordered_set<UUID>> loaded;
                for (const auto& entry : root["Scenes"])
                {
                    const auto name = entry.first.as<std::string>();
                    if (name.empty() || !entry.second.IsSequence())
                        return;
                    for (const auto& id : entry.second)
                        loaded[name].insert(UUID(id.as<uint64_t>()));
                }
                m_Scenes = std::move(loaded);
            }
        }
        catch (const std::exception&)
        {
            m_Scenes.clear();
        }
        auto& hidden = m_Scenes[m_Key];
        for (auto it = hidden.begin(); it != hidden.end();)
            if (!scene.GetEntityByUUID(*it))
                it = hidden.erase(it);
            else
                ++it;
    }
    void EditorVisibilityStore::SetSceneKey(std::string key)
    {
        if (key.empty() || key == m_Key)
            return;
        auto hidden     = Hidden();
        m_Key           = std::filesystem::path(key).lexically_normal().generic_u8string();
        m_Scenes[m_Key] = std::move(hidden);
    }
    bool EditorVisibilityStore::Save() const
    {
        const auto file = File();
        if (file.empty())
            return false;
        auto temporary = file;
        temporary += ".tmp";
        bool opened = false;
        try
        {
            YAML::Emitter out;
            out << YAML::BeginMap << YAML::Key << "Version" << YAML::Value << 1 << YAML::Key << "Scenes" << YAML::Value
                << YAML::BeginMap;
            for (const auto& [key, hidden] : m_Scenes)
            {
                if (key.empty())
                    continue;
                out << YAML::Key << key << YAML::Value << YAML::BeginSeq;
                // Stable output makes preference changes reviewable.
                std::set<UUID> sorted(hidden.begin(), hidden.end());
                for (UUID id : sorted)
                    out << uint64_t(id);
                out << YAML::EndSeq;
            }
            out << YAML::EndMap << YAML::EndMap;
            if (!out.good())
                return false;
            if (!file.parent_path().empty())
                std::filesystem::create_directories(file.parent_path());
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
                if (MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return true;
#else
                std::error_code ec;
                std::filesystem::rename(temporary, file, ec);
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
            std::filesystem::remove(temporary, ec);
        }
        return false;
    }
} // namespace Kans
