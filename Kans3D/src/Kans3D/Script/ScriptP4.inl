// Included inside the physics Command dispatcher, after scene/world validation.
if (op == 99)
{
    if (!world || !second || mode < 0 || mode > 3)
        return -1;
    world->Explode(Vector(*v), v->W, second->X, FieldFalloff(mode), second->Y, uint32_t(v->Integer));
    return 1;
}
if (op >= 80 && op <= 89)
{
    auto e = scene->GetEntityByUUID(id);
    if (!e || !e.HasComponent<RagdollComponent>())
        return 0;
    auto& c     = e.GetComponent<RagdollComponent>();
    bool  write = mode & 1024;
    int   p     = mode & 1023;
    if (op == 80)
    {
        if (p == 0)
        {
            if (write)
            {
                if (v->Integer < 0 || v->Integer > 2)
                    return -1;
                c.Mode = RagdollMode(v->Integer);
            }
            else
                v->Integer = int(c.Mode);
        }
        else if (p == 1)
        {
            if (write)
            {
                if (!std::isfinite(v->X) || v->X < 0)
                    return -1;
                c.DriveStrength = v->X;
            }
            else
                v->X = c.DriveStrength;
        }
        else if (p == 2)
        {
            if (write)
                c.Enabled = v->Integer != 0;
            else
                v->Integer = c.Enabled;
        }
        else if (p == 3)
        {
            if (write)
            {
                if (v->Integer < 0 || v->Integer > 1)
                    return -1;
                c.RootFollowing = RagdollRootFollowing(v->Integer);
            }
            else
                v->Integer = int(c.RootFollowing);
        }
        else if (p == 4)
        {
            if (write)
                c.FollowYaw = v->Integer != 0;
            else
                v->Integer = c.FollowYaw;
        }
        else if (p == 5 || p == 6)
        {
            float& value = p == 5 ? c.TeleportDistance : c.TeleportRotation;
            if (write)
            {
                if (!std::isfinite(v->X) || v->X < 0 || (p == 6 && v->X > 180))
                    return -1;
                value = v->X;
            }
            else
                v->X = value;
        }
        else
            return -1;
        return 1;
    }
    if (!world)
        return 0;
    if (op == 81)
    {
        world->SimulateRagdoll(id, text ? text : "");
        return 1;
    }
    if (op == 82)
    {
        world->BlendRagdollToAnimation(id, v->X);
        return 1;
    }
    if (op == 83)
    {
        if (!text)
            return -1;
        world->SetRagdollBlendWeight(id, text, v->X, mode != 0);
        return 1;
    }
    if (op == 84)
    {
        if (!text || !second)
            return -1;
        world->AddRagdollImpulse(id, text, Vector(*v), Vector(*second));
        return 1;
    }
    if (op == 85)
    {
        if (!text || mode < 0 || mode > 3)
            return -1;
        world->AddRagdollForce(id, text, Vector(*v), ForceMode(mode));
        return 1;
    }
    if (op == 86)
    {
        if (!text)
            return -1;
        auto state = world->GetRagdollBodyState(id, text);
        if (!state)
            return 0;
        if (p == 0)
            Vector(*v, state->Position);
        else if (p == 1)
        {
            v->X = state->Rotation.x;
            v->Y = state->Rotation.y;
            v->Z = state->Rotation.z;
            v->W = state->Rotation.w;
        }
        else if (p == 2)
            Vector(*v, state->LinearVelocity);
        else
            return -1;
        return 1;
    }
    if (op == 87)
    {
        world->ResetRagdoll(id);
        return 1;
    }
    if (op == 88)
    {
        if (!text || !std::isfinite(v->X) || !std::isfinite(v->Y) || v->X < 0 || v->Y < 0)
            return -1;
        auto it = std::find_if(c.Bodies.begin(), c.Bodies.end(), [&](const auto& b) { return b.Bone == text; });
        if (it == c.Bodies.end())
        {
            c.Bodies.push_back({text});
            it = std::prev(c.Bodies.end());
        }
        it->DriveStrength = v->X;
        it->DriveDamping  = v->Y;
        return 1;
    }
    return -1;
}
if (op >= 90 && op <= 94)
{
    auto e = scene->GetEntityByUUID(id);
    if (!e || !e.HasComponent<VehicleComponent>())
        return 0;
    auto& c     = e.GetComponent<VehicleComponent>();
    bool  write = mode & 1024;
    int   p     = mode & 1023;
    if (op == 90)
    {
        float* value = p == 0 ? &c.Throttle : p == 1 ? &c.Brake : p == 2 ? &c.Steer : p == 3 ? &c.Handbrake : nullptr;
        if (value)
        {
            if (write)
            {
                if (!std::isfinite(v->X))
                    return -1;
                *value = glm::clamp(v->X, p == 0 || p == 2 ? -1.f : 0.f, 1.f);
            }
            else
                v->X = *value;
        }
        else if (p == 4)
        {
            if (write)
            {
                if (v->Integer < -1 || v->Integer > int64_t(c.GearRatios.size()))
                    return -1;
                c.TargetGear = int32_t(v->Integer);
            }
            else
                v->Integer = c.TargetGear;
        }
        else if (p == 5)
        {
            if (write)
                c.AutomaticGearbox = v->Integer != 0;
            else
                v->Integer = c.AutomaticGearbox;
        }
        else
            return -1;
        return 1;
    }
    if (!world)
        return 0;
    auto state = world->GetVehicleState(id);
    if (!state)
        return 0;
    if (op == 91)
    {
        v->X       = state->ForwardSpeed;
        v->Y       = state->EngineRpm;
        v->Z       = state->TotalDriveTorque;
        v->Integer = state->CurrentGear;
        return 1;
    }
    if (v->Integer < 0 || uint64_t(v->Integer) >= state->Wheels.size())
        return -1;
    auto& wheel = state->Wheels[size_t(v->Integer)];
    if (op == 92)
    {
        v->Integer = int64_t(uint64_t(wheel.GroundEntity));
        Vector(*v, wheel.ContactPoint);
        v->W = wheel.Grounded ? 1 : 0;
        if (second)
        {
            second->X = wheel.ContactNormal.x;
            second->Y = wheel.ContactNormal.y;
            second->Z = wheel.ContactNormal.z;
        }
        return 1;
    }
    if (op == 93)
    {
        v->X = wheel.SuspensionCompression;
        v->Y = wheel.LongitudinalSlip;
        v->Z = wheel.LateralSlip;
        v->W = wheel.AngularSpeed;
        return 1;
    }
    return -1;
}
if (op == 95 || op == 96)
{
    auto e = scene->GetEntityByUUID(id);
    if (!e)
        return 0;
    bool write    = mode & 1024;
    int  p        = mode & 1023;
    auto property = [&](auto& c) {
        int  index = 0, result = -1;
        auto field = [&](const char*, auto& value) {
            if (index++ != p)
                return;
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_enum_v<T>)
            {
                if (write)
                    value = T(v->Integer);
                else
                    v->Integer = int64_t(value);
                result = 1;
            }
            else if constexpr (std::is_integral_v<T>)
            {
                if (write)
                    value = T(v->Integer);
                else
                    v->Integer = int64_t(value);
                result = 1;
            }
            else if constexpr (std::is_floating_point_v<T>)
            {
                if (write)
                    value = v->X;
                else
                    v->X = value;
                result = 1;
            }
            else if constexpr (std::is_same_v<T, glm::vec3>)
            {
                if (write)
                    value = Vector(*v);
                else
                    Vector(*v, value);
                result = 1;
            }
        };
        using C   = std::decay_t<decltype(c)>;
        auto next = c;
        if constexpr (std::is_same_v<C, ForceFieldComponent>)
            ForceFieldFields(next, field);
        else
            WindZoneFields(next, field);
        if (write && result > 0)
        {
            PhysicsYaml::Encode(next);
            c = std::move(next);
        }
        return result;
    };
    if (op == 95)
        return e.HasComponent<ForceFieldComponent>() ? property(e.GetComponent<ForceFieldComponent>()) : 0;
    return e.HasComponent<WindZoneComponent>() ? property(e.GetComponent<WindZoneComponent>()) : 0;
}
if (op == 97)
{
    auto e = scene->GetEntityByUUID(id);
    if (!e || !e.HasComponent<RigidBodyComponent>())
        return 0;
    auto& c = e.GetComponent<RigidBodyComponent>();
    if (mode & 1024)
        c.AffectedByFields = v->Integer != 0;
    else
        v->Integer = c.AffectedByFields;
    return 1;
}
