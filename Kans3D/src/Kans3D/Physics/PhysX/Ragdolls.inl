// Included inside PhysicsWorld::Impl; all SDK objects stay owned by the world.
struct Ragdoll
{
    Ref<const RagdollAsset>          Source;
    Ref<const SkeletonAsset>         SkeletonSource;
    RagdollBinding                   Binding;
    PxArticulationReducedCoordinate* Articulation = nullptr;
    std::vector<PxRigidBody*>        Actors;
    std::vector<bool>                Locked;
    std::vector<PxTransform>         Previous, Current, Animated, OldAnimated;
    AnimationPose                    Target, Recovery;
    glm::mat4                        EntityWorld {1};
    RagdollMode                      Mode         = RagdollMode::Kinematic;
    float                            RecoveryTime = 0, RecoveryDuration = 0;
    uint64_t                         Generation = 0;
    bool                             Reset      = false;
};
std::map<UUID, Ragdoll>               Ragdolls;
std::map<const PxShape*, std::string> ShapeBones;
struct RagdollFrame
{
    AnimationPose Previous, Current;
    glm::mat4     PreviousWorld {1}, CurrentWorld {1};
};
std::map<UUID, RagdollFrame> RagdollFrames;
double                       RagdollFrameStart = 0, RagdollFrameDuration = 0;
void                         ReleaseRagdoll(Ragdoll& r)
{
    if (!r.Actors.empty())
        InvalidatePairs(Identity(r.Actors[0]->userData));
    for (auto* actor : r.Actors)
    {
        std::vector<PxShape*> shapes(actor->getNbShapes());
        actor->getShapes(shapes.data(), PxU32(shapes.size()));
        for (auto* shape : shapes)
            ShapeBones.erase(shape);
    }
    if (r.Articulation)
    {
        r.Articulation->release();
        r.Articulation = nullptr;
    }
    else
        for (auto* actor : r.Actors)
            actor->release();
    Sdk().Actors -= r.Actors.size();
    r.Actors.clear();
}
static glm::vec3 AngularVelocity(PxQuat old, PxQuat current, float dt)
{
    auto q = Gl(current * old.getConjugate());
    if (q.w < 0)
        q = -q;
    auto  xyz = glm::vec3(q.x, q.y, q.z);
    float n   = glm::length(xyz);
    return n > 1e-8f ? xyz * (2 * std::atan2(n, q.w) / (n * dt)) : glm::vec3(0);
}
static const RagdollBodyOverride* Override(const RagdollComponent& c, const std::string& bone)
{
    auto it = std::find_if(c.Bodies.begin(), c.Bodies.end(), [&](const auto& b) { return b.Bone == bone; });
    return it == c.Bodies.end() ? nullptr : &*it;
}
static float Weight(const RagdollComponent& c, const std::string& bone)
{
    if (c.Mode == RagdollMode::Kinematic)
        return 0;
    auto* b = Override(c, bone);
    return c.Mode == RagdollMode::Blended && b ? glm::clamp(b->BlendWeight, 0.f, 1.f) : 1;
}
void CreateRagdoll(UUID id, Ragdoll& r, const RagdollComponent& c, float dt)
{
    const auto& data = r.Source->GetData();
    size_t      n    = data.Bodies.size();
    r.Generation     = ++Stats.ActorCreationCount;
    ++Stats.RebuildCount;
    r.Mode = c.Mode;
    r.Actors.resize(n, nullptr);
    r.Locked.assign(n, false);
    r.Previous = r.Current = r.Animated;
    if (c.Mode != RagdollMode::Kinematic)
    {
        r.Articulation = Sdk().Physics->createArticulationReducedCoordinate();
        r.Articulation->setArticulationFlag(PxArticulationFlag::eDISABLE_SELF_COLLISION, false);
        r.Articulation->setSolverIterationCounts(std::max(16u, Settings.SolverPositionIterations),
                                                 std::max(4u, Settings.SolverVelocityIterations));
        if (c.Mode == RagdollMode::Blended &&
            std::any_of(data.Bodies.begin(), data.Bodies.end(), [&](const auto& b) { return Weight(c, b.Bone) == 0; }))
            r.Articulation->setArticulationFlag(PxArticulationFlag::eFIX_BASE, true);
    }
    size_t created = 0;
    while (created < n)
        for (size_t j = 0; j < n; ++j)
        {
            if (r.Actors[j])
                continue;
            int32_t parent = r.Binding.Parents[j];
            if (parent >= 0 && !r.Actors[parent])
                continue;
            auto* actor =
                r.Articulation ?
                    static_cast<PxRigidBody*>(r.Articulation->createLink(
                        parent < 0 ? nullptr : static_cast<PxArticulationLink*>(r.Actors[parent]), r.Animated[j])) :
                    static_cast<PxRigidBody*>(Sdk().Physics->createRigidDynamic(r.Animated[j]));
            if (!actor)
                throw std::runtime_error("Ragdoll actor creation failed");
            r.Actors[j] = actor;
            ++created;
            ++Sdk().Actors;
            actor->userData      = reinterpret_cast<void*>(uintptr_t(uint64_t(id)));
            const auto& body     = data.Bodies[j];
            auto*       material = Material(ColliderMaterial {body.Material});
            for (const auto& primitive : body.Shapes)
            {
                PxGeometryHolder geometry;
                PxTransform      local(PxIdentity);
                std::visit(
                    [&](const auto& shape) {
                        using T   = std::decay_t<decltype(shape)>;
                        auto pose = Physics::TransformShape(shape.Offset, glm::mat4(1));
                        local     = Pose(pose);
                        if constexpr (std::is_same_v<T, Physics::Sphere>)
                            geometry.storeAny(PxSphereGeometry(Physics::ScaledRadius(shape, pose)));
                        else if constexpr (std::is_same_v<T, Physics::Box>)
                            geometry.storeAny(PxBoxGeometry(Px(shape.HalfExtents * glm::abs(pose.Scale))));
                        else
                        {
                            glm::vec3 a, b;
                            float     radius;
                            Physics::CapsuleSegment(shape, pose, a, b, radius);
                            geometry.storeAny(PxCapsuleGeometry(radius, glm::distance(a, b) * .5f));
                            local.p   = Px((a + b) * .5f);
                            auto axis = b - a;
                            local.q   = glm::length(axis) > 1e-8f ?
                                            Px(glm::rotation(glm::vec3(1, 0, 0), glm::normalize(axis))) :
                                            PxQuat(PxIdentity);
                        }
                    },
                    primitive);
                auto* shape = Sdk().Physics->createShape(geometry.any(), *material, true);
                shape->setLocalPose(local);
                shape->userData    = actor->userData;
                uint32_t     layer = body.Layer.value_or(data.DefaultLayer);
                PxFilterData filter(1u << layer,
                                    Settings.CollisionMatrix[layer],
                                    uint32_t(j) + 0x10000,
                                    0x80000000u | uint32_t(uint64_t(id) & 0x7fffffffu));
                shape->setSimulationFilterData(filter);
                shape->setQueryFilterData(filter);
                actor->attachShape(*shape);
                ShapeBones[shape] = body.Bone;
                shape->release();
            }
            material->release();
            if (body.UseDensity)
                PxRigidBodyExt::updateMassAndInertia(*actor, body.Density);
            else
                PxRigidBodyExt::setMassAndUpdateInertia(*actor, body.Mass);
            actor->setLinearDamping(body.LinearDamping);
            actor->setAngularDamping(body.AngularDamping);
            actor->setMaxAngularVelocity(50.f);
            actor->setMaxLinearVelocity(100.f);
            if (!r.Articulation)
            {
                auto* d = static_cast<PxRigidDynamic*>(actor);
                d->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
                World->addActor(*d);
            }
            else if (parent >= 0)
            {
                const auto  it    = std::find_if(data.Constraints.begin(), data.Constraints.end(), [&](const auto& k) {
                    return k.Bone == body.Bone;
                });
                const auto& k     = *it;
                auto*       joint = static_cast<PxArticulationLink*>(actor)->getInboundJoint();
                joint->setJointType(PxArticulationJointType::eSPHERICAL);

                auto                   childFrame = Pose(Physics::TransformShape(k.Frame, glm::mat4(1)));
                const auto&            skeleton   = r.SkeletonSource->GetData();
                std::vector<glm::mat4> bind(skeleton.Joints.size());
                for (size_t b = 0; b < bind.size(); ++b)
                    bind[b] = (skeleton.Joints[b].Parent < 0 ? glm::mat4(1) : bind[skeleton.Joints[b].Parent]) *
                              skeleton.Joints[b].BindLocal.Matrix();
                auto parentFrame = Pose(Physics::TransformShape(
                    {}, glm::inverse(bind[r.Binding.Bones[parent]]) * bind[r.Binding.Bones[j]] * k.Frame.Matrix()));
                joint->setChildPose(childFrame);
                joint->setParentPose(parentFrame);
                const PxArticulationAxis::Enum axes[] = {
                    PxArticulationAxis::eTWIST, PxArticulationAxis::eSWING1, PxArticulationAxis::eSWING2};
                const float lo[] = {k.TwistLow, -k.SwingY, -k.SwingZ}, hi[] = {k.TwistHigh, k.SwingY, k.SwingZ};
                for (int a = 0; a < 3; ++a)
                {
                    r.Locked[j] = Weight(c, body.Bone) == 0;
                    joint->setMotion(axes[a],
                                     r.Locked[j] ? PxArticulationMotion::eLOCKED : PxArticulationMotion::eLIMITED);
                    joint->setMaxJointVelocity(axes[a], 50.f);
                    joint->setLimitParams(axes[a], PxArticulationLimit(glm::radians(lo[a]), glm::radians(hi[a])));
                    auto initial = PxLog(joint->getParentPose().q.getConjugate() * r.Animated[parent].q.getConjugate() *
                                         r.Animated[j].q * childFrame.q);
                    joint->setJointPosition(axes[a], initial[a]);
                }
            }
        }
    if (r.Articulation)
    {
        World->addArticulation(*r.Articulation);
        size_t root =
            size_t(std::find(r.Binding.Parents.begin(), r.Binding.Parents.end(), -1) - r.Binding.Parents.begin());
        if (r.OldAnimated.size() == n && dt > 0)
        {
            r.Articulation->setRootLinearVelocity((r.Animated[root].p - r.OldAnimated[root].p) / dt);
            r.Articulation->setRootAngularVelocity(Px(AngularVelocity(r.OldAnimated[root].q, r.Animated[root].q, dt)));
            for (size_t j = 0; j < n; ++j)
                if (r.Binding.Parents[j] >= 0)
                {
                    auto* joint    = static_cast<PxArticulationLink*>(r.Actors[j])->getInboundJoint();
                    auto  p        = r.Binding.Parents[j];
                    auto  velocity = AngularVelocity(r.OldAnimated[p].q.getConjugate() * r.OldAnimated[j].q,
                                                     r.Animated[p].q.getConjugate() * r.Animated[j].q,
                                                     dt);
                    for (int a = 0; a < 3; ++a)
                        joint->setJointVelocity(PxArticulationAxis::Enum(a), velocity[a]);
                }
            r.Articulation->updateKinematic(PxArticulationKinematicFlag::eVELOCITY);
        }
    }
}
void PrepareRagdolls(float dt)
{
    std::set<UUID> live;
    for (auto h : Owner.m_Registry.view<RagdollComponent, AnimatorComponent>())
    {
        Entity      e {h, &Owner};
        auto        id = e.GetUUID();
        const auto& c  = e.GetComponent<RagdollComponent>();
        auto&       a  = e.GetComponent<AnimatorComponent>();
        if (!c.Enabled || !Owner.IsEntityActive(e) || !a.SkeletonSource || !a.Player)
            continue;
        auto source = c.Source;
        if (c.Asset.IsValid() && Owner.m_PrefabAssets)
        {
            auto status = Owner.m_PrefabAssets->Request<RagdollAsset>(c.Asset);
            if (status.IsReady())
                source = status.Asset;
        }
        if (!source)
        {
            auto old = Ragdolls.find(id);
            if (c.Asset.IsValid() && old != Ragdolls.end() && old->second.Source &&
                old->second.Source->assetID == c.Asset)
                source = old->second.Source;
        }
        if (!source)
        {
            Error(id, "Ragdoll asset unavailable");
            continue;
        }
        live.insert(id);
        auto& r      = Ragdolls[id];
        bool  rebind = source != r.Source || r.SkeletonSource != a.SkeletonSource;
        if (rebind)
        {
            ReleaseRagdoll(r);
            r.Source         = source;
            r.SkeletonSource = a.SkeletonSource;
            r.Binding        = BindRagdoll(source->GetData(), a.SkeletonSource->GetData(), a.Skeleton);
            if (!r.Binding)
                Error(id, r.Binding.Error);
        }
        if (!r.Binding)
            continue;
        r.Target         = a.PreviewPose;
        auto world       = Owner.GetWorldTransform(e);
        auto targetWorld = world;
        if (auto frame = RagdollFrames.find(id); frame != RagdollFrames.end())
        {
            const auto& f = frame->second;
            float       t = RagdollFrameDuration > 0 ?
                                float(glm::clamp((Time + dt - RagdollFrameStart) / RagdollFrameDuration, 0., 1.)) :
                                1;
            if (f.Previous.Local.size() == f.Current.Local.size())
            {
                r.Target = f.Current;
                for (size_t bone = 0; bone < r.Target.Local.size(); ++bone)
                {
                    r.Target.Local[bone].Translation =
                        glm::mix(f.Previous.Local[bone].Translation, f.Current.Local[bone].Translation, t);
                    r.Target.Local[bone].Rotation =
                        glm::slerp(f.Previous.Local[bone].Rotation, f.Current.Local[bone].Rotation, t);
                    r.Target.Local[bone].Scale = glm::mix(f.Previous.Local[bone].Scale, f.Current.Local[bone].Scale, t);
                }
                BuildModelPose(a.SkeletonSource->GetData(), r.Target);
                auto old            = JointTransform::FromMatrix(f.PreviousWorld),
                     current        = JointTransform::FromMatrix(f.CurrentWorld);
                current.Translation = glm::mix(old.Translation, current.Translation, t);
                current.Rotation    = glm::slerp(old.Rotation, current.Rotation, t);
                current.Scale       = glm::mix(old.Scale, current.Scale, t);
                targetWorld         = current.Matrix();
            }
        }
        r.OldAnimated = r.Animated;
        r.Animated.resize(r.Binding.Bones.size());
        for (size_t j = 0; j < r.Animated.size(); ++j)
            r.Animated[j] = Pose(Physics::TransformShape({}, targetWorld * r.Target.Model[r.Binding.Bones[j]]));
        auto  oldEntity = JointTransform::FromMatrix(r.EntityWorld), newEntity = JointTransform::FromMatrix(world);
        float rotationJump = glm::degrees(
            2 * std::acos(glm::clamp(std::abs(glm::dot(oldEntity.Rotation, newEntity.Rotation)), 0.f, 1.f)));
        bool teleport =
            r.Reset || (!rebind && c.Mode != RagdollMode::Kinematic &&
                        ((c.TeleportDistance > 0 &&
                          glm::distance(oldEntity.Translation, newEntity.Translation) > c.TeleportDistance) ||
                         (c.TeleportRotation > 0 && rotationJump > c.TeleportRotation)));
        if (r.Actors.empty() || r.Mode != c.Mode || teleport)
        {
            ReleaseRagdoll(r);
            if (teleport)
                r.OldAnimated = r.Animated;
            CreateRagdoll(id, r, c, dt);
            r.Reset = false;
        }
        r.EntityWorld = world;
        if (r.Articulation)
        {
            bool change = false;
            for (size_t j = 0; j < r.Actors.size(); ++j)
                if (r.Binding.Parents[j] >= 0 && r.Locked[j] != (Weight(c, r.Source->GetData().Bodies[j].Bone) == 0))
                    change = true;
            if (change)
            {
                World->removeArticulation(*r.Articulation);
                for (size_t j = 0; j < r.Actors.size(); ++j)
                    if (r.Binding.Parents[j] >= 0)
                    {
                        r.Locked[j] = Weight(c, r.Source->GetData().Bodies[j].Bone) == 0;
                        auto* joint = static_cast<PxArticulationLink*>(r.Actors[j])->getInboundJoint();
                        // PhysX turns a joint with every axis locked into eFIX when the articulation is added.
                        joint->setJointType(PxArticulationJointType::eSPHERICAL);
                        for (int axis = 0; axis < 3; ++axis)
                            joint->setMotion(PxArticulationAxis::Enum(axis),
                                             r.Locked[j] ? PxArticulationMotion::eLOCKED :
                                                           PxArticulationMotion::eLIMITED);
                    }
                World->addArticulation(*r.Articulation);
            }
        }
        if (c.Mode == RagdollMode::Kinematic)
            for (size_t j = 0; j < r.Actors.size(); ++j)
                static_cast<PxRigidDynamic*>(r.Actors[j])->setKinematicTarget(r.Animated[j]);
        else
            for (size_t j = 0; j < r.Actors.size(); ++j)
            {
                r.Previous[j] = r.Current[j];
                auto p        = r.Binding.Parents[j];
                if (p < 0)
                    continue;
                auto*       joint  = static_cast<PxArticulationLink*>(r.Actors[j])->getInboundJoint();
                const auto& body   = r.Source->GetData().Bodies[j];
                const auto& limits = *std::find_if(r.Source->GetData().Constraints.begin(),
                                                   r.Source->GetData().Constraints.end(),
                                                   [&](const auto& k) { return k.Bone == body.Bone; });
                auto*       over   = Override(c, body.Bone);
                float       strength =
                    (over && over->DriveStrength >= 0 ? over->DriveStrength : limits.DriveStrength) * c.DriveStrength;
                float damping =
                    (over && over->DriveDamping >= 0 ? over->DriveDamping : limits.DriveDamping) * c.DriveStrength;
                if (limits.LimitStiffness > 0 || limits.LimitDamping > 0)
                {
                    glm::vec3 position(joint->getJointPosition(PxArticulationAxis::eTWIST),
                                       joint->getJointPosition(PxArticulationAxis::eSWING1),
                                       joint->getJointPosition(PxArticulationAxis::eSWING2));
                    glm::vec3 lo(
                        glm::radians(limits.TwistLow), -glm::radians(limits.SwingY), -glm::radians(limits.SwingZ)),
                        hi(glm::radians(limits.TwistHigh), glm::radians(limits.SwingY), glm::radians(limits.SwingZ));
                    auto clamped = glm::clamp(position, lo * .9f, hi * .9f);
                    auto delta   = clamped - position;
                    if (glm::dot(delta, delta) > 0)
                    {
                        glm::vec3 velocity(joint->getJointVelocity(PxArticulationAxis::eTWIST),
                                           joint->getJointVelocity(PxArticulationAxis::eSWING1),
                                           joint->getJointVelocity(PxArticulationAxis::eSWING2));
                        auto      torque = Gl(r.Actors[p]->getGlobalPose().q * joint->getParentPose().q) *
                                           (delta * limits.LimitStiffness - velocity * limits.LimitDamping);
                        r.Actors[j]->addTorque(Px(torque));
                        r.Actors[p]->addTorque(Px(-torque));
                    }
                }
                auto target = joint->getParentPose().q.getConjugate() * r.Animated[p].q.getConjugate() *
                              r.Animated[j].q * joint->getChildPose().q;
                auto angles = Gl(PxLog(target));
                for (int axis = 0; axis < 3; ++axis)
                {
                    auto ax = PxArticulationAxis::Enum(axis);
                    joint->setDriveParams(
                        ax, PxArticulationDrive(strength, damping, PX_MAX_F32, PxArticulationDriveType::eFORCE));
                    joint->setDriveTarget(ax, angles[axis], strength > 0);
                    if (Weight(c, body.Bone) == 0)
                    {
                        joint->setJointPosition(ax, angles[axis]);
                        joint->setJointVelocity(ax, 0);
                    }
                }
            }
        if (r.Articulation)
        {
            auto root =
                size_t(std::find(r.Binding.Parents.begin(), r.Binding.Parents.end(), -1) - r.Binding.Parents.begin());
            const bool fixedBase = Weight(c, r.Source->GetData().Bodies[root].Bone) == 0;
            r.Articulation->setArticulationFlag(PxArticulationFlag::eFIX_BASE, fixedBase);
            if (fixedBase)
            {
                r.Articulation->setRootGlobalPose(r.Animated[root]);
                r.Articulation->setRootLinearVelocity(PxVec3(0));
                r.Articulation->setRootAngularVelocity(PxVec3(0));
            }
            r.Articulation->updateKinematic(PxArticulationKinematicFlag::ePOSITION);
        }
        for (size_t j = 0; j < r.Actors.size(); ++j)
            if (c.Mode != RagdollMode::Kinematic)
            {
                auto*    actor    = r.Actors[j];
                auto     position = Gl(actor->getGlobalPose().p);
                uint32_t layer    = source->GetData().Bodies[j].Layer.value_or(source->GetData().DefaultLayer);
                auto*    link     = static_cast<PxArticulationLink*>(actor);
                auto     sample =
                    Fields.Sample(position, Gl(link->getLinearVelocity()), Time, layer, FieldSystem::RigidBodies);
                auto force = sample.Force + sample.Acceleration * actor->getMass();
                if (glm::dot(force, force) > 0)
                    actor->addForce(Px(force));
            }
        if (r.RecoveryDuration > 0)
            r.RecoveryTime = std::min(r.RecoveryDuration, r.RecoveryTime + dt);
    }
    for (auto it = Ragdolls.begin(); it != Ragdolls.end();)
        if (!live.count(it->first))
        {
            ReleaseRagdoll(it->second);
            it = Ragdolls.erase(it);
        }
        else
            ++it;
}
void CollectRagdolls()
{
    for (auto& [id, r] : Ragdolls)
    {
        if (r.Articulation)
        {
            bool projected = false;
            for (size_t j = 0; j < r.Actors.size(); ++j)
                if (r.Binding.Parents[j] >= 0)
                {
                    auto* joint = static_cast<PxArticulationLink*>(r.Actors[j])->getInboundJoint();
                    auto& body  = r.Source->GetData().Bodies[j];
                    auto& k     = *std::find_if(r.Source->GetData().Constraints.begin(),
                                                r.Source->GetData().Constraints.end(),
                                                [&](auto& k) { return k.Bone == body.Bone; });
                    float y     = joint->getJointPosition(PxArticulationAxis::eSWING1),
                          z     = joint->getJointPosition(PxArticulationAxis::eSWING2);
                    float ry = glm::radians(k.SwingY), rz = glm::radians(k.SwingZ);
                    float norm = std::sqrt(y * y / (ry * ry) + z * z / (rz * rz));
                    // Articulation spherical limits bound the two coordinates independently. Project their
                    // coupled elliptical boundary and remove only outward velocity; the tangent is preserved.
                    if (norm > 1)
                    {
                        float     vy = joint->getJointVelocity(PxArticulationAxis::eSWING1),
                                  vz = joint->getJointVelocity(PxArticulationAxis::eSWING2);
                        glm::vec2 normal(y / (ry * ry), z / (rz * rz)), velocity(vy, vz);
                        float     n2 = glm::dot(normal, normal);
                        if (n2 > 0)
                            velocity -= normal * (std::max(0.f, glm::dot(normal, velocity)) / n2);
                        joint->setJointPosition(PxArticulationAxis::eSWING1, y / norm);
                        joint->setJointPosition(PxArticulationAxis::eSWING2, z / norm);
                        joint->setJointVelocity(PxArticulationAxis::eSWING1, velocity.x);
                        joint->setJointVelocity(PxArticulationAxis::eSWING2, velocity.y);
                        projected = true;
                    }
                }
            if (projected)
                r.Articulation->updateKinematic(PxArticulationKinematicFlag::ePOSITION |
                                                PxArticulationKinematicFlag::eVELOCITY);
        }
        for (size_t j = 0; j < r.Actors.size(); ++j)
        {
            if (r.Mode == RagdollMode::Kinematic)
                r.Previous[j] = r.Current[j];
            r.Current[j] = r.Actors[j]->getGlobalPose();
        }
    }
}
void WriteRagdolls(float alpha)
{
    for (auto& [id, r] : Ragdolls)
    {
        auto e = Owner.GetEntityByUUID(id);
        if (!e || !r.Binding || r.Actors.empty())
            continue;
        auto& a     = e.GetComponent<AnimatorComponent>();
        auto& c     = e.GetComponent<RagdollComponent>();
        auto  frame = RagdollFrames.find(id);
        auto  pose  = frame == RagdollFrames.end() ? r.Target : frame->second.Current;
        auto  world = Owner.GetWorldTransform(e);
        if (c.RootFollowing == RagdollRootFollowing::FollowPelvis && c.Mode != RagdollMode::Kinematic)
        {
            auto root =
                size_t(std::find(r.Binding.Parents.begin(), r.Binding.Parents.end(), -1) - r.Binding.Parents.begin());
            auto target = Gl(r.Current[root].p);
            auto origin = glm::vec3((world * pose.Model[r.Binding.Bones[root]])[3]);
            world[3] += glm::vec4(target.x - origin.x, 0, target.z - origin.z, 0);
            if (c.FollowYaw)
            {
                auto             t = JointTransform::FromMatrix(world);
                glm::mat4        bind(1);
                std::vector<int> ancestors;
                const auto&      skeleton = a.SkeletonSource->GetData();
                for (int bone = int(r.Binding.Bones[root]); bone >= 0; bone = skeleton.Joints[bone].Parent)
                    ancestors.push_back(bone);
                for (auto bone = ancestors.rbegin(); bone != ancestors.rend(); ++bone)
                    bind *= skeleton.Joints[*bone].BindLocal.Matrix();
                auto      bindRotation = JointTransform::FromMatrix(bind).Rotation;
                auto      animated     = Pose(Physics::TransformShape({}, world * pose.Model[r.Binding.Bones[root]])).q;
                glm::vec3 axis         = glm::inverse(bindRotation) * glm::vec3(0, 0, 1);
                auto      project      = [](glm::vec3 v) { return glm::vec2(v.x, v.z); };
                auto simulatedAxis = project(Gl(r.Current[root].q) * axis), animatedAxis = project(Gl(animated) * axis);
                if (std::min(glm::length(simulatedAxis), glm::length(animatedAxis)) < .1f)
                {
                    axis          = glm::inverse(bindRotation) * glm::vec3(1, 0, 0);
                    simulatedAxis = project(Gl(r.Current[root].q) * axis);
                    animatedAxis  = project(Gl(animated) * axis);
                }
                if (glm::length(simulatedAxis) > 1e-5f && glm::length(animatedAxis) > 1e-5f)
                {
                    float delta =
                        std::atan2(simulatedAxis.x, simulatedAxis.y) - std::atan2(animatedAxis.x, animatedAxis.y);
                    t.Rotation = glm::angleAxis(delta, glm::vec3(0, 1, 0)) * t.Rotation;
                }
                world = t.Matrix();
            }
            Owner.SetWorldTransform(e, world);
            r.EntityWorld = Owner.GetWorldTransform(e);
        }
        auto        inverse  = glm::inverse(world);
        const auto& skeleton = a.SkeletonSource->GetData();
        for (size_t bone = 0; bone < pose.Local.size(); ++bone)
        {
            auto    it     = std::find(r.Binding.Bones.begin(), r.Binding.Bones.end(), uint32_t(bone));
            int32_t parent = skeleton.Joints[bone].Parent;
            if (it != r.Binding.Bones.end())
            {
                size_t j      = size_t(it - r.Binding.Bones.begin());
                float  weight = Weight(c, r.Source->GetData().Bodies[j].Bone);
                if (weight > 0)
                {
                    auto previous = r.Previous[j], current = r.Current[j];
                    auto transform = glm::translate(glm::mat4(1), glm::mix(Gl(previous.p), Gl(current.p), alpha)) *
                                     glm::mat4_cast(glm::slerp(Gl(previous.q), Gl(current.q), alpha));
                    auto local     = JointTransform::FromMatrix(
                        (parent < 0 ? glm::mat4(1) : glm::inverse(pose.Model[parent])) * inverse * transform);
                    pose.Local[bone].Translation = glm::mix(pose.Local[bone].Translation, local.Translation, weight);
                    pose.Local[bone].Rotation    = glm::slerp(pose.Local[bone].Rotation, local.Rotation, weight);
                }
                if (r.RecoveryDuration > 0 && bone < r.Recovery.Local.size())
                {
                    float t = r.RecoveryTime / r.RecoveryDuration;
                    pose.Local[bone].Translation =
                        glm::mix(r.Recovery.Local[bone].Translation, pose.Local[bone].Translation, t);
                    pose.Local[bone].Rotation =
                        glm::slerp(r.Recovery.Local[bone].Rotation, pose.Local[bone].Rotation, t);
                }
            }
            pose.Model[bone] = (parent < 0 ? glm::mat4(1) : pose.Model[parent]) * pose.Local[bone].Matrix();
        }
        BuildModelPose(skeleton, pose);
        a.PreviewPose = pose;
        Owner.ApplyModelPose(e, skeleton, pose, a.JointEntities, a.SkinPose, a.PoseVersion);
    }
}
