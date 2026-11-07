#pragma once
#include <functional>
#include <string>
#include <string_view>

// Layout and keyboard logic of the project card grid, without UI.
namespace Kans::Launcher
{
    struct GridLayout
    {
        int   Columns   = 1;
        float CardWidth = 0.0f;
    };
    // As many columns as fit `available` with cards at least `minCard` wide and `gap` between
    // them; the cards then stretch to fill the row, up to `maxCard`. One column always fits,
    // shrinking below `minCard` when the space is narrower.
    GridLayout ComputeProjectGrid(float available, float minCard, float maxCard, float gap);

    enum class GridMove
    {
        Left,
        Right,
        Up,
        Down
    };
    // The selection after an arrow key among `count` cards laid out `columns` to a row.
    // No selection (or one past the end) starts at the first (or last) card; moves stop at
    // the edges, and Down into a shorter last row lands on its last card. -1 when empty.
    int MoveGridSelection(int current, int count, int columns, GridMove move);

    // `text` when it fits in `maxWidth`, otherwise its start and end joined by "...", keeping
    // as much as fits and never splitting a UTF-8 sequence. `measure` gives a string's width.
    std::string EllipsizeMiddle(const std::string&                             text,
                                float                                          maxWidth,
                                const std::function<float(std::string_view)>& measure);
} // namespace Kans::Launcher
