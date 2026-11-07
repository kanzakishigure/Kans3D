#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <imgui.h>
#include <glm/glm.hpp>
#include "Kans3D/Scene/Entity.h"
#include "KansEditorCore/Physics/SecondaryMotionEditing.h"

namespace Kans
{
    class EditorCamera;
    class Scene;

    // Painting a cloth asset's maps in the scene view (like the paint modes of UE's Chaos Cloth
    // and Magica Cloth): the viewport forwards its overlay, input and panel here while a cloth
    // entity is painted. It paints the welded particles of a working copy on the current
    // (animated) pose; strokes are steps of the session's own undo stack, the scene previews the
    // working copy, Save writes the .cloth file and Cancel drops the copy.
    class ClothPaintTool
    {
    public:
        enum class Tool
        {
            Brush,
            Fill,
            Gradient,
            Pin
        };
        struct View
        {
            ImVec2              Min, Max;
            const EditorCamera* Camera = nullptr;
            Scene*              Scene  = nullptr;
        };

        void Begin(Entity entity, AssetID asset, ClothData data, std::filesystem::path file);
        bool IsActive() const { return m_Session.IsActive(); }
        bool IsPainting(Entity entity) const { return entity && m_Session.GetEntity() == entity.GetUUID(); }
        UUID GetEntity() const { return m_Session.GetEntity(); }
        // Ends painting: the working copy is dropped and the scene shows the stored asset again.
        void Cancel(Scene* scene);
        // Writes the .cloth file and hands it to the saved callback (which rebuilds it).
        bool Save();
        bool Undo(Scene& scene);
        bool Redo(Scene& scene);
        void SetSavedCallback(std::function<void(const std::filesystem::path&)> saved) { m_Saved = std::move(saved); }

        // The map over the cloth, pins, the brush and the hovered particle's spheres.
        void DrawOverlay(const View& view);
        // Mouse input over the scene image; true while it uses the left button (no selection or gizmo).
        bool Update(const View& view, bool hovered);
        // Tools, map choice, legend and the session buttons.
        void DrawPanel(ImVec2 position, const View& view);

        SecondaryMotionEditing::ClothPaintSession& Session() { return m_Session; }
        SecondaryMotionEditing::Brush&             GetBrush() { return m_Brush; }
        Tool                                       ActiveTool = Tool::Brush;

    private:
        Entity Owner(Scene& scene) const;
        // The pose and the particle under the pointer, refreshed once per frame.
        void Refresh(const View& view);
        // After a change: the scene previews the working copy.
        void Changed(Scene& scene);
        bool ToScreen(const View& view, glm::vec3 point, ImVec2& out) const;
        void Ring(const View& view, glm::vec3 centre, glm::vec3 normal, float radius, ImU32 color) const;

        SecondaryMotionEditing::ClothPaintSession             m_Session;
        SecondaryMotionEditing::Brush                         m_Brush;
        std::vector<std::vector<uint32_t>>                    m_Neighbours;
        std::vector<glm::vec3>                                m_Normals;
        std::optional<SecondaryMotionEditing::ParticleHit>    m_Hover;
        std::optional<glm::vec3>                              m_LastDab, m_GradientStart, m_GradientEnd;
        bool                                                  m_Stroking = false;
        bool                                                  m_FillAll  = false;
        float                                                 m_FillValue = 0.0f, m_GradientFrom = 0.0f, m_GradientTo = 0.1f;
        float                                                 m_UnpinValue = 0.1f;
        bool                                                  m_ShowMaxDistance = true, m_ShowBackstop = true;
        int                                                   m_ImportChannel = 0;
        float                                                 m_ImportScale   = 1.0f;
        std::string                                           m_Status;
        std::function<void(const std::filesystem::path&)>    m_Saved;
        uint64_t                                              m_RefreshedFrame = UINT64_MAX;
    };
} // namespace Kans
