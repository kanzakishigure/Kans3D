#pragma once
#include "SecondaryMotion.h"
namespace Kans
{
    struct MotionColliderComponent
    {
        Physics::MotionColliderShape Shape  = Physics::Sphere {};
        bool                         Inside = false;
    };
    bool        operator==(const MotionColliderComponent&, const MotionColliderComponent&);
    inline bool operator!=(const MotionColliderComponent& a, const MotionColliderComponent& b) { return !(a == b); }
} // namespace Kans
