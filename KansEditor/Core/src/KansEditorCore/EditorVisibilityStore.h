#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include "Kans3D/Core/UUID.h"

namespace Kans
{
    class Scene;

    // Project-local preferences, independent of scene serialization and history.
    class EditorVisibilityStore
    {
    public:
        explicit EditorVisibilityStore(std::filesystem::path file = {});
        // Tests explicit eye state; RenderWorld resolves inherited hiding for a view.
        bool IsHidden(UUID entity) const;
        void SetHidden(UUID entity, bool hidden);
        // Keeps the chosen subtrees and their ancestors visible; hides all other entities.
        void                            Isolate(const std::unordered_set<UUID>& entities);
        void                            ShowAll();
        const std::unordered_set<UUID>& Hidden() const;
        // The scene must outlive this binding. A supplied key is a project-relative asset path;
        // otherwise use the scene asset ID. Unsaved scenes have session-only preferences.
        void Load(const Scene& scene, std::string key = {});
        // Drops the scene binding and live preferences, retaining the configured storage file.
        void Unload();
        // Associates the current preferences with a saved/renamed scene without reloading them.
        void SetSceneKey(std::string key);
        bool Save() const;

    private:
        std::filesystem::path                           File() const;
        std::filesystem::path                           m_File;
        const Scene*                                    m_Scene = nullptr;
        std::string                                     m_Key;
        std::map<std::string, std::unordered_set<UUID>> m_Scenes;
    };
} // namespace Kans
