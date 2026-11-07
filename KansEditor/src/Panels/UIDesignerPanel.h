#pragma once
#include <imgui.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "KansEditorCore/EditorAssetChoice.h"
#include "Kans3D/UI/UIInstance.h"

namespace Kans
{
    class EditorAssetImportService;

    // Edits a UI asset (.kui): a palette of widget types and the widget tree on the left, the
    // canvas at a chosen resolution in the middle (select, move and resize widgets with snapping
    // and guides to the parent's edges and centre), and the selected widget's details on the right.
    // The canvas is a schematic preview (fills, outlines and text); placed UI shows in the viewport.
    class UIDesignerPanel
    {
    public:
        void Draw();
        bool Open(const std::filesystem::path& file);
        void SetAssetChoices(std::vector<EditorAssetChoice> choices) { m_Choices = std::move(choices); }
        void Bind(EditorAssetImportService* service) { m_Service = service; }
        // Writes the file and rebuilds it.
        void Save();

        // For tests.
        UIData&                      Data() { return m_Data; }
        bool                         IsDirty() const { return m_Dirty; }
        const std::filesystem::path& File() const { return m_File; }
        // Adds a widget of `type` under the selection (or at the root) and selects it.
        uint64_t AddWidget(UIWidgetType type);
        void     Select(uint64_t id) { m_Selected = id; }
        uint64_t Selected() const { return m_Selected; }
        bool     DeleteSelected();
        // Moves the selected widget by `delta` canvas pixels (or resizes its bottom-right corner).
        void MoveSelected(glm::vec2 delta, bool resize = false);
        // Anchors the selected widget to (min, max) of its parent without moving it on the canvas.
        void SetAnchors(glm::vec2 min, glm::vec2 max);
        // The laid-out canvas at the preview resolution.
        const UIInstance& Layout();
        glm::vec2         Resolution() const { return m_Resolution; }
        void              SetResolution(glm::vec2 resolution) { m_Resolution = resolution, m_LayoutDirty = true; }

    private:
        UIWidget* Find(uint64_t id, std::vector<UIWidget>** siblings = nullptr, UIWidget** parent = nullptr);
        const UIRuntimeWidget* RuntimeOf(uint64_t id);
        glm::vec4              ParentBounds(uint64_t id);
        void                   Changed();
        void                   DrawToolbar();
        void                   DrawPalette();
        void                   DrawTree(std::vector<UIWidget>& widgets);
        void                   DrawCanvas();
        void                   DrawWidget(const UIRuntimeWidget& w, ImVec2 origin, float zoom);
        void                   DrawDetails();
        void                   DrawAnchorPresets();
        void                   DrawProperties(UIWidget& w);
        bool                   AssetCombo(const char* label, AssetType type, std::string& value);

        std::vector<EditorAssetChoice> m_Choices;
        EditorAssetImportService*      m_Service = nullptr;
        std::filesystem::path          m_File;
        UIData                         m_Data;
        bool                           m_Loaded = false, m_Dirty = false;
        std::string                    m_Status;

        std::unique_ptr<UIInstance> m_Layout;
        bool                        m_LayoutDirty = true;
        glm::vec2                   m_Resolution {1920, 1080};
        uint64_t                    m_Selected = 0, m_Hovered = 0;
        bool                        m_Snap = true;
        // Canvas drag: moving or resizing the selection, with the mouse's last canvas position.
        enum class Drag
        {
            None,
            Move,
            Resize
        } m_Drag = Drag::None;
        glm::vec2              m_DragLast {0}, m_DragRest {0};
        std::vector<glm::vec4> m_Guides; // Lines drawn while dragging (x0, y0, x1, y1 in canvas pixels).
    };
} // namespace Kans
