struct VehicleRuntime
{
    VehicleComponent           Settings;
    PxRigidDynamic*            Chassis    = nullptr;
    uint64_t                   Generation = 0;
    VehicleSDK::Vehicle        SDK;
    PxVehicleComponentSequence Sequence;
    PxU8                       Substeps = 0;
    VehicleState               State;
    std::vector<PxTransform>   Previous, Current;
    std::map<UUID, glm::quat>  VisualOffsets;
};
std::map<UUID, std::unique_ptr<VehicleRuntime>> Vehicles;
void                                            CreateVehicle(VehicleRuntime& v, const VehicleComponent& c, Body& body)
{
    v.Settings   = c;
    v.Chassis    = body.Actor->is<PxRigidDynamic>();
    v.Generation = body.Generation;
    auto& sdk    = v.SDK;
    auto& p      = sdk.mBaseParams;
    auto& s      = sdk.mBaseState;
    s.setToDefault();
    sdk.mDirectDriveState.setToDefault();
    sdk.mEngineDriveState.setToDefault();
    sdk.mCommandState.setToDefault();
    sdk.DirectTransmission.setToDefault();
    sdk.mTransmissionCommandState.setToDefault();
    p.axleDescription.setToDefault();
    p.frame.lngAxis        = PxVehicleAxes::ePosZ;
    p.frame.latAxis        = PxVehicleAxes::ePosX;
    p.frame.vrtAxis        = PxVehicleAxes::ePosY;
    p.scale.scale          = 1;
    p.rigidBodyParams.mass = v.Chassis->getMass();
    p.rigidBodyParams.moi  = v.Chassis->getMassSpaceInertiaTensor();
    p.suspensionStateCalculationParams.suspensionJounceCalculationType =
        c.RoadQuery == VehicleRoadQuery::Raycast ? PxVehicleSuspensionJounceCalculationType::eRAYCAST :
                                                   PxVehicleSuspensionJounceCalculationType::eSWEEP;
    p.suspensionStateCalculationParams.limitSuspensionExpansionVelocity = true;
    float maxBrake = 1, maxHandbrake = 1, maxSteer = 1, maxSplit = 1;
    for (const auto& w : c.Wheels)
    {
        maxBrake     = std::max(maxBrake, w.BrakeTorque);
        maxHandbrake = std::max(maxHandbrake, w.HandbrakeTorque);
        maxSteer     = std::max(maxSteer, glm::radians(w.MaxSteerAngle));
        maxSplit     = std::max(maxSplit, w.TorqueSplit);
    }
    p.brakeResponseParams[0].maxResponse                                 = maxBrake;
    p.brakeResponseParams[1].maxResponse                                 = maxHandbrake;
    p.steerResponseParams.maxResponse                                    = maxSteer;
    sdk.mDirectDriveParams.directDriveThrottleResponseParams.maxResponse = c.DirectDriveTorque * maxSplit;
    float totalSplit                                                     = 0;
    for (auto& wheel : c.Wheels)
        if (wheel.Driven)
            totalSplit += wheel.TorqueSplit;
    std::map<uint32_t, std::vector<PxU32>> axles;
    for (PxU32 i = 0; i < c.Wheels.size(); ++i)
        axles[c.Wheels[i].Axle].push_back(i);
    auto axlePosition = [&](uint32_t axle) {
        float sum = 0;
        for (auto index : axles.at(axle))
            sum += c.Wheels[index].AttachPosition.z;
        return sum / float(axles.at(axle).size());
    };
    auto frontAxle = axles.begin()->first, rearAxle = frontAxle;
    for (const auto& [axle, wheels] : axles)
    {
        if (axlePosition(axle) > axlePosition(frontAxle))
            frontAxle = axle;
        if (axlePosition(axle) < axlePosition(rearAxle))
            rearAxle = axle;
    }
    for (auto& [axle, wheels] : axles)
        p.axleDescription.addAxle(PxU32(wheels.size()), wheels.data());
    std::vector<PxVec3> positions;
    for (auto& w : c.Wheels)
        positions.push_back(v.Chassis->getCMassLocalPose().transformInv(Px(w.AttachPosition)));
    std::vector<PxReal> sprung(c.Wheels.size(), p.rigidBodyParams.mass / float(c.Wheels.size()));
    // Solve the load distribution about the center of mass; SDK rejects degenerate layouts.
    if (!PxVehicleComputeSprungMasses(
            PxU32(positions.size()), positions.data(), p.rigidBodyParams.mass, PxVehicleAxes::ePosY, sprung.data()))
        std::fill(sprung.begin(), sprung.end(), p.rigidBodyParams.mass / float(c.Wheels.size()));
    for (PxU32 i = 0; i < c.Wheels.size(); ++i)
    {
        const auto& w           = c.Wheels[i];
        auto&       wp          = p.wheelParams[i];
        wp.radius               = w.Radius;
        wp.halfWidth            = w.Width * .5f;
        wp.mass                 = w.Mass;
        wp.moi                  = w.MomentOfInertia;
        wp.dampingRate          = .25f;
        auto& sp                = p.suspensionParams[i];
        sp.suspensionAttachment = PxTransform(positions[i]);
        sp.suspensionTravelDir  = Px(glm::normalize(w.SuspensionDirection));
        sp.suspensionTravelDist = w.SuspensionTravel;
        sp.wheelAttachment      = PxTransform(PxIdentity);
        auto& sc                = p.suspensionComplianceParams[i];
        sc.wheelToeAngle.clear();
        sc.wheelCamberAngle.clear();
        sc.suspForceAppPoint.clear();
        sc.tireForceAppPoint.clear();
        sc.wheelToeAngle.addPair(0, 0);
        sc.wheelCamberAngle.addPair(0, 0);
        sc.suspForceAppPoint.addPair(0, positions[i]);
        sc.tireForceAppPoint.addPair(0, positions[i]);
        p.suspensionForceParams[i].stiffness  = w.SpringStiffness;
        p.suspensionForceParams[i].damping    = w.SpringDamping;
        p.suspensionForceParams[i].sprungMass = sprung[i];
        auto& tp                              = p.tireForceParams[i];
        tp.latStiffX                          = 2;
        tp.latStiffY                          = w.LateralStiffness;
        tp.longStiff                          = w.LongitudinalStiffness;
        tp.camberStiff                        = 0;
        tp.restLoad                           = sprung[i] * glm::length(Settings.Gravity);
        for (int j = 0; j < 3; ++j)
        {
            tp.frictionVsSlip[j][0] = w.FrictionVsSlip[j * 2];
            tp.frictionVsSlip[j][1] = w.FrictionVsSlip[j * 2 + 1];
        }
        tp.loadFilter[0][0]                                  = 0;
        tp.loadFilter[0][1]                                  = 0;
        tp.loadFilter[1][0]                                  = 3;
        tp.loadFilter[1][1]                                  = 3;
        p.brakeResponseParams[0].wheelResponseMultipliers[i] = w.BrakeTorque / maxBrake;
        p.brakeResponseParams[1].wheelResponseMultipliers[i] = w.HandbrakeTorque / maxHandbrake;
        p.steerResponseParams.wheelResponseMultipliers[i]    = glm::radians(w.MaxSteerAngle) / maxSteer;
        sdk.mDirectDriveParams.directDriveThrottleResponseParams.wheelResponseMultipliers[i] =
            w.Driven ? w.TorqueSplit / maxSplit : 0;
        auto split = w.Driven && totalSplit > 0 ? w.TorqueSplit / totalSplit : 0;
        if (c.Differential == VehicleDifferential::OpenFront)
            split = w.Axle == frontAxle ? 1.f / float(axles[frontAxle].size()) : 0;
        if (c.Differential == VehicleDifferential::OpenRear)
            split = w.Axle == rearAxle ? 1.f / float(axles[rearAxle].size()) : 0;
        sdk.mEngineDriveParams.multiWheelDifferentialParams.torqueRatios[i]        = split;
        sdk.mEngineDriveParams.multiWheelDifferentialParams.aveWheelSpeedRatios[i] = split;
        if (w.Visual != UUID(0) && !v.VisualOffsets.count(w.Visual))
            if (auto visual = Owner.GetEntityByUUID(w.Visual))
                v.VisualOffsets[w.Visual] = glm::quat(visual.GetComponent<TransformComponent>().Rotation);
    }
    auto& ack       = p.ackermannParams[0];
    ack.wheelIds[0] = 0;
    ack.wheelIds[1] = c.Wheels.size() > 1 ? 1 : 0;
    ack.wheelBase   = 1;
    ack.trackWidth  = 1;
    ack.strength    = 0;
    if (c.Wheels.size() >= 4)
    {
        std::vector<uint32_t> steer;
        for (uint32_t j = 0; j < c.Wheels.size(); ++j)
            if (c.Wheels[j].MaxSteerAngle > 0)
                steer.push_back(j);
        if (steer.size() >= 2)
        {
            ack.wheelIds[0] = steer[0];
            ack.wheelIds[1] = steer[1];
            ack.trackWidth  = std::abs(c.Wheels[steer[0]].AttachPosition.x - c.Wheels[steer[1]].AttachPosition.x);
            float front     = c.Wheels[steer[0]].AttachPosition.z;
            for (auto& w : c.Wheels)
                ack.wheelBase = std::max(ack.wheelBase, std::abs(front - w.AttachPosition.z));
            ack.strength = c.Ackermann;
        }
    }
    sdk.AntiRollCount = PxU32(c.AntiRollBars.size());
    for (size_t j = 0; j < c.AntiRollBars.size(); ++j)
    {
        sdk.AntiRoll[j].wheel0    = c.AntiRollBars[j].WheelA;
        sdk.AntiRoll[j].wheel1    = c.AntiRollBars[j].WheelB;
        sdk.AntiRoll[j].stiffness = c.AntiRollBars[j].Stiffness;
    }
    auto& engine                               = sdk.mEngineDriveParams;
    auto& ep                                   = engine.engineParams;
    ep.moi                                     = c.EngineMomentOfInertia;
    ep.peakTorque                              = c.MaxTorque;
    ep.idleOmega                               = c.IdleRpm * glm::two_pi<float>() / 60;
    ep.maxOmega                                = c.MaxRpm * glm::two_pi<float>() / 60;
    ep.dampingRateFullThrottle                 = c.EngineDamping;
    ep.dampingRateZeroThrottleClutchEngaged    = c.EngineDamping * 2;
    ep.dampingRateZeroThrottleClutchDisengaged = c.EngineDamping;
    ep.torqueCurve.clear();
    for (uint32_t j = 0; j < 8; ++j)
    {
        float x = float(j) / 7;
        ep.torqueCurve.addPair(x, std::max(0.f, c.TorqueCurve.Sample(x, glm::vec4(1)).x));
    }
    engine.gearBoxParams.neutralGear = 1;
    engine.gearBoxParams.ratios[0]   = c.ReverseRatio;
    engine.gearBoxParams.ratios[1]   = 0;
    for (size_t j = 0; j < c.GearRatios.size(); ++j)
        engine.gearBoxParams.ratios[j + 2] = c.GearRatios[j];
    engine.gearBoxParams.nbRatios                  = PxU32(c.GearRatios.size() + 2);
    engine.gearBoxParams.finalRatio                = c.FinalDrive;
    engine.gearBoxParams.switchTime                = c.SwitchTime;
    engine.clutchParams.accuracyMode               = PxVehicleClutchAccuracyMode::eBEST_POSSIBLE;
    engine.clutchParams.estimateIterations         = 5;
    engine.clutchCommandResponseParams.maxResponse = c.ClutchStrength;
    for (uint32_t j = 0; j < engine.gearBoxParams.nbRatios; ++j)
    {
        engine.autoboxParams.upRatios[j]   = c.UpShiftRatio;
        engine.autoboxParams.downRatios[j] = c.DownShiftRatio;
    }
    engine.autoboxParams.latency                                   = c.SwitchTime;
    auto& diff                                                     = engine.fourWheelDifferentialParams;
    static_cast<PxVehicleMultiWheelDriveDifferentialParams&>(diff) = engine.multiWheelDifferentialParams;
    if (c.Wheels.size() == 4 && axles.size() == 2 && axles.begin()->second.size() == 2 &&
        axles.rbegin()->second.size() == 2)
    {
        diff.frontWheelIds[0] = axles.at(frontAxle)[0];
        diff.frontWheelIds[1] = axles.at(frontAxle)[1];
        diff.rearWheelIds[0]  = axles.at(rearAxle)[0];
        diff.rearWheelIds[1]  = axles.at(rearAxle)[1];
    }
    diff.frontBias = diff.rearBias = diff.centerBias = c.TorqueBias;
    diff.frontTarget = diff.rearTarget = diff.centerTarget = std::max(1.f, c.TorqueBias * .8f);
    diff.rate                                              = 10;
    sdk.mEngineDriveState.engineState.rotationSpeed        = ep.idleOmega;
    sdk.mEngineDriveState.gearboxState.currentGear         = 2;
    sdk.mEngineDriveState.gearboxState.targetGear          = 2;
    if (c.DriveModel == VehicleDriveModel::Direct)
    {
        v.Sequence.add(static_cast<PxVehicleDirectDriveCommandResponseComponent*>(&sdk));
        v.Sequence.add(static_cast<PxVehicleDirectDriveActuationStateComponent*>(&sdk));
    }
    else
    {
        v.Sequence.add(static_cast<PxVehicleEngineDriveCommandResponseComponent*>(&sdk));
        if (c.Differential == VehicleDifferential::LimitedSlip)
            v.Sequence.add(static_cast<PxVehicleFourWheelDriveDifferentialStateComponent*>(&sdk));
        else
            v.Sequence.add(static_cast<PxVehicleMultiWheelDriveDifferentialStateComponent*>(&sdk));
        v.Sequence.add(static_cast<PxVehicleEngineDriveActuationStateComponent*>(&sdk));
    }
    v.Substeps = v.Sequence.beginSubstepGroup(PxU8(c.LowSpeedSubsteps));
    v.Sequence.add(static_cast<PxVehicleSuspensionComponent*>(&sdk));
    v.Sequence.add(static_cast<PxVehicleTireComponent*>(&sdk));
    if (c.DriveModel == VehicleDriveModel::Direct)
        v.Sequence.add(static_cast<PxVehicleDirectDrivetrainComponent*>(&sdk));
    else
        v.Sequence.add(static_cast<PxVehicleEngineDrivetrainComponent*>(&sdk));
    v.Sequence.add(static_cast<PxVehicleRigidBodyComponent*>(&sdk));
    v.Sequence.endSubstepGroup();
    v.Sequence.add(static_cast<PxVehicleWheelComponent*>(&sdk));
    v.State.Wheels.resize(c.Wheels.size());
    v.Previous.resize(c.Wheels.size(), PxTransform(PxIdentity));
    v.Current = v.Previous;
}
struct RoadFilter : PxQueryFilterCallback
{
    const PxRigidActor* Chassis;
    uint32_t            Mask;
    RoadFilter(const PxRigidActor* actor, uint32_t mask) : Chassis(actor), Mask(mask) {}
    PxQueryHitType::Enum
    preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor, PxHitFlags&) override
    {
        return actor != Chassis && !(shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE) &&
                       (shape->getQueryFilterData().word0 & Mask) ?
                   PxQueryHitType::eBLOCK :
                   PxQueryHitType::eNONE;
    }
    PxQueryHitType::Enum
    postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override
    {
        return PxQueryHitType::eBLOCK;
    }
};
void UpdateVehicles(float dt)
{
    std::set<UUID> live;
    for (auto h : Owner.m_Registry.view<VehicleComponent, RigidBodyComponent>())
    {
        Entity entity {h, &Owner};
        if (!Owner.IsEntityActive(entity))
            continue;
        auto id = entity.GetUUID();
        auto it = Bodies.find(id);
        if (it == Bodies.end() || it->second.Settings.Type != RigidBodyType::Dynamic)
        {
            Error(id, "Vehicle requires a dynamic chassis rigid body");
            continue;
        }
        const auto& c = entity.GetComponent<VehicleComponent>();
        try
        {
            PhysicsYaml::Encode(c);
            for (const auto& wheel : c.Wheels)
                if (wheel.Visual != UUID(0))
                {
                    auto visual = Owner.GetEntityByUUID(wheel.Visual);
                    if (!visual || !Owner.IsDescendantOf(visual, entity))
                        throw std::invalid_argument("Vehicle wheel visual must be a child of the chassis");
                }
            if (c.Differential == VehicleDifferential::LimitedSlip && c.Wheels.size() != 4)
                throw std::invalid_argument("Limited-slip differential requires four wheels; use MultiWheel otherwise");
        }
        catch (const std::exception& e)
        {
            Error(id, e.what());
            continue;
        }
        live.insert(id);
        auto& owned     = Vehicles[id];
        auto  config    = c;
        config.Throttle = config.Brake = config.Steer = config.Handbrake = 0;
        config.TargetGear                                                = 1;
        config.AutomaticGearbox                                          = true;
        auto old                                                         = owned ? owned->Settings : config;
        old.Throttle = old.Brake = old.Steer = old.Handbrake = 0;
        old.TargetGear                                       = 1;
        old.AutomaticGearbox                                 = true;
        if (!owned || config != old || owned->Generation != it->second.Generation)
        {
            auto offsets         = owned ? owned->VisualOffsets : std::map<UUID, glm::quat>();
            owned                = std::make_unique<VehicleRuntime>();
            owned->VisualOffsets = std::move(offsets);
            CreateVehicle(*owned, c, it->second);
        }
        auto& v                              = *owned;
        auto& sdk                            = v.SDK;
        auto& state                          = sdk.mBaseState;
        auto* actor                          = v.Chassis;
        state.rigidBodyState.pose            = actor->getGlobalPose() * actor->getCMassLocalPose();
        state.rigidBodyState.linearVelocity  = actor->getLinearVelocity();
        state.rigidBodyState.angularVelocity = actor->getAngularVelocity();
        state.rigidBodyState.externalForce   = PxVec3(0);
        state.rigidBodyState.externalTorque  = PxVec3(0);
        sdk.mCommandState.throttle           = glm::clamp(std::abs(c.Throttle), 0.f, 1.f);
        sdk.mCommandState.steer              = glm::clamp(c.Steer, -1.f, 1.f);
        sdk.mCommandState.nbBrakes           = 2;
        sdk.mCommandState.brakes[0]          = glm::clamp(c.Brake, 0.f, 1.f);
        sdk.mCommandState.brakes[1]          = glm::clamp(c.Handbrake, 0.f, 1.f);
        sdk.DirectTransmission.gear          = c.Throttle < 0 ? PxVehicleDirectDriveTransmissionCommandState::eREVERSE :
                                                                PxVehicleDirectDriveTransmissionCommandState::eFORWARD;
        sdk.mTransmissionCommandState.clutch = 0;
        sdk.mTransmissionCommandState.targetGear =
            c.AutomaticGearbox ? PxVehicleEngineDriveTransmissionCommandState::eAUTOMATIC_GEAR :
                                 PxU32(glm::clamp(c.TargetGear + 1, 0, int(c.GearRatios.size() + 1)));
        RoadFilter        filter(actor, c.RoadLayerMask);
        PxQueryFilterData query(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
        for (size_t j = 0; j < c.Wheels.size(); ++j)
        {
            auto& w    = c.Wheels[j];
            auto& road = state.roadGeomStates[j];
            road.setToDefault();
            auto  origin      = actor->getGlobalPose().transform(Px(w.AttachPosition));
            auto  direction   = actor->getGlobalPose().q.rotate(Px(glm::normalize(w.SuspensionDirection)));
            auto& read        = v.State.Wheels[j];
            read.Grounded     = false;
            read.GroundEntity = 0;
            PxRaycastBuffer      ray;
            PxSweepBuffer        sweep;
            const PxLocationHit* hit    = nullptr;
            const PxShape*       shape  = nullptr;
            const PxRigidActor*  ground = nullptr;
            if (c.RoadQuery == VehicleRoadQuery::SphereSweep)
            {
                PxSphereGeometry wheel(w.Radius);
                if (World->sweep(wheel,
                                 PxTransform(origin),
                                 direction,
                                 w.SuspensionTravel,
                                 sweep,
                                 PxHitFlag::eDEFAULT,
                                 query,
                                 &filter))
                {
                    hit    = &sweep.block;
                    shape  = sweep.block.shape;
                    ground = sweep.block.actor;
                }
            }
            else if (World->raycast(
                         origin, direction, w.SuspensionTravel + w.Radius, ray, PxHitFlag::eDEFAULT, query, &filter))
            {
                hit    = &ray.block;
                shape  = ray.block.shape;
                ground = ray.block.actor;
            }
            if (hit)
            {
                read.Grounded        = true;
                read.GroundEntity    = Identity(ground->userData);
                read.ContactPoint    = Gl(hit->position);
                read.ContactNormal   = Gl(hit->normal);
                road.hitState        = true;
                road.plane           = PxPlane(hit->position, hit->normal);
                road.friction        = 1;
                road.velocity        = PxVec3(0);
                PxMaterial* material = nullptr;
                shape->getMaterials(&material, 1);
                if (material)
                {
                    road.friction = material->getDynamicFriction();
                    for (auto& [asset, m] : Materials)
                        if (m.Material == material)
                            for (auto& f : w.MaterialFriction)
                                if (f.Material == asset)
                                    road.friction *= f.Multiplier;
                }
                if (auto* dynamic = ground->is<PxRigidDynamic>())
                    road.velocity = PxRigidBodyExt::getVelocityAtPos(*dynamic, hit->position);
            }
        }
        PxVehicleSimulationContext context;
        context.setToDefault();
        context.frame   = sdk.mBaseParams.frame;
        context.gravity = Px(Settings.Gravity);
        v.State.ForwardSpeed =
            state.rigidBodyState.linearVelocity.dot(state.rigidBodyState.pose.q.rotate(PxVec3(0, 0, 1)));
        v.Sequence.setSubsteps(
            v.Substeps,
            PxU8(std::abs(v.State.ForwardSpeed) < c.SubstepSpeedThreshold ? c.LowSpeedSubsteps : c.HighSpeedSubsteps));
        v.Sequence.update(dt, context);
        actor->setLinearVelocity(state.rigidBodyState.linearVelocity - context.gravity * dt, false);
        actor->setAngularVelocity(state.rigidBodyState.angularVelocity, false);
        if (c.Throttle != 0 || c.Brake != 0 || c.Handbrake != 0)
            actor->wakeUp();
        v.State.EngineRpm        = sdk.mEngineDriveState.engineState.rotationSpeed * 60 / glm::two_pi<float>();
        v.State.CurrentGear      = c.DriveModel == VehicleDriveModel::Engine ?
                                       int32_t(sdk.mEngineDriveState.gearboxState.currentGear) - 1 :
                                       (c.Throttle < 0 ? -1 : 1);
        v.State.TotalDriveTorque = 0;
        for (size_t j = 0; j < c.Wheels.size(); ++j)
        {
            auto& read                 = v.State.Wheels[j];
            read.SuspensionCompression = state.suspensionStates[j].jounce;
            read.LongitudinalSlip      = state.tireSlipStates[j].slips[0];
            read.LateralSlip           = state.tireSlipStates[j].slips[1];
            read.AngularSpeed          = state.wheelRigidBody1dStates[j].rotationSpeed;
            v.Previous[j]              = v.Current[j];
            v.Current[j]               = actor->getCMassLocalPose() * state.wheelLocalPoses[j].localPose;
            if (c.DriveModel == VehicleDriveModel::Direct)
                v.State.TotalDriveTorque += sdk.mDirectDriveState.directDriveThrottleResponseStates[j];
        }
        if (c.DriveModel == VehicleDriveModel::Engine)
        {
            auto        gear    = sdk.mEngineDriveState.gearboxState.currentGear;
            const auto& engine  = sdk.mEngineDriveParams.engineParams;
            const auto& gearbox = sdk.mEngineDriveState.gearboxState;
            if (gear != sdk.mEngineDriveParams.gearBoxParams.neutralGear &&
                gearbox.gearSwitchTime == PX_VEHICLE_NO_GEAR_SWITCH_PENDING &&
                gearbox.currentGear == gearbox.targetGear)
            {
                float normalisedRpm =
                    glm::clamp(sdk.mEngineDriveState.engineState.rotationSpeed / engine.maxOmega, 0.f, 1.f);
                v.State.TotalDriveTorque = engine.torqueCurve.interpolate(normalisedRpm) * engine.peakTorque *
                                           c.Throttle * sdk.mEngineDriveParams.gearBoxParams.ratios[gear] *
                                           c.FinalDrive;
            }
        }
    }
    for (auto it = Vehicles.begin(); it != Vehicles.end();)
        if (!live.count(it->first))
            it = Vehicles.erase(it);
        else
            ++it;
}
void WriteVehicles(float alpha)
{
    for (auto& [id, owned] : Vehicles)
    {
        auto& v    = *owned;
        auto  body = Bodies.find(id);
        if (body == Bodies.end())
            continue;
        auto chassis = PxTransform(Px(glm::mix(Gl(body->second.Previous.p), Gl(body->second.Current.p), alpha)),
                                   Px(glm::slerp(Gl(body->second.Previous.q), Gl(body->second.Current.q), alpha)));
        for (size_t j = 0; j < v.Current.size(); ++j)
        {
            auto local                 = PxTransform(Px(glm::mix(Gl(v.Previous[j].p), Gl(v.Current[j].p), alpha)),
                                                     Px(glm::slerp(Gl(v.Previous[j].q), Gl(v.Current[j].q), alpha)));
            auto pose                  = chassis * local;
            v.State.Wheels[j].Position = Gl(pose.p);
            v.State.Wheels[j].Rotation = Gl(pose.q);
            if (auto visual = Owner.GetEntityByUUID(v.Settings.Wheels[j].Visual))
            {
                auto offset   = v.VisualOffsets.find(visual.GetUUID());
                auto rotation = Gl(pose.q) * (offset == v.VisualOffsets.end() ? glm::quat(1, 0, 0, 0) : offset->second);
                auto scale    = JointTransform::FromMatrix(Owner.GetWorldTransform(visual)).Scale;
                Owner.SetWorldTransform(visual,
                                        glm::translate(glm::mat4(1), Gl(pose.p)) * glm::mat4_cast(rotation) *
                                            glm::scale(glm::mat4(1), scale));
            }
        }
    }
}
