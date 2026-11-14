#pragma once
#include <algorithm>
#include <cstdint>

namespace Kans
{
    // CPU-only COLORREF conversion/cache; no Windows headers required.
    class Win32WindowEdgeColor
    {
    public:
        bool Update(float r, float g, float b)
        {
            const auto byte = [](float value) {
                return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            const uint32_t color = byte(r) | (byte(g) << 8) | (byte(b) << 16); // 0x00BBGGRR.
            if (color == m_Color)
                return false;
            m_Color = color;
            return true;
        }
        uint32_t Value() const { return m_Color; }

    private:
        // DWMWA_COLOR_DEFAULT until the editor supplies its theme color. Also ensures
        // the first update is applied, including black (a valid COLORREF of zero).
        uint32_t m_Color = 0xFFFFFFFFu;
    };
} // namespace Kans
