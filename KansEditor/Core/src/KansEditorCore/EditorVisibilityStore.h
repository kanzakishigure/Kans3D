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
        // The key a scene's editor preferences are stored under (this store and the editor view
        // state): `key`, a project-relative asset path, when given, otherwise the scene asset ID;
        // empty for an unsaved scene. Paths are normalized, so "a/../b.kans" and "b.kans" match.
        static std::string SceneKey(const Scene& scene, std::string key = {});
        static std::string NormalizeKey(std::string key);
        // The asset path key of a scene file in a project (empty path: empty key).
        static std::string ProjectKey(const std::filesystem::path& file, const std::filesystem::path& projectRoot);
        // Replaces `file` with `text` through a temporary beside it; on failure the old file stays.
        static bool WriteFile(const std::filesystem::path& file, const std::string& text);

    private:
        std::filesystem::path                           File() const;
        std::filesystem::path                           m_File;
        const Scene*                                    m_Scene = nullptr;
        std::string                                     m_Key;
        std::map<std::string, std::unordered_set<UUID>> m_Scenes;
    };
} // namespace Kans
