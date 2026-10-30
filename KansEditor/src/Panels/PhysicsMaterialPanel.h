#pragma once
#include <filesystem>
#include <string>
#include "Kans3D/Physics/PhysicsSettings.h"

namespace Kans
{
    class EditorAssetImportService;

    // Edits a .physmat file: friction, bounciness and their combine modes. Saving writes the
    // YAML and rebuilds the asset; colliders using it (also while playing) pick up the change.
    class PhysicsMaterialPanel
    {
    public:
        void Bind(EditorAssetImportService* service) { m_Service = service; }
        // Loads `file`; false (with the reason in the status) when it cannot be read.
        bool Open(const std::filesystem::path& file);
        void Draw();
        void Save();
        bool IsDirty() const { return m_Dirty; }
        // Ctrl+S saves the material instead of the scene while the panel has focus.
        bool                         HasFocus() const { return m_Focused; }
        const std::filesystem::path& GetFile() const { return m_File; }
        const PhysicsMaterialValues& GetValues() const { return m_Values; }

    private:
        EditorAssetImportService* m_Service = nullptr;
        std::filesystem::path     m_File;
        PhysicsMaterialValues     m_Values, m_Saved;
        bool                      m_Dirty = false, m_Focused = false;
        std::string               m_Status;
    };
} // namespace Kans
