#pragma once
#include <imgui.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "KansEditorCore/EditorAssetChoice.h"
#include "Kans3D/Timeline/Timeline.h"
#include "HierarchyPanel.h"

namespace Kans
{
    class AssetManager;
    class EditorAssetImportService;

    // The timeline of a Timeline Player: its bindings on the left, each with its tracks, and on
    // the right a time ruler with the tracks' keys, clips, ranges, events and camera cuts, which
    // drag along the time axis (snapping to frames). The selected item is edited on the far right.
    // Preview evaluates the timeline in the scene at the playhead and restores the bound entities
    // when it ends; edits are saved to the .timeline file, which is then rebuilt.
    class TimelinePanel
    {
    public:
        explicit TimelinePanel(HierarchyPanel& hierarchy) : m_Hierarchy(hierarchy) {}
        void Draw();
        // Edits the timeline of `owner`, an entity with a Timeline Player.
        void Open(Entity owner);
        void SetAssetChoices(std::vector<EditorAssetChoice> choices) { m_Choices = std::move(choices); }
        void SetAssetManager(AssetManager* assets) { m_Assets = assets; }
        void Bind(EditorAssetImportService* service) { m_Service = service; }
        // Shows the viewport through the scene's primary camera (camera cuts) while previewing.
        void SetViewThroughCamera(std::function<void(bool)> show) { m_ViewThrough = std::move(show); }
        bool IsPreviewing() const { return m_Previewing; }
        // Writes the .timeline file and rebuilds it (stops preview first).
        void Save();
        // Restores the scene; called before play mode, saving, undo and closing the project.
        void StopPreview();
        // Preview also ends when the window was not drawn (closed or hidden) last frame.
        void StopPreviewIfHidden();

        // For tests: the timeline being edited, and its unsaved state.
        TimelineData&       Data() { return m_Data; }
        const TimelineData& Data() const { return m_Data; }
        bool                IsDirty() const { return m_Dirty; }
        double              Playhead() const { return m_Time; }
        void                SetPlayhead(double time);
        void                SetPreview(bool preview);
        // Keys the entity's transform (or its offset from where preview started, for a relative
        // track) at the playhead on its binding's transform track, creating them as needed.
        bool KeyTransform(Entity entity);

    private:
        enum class RowKind
        {
            Binding,
            Transform,
            Animation,
            Activation,
            Particle,
            Property,
            Events,
            Cuts
        };
        struct Row
        {
            RowKind     Kind;
            size_t      Track = 0; // Index into the track list of its kind (Binding: into Bindings).
            std::string Binding;
        };
        // What the inspector edits: a track's item (key time for transforms).
        struct Selection
        {
            RowKind Kind  = RowKind::Binding;
            size_t  Track = SIZE_MAX, Item = SIZE_MAX;
            double  Time = 0;
            bool    Valid() const { return Track != SIZE_MAX; }
        };
        enum class DragKind
        {
            None,
            Playhead,
            Move,
            Start,
            End,
            BlendIn,
            BlendOut
        };

        Scene*                   GetScene() const { return m_Hierarchy.GetScene(); }
        TimelinePlayerComponent* Component() const;
        Entity                   BoundEntity(const std::string& binding) const;
        void                     Load();
        void                     CreateAsset();
        void                     Changed();
        void                     UpdatePreview();
        std::vector<Row>         Rows() const;
        double                   Snap(double time) const;
        float                    ToX(double time) const;
        double                   ToTime(float x) const;

        void DrawToolbar();
        void DrawNames(const std::vector<Row>& rows, float rowHeight);
        void DrawCanvas(const std::vector<Row>& rows, float rowHeight);
        void DrawRuler(ImVec2 origin, float width);
        void DrawRow(const Row& row, ImVec2 min, ImVec2 max);
        void DrawInspector();
        void DrawAddTrackMenu(const std::string& binding);
        void DeleteSelection();
        bool AssetCombo(const char* label, AssetType type, AssetID& id);

        HierarchyPanel&                m_Hierarchy;
        std::vector<EditorAssetChoice> m_Choices;
        AssetManager*                  m_Assets  = nullptr;
        EditorAssetImportService*      m_Service = nullptr;
        std::function<void(bool)>      m_ViewThrough;

        Scene*                m_Scene = nullptr;
        UUID                  m_Owner = 0;
        AssetID               m_Asset;
        std::filesystem::path m_File, m_PendingFile; // Pending: a new file waiting for its first build.
        TimelineData          m_Data;
        bool                  m_Loaded = false, m_Dirty = false;
        std::string           m_Status;

        double    m_Time = 0, m_Scroll = 0;
        float     m_Zoom = 120; // Pixels per second.
        bool      m_Snap = true, m_Playing = false;
        float     m_CanvasX = 0;
        Selection m_Selected;
        DragKind  m_Drag = DragKind::None;
        Selection m_DragTarget;
        double    m_DragOffset = 0;

        bool                               m_Previewing = false, m_PlayerDirty = true, m_NeedsEvaluate = true;
        bool                               m_ViewingCamera = false;
        std::unique_ptr<TimelinePlayer>    m_Player;
        std::map<UUID, TransformComponent> m_Bases; // Bound transforms when preview started.
        int                                m_LastDrawnFrame   = -1;
        char                               m_PropertyPath[96] = "Light.Intensity";
    };
} // namespace Kans
