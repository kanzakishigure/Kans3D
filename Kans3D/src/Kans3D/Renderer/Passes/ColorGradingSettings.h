#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include "Kans3D/Asset/Asset.h"
namespace Kans
{
    struct ColorGradingSettings
    {
        float     Temperature = 6500, Tint = 0, Contrast = 1, Saturation = 1;
        glm::vec3 Lift {0}, Gamma {1}, Gain {1};
        AssetID   LUT;
        float     Contribution = 1;
        bool      operator==(const ColorGradingSettings& o) const
        {
            return Temperature == o.Temperature && Tint == o.Tint && Contrast == o.Contrast &&
                   Saturation == o.Saturation && Lift == o.Lift && Gamma == o.Gamma && Gain == o.Gain && LUT == o.LUT &&
                   Contribution == o.Contribution;
        }
        ColorGradingSettings Validated() const
        {
            auto r      = *this;
            auto finite = [](float x, float lo, float hi, float fallback) {
                return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
            };
            r.Temperature  = finite(Temperature, 1000, 40000, 6500);
            r.Tint         = finite(Tint, -1, 1, 0);
            r.Contrast     = finite(Contrast, 0, 4, 1);
            r.Saturation   = finite(Saturation, 0, 4, 1);
            r.Contribution = finite(Contribution, 0, 1, 1);
            for (int i = 0; i < 3; ++i)
            {
                r.Lift[i]  = finite(Lift[i], -1, 1, 0);
                r.Gamma[i] = finite(Gamma[i], .01f, 8, 1);
                r.Gain[i]  = finite(Gain[i], 0, 8, 1);
            }
            return r;
        }
        bool Active() const
        {
            auto r         = *this;
            r.LUT          = {};
            r.Contribution = 1;
            return !(r == ColorGradingSettings {}) || (LUT.IsValid() && Contribution > 0);
        }
        uint64_t SettingsHash() const
        {
            uint64_t hash = 14695981039346656037ull;
            auto     add  = [&](uint64_t word) {
                for (int i = 0; i < 8; ++i)
                {
                    hash ^= uint8_t(word >> (i * 8));
                    hash *= 1099511628211ull;
                }
            };
            auto scalar = [&](float v) {
                uint32_t word;
                if (v == 0)
                    v = 0;
                std::memcpy(&word, &v, 4);
                add(word);
            };
            scalar(Temperature);
            scalar(Tint);
            scalar(Contrast);
            scalar(Saturation);
            for (int i = 0; i < 3; ++i)
            {
                scalar(Lift[i]);
                scalar(Gamma[i]);
                scalar(Gain[i]);
            }
            scalar(Contribution);
            add(std::hash<AssetID> {}(LUT));
            return hash;
        }
    };
} // namespace Kans
