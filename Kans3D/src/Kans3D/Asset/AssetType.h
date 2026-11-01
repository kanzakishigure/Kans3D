#pragma once
#include "Kans3D/Core/Base/Base.h"
namespace Kans
{
    enum class AssetFlag : uint16_t
    {
        None    = 0,
        Missing = BIT(0),
        Invalid = BIT(1)
    };

    enum class AssetType : uint16_t
    {
        None                = 0,
        Scene               = 1,
        Texture             = 2,
        Material            = 3,
        StaticMesh          = 4,
        Mesh                = 5,
        MeshSource          = 6,
        EnvMap              = 7,
        Model               = 8,
        Shader              = 9,
        Skeleton            = 10,
        AnimationClip       = 11,
        AnimationController = 12,
        AudioClip           = 13,
        Font                = 14,
        UI                  = 15,
        Expression          = 16,
        Action              = 17,
        Timeline            = 18,
        PhysicsMaterial     = 19,
        Cloth               = 20,
        Ragdoll             = 21,
        VfxSystem           = 22,
        VfxEmitter          = 23,
        VfxModule           = 24,
    };
} // namespace Kans
