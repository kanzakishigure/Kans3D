#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Kans
{
    struct ContactShadowSettings
    {
        bool  Enabled   = false;
        float Length    = .2f;  // World-space metres toward the light.
        float Thickness = .05f; // View-space depth slab behind visible surfaces, in metres.
        bool  operator==(const ContactShadowSettings& b) const
        {
            return Enabled == b.Enabled && Length == b.Length && Thickness == b.Thickness;
        }
        ContactShadowSettings Validated() const
        {
            auto s      = *this;
            s.Length    = std::clamp(std::isfinite(s.Length) ? s.Length : .2f, 0.f, 10.f);
            s.Thickness = std::clamp(std::isfinite(s.Thickness) ? s.Thickness : .05f, .001f, 1.f);
            return s;
        }
    };
    enum class ShadowFilter
    {
        PCF3x3,
        PCF5x5Tent,
        PCSS
    };
    struct ShadowSettings
    {
        bool                  Enabled      = false;
        uint32_t              Resolution   = 2048;
        float                 ConstantBias = 1.25f, SlopeBias = 1.75f, NormalOffset = 0.02f;
        ShadowFilter          Filter        = ShadowFilter::PCSS;
        float                 AngularRadius = 0.5f;  // Degrees (directional).
        float                 SourceRadius  = 0.05f; // Metres (point and spot).
        uint32_t              Cascades      = 4;
        float                 Lambda = 0.75f, MaxDistance = 80.0f, Blend = 0.1f;
        static ShadowSettings PointDefaults()
        {
            ShadowSettings s;
            s.Resolution = 1024;
            return s;
        }
        static ShadowSettings SpotDefaults() { return PointDefaults(); }
        bool operator==(const ShadowSettings& s) const
        {
            return Enabled == s.Enabled && Resolution == s.Resolution && ConstantBias == s.ConstantBias &&
                   SlopeBias == s.SlopeBias && NormalOffset == s.NormalOffset && Filter == s.Filter &&
                   AngularRadius == s.AngularRadius && SourceRadius == s.SourceRadius && Cascades == s.Cascades &&
                   Lambda == s.Lambda && MaxDistance == s.MaxDistance && Blend == s.Blend;
        }
        bool           operator!=(const ShadowSettings& s) const { return !(*this == s); }
        ShadowSettings Validated() const
        {
            auto s          = *this;
            auto finite     = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
            s.Resolution    = std::clamp(s.Resolution, 64u, 4096u);
            s.Cascades      = std::clamp(s.Cascades, 1u, 4u);
            s.ConstantBias  = std::clamp(finite(s.ConstantBias, 1.25f), 0.0f, 32.0f);
            s.SlopeBias     = std::clamp(finite(s.SlopeBias, 1.75f), 0.0f, 32.0f);
            s.NormalOffset  = std::clamp(finite(s.NormalOffset, 0.02f), 0.0f, 1.0f);
            s.Lambda        = std::clamp(finite(s.Lambda, 0.75f), 0.0f, 1.0f);
            s.MaxDistance   = std::clamp(finite(s.MaxDistance, 80.0f), 0.1f, 10000.0f);
            s.Blend         = std::clamp(finite(s.Blend, 0.1f), 0.0f, 0.5f);
            s.AngularRadius = std::clamp(finite(s.AngularRadius, 0.5f), 0.0f, 10.0f);
            s.SourceRadius  = std::clamp(finite(s.SourceRadius, 0.05f), 0.0f, 10.0f);
            if (uint32_t(s.Filter) > uint32_t(ShadowFilter::PCSS))
                s.Filter = ShadowFilter::PCSS;
            return s;
        }
    };
} // namespace Kans
