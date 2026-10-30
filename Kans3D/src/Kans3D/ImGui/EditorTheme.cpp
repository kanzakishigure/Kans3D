#include "kspch.h"
#include "Kans3D/ImGui/EditorTheme.h"

namespace Kans::EditorTheme
{
    namespace
    {
        constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return {r / 255.0f, g / 255.0f, b / 255.0f, a}; }
        constexpr ImVec4 WithAlpha(ImVec4 c, float a) { return {c.x, c.y, c.z, a}; }

        // Tone steps are ~10 levels apart so each layer reads clearly against the next.
        constexpr Palette Dark {
            Rgb(14, 14, 15),    // App
            Rgb(30, 30, 32),    // Panel
            Rgb(41, 41, 44),    // Raised
            Rgb(53, 53, 57),    // Hover
            Rgb(64, 64, 69),    // Pressed
            Rgb(20, 20, 22),    // Inset
            Rgb(50, 50, 54),    // Line
            Rgb(232, 232, 235), // Text
            Rgb(156, 156, 162), // TextDim
            Rgb(102, 102, 108), // TextOff
            Rgb(47, 128, 237),  // Accent
            Rgb(76, 148, 245),  // AccentHover
            Rgb(36, 108, 206),  // AccentPressed
            Rgb(37, 62, 102),   // Selection
            Rgb(232, 86, 77),   // Danger
            Rgb(232, 168, 56),  // Warning
            Rgb(76, 175, 106),  // Success
            Rgb(240, 138, 60),  // TypeModel
            Rgb(76, 175, 106),  // TypeImage
            Rgb(47, 128, 237),  // TypeScene
            Rgb(155, 107, 242), // TypeMaterial
            Rgb(232, 197, 71),  // TypeScript
            Rgb(60, 200, 216),  // TypeShader
            Rgb(240, 98, 146),  // TypeAudio
            Rgb(190, 170, 140), // TypeFont
            Rgb(110, 122, 238), // TypeUI
            Rgb(156, 200, 70),  // TypeTimeline
            Rgb(52, 186, 150),  // TypeAction
            Rgb(214, 100, 214), // TypeExpression
            Rgb(232, 140, 60),  // TypePhysics
        };

        float g_Scale = 1.0f;
    } // namespace

    const Palette& Colors() { return Dark; }
    float          Scale() { return g_Scale; }

    void Apply(float scale)
    {
        const Palette& p     = Dark;
        ImGuiStyle&    style = ImGui::GetStyle();
        style                = ImGuiStyle(); // Sizes below are at scale 1; ScaleAllSizes multiplies them once.
        auto& c              = style.Colors;

        c[ImGuiCol_Text]                  = p.Text;
        c[ImGuiCol_TextDisabled]          = p.TextOff;
        c[ImGuiCol_WindowBg]              = p.Panel;
        c[ImGuiCol_ChildBg]               = {0, 0, 0, 0}; // Children share their panel's tone unless they ask.
        c[ImGuiCol_PopupBg]               = p.Raised;
        // Dock splitters are drawn in the border colour: the app tone turns them into gaps, and
        // popups and bordered children get a dark outline instead of a light line.
        c[ImGuiCol_Border]                = p.App;
        c[ImGuiCol_BorderShadow]          = {0, 0, 0, 0};
        c[ImGuiCol_FrameBg]               = p.Inset;
        c[ImGuiCol_FrameBgHovered]        = Rgb(26, 26, 29);
        c[ImGuiCol_FrameBgActive]         = Rgb(16, 16, 18);
        c[ImGuiCol_TitleBg]               = p.App;
        c[ImGuiCol_TitleBgActive]         = p.App;
        c[ImGuiCol_TitleBgCollapsed]      = p.App;
        c[ImGuiCol_MenuBarBg]             = p.App;
        c[ImGuiCol_ScrollbarBg]           = {0, 0, 0, 0};
        c[ImGuiCol_ScrollbarGrab]         = p.Hover;
        c[ImGuiCol_ScrollbarGrabHovered]  = p.Pressed;
        c[ImGuiCol_ScrollbarGrabActive]   = Rgb(84, 84, 90);
        c[ImGuiCol_CheckMark]             = p.Accent;
        c[ImGuiCol_CheckboxSelectedBg]    = p.Inset;
        c[ImGuiCol_SliderGrab]            = p.Accent;
        c[ImGuiCol_SliderGrabActive]      = p.AccentHover;
        c[ImGuiCol_Button]                = p.Hover;
        c[ImGuiCol_ButtonHovered]         = p.Pressed;
        c[ImGuiCol_ButtonActive]          = Rgb(74, 74, 80);
        c[ImGuiCol_Header]                = p.Selection;
        c[ImGuiCol_HeaderHovered]         = p.Hover;
        c[ImGuiCol_HeaderActive]          = Rgb(44, 76, 124);
        // Lines between menu sections and in panels; visible on both panel and popup tones.
        // (Dock gaps are drawn in the border colour, above.)
        c[ImGuiCol_Separator]             = Rgb(62, 62, 67);
        c[ImGuiCol_SeparatorHovered]      = WithAlpha(p.Accent, 0.6f);
        c[ImGuiCol_SeparatorActive]       = p.Accent;
        c[ImGuiCol_ResizeGrip]            = {0, 0, 0, 0};
        c[ImGuiCol_ResizeGripHovered]     = WithAlpha(p.Accent, 0.6f);
        c[ImGuiCol_ResizeGripActive]      = p.Accent;
        c[ImGuiCol_InputTextCursor]       = p.Text;
        // Inactive tabs blend into the tab strip; the selected one takes its panel's tone.
        c[ImGuiCol_Tab]                       = p.App;
        c[ImGuiCol_TabHovered]                = p.Raised;
        c[ImGuiCol_TabSelected]               = p.Panel;
        c[ImGuiCol_TabSelectedOverline]       = p.Accent;
        c[ImGuiCol_TabDimmed]                 = p.App;
        c[ImGuiCol_TabDimmedSelected]         = p.Panel;
        c[ImGuiCol_TabDimmedSelectedOverline] = {0, 0, 0, 0}; // Only the focused panel shows the accent.
        c[ImGuiCol_DockingPreview]            = WithAlpha(p.Accent, 0.5f);
        c[ImGuiCol_DockingEmptyBg]            = p.App;
        c[ImGuiCol_PlotLines]                 = p.TextDim;
        c[ImGuiCol_PlotLinesHovered]          = p.Accent;
        c[ImGuiCol_PlotHistogram]             = p.Accent;
        c[ImGuiCol_PlotHistogramHovered]      = p.AccentHover;
        c[ImGuiCol_TableHeaderBg]             = p.Raised;
        c[ImGuiCol_TableBorderStrong]         = p.App;
        c[ImGuiCol_TableBorderLight]          = Rgb(38, 38, 41);
        c[ImGuiCol_TableRowBg]                = {0, 0, 0, 0};
        c[ImGuiCol_TableRowBgAlt]             = {1, 1, 1, 0.025f};
        c[ImGuiCol_TextLink]                  = p.AccentHover;
        c[ImGuiCol_TextSelectedBg]            = WithAlpha(p.Accent, 0.35f);
        c[ImGuiCol_TreeLines]                 = p.Line;
        c[ImGuiCol_DragDropTarget]            = p.Accent;
        c[ImGuiCol_DragDropTargetBg]          = WithAlpha(p.Accent, 0.12f);
        c[ImGuiCol_UnsavedMarker]             = p.Text;
        c[ImGuiCol_NavCursor]                 = p.Accent;
        c[ImGuiCol_NavWindowingHighlight]     = WithAlpha(p.Text, 0.7f);
        c[ImGuiCol_NavWindowingDimBg]         = {0, 0, 0, 0.45f};
        c[ImGuiCol_ModalWindowDimBg]          = {0, 0, 0, 0.55f};

        // Sizes on the 4/8 px grid.
        style.WindowPadding        = {Space2, Space2};
        style.FramePadding         = {6.0f, 4.0f};
        style.CellPadding          = {6.0f, 3.0f};
        style.ItemSpacing          = {Space2, Space1};
        style.ItemInnerSpacing     = {Space1, Space1};
        style.IndentSpacing        = Space4;
        style.ScrollbarSize        = 10.0f;
        style.GrabMinSize          = 10.0f;
        style.WindowBorderSize     = 0.0f;
        style.ChildBorderSize      = 1.0f;
        style.PopupBorderSize      = 1.0f;
        style.FrameBorderSize      = 0.0f;
        style.TabBorderSize        = 0.0f;
        style.TabBarBorderSize     = 0.0f;
        style.TabBarOverlineSize   = 2.0f;
        style.DockingSeparatorSize = Space1;
        style.WindowRounding       = 6.0f;
        style.ChildRounding        = 4.0f;
        style.FrameRounding        = 3.0f;
        style.PopupRounding        = 4.0f;
        style.ScrollbarRounding    = 6.0f;
        style.GrabRounding         = 3.0f;
        style.TabRounding          = 4.0f;
        style.WindowTitleAlign     = {0.0f, 0.5f};
        style.SeparatorTextBorderSize = 1.0f;
        style.SeparatorTextPadding    = {0.0f, 4.0f}; // Section headings start at the left edge.
        // The dock node's window-menu triangle clutters every tab strip.
        style.WindowMenuButtonPosition = ImGuiDir_None;

        g_Scale = scale;
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;

        // Platform windows dragged out of the main one are plain rectangles.
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            style.WindowRounding = 0.0f;
    }
} // namespace Kans::EditorTheme
