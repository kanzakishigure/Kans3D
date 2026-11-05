#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "Gizmo/Handles.h"
#include "Gizmo/TransformGizmo.h"
#include "Kans3D/Core/UUID.h"
#include "Panels/VfxPreview.h"
#include "Panels/VfxSystemEditing.h"

// Viewport handles for the spatial inputs of a VFX system, on the generic handle API: shape
// locations (sphere radius, box extents, cone length and angle, disc radius), positions (kill
// volume centres, velocity points), velocity arrows and cones, an emitter's fixed bounds, Grid3D
// bounds with their cell lattice, and the placement of a vector field (the transform gizmo on a
// virtual pivot). Only the selected module use, the emitter's bounds and the grids that use reads
// get handles. Values are in the input's space: inputs of a local-space emitter move with the
// owner and are edited in its frame. A drag restores the document to its start every frame and
// applies the dragged value through the editing layer (VfxSystemEditing), so an inherited emitter
// records overrides; release commits one undo step and cancel puts the start back exactly.
// ImGui-free: the system editor's preview and the scene viewport draw and feed it.
namespace Kans::VfxGizmos
{
    // Wireframes and knobs.
    inline constexpr glm::vec4 Accent {1.0f, 0.42f, 0.78f, 1.0f};
    inline constexpr uint32_t  LatticeColor = 0x90c46cffu; // ABGR: the accent, translucent.

    // The pointer frame the handles run in (the viewport's view, input and toolbar settings).
    struct Pointer
    {
        Gizmo::View     View;
        Gizmo::Input    Input;
        Gizmo::Settings Settings;
    };

    // What gets handles.
    struct Selection
    {
        std::string Emitter;     // Handle GUID.
        std::string Use;         // The selected (or expanded) module use; empty: none.
        std::string Grid;        // A Grid3D data interface chosen besides the use's own.
        bool        Bounds = true; // The emitter's fixed bounds, when it has them.
    };
    struct Options
    {
        glm::mat4 Owner {1.0f};       // The effect's world transform.
        bool      Editable = true;    // False: drawn only, never hot (read-only).
        bool      LockBoundsCenter = false;
        uint32_t  LatticeLines = 16;  // At most this many cell lines per axis.
    };

    enum class Kind : uint8_t
    {
        Sphere,  // Radius knobs.
        Box,     // Face knobs.
        Length,  // One scalar along an axis (cone length, speed, disc radius).
        Angle,   // A cone's half angle.
        Point,   // A position.
        Arrow,   // A vector's tip (velocity).
        Bounds,  // The emitter's fixed bounds.
        Grid,    // A Grid3D's WorldBounds.
        Field    // A vector field's translate, rotation and scale.
    };
    // One declared handle: what it edits.
    struct Declared
    {
        Gizmo::HandleId Id = 0;
        Kind            What = Kind::Point;
        std::string     Use, Input; // The module use and input (the setting or data interface for bounds and grids).
        bool            Interactive = true;
    };

    // The frame an emitter's simulation-space values are in: the owner (local space) or the world.
    glm::mat4 SimulationFrame(const Vfx::Emitter&, const glm::mat4& owner);
    // Every how many cells a lattice line is drawn so an axis has at most `maxLines` of them.
    uint32_t LatticeStep(uint32_t resolution, uint32_t maxLines);
    // A Grid3D's cell lattice on the faces of its bounds (centred on `frame`'s origin), every Nth line.
    std::vector<VfxPreview::DebugLine> GridLattice(const Vfx::InterfaceBinding& grid, const glm::mat4& frame,
                                                   uint32_t maxLines = 16, uint32_t color = LatticeColor);
    // The Grid3D data interfaces a use reads (inputs bound to one, or the simulation stage it runs in).
    std::vector<std::string> GridsOf(const Vfx::Emitter&, const std::string& use);
    // Whether a module use gets handles (for pickers).
    bool HasHandles(const Vfx::ModuleUse&, const VfxSystemEditing::Context&);
    // The uses of an emitter that get handles, in stack order: GUID and "Stage › Module".
    std::vector<std::pair<std::string, std::string>> UsesWithHandles(const Vfx::System&, const VfxSystemEditing::Context&,
                                                                     const std::string& emitter);

    class Gizmos
    {
    public:
        // The vector field gizmo's ID in the hover resolution.
        static constexpr Gizmo::HandleId FieldGizmoId = 0x7666786669656c64ull;

        // Between HandleContext::Begin and End: declares the selection's handles, offers the field
        // gizmo, and applies this frame's drag to `document` (when editable and not read-only).
        void Declare(Gizmo::HandleContext&, const Pointer&, VfxSystemEditing::SystemDocument& document,
                     const VfxSystemEditing::Context&, const Selection&, const Options&);
        // After End: runs the vector field's transform gizmo (it moves only when it won the hover).
        void Update(const Gizmo::HandleContext&, const Pointer&);
        // Drops a drag without writing (the document went away).
        void Reset();

        const std::vector<Declared>& Handles() const { return m_Declared; }
        bool IsDragging() const { return m_Start.has_value(); }
        const Gizmo::TransformGizmo& FieldGizmo() const { return m_Field; }
        // Wireframes, read-only knobs and the field gizmo as of the last Declare and Update.
        void AppendDraws(std::vector<GizmoDraw>& out) const;
        const std::vector<VfxPreview::DebugLine>& Lattice() const { return m_Lattice; }
        // The dragged value with its units ("Radius 1.25 m"); empty when nothing is dragged.
        const std::string& Readout() const { return m_Readout; }
        glm::vec2          ReadoutPosition() const { return m_Mouse + glm::vec2(18.0f, 14.0f); }
        // Why the last edit was refused (read-only documents, layers that cannot change); empty if none.
        const std::string& Message() const { return m_Message; }

    private:
        using Edit = std::function<VfxSystemEditing::Result(Vfx::System&, const VfxSystemEditing::Context&)>;
        // Applies a handle frame: stash at the start, restore and re-apply while dragging, commit on
        // release, restore on cancel.
        void Write(const Gizmo::HandleResult&, const Edit& edit, std::string readout);
        void DeclareUse(Gizmo::HandleContext&, const Pointer&, const Vfx::Emitter&, const Vfx::ModuleUse&, const Options&);
        // A box's face knobs and wireframe; returns the (dragged) centre and half extents.
        std::pair<glm::vec3, glm::vec3> DeclareBox(Gizmo::HandleContext&, const Pointer&, Gizmo::HandleId, Kind,
                                                   const std::string& use, const std::string& input, const glm::mat4& frame,
                                                   glm::vec3 centre, glm::vec3 half, bool lock,
                                                   const std::function<Edit(glm::vec3 centre, glm::vec3 half)>& edit,
                                                   const char* label);
        Gizmo::HandleStyle Style(Gizmo::KnobShape knob = Gizmo::KnobShape::Sphere) const;
        Gizmo::HandleId    Id(const std::string& use, const std::string& input, uint64_t part = 0) const;
        void               Line(glm::vec3 a, glm::vec3 b, float alpha = 0.9f);
        void               Circle(const glm::mat4& frame, glm::vec3 centre, glm::vec3 u, glm::vec3 v, float radius);
        void               WireBox(const glm::mat4& frame, glm::vec3 centre, glm::vec3 half);
        // A cone from `apex`: flat (height `length`, a spawn cone) or spherical (directions within
        // the angle at `length`, a velocity cone).
        void               WireCone(glm::vec3 apex, glm::vec3 axis, glm::vec3 side, float length, float radians, bool spherical);
        void               Arrow(glm::vec3 from, glm::vec3 to);

        VfxSystemEditing::SystemDocument* m_Document = nullptr;
        const VfxSystemEditing::Context*  m_Context  = nullptr;
        std::string                       m_Emitter;
        bool                              m_Editable = false;
        std::optional<Vfx::System>        m_Start; // The document when the drag began.
        std::vector<Declared>             m_Declared;
        std::vector<GizmoDraw>            m_Wire;
        std::vector<VfxPreview::DebugLine> m_Lattice;
        std::string                       m_Readout, m_Message;
        glm::vec2                         m_Mouse {0};
        float                             m_UnitsPerPixel = 0.0f;
        Pointer                           m_Pointer;
        // A point drag's constraint, chosen from the modifiers when it starts.
        Gizmo::PointHandle::Constraint    m_PointMode = Gizmo::PointHandle::Constraint::Screen;
        glm::vec3                         m_PointAxis {0, 0, 1};

        // The vector field placement this frame: the use, its frame and values, and the drag start.
        struct Field
        {
            std::string Use;
            glm::mat4   Frame {1.0f};
            glm::vec3   Translate {0}, Scale {1};
            glm::quat   Rotation {1, 0, 0, 0};
        };
        std::optional<Field>  m_FieldNow, m_FieldStart;
        Gizmo::Pivot          m_FieldPivot;
        Gizmo::TransformGizmo m_Field;
        bool                  m_FieldShown = false;
    };

    // The scene viewport's handles for particle entities: per entity which emitter and use the
    // card picked and whether "Edit asset in viewport" is on. By default the handles are visual
    // only (editing an asset from the scene affects every instance); edit mode writes the system
    // asset through the same editing layer, so the document turns dirty and is saved explicitly.
    // Read-only documents (built-in templates) refuse edit mode with a message.
    class SceneHandles
    {
    public:
        struct Choice
        {
            bool        Edit = false;
            std::string Emitter, Use;
        };
        // The document a system asset is edited through (an open system editor's, or a read-only
        // copy for display) and its context; null when it has no source.
        struct Source
        {
            VfxSystemEditing::SystemDocument* Document = nullptr;
            const VfxSystemEditing::Context*  Context  = nullptr;
            bool                              Editor   = false; // An editor's document (edits can be saved).
        };
        // `forEditing` opens the asset's editor when none has it.
        using SourceResolver = std::function<Source(AssetID system, bool forEditing)>;
        void SetSource(SourceResolver resolve) { m_Resolve = std::move(resolve); }

        Choice&       ChoiceFor(UUID entity) { return m_Choices[uint64_t(entity)]; }
        const Choice* Find(UUID entity) const;
        // Turns edit mode on or off; on is refused (with Message) for read-only documents.
        bool SetEdit(UUID entity, AssetID system, bool edit);
        // The document an entity's handles read now (null without one).
        Source Resolve(AssetID system, bool forEditing) const;

        // One frame for the selected entity (between Begin and End, then Update after End).
        void Declare(Gizmo::HandleContext&, const Pointer&, UUID entity, AssetID system, const glm::mat4& world,
                     const Options& options = {});
        void Update(const Gizmo::HandleContext&, const Pointer&);
        void Reset() { m_Gizmos.Reset(); }
        const Gizmos&      GetGizmos() const { return m_Gizmos; }
        bool               IsDragging() const { return m_Gizmos.IsDragging(); }
        const std::string& Message() const { return m_Message; }

    private:
        SourceResolver                  m_Resolve;
        std::map<uint64_t, Choice>      m_Choices;
        Gizmos                          m_Gizmos;
        bool                            m_Declared = false;
        std::string                     m_Message;
    };
} // namespace Kans::VfxGizmos
