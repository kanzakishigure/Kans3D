#pragma once
#include <imgui.h>

// The editor's visual tokens. Regions are told apart by tone and gaps rather
// than border lines: the darkest tone shows between docked panels, panels sit
// one step lighter, cards and toolbars another, and inputs are inset darker.
// The accent marks only selection, focus and active state.
namespace Kans::EditorTheme
{
    struct Palette
    {
        ImVec4 App;      // Gutters between panels, the top band.
        ImVec4 Panel;    // Panel backgrounds and the active tab.
        ImVec4 Raised;   // Cards, toolbars, headers, popups.
        ImVec4 Hover;    // Hovered rows and controls.
        ImVec4 Pressed;  // Pressed controls.
        ImVec4 Inset;    // Input fields and wells.
        ImVec4 Line;     // The few lines that remain (tree guides).
        ImVec4 Text;
        ImVec4 TextDim;  // Secondary labels, paths, hints.
        ImVec4 TextOff;  // Disabled.
        ImVec4 Accent;
        ImVec4 AccentHover;
        ImVec4 AccentPressed;
        ImVec4 Selection; // Selected rows: accent mixed into the panel tone.
        ImVec4 Danger;
        ImVec4 Warning;
        ImVec4 Success;
        // Asset types, the same in icons, browser cards, filter pills and pickers.
        ImVec4 TypeModel;
        ImVec4 TypeImage;
        ImVec4 TypeScene;
        ImVec4 TypeMaterial;
        ImVec4 TypeScript;
        ImVec4 TypeShader;
        ImVec4 TypeAudio;
        ImVec4 TypeFont;
        ImVec4 TypeUI;
        ImVec4 TypeTimeline;
        ImVec4 TypeAction;
        ImVec4 TypeExpression;
        ImVec4 TypePhysics;
    };

    const Palette& Colors();

    // Applies colours and sizes to the current ImGui style. Every size is
    // multiplied by `scale`, fonts included.
    void Apply(float scale = 1.0f);
    float Scale();
    // A size in pixels at scale 1, converted to the current scale.
    inline float Px(float pixels) { return pixels * Scale(); }

    // Spacing steps (4/8 px scale) at scale 1.
    inline constexpr float Space1 = 4.0f;
    inline constexpr float Space2 = 8.0f;
    inline constexpr float Space3 = 12.0f;
    inline constexpr float Space4 = 16.0f;
} // namespace Kans::EditorTheme
