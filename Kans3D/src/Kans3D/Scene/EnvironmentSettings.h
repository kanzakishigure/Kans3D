#pragma once
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <glm/vec3.hpp>
#include "Kans3D/Renderer/Lighting/FogSettings.h"

namespace Kans
{
    struct EnvironmentSettings
    {
        std::filesystem::path Map; // Relative to RenderWorld::EnvironmentRoot (the project directory).
        float                 DiffuseIntensity  = 1;
        float                 SpecularIntensity = 1;
        float Rotation        = 0; // Degrees around +Y; positive rotation moves the environment in world space.
        float SkyboxIntensity = 1;
        bool  ShowSkybox      = true;
        glm::vec3 BackgroundColor {0, 0, 0}; // Scene-linear radiance behind geometry when no skybox is drawn.

        FogSettings Fog;

        // One normalization boundary for scene edits and deserialization.
        void Validate()
        {
            Fog               = Fog.Validated();
            auto intensity    = [](float value) { return std::isfinite(value) ? std::max(0.0f, value) : 1.0f; };
            DiffuseIntensity  = intensity(DiffuseIntensity);
            SpecularIntensity = intensity(SpecularIntensity);
            SkyboxIntensity   = intensity(SkyboxIntensity);
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(BackgroundColor[i]) || BackgroundColor[i] < 0)
                    BackgroundColor[i] = 0;
            Rotation          = std::isfinite(Rotation) ? std::fmod(Rotation, 360.0f) : 0.0f;
            if (Rotation < 0)
                Rotation += 360;
            if (!Map.empty())
            {
                Map = Map.lexically_normal();
                if (Map.is_absolute() || Map.has_root_name() || *Map.begin() == "..")
                    throw std::invalid_argument("Environment Map must be project-relative");
            }
        }
    };

    inline bool operator==(const EnvironmentSettings& a, const EnvironmentSettings& b)
    {
        return a.Map == b.Map && a.DiffuseIntensity == b.DiffuseIntensity &&
               a.SpecularIntensity == b.SpecularIntensity && a.Rotation == b.Rotation &&
               a.SkyboxIntensity == b.SkyboxIntensity && a.ShowSkybox == b.ShowSkybox &&
               a.BackgroundColor == b.BackgroundColor && a.Fog == b.Fog;
    }
    inline bool operator!=(const EnvironmentSettings& a, const EnvironmentSettings& b) { return !(a == b); }
} // namespace Kans
