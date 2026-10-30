#pragma once
#include <filesystem>
#include <string>
#include "Physics/PhysicsEditing.h"

namespace Kans
{
    // Project-wide settings, a page per area (Physics for now). Edits apply at once (the runtime
    // copy) and are written to the project file when the interaction ends.
    class ProjectSettingsPanel
    {
    public:
        // Reads the settings of `projectFile`; empty unbinds.
        void Bind(const std::filesystem::path& projectFile);
        void Draw();

    private:
        void DrawPhysics();
        void DrawCollisionMatrix(PhysicsSettings& settings, bool& changed);

        PhysicsEditing::ProjectPhysicsSettings m_Physics;
        bool                                   m_Bound     = false;
        bool                                   m_AllLayers = false, m_SavePending = false;
        std::string                            m_Status;
    };
} // namespace Kans
