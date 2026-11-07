#pragma once
#include <functional>
#include <string>
#include <glm/glm.hpp>
#include <imgui.h>

// Inspector layout: cards (a header with a menu) hold groups (a heading with an
// optional feature toggle) that hold "label | value" rows.
//
// Every grid shares one label column, so values line up across cards; dragging
// the column edge in any grid resizes it everywhere. Rows given a default show a
// reset button when they differ from it, and count as modified for the group
// heading and the "modified only" filter.
namespace Kans::PropertyGrid
{
    // ---- Filtering (set by the panel around its cards) ----
    // Rows whose label does not contain `text` (any case), or that are unmodified
    // while `modifiedOnly` is set, are skipped.
    void SetFilter(const std::string& text, bool modifiedOnly);
    void ClearFilter();
    bool IsFiltering();

    // ---- Cards ----
    struct CardOptions
    {
        bool                  DefaultOpen = true;
        bool                  Overridden  = false; // Accent bar on the left (prefab override).
        const char*           Note        = nullptr; // Dim text after the title.
        std::function<void()> Menu;                 // Items of the header's ⋮ menu.
    };
    // Returns true when the body is shown; always call EndCard().
    bool BeginCard(const char* id, const char* icon, const char* title, const CardOptions& options = {});
    void EndCard();

    // ---- Groups inside a card ----

    // A heading band; rows inside are separated by hairlines, and a group opened inside another
    // steps in with a guide line. `enabled` adds the feature toggle on it and hides the rows while off
    // (`toggled` reports a click). Returns true when the rows are shown: then call EndGroup().
    bool BeginGroup(const char* title, bool* enabled = nullptr, bool* toggled = nullptr, bool defaultOpen = true);
    void EndGroup();
    // Keeps `width` free at the right end of the next group heading (for the owner's own buttons).
    void SetNextGroupTrailer(float width);
    // The screen rectangle of the last group heading drawn (for owners drawing over it).
    bool LastGroupHeading(ImVec2& min, ImVec2& max);

    // ---- Grids (rows outside a group) ----
    bool Begin(const char* id, float labelWidth = 0.0f); // 0: the shared label column.
    void End();
    // Free-form content inside a grid or group: no label column and no column edge over it.
    void BeginFree();
    void EndFree();

    // Starts a row and moves to the value cell, where the next item takes the full
    // width. Returns false when the row is filtered out: then draw nothing for it.
    bool Row(const char* label, const char* tooltip = nullptr, bool modified = false);
    // After a modified Row's value widget: its reset button; true when clicked.
    bool RowReset(bool modified);

    // Read-only text, wrapped in the value column.
    void Text(const char* label, const std::string& value, bool disabled = false);
    bool Float(const char* label, float& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f,
               const char* format = "%.3f", const float* reset = nullptr);
    bool Int(const char* label, int& value, int min, int max, const int* reset = nullptr);
    bool Checkbox(const char* label, bool& value, const bool* reset = nullptr);
    bool Color3(const char* label, glm::vec3& value, const glm::vec3* reset = nullptr);
    bool Color4(const char* label, glm::vec4& value, const glm::vec4* reset = nullptr);
    bool InputText(const char* label, std::string& value);
    bool Combo(const char* label, const char* const items[], int count, int& index, const int* reset = nullptr);
    // Equal-width X/Y/Z fields; `reset` is every axis's default. With `linked`, a
    // toggle keeps the axes' ratios when one changes (uniform scale).
    bool Vec3(const char* label, glm::vec3& value, float reset = 0.0f, float speed = 0.1f, bool* linked = nullptr);
    bool Vec3(const char* label, glm::vec3& value, const glm::vec3& reset, float speed = 0.1f, bool* linked = nullptr);
    // Uniform scale: after `axis` changed from `before`, the other axes keep their ratio to it
    // (or take its value when it was zero).
    void KeepRatio(glm::vec3& value, int axis, float before);
} // namespace Kans::PropertyGrid
