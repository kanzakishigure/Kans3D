void PhysicsWorld::CaptureRagdollTargets(double frameSeconds)
{
    auto& i                = *m_Impl;
    i.RagdollFrameStart    = i.Time + i.Owner.m_PhysicsAccumulator;
    i.RagdollFrameDuration = frameSeconds;
    std::set<UUID> live;
    for (auto h : i.Owner.m_Registry.view<RagdollComponent, AnimatorComponent>())
    {
        Entity      e {h, &i.Owner};
        const auto& a = e.GetComponent<AnimatorComponent>();
        if (!a.Player)
            continue;
        auto id = e.GetUUID();
        live.insert(id);
        auto& frame         = i.RagdollFrames[id];
        frame.Previous      = frame.Current;
        frame.PreviousWorld = frame.CurrentWorld;
        frame.Current       = a.PreviewPose;
        frame.CurrentWorld  = i.Owner.GetWorldTransform(e);
        if (frame.Previous.Local.size() != frame.Current.Local.size())
        {
            frame.Previous      = frame.Current;
            frame.PreviousWorld = frame.CurrentWorld;
        }
    }
    for (auto it = i.RagdollFrames.begin(); it != i.RagdollFrames.end();)
        if (!live.count(it->first))
            it = i.RagdollFrames.erase(it);
        else
            ++it;
}
void PhysicsWorld::ResetRagdoll(UUID id)
{
    if (auto it = m_Impl->Ragdolls.find(id); it != m_Impl->Ragdolls.end())
        it->second.Reset = true;
}
void PhysicsWorld::SetRagdollBlendWeight(UUID id, const std::string& bone, float weight, bool children)
{
    auto e = m_Impl->Owner.GetEntityByUUID(id);
    if (!e || !e.HasComponent<RagdollComponent, AnimatorComponent>() || !std::isfinite(weight))
        return;
    auto& c = e.GetComponent<RagdollComponent>();
    auto& a = e.GetComponent<AnimatorComponent>();
    if (!a.SkeletonSource)
        return;
    auto& skeleton = a.SkeletonSource->GetData();
    auto  root     = skeleton.FindJoint(bone);
    if (root < 0)
        return;
    for (size_t j = 0; j < skeleton.Joints.size(); ++j)
    {
        auto p = int32_t(j);
        while (p >= 0 && p != root && children)
            p = skeleton.Joints[p].Parent;
        if (p != root)
            continue;
        auto it = std::find_if(
            c.Bodies.begin(), c.Bodies.end(), [&](const auto& b) { return b.Bone == skeleton.Joints[j].Name; });
        if (it == c.Bodies.end())
        {
            c.Bodies.push_back({skeleton.Joints[j].Name, glm::clamp(weight, 0.f, 1.f)});
        }
        else
            it->BlendWeight = glm::clamp(weight, 0.f, 1.f);
    }
}
void PhysicsWorld::SimulateRagdoll(UUID id, const std::string& from)
{
    auto e = m_Impl->Owner.GetEntityByUUID(id);
    if (!e || !e.HasComponent<RagdollComponent, AnimatorComponent>())
        return;
    auto& c = e.GetComponent<RagdollComponent>();
    c.Mode  = from.empty() ? RagdollMode::Simulated : RagdollMode::Blended;
    if (!from.empty())
    {
        auto& a = e.GetComponent<AnimatorComponent>();
        if (!a.SkeletonSource)
            return;
        c.Bodies.clear();
        for (const auto& bone : a.SkeletonSource->GetData().Joints)
            c.Bodies.push_back({bone.Name, 0});
        SetRagdollBlendWeight(id, from, 1, true);
    }
}
void PhysicsWorld::BlendRagdollToAnimation(UUID id, float duration)
{
    if (!std::isfinite(duration) || duration < 0)
        return;
    auto e  = m_Impl->Owner.GetEntityByUUID(id);
    auto it = m_Impl->Ragdolls.find(id);
    if (!e || !e.HasComponent<RagdollComponent, AnimatorComponent>() || it == m_Impl->Ragdolls.end())
        return;
    auto& r                                 = it->second;
    r.Recovery                              = e.GetComponent<AnimatorComponent>().PreviewPose;
    r.RecoveryTime                          = 0;
    r.RecoveryDuration                      = duration;
    e.GetComponent<RagdollComponent>().Mode = RagdollMode::Kinematic;
}
std::optional<PhysicsBodyState> PhysicsWorld::GetRagdollBodyState(UUID id, const std::string& bone) const
{
    auto it = m_Impl->Ragdolls.find(id);
    if (it == m_Impl->Ragdolls.end())
        return {};
    auto& r = it->second;
    for (size_t j = 0; j < r.Actors.size(); ++j)
        if (r.Source->GetData().Bodies[j].Bone == bone)
        {
            PhysicsBodyState s;
            auto*            b    = r.Actors[j];
            auto             pose = b->getGlobalPose();
            s.Position            = Gl(pose.p);
            s.Rotation            = Gl(pose.q);
            s.Mass                = b->getMass();
            s.ActorGeneration     = r.Generation;
            s.ShapeCount          = b->getNbShapes();
            if (r.Articulation)
            {
                auto* link        = static_cast<PxArticulationLink*>(b);
                s.LinearVelocity  = Gl(link->getLinearVelocity());
                s.AngularVelocity = Gl(link->getAngularVelocity());
                s.Sleeping        = r.Articulation->isSleeping();
            }
            else
            {
                auto* d           = static_cast<PxRigidDynamic*>(b);
                s.LinearVelocity  = Gl(d->getLinearVelocity());
                s.AngularVelocity = Gl(d->getAngularVelocity());
            }
            return s;
        }
    return {};
}
void PhysicsWorld::AddRagdollForce(UUID id, const std::string& bone, glm::vec3 force, ForceMode mode)
{
    if (m_Impl->Dispatching)
    {
        m_Impl->Commands.push_back([this, id, bone, force, mode] { AddRagdollForce(id, bone, force, mode); });
        return;
    }
    auto it = m_Impl->Ragdolls.find(id);
    if (it == m_Impl->Ragdolls.end() || !it->second.Articulation)
        return;
    auto& r = it->second;
    for (size_t j = 0; j < r.Actors.size(); ++j)
        if (r.Source->GetData().Bodies[j].Bone == bone)
        {
            auto* body = r.Actors[j];
            if (mode == ForceMode::VelocityChange || mode == ForceMode::Acceleration)
                force *= body->getMass();
            if (mode == ForceMode::VelocityChange || mode == ForceMode::Impulse)
                force /= float(m_Impl->Settings.FixedTimeStep);
            body->addForce(Px(force));
        }
}
void PhysicsWorld::AddRagdollImpulse(UUID id, const std::string& bone, glm::vec3 impulse, glm::vec3 point)
{
    if (m_Impl->Dispatching)
    {
        m_Impl->Commands.push_back([this, id, bone, impulse, point] { AddRagdollImpulse(id, bone, impulse, point); });
        return;
    }
    AddRagdollForce(id, bone, impulse, ForceMode::Impulse);
    auto it = m_Impl->Ragdolls.find(id);
    if (it == m_Impl->Ragdolls.end() || !it->second.Articulation)
        return;
    auto& r = it->second;
    for (size_t j = 0; j < r.Actors.size(); ++j)
        if (r.Source->GetData().Bodies[j].Bone == bone)
        {
            auto* body   = r.Actors[j];
            auto  center = body->getGlobalPose() * body->getCMassLocalPose();
            body->addTorque(Px(glm::cross(point - Gl(center.p), impulse) / float(m_Impl->Settings.FixedTimeStep)));
        }
}
