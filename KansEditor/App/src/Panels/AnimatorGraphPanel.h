#pragma once
#include <string>
#include <imgui.h>
#include <vector>
#include "HierarchyPanel.h"
#include "KansEditorCore/EditorAssetChoice.h"

namespace Kans
{
    struct AnimationControllerData;
    struct AnimationLayer;

    // The selected model's animator as a node graph: one tab per layer, its states as
    // nodes and transitions as arrows, the playing state highlighted. Right-click adds a
    // state, dragging from a node's port to another node adds a transition, and the side
    // panel edits the selected state (including its blend tree) or transition, or with
    // nothing selected the layer and the parameters. Also lists the controller's legacy
    // expression presets.
    class AnimatorGraphPanel
    {
    public:
        explicit AnimatorGraphPanel(HierarchyPanel& hierarchy) : m_Hierarchy(hierarchy) {}
        void Draw();
        // Clips offered for states and blend tree children.
        void SetAssetChoices(std::vector<EditorAssetChoice> choices) { m_Choices = std::move(choices); }

    private:
        bool DrawGraph(AnimationControllerData& controller, size_t layer);
        bool DrawSelection(AnimationControllerData& controller, size_t layer);
        // With nothing selected: the layer's settings and mask, and the controller's parameters.
        bool          DrawLayerSettings(AnimationControllerData& controller, size_t layer);
        bool DrawExpressions(AnimationControllerData& controller);
        static ImVec2 NodePosition(const AnimationLayer& layer, size_t state);
        static void   MoveNode(AnimationLayer& layer, size_t state, ImVec2 delta);

        // A clip from the project (true when `clip` changed).
        bool ClipCombo(const char* id, AssetID& clip);

        HierarchyPanel&                m_Hierarchy;
        std::vector<EditorAssetChoice> m_Choices;
        UUID            m_Entity = 0;
        size_t          m_Layer = 0;
        int             m_SelectedState = -1, m_SelectedTransition = -1;
        int             m_LinkFrom = -2; // State a new transition starts from (-1 = Any State), -2 = none.
        ImVec2          m_Pan {240, 60}; // Leaves room for Any State, left of the first state.
    };
} // namespace Kans
