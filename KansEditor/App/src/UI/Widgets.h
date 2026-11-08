#pragma once
#include <functional>
#include <imgui.h>

// Small widgets shared by editor toolbars and overlays.
namespace Kans::Widgets
{
    // Draws an icon glyph centred on its drawn pixels within [min, max] (on the current
    // window's draw list unless `draw` is given).
    void CenterGlyph(const char* icon, ImVec2 min, ImVec2 max, ImU32 color, ImDrawList* draw = nullptr);
    // A section heading inside a menu: dim small text over a line.
    void MenuSection(const char* title);

    // Menus share one look: icons in a fixed-width column a Space2 gap before the labels,
    // and taller rows. A menu opened with BeginMenu or BeginIconMenu ends with EndMenu;
    // a menu drawn in a popup wraps its items in PushMenuStyle / PopMenuStyle.
    bool BeginMenu(const char* label, bool enabled = true);
    bool BeginIconMenu(const char* icon, const char* label, bool enabled = true);
    void EndMenu();
    bool IconMenuItem(const char* icon,
                      const char* label,
                      const char* shortcut = nullptr,
                      bool        selected = false,
                      bool        enabled  = true);
    void PushMenuStyle();
    void PopMenuStyle();
    // Width of the icon column and the gap from it to the labels.
    float MenuIconColumn();
    float MenuIconGap();
    // Icon button with a tooltip; `active` highlights toggles that are on. `id` names the button
    // in the current ID scope: buttons sharing an icon in one window (Local orientation and
    // Object mode both show a cube) would otherwise conflict, and an ID taken from the icon or
    // tooltip changes whenever a toggle changes its icon.
    bool ToolButton(const char* id, const char* icon, const char* tooltip, bool active = false, bool enabled = true);
    // Thin vertical line between toolbar groups (call between items on one line).
    void ToolSeparator();
    // Draws `contents` (items on one line) on a raised pill starting at `position`;
    // returns the pill's right edge.
    float Pill(ImVec2 position, const std::function<void()>& contents);
    enum class Transport
    {
        None,
        PlayStop,
        Pause,
        Step
    };
    // Play (Stop while playing), Pause and Step as one segmented control at `position`,
    // drawn as shapes centred in each segment. Returns the clicked segment; `right`
    // receives the control's right edge.
    Transport TransportControls(ImVec2 position, bool playing, bool paused, float* right = nullptr);
    // Width of TransportControls, for centring it.
    float TransportWidth();
    // Height of a pill or the transport controls: groups in one band share it.
    float BandGroupHeight();
    // Icon and text on one button; `dropdown` adds the arrow of a button that opens a menu.
    bool TextButton(const char* icon, const char* text, const char* tooltip, bool active = false, bool dropdown = false);
    // Draws `contents` at `position` on a translucent pill floating over the viewport;
    // returns the pill's right edge.
    float OverlayGroup(ImVec2 position, const std::function<void()>& contents);
} // namespace Kans::Widgets
