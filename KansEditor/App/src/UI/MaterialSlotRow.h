#pragma once
#include <functional>
#include <string>
#include <glm/glm.hpp>
#include "Components/ComponentRegistry.h"

namespace Kans::Widgets
{
    // Viewport colours of a slot row's parts, also marking the row.
    inline constexpr glm::vec4 SlotHoverColor {.35f, .7f, 1.0f, .85f};
    inline constexpr glm::vec4 SlotPinnedColor {1.0f, .78f, .2f, .85f};
    inline constexpr glm::vec4 SlotIsolatedColor {.35f, .7f, 1.0f, 1.0f};

    // One material slot: preview, name and part count, the material combo and the
    // viewport tools (highlight, isolate, use the Content Browser's material, restore).
    // Hovering lights the parts in the viewport; a material file dropped on the row
    // replaces it. The row's context menu opens anywhere on it; `remove`, when set, adds
    // Remove Slot there. True with `chosen` set (invalid restores the imported material).
    bool MaterialSlotRow(ComponentUI&                 ui,
                         const std::string&           title,
                         const std::string&           detail,
                         const SlotParts&             parts,
                         AssetID                      imported,
                         AssetID                      current,
                         AssetID&                     chosen,
                         const std::function<void()>& remove = {});
} // namespace Kans::Widgets
