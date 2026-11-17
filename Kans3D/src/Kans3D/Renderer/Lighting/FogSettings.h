#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include "Kans3D/Physics/ForceFields.h"

namespace Kans
{
    struct FogSettings
    {
        bool      Enabled = false;
        float     Density = .02f, HeightFalloff = .2f, BaseHeight = 0;
        glm::vec3 Albedo {1};
        float     Extinction = 1, Anisotropy = .2f;
        float     VolumetricFogDistance = 64;
        // Pixel footprint at render resolution: 1280x720 gives 160x90x64.
        uint32_t    GridPixelSize = 8, DepthSlices = 64;
        float       HistoryWeight = .9f;
        bool        Noise         = false;
        bool        FollowWind    = false;
        float       NoiseScale = .1f, NoiseAmount = .5f;
        glm::vec3   NoiseScroll {.03f, 0, .01f};
        FogSettings Validated() const
        {
            auto s                  = *this;
            auto finite             = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
            s.Density               = std::clamp(finite(s.Density, .02f), 0.f, 100.f);
            s.HeightFalloff         = std::clamp(finite(s.HeightFalloff, .2f), 0.f, 100.f);
            s.BaseHeight            = finite(s.BaseHeight, 0);
            s.Extinction            = std::clamp(finite(s.Extinction, 1), 0.f, 100.f);
            s.Anisotropy            = std::clamp(finite(s.Anisotropy, .2f), -.95f, .95f);
            s.VolumetricFogDistance = std::clamp(finite(s.VolumetricFogDistance, 64), .1f, 10000.f);
            s.GridPixelSize         = std::clamp(s.GridPixelSize, 2u, 64u);
            s.DepthSlices           = std::clamp(s.DepthSlices, 8u, 128u);
            s.HistoryWeight         = std::clamp(finite(s.HistoryWeight, .9f), 0.f, .99f);
            s.NoiseScale            = std::clamp(finite(s.NoiseScale, .1f), .0001f, 100.f);
            s.NoiseAmount           = std::clamp(finite(s.NoiseAmount, .5f), 0.f, 1.f);
            for (int i = 0; i < 3; ++i)
            {
                s.Albedo[i]      = std::clamp(finite(s.Albedo[i], 1), 0.f, 1.f);
                s.NoiseScroll[i] = finite(s.NoiseScroll[i], 0);
            }
            return s;
        }
        bool operator==(const FogSettings& b) const
        {
            return Enabled == b.Enabled && Density == b.Density && HeightFalloff == b.HeightFalloff &&
                   BaseHeight == b.BaseHeight && Albedo == b.Albedo && Extinction == b.Extinction &&
                   Anisotropy == b.Anisotropy && VolumetricFogDistance == b.VolumetricFogDistance &&
                   GridPixelSize == b.GridPixelSize && DepthSlices == b.DepthSlices &&
                   HistoryWeight == b.HistoryWeight && Noise == b.Noise && NoiseScale == b.NoiseScale &&
                   NoiseAmount == b.NoiseAmount && NoiseScroll == b.NoiseScroll && FollowWind == b.FollowWind;
        }
        bool operator!=(const FogSettings& b) const { return !(*this == b); }
    };
    inline glm::uvec3 FogGridSize(glm::uvec2 renderSize, const FogSettings& settings)
    {
        const auto s = settings.Validated();
        return {std::max(1u, (renderSize.x + s.GridPixelSize - 1) / s.GridPixelSize),
                std::max(1u, (renderSize.y + s.GridPixelSize - 1) / s.GridPixelSize),
                s.DepthSlices};
    }
    // Zero is a boundary, so the first cell includes the medium directly in front of the camera.
    inline float FogSliceDepth(float slice, uint32_t slices, float distance, float nearDepth = .1f)
    {
        return nearDepth * std::expm1(std::log1p(distance / nearDepth) * slice / float(slices));
    }
    inline float FogDepthSlice(float depth, uint32_t slices, float distance, float nearDepth = .1f)
    {
        return std::log1p(std::max(0.f, depth) / nearDepth) / std::log1p(distance / nearDepth) * float(slices);
    }
    inline float HenyeyGreenstein(float cosine, float g)
    {
        const float d = 1 + g * g - 2 * g * std::clamp(cosine, -1.f, 1.f);
        return (1 - g * g) / (4 * 3.14159265358979323846f * d * std::sqrt(d));
    }
    // World-space displacement since t=0. Infinite particle-affecting winds only; bounded fields cannot
    // translate an entire noise tile. The integral of smooth turbulence/pulses is independent of frame rate.
    glm::vec3 FogWindDisplacement(const std::vector<FieldRecord>& fields, double time);
    enum class FogVolumeShape
    {
        Box,
        Sphere
    };
    struct FogVolumeComponent
    {
        FogVolumeShape Shape   = FogVolumeShape::Box;
        float          Density = .1f;
        glm::vec3      Albedo {1};
        bool           Noise = false, FollowWind = false;
        float          NoiseScale = .1f, NoiseAmount = .5f;
        glm::vec3      NoiseScroll {.03f, 0, .01f};
        float          EdgeFalloff = .1f; // Fraction of the half extent; shapes have unit diameter before transform.
        bool           operator==(const FogVolumeComponent& b) const
        {
            return Shape == b.Shape && Density == b.Density && Albedo == b.Albedo && EdgeFalloff == b.EdgeFalloff &&
                   Noise == b.Noise && FollowWind == b.FollowWind && NoiseScale == b.NoiseScale &&
                   NoiseAmount == b.NoiseAmount && NoiseScroll == b.NoiseScroll;
        }
        bool operator!=(const FogVolumeComponent& b) const { return !(*this == b); }
    };
} // namespace Kans
