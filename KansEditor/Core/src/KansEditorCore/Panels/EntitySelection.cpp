#include "KansEditorCore/Panels/EntitySelection.h"
#include <algorithm>

namespace Kans
{
    void EntitySelection::Click(UUID id, bool ctrl, bool shift, const std::vector<UUID>& order)
    {
        if (shift)
        {
            const auto anchor = std::find(order.begin(), order.end(), m_Anchor);
            const auto target = std::find(order.begin(), order.end(), id);
            if (anchor != order.end() && target != order.end())
            {
                // Ctrl+Shift adds the range; Shift alone replaces the selection with it.
                if (!ctrl)
                    m_Items.clear();
                const auto first = std::min(anchor, target), last = std::max(anchor, target);
                for (auto it = first; it <= last; ++it)
                    if (!Contains(*it))
                        m_Items.push_back(*it);
                m_Primary = id; // The anchor stays for the next Shift click.
                return;
            }
        }
        if (ctrl)
        {
            const auto found = std::find(m_Items.begin(), m_Items.end(), id);
            if (found != m_Items.end())
            {
                m_Items.erase(found);
                if (m_Primary == id)
                    m_Primary = m_Items.empty() ? UUID(0) : m_Items.back();
            }
            else
            {
                m_Items.push_back(id);
                m_Primary = id;
            }
            m_Anchor = id;
            return;
        }
        Set(id);
    }

    void EntitySelection::Set(UUID id)
    {
        m_Items.clear();
        if (id)
            m_Items.push_back(id);
        m_Primary = m_Anchor = id;
    }

    void EntitySelection::Clear() { Set(0); }

    bool EntitySelection::Contains(UUID id) const
    {
        return id && std::find(m_Items.begin(), m_Items.end(), id) != m_Items.end();
    }
} // namespace Kans
