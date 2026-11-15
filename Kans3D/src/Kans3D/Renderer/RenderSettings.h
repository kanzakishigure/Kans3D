#pragma once
#include <glm/glm.hpp>
#include "Kans3D/Core/UUID.h"
#include "Kans3D/Renderer/Lighting/ShadowSettings.h"
#include "Kans3D/Scene/Components.h"
namespace Kans
{
    struct DirLight : DirLightComponent
    {
        glm::vec3 Dirction {0};
        UUID      EntityID = 0;
        DirLight() { Color = glm::vec3(0); }
    };
    struct PointLight : PointLightComponent
    {
        glm::vec3 Position {0};
        UUID      EntityID = 0;
        PointLight()
        {
            Color = glm::vec3(0);
            Range = 0;
        }
    };
    struct SpotLight : SpotLightComponent
    {
        glm::vec3 Position {0}, Direction {0, 0, -1};
        UUID      EntityID = 0;
    };

    struct PipLineState
    {
        bool EnableOutline       = false;
        bool EnableStencil       = false;
        bool EnableDebugNormal   = false;
        bool EnableDefaultShader = true;
        bool EnableGridShader    = true;
    };

    struct RenderResource
    {
        PointLight   pointLight;
        DirLight     dirLight;
        PipLineState Piplinestate;
    };

} // namespace Kans
