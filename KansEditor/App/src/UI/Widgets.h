#pragma once
#include <functional>
#include <imgui.h>

// Small widgets shared by editor toolbars and overlays.
namespace Kans::Widgets
{
    // Draws an icon glyph centred on its drawn pixels within [min, max].
    void CenterGlyph(const char* icon, ImVec2 min, ImVec2 max, ImU32 color);
    // A section heading inside a menu: dim small text over a line.
    void MenuSection(const char* title);
    // Icon button with a tooltip; `active` highlights toggles that are on.
    bool ToolButton(const char* icon, const char* tooltip, bool active = false, bool enabled = true);
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
