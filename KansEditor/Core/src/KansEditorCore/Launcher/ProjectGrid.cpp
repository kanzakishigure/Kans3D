#include "KansEditorCore/Launcher/ProjectGrid.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace Kans::Launcher
{
    GridLayout ComputeProjectGrid(float available, float minCard, float maxCard, float gap)
    {
        GridLayout layout;
        available        = std::max(available, 1.0f);
        layout.Columns   = std::max(1, int(std::floor((available + gap) / (minCard + gap))));
        const float fill = (available - gap * float(layout.Columns - 1)) / float(layout.Columns);
        layout.CardWidth = std::min(maxCard, fill);
        return layout;
    }

    int MoveGridSelection(int current, int count, int columns, GridMove move)
    {
        if (count <= 0)
            return -1;
        columns = std::max(columns, 1);
        if (current < 0)
            return 0;
        if (current >= count)
            return count - 1;
        switch (move)
        {
            case GridMove::Left:
                return std::max(current - 1, 0);
            case GridMove::Right:
                return std::min(current + 1, count - 1);
            case GridMove::Up:
                return current - columns >= 0 ? current - columns : current;
            case GridMove::Down:
                if (current + columns < count)
                    return current + columns;
                // A shorter last row below: its last card, as in a file browser.
                return current / columns < (count - 1) / columns ? count - 1 : current;
        }
        return current;
    }

    std::string EllipsizeMiddle(const std::string&                             text,
                                float                                          maxWidth,
                                const std::function<float(std::string_view)>& measure)
    {
        if (measure(text) <= maxWidth)
            return text;
        // Byte offsets where each code point starts, plus the end.
        std::vector<size_t> starts;
        for (size_t i = 0; i < text.size(); ++i)
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                starts.push_back(i);
        starts.push_back(text.size());
        const int  points = int(starts.size()) - 1;
        const auto joined = [&](int kept) {
            const int head = (kept + 1) / 2, tail = kept / 2;
            return text.substr(0, starts[head]) + "..." + text.substr(starts[points - tail]);
        };
        // Widest kept count that fits; widths grow with the count.
        int low = 0, high = points - 1;
        while (low < high)
        {
            const int middle = (low + high + 1) / 2;
            if (measure(joined(middle)) <= maxWidth)
                low = middle;
            else
                high = middle - 1;
        }
        return joined(std::max(low, 0));
    }
} // namespace Kans::Launcher
