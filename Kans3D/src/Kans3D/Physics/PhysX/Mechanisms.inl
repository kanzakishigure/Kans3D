// Included inside PhysicsWorld::Impl; SDK types remain private to this backend.
struct Controller
{
    PxController*                      Native = nullptr;
    CharacterControllerComponent       Settings;
    CharacterControllerState           State;
    PxTransform                        PlatformPose {PxIdentity};
    std::vector<ControllerColliderHit> Hits;
};
struct JointRecord
{
    PxJoint*       Native = nullptr;
    JointComponent Settings;
    JointState     State;
    bool           Captured = false;
};
PxControllerManager*           ControllerManager = nullptr;
std::map<UUID, Controller>     Controllers;
std::map<UUID, JointRecord>    Joints;
std::map<UUID, std::set<UUID>> BodyJoints;
std::set<UUID>                 DirtyJoints, RemovedJoints;
UUID                           MovingController = 0;
std::vector<JointBreakEvent>   BrokenEvents;
struct ControllerFilter : PxQueryFilterCallback, PxControllerFilterCallback
{
    Impl&                               World;
    UUID                                ID;
    const CharacterControllerComponent& Settings;
    UUID                                IgnoreBody = 0;
    ControllerFilter(Impl& w, UUID id, const CharacterControllerComponent& c) : World(w), ID(id), Settings(c) {}
    bool Accept(PxFilterData f) const
    {
        return (f.word0 & Settings.CollisionMask & World.Settings.CollisionMatrix[Settings.Layer]) &&
               (f.word1 & (1u << Settings.Layer));
    }
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* s, const PxRigidActor* a, PxHitFlags&) override
    {
        if (Identity(a->userData) == ID || (uint64_t(IgnoreBody) && Identity(a->userData) == IgnoreBody) ||
            (s->getFlags() & PxShapeFlag::eTRIGGER_SHAPE) || !Accept(s->getQueryFilterData()))
            return PxQueryHitType::eNONE;
        return PxQueryHitType::eBLOCK;
    }
    PxQueryHitType::Enum
    postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override
    {
        return PxQueryHitType::eBLOCK;
    }
    bool filter(const PxController& a, const PxController& b) override
    {
        const auto other = World.Controllers.find(Identity((Identity(a.getUserData()) == ID ? b : a).getUserData()));
        if (other == World.Controllers.end())
            return false;
        const auto& c = other->second.Settings;
        return (Settings.CollisionMask & (1u << c.Layer)) && (c.CollisionMask & (1u << Settings.Layer)) &&
               World.Settings.LayersCollide(Settings.Layer, c.Layer);
    }
};
PxControllerBehaviorFlags getBehaviorFlags(const PxShape&, const PxActor&) override
{
    return PxControllerBehaviorFlag::eCCT_USER_DEFINED_RIDE;
}
PxControllerBehaviorFlags getBehaviorFlags(const PxController&) override
{
    return PxControllerBehaviorFlag::eCCT_USER_DEFINED_RIDE;
}
PxControllerBehaviorFlags getBehaviorFlags(const PxObstacle&) override
{
    return PxControllerBehaviorFlag::eCCT_USER_DEFINED_RIDE;
}
void onObstacleHit(const PxControllerObstacleHit&) override {}
void onControllerHit(const PxControllersHit& hit) override
{
    auto found = Controllers.find(MovingController);
    if (found == Controllers.end())
        return;
    auto id = Identity(hit.other->getUserData());
    found->second.Hits.push_back({id,
                                  id,
                                  {float(hit.worldPos.x), float(hit.worldPos.y), float(hit.worldPos.z)},
                                  Gl(hit.worldNormal),
                                  Gl(hit.dir),
                                  hit.length});
}
void onShapeHit(const PxControllerShapeHit& hit) override
{
    auto found = Controllers.find(MovingController);
    if (found == Controllers.end() || !hit.shape || !hit.actor)
        return;
    auto&                 c = found->second;
    ControllerColliderHit h {Identity(hit.shape->userData),
                             Identity(hit.actor->userData),
                             {float(hit.worldPos.x), float(hit.worldPos.y), float(hit.worldPos.z)},
                             Gl(hit.worldNormal),
                             Gl(hit.dir),
                             hit.length};
    c.Hits.push_back(h);
    const auto up = glm::normalize(c.Settings.UpDirection);
    if (glm::dot(h.Normal, up) >= std::cos(glm::radians(c.Settings.SlopeLimit)) - 1e-4f)
    {
        c.State.GroundNormal         = h.Normal;
        c.State.GroundPoint          = h.Point;
        c.State.GroundColliderEntity = h.ColliderEntity;
        c.State.GroundBodyEntity     = h.BodyEntity;
    }
    auto* dynamic = hit.actor->is<PxRigidDynamic>();
    if (c.Settings.PushRigidBodies && dynamic && !(dynamic->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
    {
        // Impulse is independent of the number of contact reports for the same shape in a move.
        const bool already = std::any_of(
            c.Hits.begin(), c.Hits.end() - 1, [&](const auto& old) { return old.BodyEntity == h.BodyEntity; });
        auto push = -h.Normal;
        if (!already && glm::dot(push, push) > 1e-8f && glm::dot(push, h.MoveDirection) > 0)
            PxRigidBodyExt::addForceAtPos(
                *dynamic,
                Px(glm::normalize(push) * c.Settings.Mass * c.Settings.PushForceScale * hit.length),
                Px(h.Point),
                PxForceMode::eIMPULSE);
    }
}
void ReleaseJoint(JointRecord& j)
{
    if (j.Native)
    {
        j.Native->release();
        j.Native = nullptr;
    }
}
void DetachBodyJoints(UUID id)
{
    if (auto found = BodyJoints.find(id); found != BodyJoints.end())
        for (auto joint : found->second)
            if (auto j = Joints.find(joint); j != Joints.end())
            {
                ReleaseJoint(j->second);
                DirtyJoints.insert(joint);
            }
}
void RemoveJoint(UUID id)
{
    if (auto j = Joints.find(id); j != Joints.end())
    {
        ReleaseJoint(j->second);
        for (auto body : {j->second.State.BodyA, j->second.State.BodyB})
            BodyJoints[body].erase(id);
        Joints.erase(j);
    }
}
void ReleaseBody(UUID id)
{
    DetachBodyJoints(id);
    auto b = Bodies.find(id);
    if (b == Bodies.end())
        return;
    if (auto c = Controllers.find(id); c != Controllers.end())
    {
        c->second.Native->release();
        Controllers.erase(c);
    }
    else
        b->second.Actor->release();
    --Sdk().Actors;
    Bodies.erase(b);
}
void ControllerFilters(UUID id, Controller& c)
{
    auto* actor = c.Native->getActor();
    actor->setRigidBodyFlag(PxRigidBodyFlag::eUSE_KINEMATIC_TARGET_FOR_SCENE_QUERIES, true);
    actor->userData = reinterpret_cast<void*>(uintptr_t(uint64_t(id)));
    std::vector<PxShape*> shapes(actor->getNbShapes());
    actor->getShapes(shapes.data(), PxU32(shapes.size()));
    for (auto* s : shapes)
    {
        s->userData = actor->userData;
        PxFilterData f(
            1u << c.Settings.Layer, Settings.CollisionMatrix[c.Settings.Layer] & c.Settings.CollisionMask, 4, 1);
        s->setSimulationFilterData(f);
        s->setQueryFilterData(f);
    }
    World->resetFiltering(*actor);
}
void BuildController(Entity e)
{
    const auto id       = e.GetUUID();
    auto       settings = e.GetComponent<CharacterControllerComponent>();
    try
    {
        PhysicsYaml::Encode(settings);
    }
    catch (const std::exception& ex)
    {
        Error(id, ex.what());
        return;
    }
    if (e.HasComponent<RigidBodyComponent>())
        Error(id, "RigidBodyComponent and CharacterControllerComponent conflict; controller wins");
    auto found = Controllers.find(id);
    if (found != Controllers.end() && found->second.Settings.Shape != settings.Shape)
    {
        InvalidatePairs(id);
        ReleaseBody(id);
        found = Controllers.end();
    }
    auto w = Physics::TransformShape({}, Owner.GetWorldTransform(e));
    if (found == Controllers.end())
    {
        if (Bodies.count(id))
            ReleaseBody(id);
        auto*                   material = Material({});
        PxCapsuleControllerDesc capsule;
        PxBoxControllerDesc     box;
        capsule.radius         = settings.Radius;
        capsule.height         = settings.Height;
        capsule.climbingMode   = PxCapsuleClimbingMode::eCONSTRAINED;
        box.halfHeight         = settings.HalfExtents.y;
        box.halfSideExtent     = settings.HalfExtents.x;
        box.halfForwardExtent  = settings.HalfExtents.z;
        PxControllerDesc& desc = settings.Shape == ControllerShape::Capsule ? static_cast<PxControllerDesc&>(capsule) :
                                                                              static_cast<PxControllerDesc&>(box);
        auto              center = w.Position + settings.Center;
        desc.position            = {center.x, center.y, center.z};
        desc.upDirection         = Px(glm::normalize(settings.UpDirection));
        desc.slopeLimit          = std::max(0.f, std::cos(glm::radians(settings.SlopeLimit)));
        desc.stepOffset          = settings.StepOffset;
        desc.contactOffset       = settings.SkinWidth;
        desc.material            = material;
        desc.nonWalkableMode     = PxControllerNonWalkableMode::Enum(settings.NonWalkable);
        desc.reportCallback      = this;
        desc.behaviorCallback    = this;
        desc.scaleCoeff          = 1;
        desc.userData            = reinterpret_cast<void*>(uintptr_t(uint64_t(id)));
        auto* native             = ControllerManager->createController(desc);
        material->release();
        if (!native)
        {
            Error(id, "Cannot create character controller");
            return;
        }
        auto& c    = Controllers[id];
        c.Native   = native;
        c.Settings = settings;
        Body b;
        b.Actor         = native->getActor();
        b.Generation    = ++Stats.ActorCreationCount;
        b.Explicit      = true;
        b.Settings.Type = RigidBodyType::Kinematic;
        b.Settings.Mass = settings.Mass;
        b.Previous      = Pose(w);
        b.Current       = Pose(w);
        b.Authored      = Pose(w);
        b.Scale         = w.Scale;
        Bodies.emplace(id, b);
        ++Sdk().Actors;
        ++Stats.RebuildCount;
        ControllerFilters(id, c);
    }
    else
    {
        auto& c      = found->second;
        auto  center = c.Native->getPosition();
        auto  position =
            glm::vec3(float(center.x), float(center.y), float(center.z)) - c.Settings.Center + settings.Center;
        c.Native->setUpDirection(Px(glm::normalize(settings.UpDirection)));
        c.Native->setPosition({position.x, position.y, position.z});
        c.Native->setSlopeLimit(std::max(0.f, std::cos(glm::radians(settings.SlopeLimit))));
        c.Native->setNonWalkableMode(PxControllerNonWalkableMode::Enum(settings.NonWalkable));
        c.Native->setStepOffset(settings.StepOffset);
        c.Native->setContactOffset(settings.SkinWidth);
        if (settings.Shape == ControllerShape::Capsule)
        {
            auto* n = static_cast<PxCapsuleController*>(c.Native);
            n->setRadius(settings.Radius);
            n->setHeight(settings.Height);
        }
        else
        {
            auto* n = static_cast<PxBoxController*>(c.Native);
            n->setHalfHeight(settings.HalfExtents.y);
            n->setHalfSideExtent(settings.HalfExtents.x);
            n->setHalfForwardExtent(settings.HalfExtents.z);
        }
        c.Settings = settings;
        c.State    = {};
        ControllerFilters(id, c);
        c.Native->invalidateCache();
        Bodies.at(id).Settings.Mass = settings.Mass;
    }
    ColliderOwners[id] = id;
}
void ControllerPose(UUID id, Controller& c)
{
    auto& b                          = Bodies.at(id);
    auto  p                          = c.Native->getPosition();
    b.Current.p                      = Px(glm::vec3(float(p.x), float(p.y), float(p.z)) - c.Settings.Center);
    b.Previous                       = b.Current;
    auto       e                     = Owner.GetEntityByUUID(id);
    const bool writing               = Owner.m_PhysicsWritingTransforms;
    Owner.m_PhysicsWritingTransforms = true;
    Owner.SetWorldTransform(e,
                            glm::translate(glm::mat4(1), Gl(b.Current.p)) * glm::mat4_cast(Gl(b.Current.q)) *
                                glm::scale(glm::mat4(1), b.Scale));
    Owner.m_PhysicsWritingTransforms = writing;
}
bool ControllerGeometry(const Controller& c, float height, PxGeometryHolder& g, PxTransform& pose) const
{
    if (!std::isfinite(height) || height <= 0)
        return false;
    const auto& s      = c.Settings;
    const auto  up     = glm::normalize(s.UpDirection);
    const auto  p      = c.Native->getPosition();
    auto        center = glm::vec3(float(p.x), float(p.y), float(p.z));
    if (s.Shape == ControllerShape::Capsule)
    {
        center += up * ((height - s.Height) * .5f);
        g.storeAny(PxCapsuleGeometry(s.Radius, height * .5f));
        pose = {Px(center), Px(glm::rotation(glm::vec3(1, 0, 0), up))};
    }
    else
    {
        center += up * ((height - 2 * s.HalfExtents.y) * .5f);
        g.storeAny(PxBoxGeometry(height * .5f, s.HalfExtents.x, s.HalfExtents.z));
        pose = {Px(center), PxShortestRotation(PxVec3(1, 0, 0), Px(up))};
    }
    return true;
}
PxSweepBuffer GroundProbe(UUID id, Controller& c, float distance, bool walkableOnly = false)
{
    PxGeometryHolder g;
    PxTransform      pose(PxIdentity);
    ControllerGeometry(
        c, c.Settings.Shape == ControllerShape::Capsule ? c.Settings.Height : c.Settings.HalfExtents.y * 2, g, pose);
    struct SupportFilter : ControllerFilter
    {
        using ControllerFilter::ControllerFilter;
        bool WalkableOnly = false;
        PxQueryHitType::Enum
        postFilter(const PxFilterData&, const PxQueryHit& hit, const PxShape*, const PxRigidActor*) override
        {
            const auto& sweep = static_cast<const PxSweepHit&>(hit);
            return !WalkableOnly || glm::dot(Gl(sweep.normal), glm::normalize(Settings.UpDirection)) >=
                                        std::cos(glm::radians(Settings.SlopeLimit)) - 1e-4f ?
                       PxQueryHitType::eBLOCK :
                       PxQueryHitType::eNONE;
        }
    };
    SupportFilter filter(*this, id, c.Settings);
    filter.WalkableOnly = walkableOnly;
    PxSweepBuffer hit;
    World->sweep(g.any(),
                 pose,
                 -Px(glm::normalize(c.Settings.UpDirection)),
                 distance,
                 hit,
                 PxHitFlag::eDEFAULT,
                 PxQueryFilterData(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER |
                                   PxQueryFlag::ePOSTFILTER),
                 &filter);
    return hit;
}
CollisionFlags MoveCharacter(UUID id, glm::vec3 displacement, float dt, bool platformMove = false)
{
    auto found = Controllers.find(id);
    if (found == Controllers.end())
        return CollisionFlags::None;
    auto&      c           = found->second;
    const auto before      = c.Native->getPosition();
    const bool wasGrounded = c.State.IsGrounded;
    const UUID platform    = platformMove ? c.State.GroundBodyEntity : UUID(0);
    c.State                = {};
    c.Hits.clear();
    MovingController = id;
    ControllerFilter filter(*this, id, c.Settings);
    filter.IgnoreBody = platform;
    if (platformMove)
        c.Native->invalidateCache();
    PxControllerFilters filters(nullptr, &filter, &filter);
    auto                flags = c.Native->move(Px(displacement), c.Settings.MinMoveDistance, dt, filters);
    const auto          up    = glm::normalize(c.Settings.UpDirection);
    const bool snap = !platformMove && wasGrounded && c.Settings.SnapDistance > 0 && glm::dot(displacement, up) <= 0;
    if (snap)
    {
        auto ground = GroundProbe(id, c, c.Settings.SnapDistance + c.Settings.SkinWidth, true);
        if (ground.hasBlock &&
            glm::dot(Gl(ground.block.normal), up) >= std::cos(glm::radians(c.Settings.SlopeLimit)) - 1e-4f)
        {
            float distance = std::max(0.f, ground.block.distance - c.Settings.SkinWidth);
            if (distance > c.Settings.MinMoveDistance)
                flags |= c.Native->move(-Px(up) * distance, c.Settings.MinMoveDistance, dt, filters);
        }
    }
    // A short support probe preserves grounded state on zero/horizontal moves without applying gravity.
    auto ground = GroundProbe(id, c, c.Settings.SkinWidth + .015f);
    // At a stair lip the capsule can rest on the edge before its foot reaches the lower tread.
    // Retain snap support through that transition so the next move continues descending.
    if (snap && (!ground.hasBlock ||
                 glm::dot(Gl(ground.block.normal), up) < std::cos(glm::radians(c.Settings.SlopeLimit)) - 1e-4f))
        ground = GroundProbe(id, c, c.Settings.SnapDistance + c.Settings.SkinWidth, true);
    if ((platformMove || glm::dot(displacement, up) <= 0) && ground.hasBlock &&
        glm::dot(Gl(ground.block.normal), up) >= std::cos(glm::radians(c.Settings.SlopeLimit)) - 1e-4f)
    {
        c.State.IsGrounded = true;
        flags |= PxControllerCollisionFlag::eCOLLISION_DOWN;
        c.State.GroundNormal         = Gl(ground.block.normal);
        c.State.GroundPoint          = Gl(ground.block.position);
        c.State.GroundColliderEntity = Identity(ground.block.shape->userData);
        c.State.GroundBodyEntity     = Identity(ground.block.actor->userData);
    }
    else
    {
        flags &= ~PxControllerCollisionFlags(PxControllerCollisionFlag::eCOLLISION_DOWN);
        c.State.GroundColliderEntity = 0;
        c.State.GroundBodyEntity     = 0;
        c.State.GroundNormal         = {};
        c.State.GroundPoint          = {};
    }
    c.State.Flags    = CollisionFlags(uint32_t(flags));
    auto after       = c.Native->getPosition();
    c.State.Velocity = glm::vec3(float(after.x - before.x), float(after.y - before.y), float(after.z - before.z)) / dt;
    if (auto platform = Bodies.find(c.State.GroundBodyEntity); platform != Bodies.end())
        c.PlatformPose = platform->second.Current;
    ControllerPose(id, c);
    MovingController = 0;
    return c.State.Flags;
}
void FollowPlatforms(float dt)
{
    for (auto& [id, c] : Controllers)
    {
        if (!c.Settings.FollowPlatforms || !c.State.IsGrounded)
            continue;
        auto b = Bodies.find(c.State.GroundBodyEntity);
        if (b == Bodies.end())
        {
            c.State = {};
            continue;
        }
        if (b->second.Settings.Type == RigidBodyType::Static)
            continue;
        auto pose  = b->second.Current;
        auto delta = Gl(pose.q * c.PlatformPose.q.getConjugate());
        auto up    = glm::normalize(c.Settings.UpDirection);
        // Extract twist about the character's up axis (yaw), excluding platform pitch/roll.
        glm::vec3 v(delta.x, delta.y, delta.z);
        v = up * glm::dot(v, up);
        glm::quat yaw(delta.w, v.x, v.y, v.z);
        yaw           = glm::length(yaw) > 1e-6f ? glm::normalize(yaw) : glm::quat(1, 0, 0, 0);
        auto position = Gl(Bodies.at(id).Current.p);
        auto target   = Gl(pose.p) + yaw * (position - Gl(c.PlatformPose.p));
        if (glm::length(target - position) > 1e-7f)
            MoveCharacter(id, target - position, dt, true);
        auto& body     = Bodies.at(id);
        body.Current.q = Px(yaw * Gl(body.Current.q));
        c.PlatformPose = pose;
        ControllerPose(id, c);
    }
}
void UpdateJoint(JointRecord& r)
{
    auto* j = r.Native;
    if (!j)
        return;
    const auto& c = r.Settings;
    j->setBreakForce(c.BreakForce, c.BreakTorque);
    j->setConstraintFlag(PxConstraintFlag::eCOLLISION_ENABLED, c.EnableCollision);
    j->setConstraintFlag(PxConstraintFlag::eDRIVE_LIMITS_ARE_FORCES, true);
    const bool extended = c.Type == JointType::Hinge ?
                              c.HingeLimitEnabled && (c.HingeLowerAngle < -180 || c.HingeUpperAngle > 180) :
                              c.Type == JointType::D6 && c.D6Motion[3] == JointMotion::Limited &&
                                  (c.D6TwistLower < -180 || c.D6TwistUpper > 180);
    j->setConstraintFlag(PxConstraintFlag::eENABLE_EXTENDED_LIMITS, extended);
    j->setInvMassScale0(c.InvMassScaleA);
    j->setInvMassScale1(c.InvMassScaleB);
    j->setInvInertiaScale0(c.InvInertiaScaleA);
    j->setInvInertiaScale1(c.InvInertiaScaleB);
    auto angular = [](float low, float high, float stiffness, float damping) {
        PxJointAngularLimitPair l(glm::radians(low), glm::radians(high));
        l.stiffness = stiffness;
        l.damping   = damping;
        return l;
    };
    auto linear = [&](float low, float high, float stiffness, float damping) {
        PxJointLinearLimitPair l(Sdk().Physics->getTolerancesScale(), low, high);
        l.stiffness = stiffness;
        l.damping   = damping;
        return l;
    };
    auto cone = [](float y, float z, float stiffness, float damping) {
        PxJointLimitCone l(glm::radians(y), glm::radians(z));
        l.stiffness = stiffness;
        l.damping   = damping;
        return l;
    };
    if (auto* d = j->is<PxD6Joint>())
    {
        const bool slerp = c.Type == JointType::D6 && c.SlerpDrive;
        d->setAngularDriveConfig(slerp ? PxD6AngularDriveConfig::eSLERP : PxD6AngularDriveConfig::eSWING_TWIST);
        for (unsigned a = 0; a < 6; ++a)
            d->setMotion(PxD6Axis::Enum(a), PxD6Motion::eLOCKED);
        for (auto axis : {PxD6Drive::eX, PxD6Drive::eY, PxD6Drive::eZ})
            d->setDrive(axis, PxD6JointDrive());
        if (slerp)
            d->setDrive(PxD6Drive::eSLERP, PxD6JointDrive());
        else
            for (auto axis : {PxD6Drive::eTWIST, PxD6Drive::eSWING1, PxD6Drive::eSWING2})
                d->setDrive(axis, PxD6JointDrive());
        if (c.Type == JointType::Hinge)
        {
            d->setMotion(PxD6Axis::eTWIST, c.HingeLimitEnabled ? PxD6Motion::eLIMITED : PxD6Motion::eFREE);
            d->setTwistLimit(angular(c.HingeLowerAngle, c.HingeUpperAngle, c.HingeLimitStiffness, c.HingeLimitDamping));
            if (c.HingeSpringEnabled || c.HingeMotorEnabled)
            {
                // Velocity motor uses a high damping drive, clamped to the authored force.
                float damping = c.HingeSpringEnabled ? c.HingeSpringDamping : 1e6f;
                if (c.HingeFreeSpin && c.HingeMotorEnabled)
                {
                    float velocity = d->getRelativeAngularVelocity().x;
                    if (velocity * c.HingeMotorVelocity > 0 && std::abs(velocity) > std::abs(c.HingeMotorVelocity))
                        damping = 0;
                }
                d->setDrive(
                    PxD6Drive::eTWIST,
                    PxD6JointDrive(c.HingeSpringEnabled ? c.HingeSpringStiffness : 0, damping, c.HingeMotorMaxForce));
                d->setDrivePosition(PxTransform(PxQuat(glm::radians(c.HingeTargetAngle), PxVec3(1, 0, 0))));
                d->setDriveVelocity(PxVec3(0), PxVec3(c.HingeMotorEnabled ? -c.HingeMotorVelocity : 0, 0, 0));
            }
        }
        else if (c.Type == JointType::Slider)
        {
            d->setMotion(PxD6Axis::eX, c.SliderLimitEnabled ? PxD6Motion::eLIMITED : PxD6Motion::eFREE);
            d->setLinearLimit(
                PxD6Axis::eX,
                linear(c.SliderLowerLimit, c.SliderUpperLimit, c.SliderLimitStiffness, c.SliderLimitDamping));
            if (c.SliderDriveEnabled)
            {
                d->setDrive(PxD6Drive::eX,
                            PxD6JointDrive(c.SliderDriveStiffness, c.SliderDriveDamping, c.SliderDriveMaxForce));
                d->setDrivePosition(PxTransform(PxVec3(c.SliderTargetPosition, 0, 0)));
                d->setDriveVelocity(PxVec3(c.SliderTargetVelocity, 0, 0), PxVec3(0));
            }
        }
        else
        {
            for (unsigned a = 0; a < 6; ++a)
                d->setMotion(PxD6Axis::Enum(a), PxD6Motion::Enum(c.D6Motion[a]));
            for (unsigned a = 0; a < 3; ++a)
                d->setLinearLimit(
                    PxD6Axis::Enum(a),
                    linear(c.D6LinearLower[a], c.D6LinearUpper[a], c.D6LinearStiffness, c.D6LinearDamping));
            d->setTwistLimit(angular(c.D6TwistLower, c.D6TwistUpper, c.D6AngularStiffness, c.D6AngularDamping));
            d->setSwingLimit(cone(c.D6SwingY, c.D6SwingZ, c.D6AngularStiffness, c.D6AngularDamping));
            // Public drive slots are X/Y/Z/Swing/Twist/Slerp. PhysX exposes two separate swing drives.
            auto drive = [&](PxD6Drive::Enum axis, unsigned slot) {
                d->setDrive(axis, PxD6JointDrive(c.DriveStiffness[slot], c.DriveDamping[slot], c.DriveMaxForce[slot]));
            };
            for (unsigned a = 0; a < 3; ++a)
                drive(PxD6Drive::Enum(a), a);
            if (slerp)
                drive(PxD6Drive::eSLERP, 5);
            else
            {
                drive(PxD6Drive::eSWING1, 3);
                drive(PxD6Drive::eSWING2, 3);
                drive(PxD6Drive::eTWIST, 4);
            }
            d->setDrivePosition(PxTransform(Px(c.DrivePosition), Px(glm::normalize(c.DriveRotation))));
            // PhysX swing/twist velocity rows use the opposite sign to its SLERP rows.
            // Both public modes express the derivative of frame B relative to frame A.
            d->setDriveVelocity(Px(c.DriveLinearVelocity),
                                Px(slerp ? c.DriveAngularVelocity : -c.DriveAngularVelocity));
        }
    }
    else if (auto* ball = j->is<PxSphericalJoint>())
    {
        ball->setLimitCone(
            cone(c.SphericalYAngle, c.SphericalZAngle, c.SphericalLimitStiffness, c.SphericalLimitDamping));
        ball->setSphericalJointFlag(PxSphericalJointFlag::eLIMIT_ENABLED, c.SphericalLimitEnabled);
    }
    else if (auto* distance = j->is<PxDistanceJoint>())
    {
        distance->setMinDistance(c.DistanceMin);
        distance->setMaxDistance(c.DistanceMax);
        distance->setStiffness(c.DistanceStiffness);
        distance->setDamping(c.DistanceDamping);
        distance->setTolerance(c.DistanceTolerance);
        distance->setDistanceJointFlag(PxDistanceJointFlag::eMIN_DISTANCE_ENABLED, c.DistanceMinEnabled);
        distance->setDistanceJointFlag(PxDistanceJointFlag::eMAX_DISTANCE_ENABLED, c.DistanceMaxEnabled);
        distance->setDistanceJointFlag(PxDistanceJointFlag::eSPRING_ENABLED,
                                       c.DistanceStiffness > 0 || c.DistanceDamping > 0);
    }
    PxRigidActor* actors[2];
    j->getActors(actors[0], actors[1]);
    for (auto* actor : actors)
        if (actor)
            if (auto* d = actor->is<PxRigidDynamic>(); d && !(d->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
                d->wakeUp();
}
void RefreshJoint(UUID id)
{
    auto e = Owner.GetEntityByUUID(id);
    if (!Valid(e) || !e.HasComponent<JointComponent>())
    {
        RemoveJoint(id);
        return;
    }
    if (!Owner.IsEntityActive(e))
    {
        if (auto j = Joints.find(id); j != Joints.end())
            ReleaseJoint(j->second);
        return;
    }
    auto c = e.GetComponent<JointComponent>();
    try
    {
        PhysicsYaml::Encode(c);
    }
    catch (const std::exception& ex)
    {
        Error(id, ex.what());
        if (auto old = Joints.find(id); old != Joints.end())
            ReleaseJoint(old->second);
        return;
    }
    UUID a = c.BodyA, b = c.BodyB;
    if (!uint64_t(a))
        for (auto p = e; p; p = Owner.GetParent(p))
            if (p.HasComponent<RigidBodyComponent>())
            {
                a = p.GetUUID();
                break;
            }
    auto actor = [&](UUID body) -> PxRigidActor* {
        if (!uint64_t(body))
            return nullptr;
        auto entity = Owner.GetEntityByUUID(body);
        auto found  = Bodies.find(body);
        return Valid(entity) && Owner.IsEntityActive(entity) && entity.HasComponent<RigidBodyComponent>() &&
                       !Controllers.count(body) && found != Bodies.end() ?
                   found->second.Actor :
                   nullptr;
    };
    auto* aa            = actor(a);
    auto* bb            = actor(b);
    auto [it, inserted] = Joints.try_emplace(id);
    auto& r             = it->second;
    bool  changedBodies = r.State.BodyA != a || r.State.BodyB != b;
    if (changedBodies)
    {
        for (auto old : {r.State.BodyA, r.State.BodyB})
            BodyJoints[old].erase(id);
        r.State.BodyA = a;
        r.State.BodyB = b;
        r.Captured    = false;
        ReleaseJoint(r);
    }
    BodyJoints[a].insert(id);
    if (uint64_t(b))
        BodyJoints[b].insert(id);
    if (!aa || (uint64_t(b) && !bb) || a == b || (!aa->is<PxRigidDynamic>() && (!bb || !bb->is<PxRigidDynamic>())))
    {
        ReleaseJoint(r);
        Error(id, "Joint requires distinct existing bodies and at least one non-static body");
        return;
    }
    if (!inserted && r.Settings.Type != c.Type)
        ReleaseJoint(r);
    const bool collisionChanged = !inserted && r.Settings.EnableCollision != c.EnableCollision;
    r.Settings                  = c;
    if (!r.Captured || ChangedTransforms.count(id))
    {
        // Recapture only for authored joint transforms, never because a body's actor was replaced.
        const auto frame             = Pose(Physics::TransformShape({}, Owner.GetWorldTransform(e)));
        auto       localA            = aa->getGlobalPose().getInverse() * frame;
        auto       localB            = bb ? bb->getGlobalPose().getInverse() * frame : frame;
        r.State.LocalFrameA.Position = Gl(localA.p);
        r.State.LocalFrameA.Rotation = Gl(localA.q);
        r.State.LocalFrameB.Position = Gl(localB.p);
        r.State.LocalFrameB.Rotation = Gl(localB.q);
        r.Captured                   = true;
        if (r.Native)
        {
            r.Native->setLocalPose(PxJointActorIndex::eACTOR0, localA);
            r.Native->setLocalPose(PxJointActorIndex::eACTOR1, localB);
        }
    }
    if (r.State.IsBroken)
        return;
    if (!r.Native)
    {
        auto fa = Pose(r.State.LocalFrameA), fb = Pose(r.State.LocalFrameB);
        switch (c.Type)
        {
            case JointType::Fixed:
                r.Native = PxFixedJointCreate(*Sdk().Physics, aa, fa, bb, fb);
                break;
            case JointType::Hinge:
            case JointType::Slider:
            case JointType::D6:
                r.Native = PxD6JointCreate(*Sdk().Physics, aa, fa, bb, fb);
                break;
            case JointType::Spherical:
                r.Native = PxSphericalJointCreate(*Sdk().Physics, aa, fa, bb, fb);
                break;
            case JointType::Distance:
                r.Native = PxDistanceJointCreate(*Sdk().Physics, aa, fa, bb, fb);
                break;
        }
        if (!r.Native)
        {
            Error(id, "Joint creation failed");
            return;
        }
        r.Native->userData = reinterpret_cast<void*>(uintptr_t(uint64_t(id)));
    }
    UpdateJoint(r);
    if (collisionChanged)
    {
        World->resetFiltering(*aa);
        if (bb)
            World->resetFiltering(*bb);
    }
}
void CollectJointStates()
{
    for (auto& [id, r] : Joints)
    {
        if (!r.Native || r.State.IsBroken)
            continue;
        PxVec3 force(0), torque(0);
        r.Native->getConstraint()->getForce(force, torque);
        r.State.CurrentForce  = Gl(force);
        r.State.CurrentTorque = Gl(torque);
        if (auto* d = r.Native->is<PxD6Joint>())
        {
            const auto angle        = glm::degrees(d->getTwistAngle());
            r.State.CurrentAngle    = (d->getConstraintFlags() & PxConstraintFlag::eENABLE_EXTENDED_LIMITS) ?
                                          angle :
                                          std::remainder(angle, 360.f);
            r.State.CurrentPosition = d->getRelativeTransform().p.x;
        }
    }
}
void CollectBreaks(PxConstraintInfo* constraints, PxU32 count)
{
    std::lock_guard lock(EventMutex);
    for (PxU32 n = 0; n < count; ++n)
    {
        if (constraints[n].type != PxConstraintExtIDs::eJOINT)
            continue;
        auto* native = static_cast<PxJoint*>(constraints[n].externalReference);
        auto  id     = Identity(native->userData);
        auto  found  = Joints.find(id);
        if (found == Joints.end() || found->second.State.IsBroken)
            continue;
        auto& r          = found->second;
        r.State.IsBroken = true;
        // PhysX may zero force after breaking; do not mislabel a stale force as the breaking force.
        PxVec3 f(0), t(0);
        constraints[n].constraint->getForce(f, t);
        r.State.CurrentForce  = Gl(f);
        r.State.CurrentTorque = Gl(t);
        BrokenEvents.push_back(
            {id, r.State.BodyA, r.State.BodyB, Gl(f), Gl(t), f.magnitudeSquared() > 0 || t.magnitudeSquared() > 0});
    }
}
void DispatchMechanisms()
{
    auto breaks = std::move(BrokenEvents);
    BrokenEvents.clear();
    for (const auto& e : breaks)
    {
        auto listeners = Subscribers->JointBreak;
        for (auto& [id, f] : listeners)
            if (Subscribers->JointBreak.count(id))
                f(e);
        ScriptEngine::OnJointBreak(Owner, e);
    }
    for (auto& [id, c] : Controllers)
    {
        for (const auto& h : c.Hits)
        {
            auto listeners = Subscribers->ControllerHit;
            for (auto& [token, f] : listeners)
                if (Subscribers->ControllerHit.count(token))
                    f(id, h);
            ScriptEngine::OnControllerHit(Owner, id, h);
        }
        c.Hits.clear();
    }
}
void DrawJoints(DebugDraw& draw) const
{
    for (const auto& [id, j] : Joints)
    {
        if (!j.Native)
            continue;
        PxRigidActor* a;
        PxRigidActor* b;
        j.Native->getActors(a, b);
        auto fa    = (a ? a->getGlobalPose() : PxTransform(PxIdentity)) * Pose(j.State.LocalFrameA);
        auto fb    = (b ? b->getGlobalPose() : PxTransform(PxIdentity)) * Pose(j.State.LocalFrameB);
        auto color = j.State.IsBroken ? glm::vec4(1, 0, 0, 1) : glm::vec4(1, .8f, .1f, 1);
        for (const auto& f : {fa, fb})
        {
            draw.Arrow(Gl(f.p), Gl(f.transform(PxVec3(.4f, 0, 0))), {1, 0, 0, 1});
            draw.Arrow(Gl(f.p), Gl(f.transform(PxVec3(0, .4f, 0))), {0, 1, 0, 1});
            draw.Arrow(Gl(f.p), Gl(f.transform(PxVec3(0, 0, .4f))), {0, .4f, 1, 1});
        }
        draw.Line(Gl(fa.p), Gl(fb.p), color);
        const auto& c = j.Settings;
        if (c.Type == JointType::Hinge || c.Type == JointType::D6)
        {
            float low  = glm::radians(c.Type == JointType::Hinge ? c.HingeLowerAngle : c.D6TwistLower);
            float high = glm::radians(c.Type == JointType::Hinge ? c.HingeUpperAngle : c.D6TwistUpper);
            for (int n = 0; n < 32; ++n)
            {
                float t = glm::mix(low, high, float(n) / 32), u = glm::mix(low, high, float(n + 1) / 32);
                draw.Line(Gl(fa.transform(PxVec3(0, .6f * std::cos(t), .6f * std::sin(t)))),
                          Gl(fa.transform(PxVec3(0, .6f * std::cos(u), .6f * std::sin(u)))),
                          color);
            }
        }
        if (c.Type == JointType::Spherical || c.Type == JointType::D6)
        {
            float y     = glm::radians(c.Type == JointType::Spherical ? c.SphericalYAngle : c.D6SwingY);
            float z     = glm::radians(c.Type == JointType::Spherical ? c.SphericalZAngle : c.D6SwingZ);
            auto  point = [&](float angle) {
                auto swing = glm::angleAxis(y * std::sin(angle), glm::vec3(0, 1, 0)) *
                             glm::angleAxis(z * std::cos(angle), glm::vec3(0, 0, 1));
                return Gl(fa.transform(Px(swing * glm::vec3(.6f, 0, 0))));
            };
            for (int n = 0; n < 32; ++n)
            {
                draw.Line(point(n * glm::two_pi<float>() / 32), point((n + 1) * glm::two_pi<float>() / 32), color);
                if (n % 8 == 0)
                    draw.Line(Gl(fa.p), point(n * glm::two_pi<float>() / 32), color);
            }
        }
        if (c.Type == JointType::Slider)
            draw.Line(Gl(fa.transform(PxVec3(c.SliderLowerLimit, 0, 0))),
                      Gl(fa.transform(PxVec3(c.SliderUpperLimit, 0, 0))),
                      color);
        if (c.Type == JointType::Distance)
            draw.Sphere(Gl(fb.p), c.DistanceMax, color);
    }
}
