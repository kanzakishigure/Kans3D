// Copyright (c) 2001-2004 NovodeX AG. All rights reserved.
// Copyright (c) 2004-2008 AGEIA Technologies, Inc. All rights reserved.
// SPDX-FileCopyrightText: Copyright (c) 2008-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Adapted SDK data bindings; simulation logic is provided by PhysXVehicle.
#pragma once
#include <vehicle/PxVehicleAPI.h>
namespace Kans::VehicleSDK
{
    using namespace physx;
    struct BaseVehicleParams
    {
        PxVehicleAxleDescription                  axleDescription;
        PxVehicleFrame                            frame;
        PxVehicleScale                            scale;
        PxVehicleSuspensionStateCalculationParams suspensionStateCalculationParams;

        // Command response
        PxVehicleBrakeCommandResponseParams brakeResponseParams[2];
        PxVehicleSteerCommandResponseParams steerResponseParams;
        PxVehicleAckermannParams            ackermannParams[1];

        // Suspension
        PxVehicleSuspensionParams           suspensionParams[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleSuspensionComplianceParams suspensionComplianceParams[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleSuspensionForceParams      suspensionForceParams[PxVehicleLimits::eMAX_NB_WHEELS];

        // Tires
        PxVehicleTireForceParams tireForceParams[PxVehicleLimits::eMAX_NB_WHEELS];

        // Wheels
        PxVehicleWheelParams wheelParams[PxVehicleLimits::eMAX_NB_WHEELS];

        // Rigid body
        PxVehicleRigidBodyParams rigidBodyParams;

        BaseVehicleParams transformAndScale(const PxVehicleFrame& srcFrame,
                                            const PxVehicleFrame& trgFrame,
                                            const PxVehicleScale& srcScale,
                                            const PxVehicleScale& trgScale) const;

        PX_FORCE_INLINE bool isValid() const
        {
            if (!axleDescription.isValid())
                return false;
            if (!frame.isValid())
                return true;
            if (!scale.isValid())
                return false;
            if (!suspensionStateCalculationParams.isValid())
                return false;

            if (!brakeResponseParams[0].isValid(axleDescription))
                return false;
            if (!brakeResponseParams[1].isValid(axleDescription))
                return false;
            if (!steerResponseParams.isValid(axleDescription))
                return false;
            if (!ackermannParams[0].isValid(axleDescription))
                return false;

            for (PxU32 i = 0; i < axleDescription.nbWheels; i++)
            {
                const PxU32 wheelId = axleDescription.wheelIdsInAxleOrder[i];

                if (!suspensionParams[wheelId].isValid())
                    return false;
                if (!suspensionComplianceParams[wheelId].isValid())
                    return false;
                if (!suspensionForceParams[wheelId].isValid())
                    return false;

                if (!tireForceParams[wheelId].isValid())
                    return false;

                if (!wheelParams[wheelId].isValid())
                    return false;
            }

            if (!rigidBodyParams.isValid())
                return false;

            return true;
        }
    };

    struct BaseVehicleState
    {
        // Command responses
        PxReal                       brakeCommandResponseStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxReal                       steerCommandResponseStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleWheelActuationState actuationStates[PxVehicleLimits::eMAX_NB_WHEELS];

        // Road geometry
        PxVehicleRoadGeometryState roadGeomStates[PxVehicleLimits::eMAX_NB_WHEELS];

        // Suspensions
        PxVehicleSuspensionState           suspensionStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleSuspensionComplianceState suspensionComplianceStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleSuspensionForce           suspensionForces[PxVehicleLimits::eMAX_NB_WHEELS];

        // Tires
        PxVehicleTireGripState        tireGripStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleTireDirectionState   tireDirectionStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleTireSpeedState       tireSpeedStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleTireSlipState        tireSlipStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleTireCamberAngleState tireCamberAngleStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleTireStickyState      tireStickyStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleTireForce            tireForces[PxVehicleLimits::eMAX_NB_WHEELS];

        // Wheels
        PxVehicleWheelRigidBody1dState wheelRigidBody1dStates[PxVehicleLimits::eMAX_NB_WHEELS];
        PxVehicleWheelLocalPose        wheelLocalPoses[PxVehicleLimits::eMAX_NB_WHEELS];

        // Rigid body
        PxVehicleRigidBodyState rigidBodyState;

        PX_FORCE_INLINE void setToDefault()
        {
            for (unsigned int i = 0; i < PxVehicleLimits::eMAX_NB_WHEELS; i++)
            {
                brakeCommandResponseStates[i] = 0.0;
                steerCommandResponseStates[i] = 0.0f;

                actuationStates[i].setToDefault();

                roadGeomStates[i].setToDefault();

                suspensionStates[i].setToDefault();
                suspensionComplianceStates[i].setToDefault();
                suspensionForces[i].setToDefault();

                tireGripStates[i].setToDefault();
                tireDirectionStates[i].setToDefault();
                tireSpeedStates[i].setToDefault();
                tireSlipStates[i].setToDefault();
                tireCamberAngleStates[i].setToDefault();
                tireStickyStates[i].setToDefault();
                tireForces[i].setToDefault();

                wheelRigidBody1dStates[i].setToDefault();
                wheelLocalPoses[i].setToDefault();
            }

            rigidBodyState.setToDefault();
        }
    };

    struct DirectDrivetrainParams
    {
        PxVehicleDirectDriveThrottleCommandResponseParams directDriveThrottleResponseParams;

        DirectDrivetrainParams transformAndScale(const PxVehicleFrame& srcFrame,
                                                 const PxVehicleFrame& trgFrame,
                                                 const PxVehicleScale& srcScale,
                                                 const PxVehicleScale& trgScale) const;

        PX_FORCE_INLINE bool isValid(const PxVehicleAxleDescription& axleDesc) const
        {
            if (!directDriveThrottleResponseParams.isValid(axleDesc))
                return false;

            return true;
        }
    };

    struct DirectDrivetrainState
    {
        PxReal directDriveThrottleResponseStates[PxVehicleLimits::eMAX_NB_WHEELS];

        PX_FORCE_INLINE void setToDefault() { PxMemZero(this, sizeof(DirectDrivetrainState)); }
    };

    struct EngineDrivetrainParams
    {
        PxVehicleAutoboxParams                     autoboxParams;
        PxVehicleClutchCommandResponseParams       clutchCommandResponseParams;
        PxVehicleEngineParams                      engineParams;
        PxVehicleGearboxParams                     gearBoxParams;
        PxVehicleMultiWheelDriveDifferentialParams multiWheelDifferentialParams;
        PxVehicleFourWheelDriveDifferentialParams  fourWheelDifferentialParams;
        PxVehicleTankDriveDifferentialParams       tankDifferentialParams;
        PxVehicleClutchParams                      clutchParams;

        EngineDrivetrainParams transformAndScale(const PxVehicleFrame& srcFrame,
                                                 const PxVehicleFrame& trgFrame,
                                                 const PxVehicleScale& srcScale,
                                                 const PxVehicleScale& trgScale) const;

        PX_FORCE_INLINE bool isValid(const PxVehicleAxleDescription& axleDesc) const
        {
            if (!autoboxParams.isValid(gearBoxParams))
                return false;
            if (!clutchCommandResponseParams.isValid())
                return false;
            if (!engineParams.isValid())
                return false;
            if (!gearBoxParams.isValid())
                return false;
            if (!multiWheelDifferentialParams.isValid(axleDesc))
                return false;
            if (!fourWheelDifferentialParams.isValid(axleDesc))
                return false;
            if (!tankDifferentialParams.isValid(axleDesc))
                return false;
            if (!clutchParams.isValid())
                return false;
            return true;
        }
    };

    struct EngineDrivetrainState
    {
        PxVehicleEngineDriveThrottleCommandResponseState throttleCommandResponseState;
        PxVehicleAutoboxState                            autoboxState;
        PxVehicleClutchCommandResponseState              clutchCommandResponseState;
        PxVehicleDifferentialState                       differentialState;
        PxVehicleWheelConstraintGroupState               wheelConstraintGroupState;
        PxVehicleEngineState                             engineState;
        PxVehicleGearboxState                            gearboxState;
        PxVehicleClutchSlipState                         clutchState;

        PX_FORCE_INLINE void setToDefault()
        {
            throttleCommandResponseState.setToDefault();
            autoboxState.setToDefault();
            clutchCommandResponseState.setToDefault();
            differentialState.setToDefault();
            wheelConstraintGroupState.setToDefault();
            engineState.setToDefault();
            gearboxState.setToDefault();
            clutchState.setToDefault();
        }
    };

    class Vehicle : public PxVehicleRigidBodyComponent,
                    public PxVehicleSuspensionComponent,
                    public PxVehicleTireComponent,
                    public PxVehicleWheelComponent,
                    public PxVehicleDirectDriveCommandResponseComponent,
                    public PxVehicleDirectDriveActuationStateComponent,
                    public PxVehicleDirectDrivetrainComponent,
                    public PxVehicleEngineDriveCommandResponseComponent,
                    public PxVehicleFourWheelDriveDifferentialStateComponent,
                    public PxVehicleMultiWheelDriveDifferentialStateComponent,
                    public PxVehicleEngineDriveActuationStateComponent,
                    public PxVehicleEngineDrivetrainComponent
    {
    public:
        virtual void getDataForRigidBodyComponent(const PxVehicleAxleDescription*&                    axleDescription,
                                                  const PxVehicleRigidBodyParams*&                    rigidBodyParams,
                                                  PxVehicleArrayData<const PxVehicleSuspensionForce>& suspensionForces,
                                                  PxVehicleArrayData<const PxVehicleTireForce>&       tireForces,
                                                  const PxVehicleAntiRollTorque*&                     antiRollTorque,
                                                  PxVehicleRigidBodyState*&                           rigidBodyState)
        {
            axleDescription = &mBaseParams.axleDescription;
            rigidBodyParams = &mBaseParams.rigidBodyParams;
            suspensionForces.setData(mBaseState.suspensionForces);
            tireForces.setData(mBaseState.tireForces);
            antiRollTorque = &AntiRollTorque;
            rigidBodyState = &mBaseState.rigidBodyState;
        }
        virtual void getDataForSuspensionComponent(
            const PxVehicleAxleDescription*&                               axleDescription,
            const PxVehicleRigidBodyParams*&                               rigidBodyParams,
            const PxVehicleSuspensionStateCalculationParams*&              suspensionStateCalculationParams,
            PxVehicleArrayData<const PxReal>&                              steerResponseStates,
            const PxVehicleRigidBodyState*&                                rigidBodyState,
            PxVehicleArrayData<const PxVehicleWheelParams>&                wheelParams,
            PxVehicleArrayData<const PxVehicleSuspensionParams>&           suspensionParams,
            PxVehicleArrayData<const PxVehicleSuspensionComplianceParams>& suspensionComplianceParams,
            PxVehicleArrayData<const PxVehicleSuspensionForceParams>&      suspensionForceParams,
            PxVehicleSizedArrayData<const PxVehicleAntiRollForceParams>&   antiRollForceParams,
            PxVehicleArrayData<const PxVehicleRoadGeometryState>&          wheelRoadGeomStates,
            PxVehicleArrayData<PxVehicleSuspensionState>&                  suspensionStates,
            PxVehicleArrayData<PxVehicleSuspensionComplianceState>&        suspensionComplianceStates,
            PxVehicleArrayData<PxVehicleSuspensionForce>&                  suspensionForces,
            PxVehicleAntiRollTorque*&                                      antiRollTorque)
        {
            axleDescription                  = &mBaseParams.axleDescription;
            rigidBodyParams                  = &mBaseParams.rigidBodyParams;
            suspensionStateCalculationParams = &mBaseParams.suspensionStateCalculationParams;
            steerResponseStates.setData(mBaseState.steerCommandResponseStates);
            rigidBodyState = &mBaseState.rigidBodyState;
            wheelParams.setData(mBaseParams.wheelParams);
            suspensionParams.setData(mBaseParams.suspensionParams);
            suspensionComplianceParams.setData(mBaseParams.suspensionComplianceParams);
            suspensionForceParams.setData(mBaseParams.suspensionForceParams);
            antiRollForceParams.setDataAndCount(AntiRoll, AntiRollCount);
            wheelRoadGeomStates.setData(mBaseState.roadGeomStates);
            suspensionStates.setData(mBaseState.suspensionStates);
            suspensionComplianceStates.setData(mBaseState.suspensionComplianceStates);
            suspensionForces.setData(mBaseState.suspensionForces);
            antiRollTorque = &AntiRollTorque;
        }
        virtual void getDataForTireComponent(
            const PxVehicleAxleDescription*&                              axleDescription,
            PxVehicleArrayData<const PxReal>&                             steerResponseStates,
            const PxVehicleRigidBodyState*&                               rigidBodyState,
            PxVehicleArrayData<const PxVehicleWheelActuationState>&       actuationStates,
            PxVehicleArrayData<const PxVehicleWheelParams>&               wheelParams,
            PxVehicleArrayData<const PxVehicleSuspensionParams>&          suspensionParams,
            PxVehicleArrayData<const PxVehicleTireForceParams>&           tireForceParams,
            PxVehicleArrayData<const PxVehicleRoadGeometryState>&         roadGeomStates,
            PxVehicleArrayData<const PxVehicleSuspensionState>&           suspensionStates,
            PxVehicleArrayData<const PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
            PxVehicleArrayData<const PxVehicleSuspensionForce>&           suspensionForces,
            PxVehicleArrayData<const PxVehicleWheelRigidBody1dState>&     wheelRigidBody1DStates,
            PxVehicleArrayData<PxVehicleTireGripState>&                   tireGripStates,
            PxVehicleArrayData<PxVehicleTireDirectionState>&              tireDirectionStates,
            PxVehicleArrayData<PxVehicleTireSpeedState>&                  tireSpeedStates,
            PxVehicleArrayData<PxVehicleTireSlipState>&                   tireSlipStates,
            PxVehicleArrayData<PxVehicleTireCamberAngleState>&            tireCamberAngleStates,
            PxVehicleArrayData<PxVehicleTireStickyState>&                 tireStickyStates,
            PxVehicleArrayData<PxVehicleTireForce>&                       tireForces)
        {
            axleDescription = &mBaseParams.axleDescription;
            steerResponseStates.setData(mBaseState.steerCommandResponseStates);
            rigidBodyState = &mBaseState.rigidBodyState;
            actuationStates.setData(mBaseState.actuationStates);
            wheelParams.setData(mBaseParams.wheelParams);
            suspensionParams.setData(mBaseParams.suspensionParams);
            tireForceParams.setData(mBaseParams.tireForceParams);
            roadGeomStates.setData(mBaseState.roadGeomStates);
            suspensionStates.setData(mBaseState.suspensionStates);
            suspensionComplianceStates.setData(mBaseState.suspensionComplianceStates);
            suspensionForces.setData(mBaseState.suspensionForces);
            wheelRigidBody1DStates.setData(mBaseState.wheelRigidBody1dStates);
            tireGripStates.setData(mBaseState.tireGripStates);
            tireDirectionStates.setData(mBaseState.tireDirectionStates);
            tireSpeedStates.setData(mBaseState.tireSpeedStates);
            tireSlipStates.setData(mBaseState.tireSlipStates);
            tireCamberAngleStates.setData(mBaseState.tireCamberAngleStates);
            tireStickyStates.setData(mBaseState.tireStickyStates);
            tireForces.setData(mBaseState.tireForces);
        }
        virtual void getDataForWheelComponent(
            const PxVehicleAxleDescription*&                              axleDescription,
            PxVehicleArrayData<const PxReal>&                             steerResponseStates,
            PxVehicleArrayData<const PxVehicleWheelParams>&               wheelParams,
            PxVehicleArrayData<const PxVehicleSuspensionParams>&          suspensionParams,
            PxVehicleArrayData<const PxVehicleWheelActuationState>&       actuationStates,
            PxVehicleArrayData<const PxVehicleSuspensionState>&           suspensionStates,
            PxVehicleArrayData<const PxVehicleSuspensionComplianceState>& suspensionComplianceStates,
            PxVehicleArrayData<const PxVehicleTireSpeedState>&            tireSpeedStates,
            PxVehicleArrayData<PxVehicleWheelRigidBody1dState>&           wheelRigidBody1dStates,
            PxVehicleArrayData<PxVehicleWheelLocalPose>&                  wheelLocalPoses)
        {
            axleDescription = &mBaseParams.axleDescription;
            steerResponseStates.setData(mBaseState.steerCommandResponseStates);
            wheelParams.setData(mBaseParams.wheelParams);
            suspensionParams.setData(mBaseParams.suspensionParams);
            actuationStates.setData(mBaseState.actuationStates);
            suspensionStates.setData(mBaseState.suspensionStates);
            suspensionComplianceStates.setData(mBaseState.suspensionComplianceStates);
            tireSpeedStates.setData(mBaseState.tireSpeedStates);
            wheelRigidBody1dStates.setData(mBaseState.wheelRigidBody1dStates);
            wheelLocalPoses.setData(mBaseState.wheelLocalPoses);
        }
        void getDataForDirectDriveCommandResponseComponent(
            const PxVehicleAxleDescription*&                                    axleDescription,
            PxVehicleSizedArrayData<const PxVehicleBrakeCommandResponseParams>& brakeResponseParams,
            const PxVehicleDirectDriveThrottleCommandResponseParams*&           throttleResponseParams,
            const PxVehicleSteerCommandResponseParams*&                         steerResponseParams,
            PxVehicleSizedArrayData<const PxVehicleAckermannParams>&            ackermannParams,
            const PxVehicleCommandState*&                                       commands,
            const PxVehicleDirectDriveTransmissionCommandState*&                transmissionCommands,
            const PxVehicleRigidBodyState*&                                     rigidBodyState,
            PxVehicleArrayData<PxReal>&                                         brakeResponseStates,
            PxVehicleArrayData<PxReal>&                                         throttleResponseStates,
            PxVehicleArrayData<PxReal>&                                         steerResponseStates)
        {
            axleDescription = &mBaseParams.axleDescription;
            brakeResponseParams.setDataAndCount(mBaseParams.brakeResponseParams,
                                                sizeof(mBaseParams.brakeResponseParams) /
                                                    sizeof(PxVehicleBrakeCommandResponseParams));
            throttleResponseParams = &mDirectDriveParams.directDriveThrottleResponseParams;
            steerResponseParams    = &mBaseParams.steerResponseParams;
            ackermannParams.setDataAndCount(mBaseParams.ackermannParams,
                                            sizeof(mBaseParams.ackermannParams) / sizeof(PxVehicleAckermannParams));
            commands             = &mCommandState;
            transmissionCommands = &DirectTransmission;
            rigidBodyState       = &mBaseState.rigidBodyState;
            brakeResponseStates.setData(mBaseState.brakeCommandResponseStates);
            throttleResponseStates.setData(mDirectDriveState.directDriveThrottleResponseStates);
            steerResponseStates.setData(mBaseState.steerCommandResponseStates);
        }
        virtual void
        getDataForDirectDriveActuationStateComponent(const PxVehicleAxleDescription*&  axleDescription,
                                                     PxVehicleArrayData<const PxReal>& brakeResponseStates,
                                                     PxVehicleArrayData<const PxReal>& throttleResponseStates,
                                                     PxVehicleArrayData<PxVehicleWheelActuationState>& actuationStates)
        {
            axleDescription = &mBaseParams.axleDescription;
            brakeResponseStates.setData(mBaseState.brakeCommandResponseStates);
            throttleResponseStates.setData(mDirectDriveState.directDriveThrottleResponseStates);
            actuationStates.setData(mBaseState.actuationStates);
        }
        virtual void
        getDataForDirectDrivetrainComponent(const PxVehicleAxleDescription*&                axleDescription,
                                            PxVehicleArrayData<const PxReal>&               brakeResponseStates,
                                            PxVehicleArrayData<const PxReal>&               throttleResponseStates,
                                            PxVehicleArrayData<const PxVehicleWheelParams>& wheelParams,
                                            PxVehicleArrayData<const PxVehicleWheelActuationState>& actuationStates,
                                            PxVehicleArrayData<const PxVehicleTireForce>&           tireForces,
                                            PxVehicleArrayData<PxVehicleWheelRigidBody1dState>& wheelRigidBody1dStates)
        {
            axleDescription = &mBaseParams.axleDescription;
            brakeResponseStates.setData(mBaseState.brakeCommandResponseStates);
            throttleResponseStates.setData(mDirectDriveState.directDriveThrottleResponseStates);
            wheelParams.setData(mBaseParams.wheelParams);
            actuationStates.setData(mBaseState.actuationStates);
            tireForces.setData(mBaseState.tireForces);
            wheelRigidBody1dStates.setData(mBaseState.wheelRigidBody1dStates);
        }
        virtual void getDataForEngineDriveCommandResponseComponent(
            const PxVehicleAxleDescription*&                                    axleDescription,
            PxVehicleSizedArrayData<const PxVehicleBrakeCommandResponseParams>& brakeResponseParams,
            const PxVehicleSteerCommandResponseParams*&                         steerResponseParams,
            PxVehicleSizedArrayData<const PxVehicleAckermannParams>&            ackermannParams,
            const PxVehicleGearboxParams*&                                      gearboxParams,
            const PxVehicleClutchCommandResponseParams*&                        clutchResponseParams,
            const PxVehicleEngineParams*&                                       engineParams,
            const PxVehicleRigidBodyState*&                                     rigidBodyState,
            const PxVehicleEngineState*&                                        engineState,
            const PxVehicleAutoboxParams*&                                      autoboxParams,
            const PxVehicleCommandState*&                                       commands,
            const PxVehicleEngineDriveTransmissionCommandState*&                transmissionCommands,
            PxVehicleArrayData<PxReal>&                                         brakeResponseStates,
            PxVehicleEngineDriveThrottleCommandResponseState*&                  throttleResponseState,
            PxVehicleArrayData<PxReal>&                                         steerResponseStates,
            PxVehicleGearboxState*&                                             gearboxResponseState,
            PxVehicleClutchCommandResponseState*&                               clutchResponseState,
            PxVehicleAutoboxState*&                                             autoboxState)
        {
            axleDescription = &mBaseParams.axleDescription;
            brakeResponseParams.setDataAndCount(mBaseParams.brakeResponseParams,
                                                sizeof(mBaseParams.brakeResponseParams) /
                                                    sizeof(PxVehicleBrakeCommandResponseParams));
            steerResponseParams = &mBaseParams.steerResponseParams;
            ackermannParams.setDataAndCount(mBaseParams.ackermannParams,
                                            sizeof(mBaseParams.ackermannParams) / sizeof(PxVehicleAckermannParams));
            gearboxParams        = &mEngineDriveParams.gearBoxParams;
            clutchResponseParams = &mEngineDriveParams.clutchCommandResponseParams;
            engineParams         = &mEngineDriveParams.engineParams;
            rigidBodyState       = &mBaseState.rigidBodyState;
            engineState          = &mEngineDriveState.engineState;
            autoboxParams        = &mEngineDriveParams.autoboxParams;
            commands             = &mCommandState;
            transmissionCommands = &mTransmissionCommandState;
            brakeResponseStates.setData(mBaseState.brakeCommandResponseStates);
            throttleResponseState = &mEngineDriveState.throttleCommandResponseState;
            steerResponseStates.setData(mBaseState.steerCommandResponseStates);
            gearboxResponseState = &mEngineDriveState.gearboxState;
            clutchResponseState  = &mEngineDriveState.clutchCommandResponseState;
            autoboxState         = &mEngineDriveState.autoboxState;
        }
        virtual void getDataForFourWheelDriveDifferentialStateComponent(
            const PxVehicleAxleDescription*&                          axleDescription,
            const PxVehicleFourWheelDriveDifferentialParams*&         differentialParams,
            PxVehicleArrayData<const PxVehicleWheelRigidBody1dState>& wheelRigidbody1dStates,
            PxVehicleDifferentialState*&                              differentialState,
            PxVehicleWheelConstraintGroupState*&                      wheelConstraintGroups)
        {
            axleDescription    = &mBaseParams.axleDescription;
            differentialParams = &mEngineDriveParams.fourWheelDifferentialParams;
            wheelRigidbody1dStates.setData(mBaseState.wheelRigidBody1dStates);
            differentialState     = &mEngineDriveState.differentialState;
            wheelConstraintGroups = &mEngineDriveState.wheelConstraintGroupState;
        }
        virtual void getDataForMultiWheelDriveDifferentialStateComponent(
            const PxVehicleAxleDescription*&                   axleDescription,
            const PxVehicleMultiWheelDriveDifferentialParams*& differentialParams,
            PxVehicleDifferentialState*&                       differentialState)
        {
            axleDescription    = &mBaseParams.axleDescription;
            differentialParams = &mEngineDriveParams.multiWheelDifferentialParams;
            differentialState  = &mEngineDriveState.differentialState;
        }
        virtual void getDataForEngineDriveActuationStateComponent(
            const PxVehicleAxleDescription*&                         axleDescription,
            const PxVehicleGearboxParams*&                           gearboxParams,
            PxVehicleArrayData<const PxReal>&                        brakeResponseStates,
            const PxVehicleEngineDriveThrottleCommandResponseState*& throttleResponseState,
            const PxVehicleGearboxState*&                            gearboxState,
            const PxVehicleDifferentialState*&                       differentialState,
            const PxVehicleClutchCommandResponseState*&              clutchResponseState,
            PxVehicleArrayData<PxVehicleWheelActuationState>&        actuationStates)
        {
            axleDescription = &mBaseParams.axleDescription;
            gearboxParams   = &mEngineDriveParams.gearBoxParams;
            brakeResponseStates.setData(mBaseState.brakeCommandResponseStates);
            throttleResponseState = &mEngineDriveState.throttleCommandResponseState;
            gearboxState          = &mEngineDriveState.gearboxState;
            differentialState     = &mEngineDriveState.differentialState;
            clutchResponseState   = &mEngineDriveState.clutchCommandResponseState;
            actuationStates.setData(mBaseState.actuationStates);
        }
        virtual void getDataForEngineDrivetrainComponent(
            const PxVehicleAxleDescription*&                         axleDescription,
            PxVehicleArrayData<const PxVehicleWheelParams>&          wheelParams,
            const PxVehicleEngineParams*&                            engineParams,
            const PxVehicleClutchParams*&                            clutchParams,
            const PxVehicleGearboxParams*&                           gearboxParams,
            PxVehicleArrayData<const PxReal>&                        brakeResponseStates,
            PxVehicleArrayData<const PxVehicleWheelActuationState>&  actuationStates,
            PxVehicleArrayData<const PxVehicleTireForce>&            tireForces,
            const PxVehicleEngineDriveThrottleCommandResponseState*& throttleResponseState,
            const PxVehicleClutchCommandResponseState*&              clutchResponseState,
            const PxVehicleDifferentialState*&                       differentialState,
            const PxVehicleWheelConstraintGroupState*&               constraintGroupState,
            PxVehicleArrayData<PxVehicleWheelRigidBody1dState>&      wheelRigidBody1dStates,
            PxVehicleEngineState*&                                   engineState,
            PxVehicleGearboxState*&                                  gearboxState,
            PxVehicleClutchSlipState*&                               clutchState)
        {
            axleDescription = &mBaseParams.axleDescription;
            wheelParams.setData(mBaseParams.wheelParams);
            engineParams  = &mEngineDriveParams.engineParams;
            clutchParams  = &mEngineDriveParams.clutchParams;
            gearboxParams = &mEngineDriveParams.gearBoxParams;
            brakeResponseStates.setData(mBaseState.brakeCommandResponseStates);
            actuationStates.setData(mBaseState.actuationStates);
            tireForces.setData(mBaseState.tireForces);
            throttleResponseState = &mEngineDriveState.throttleCommandResponseState;
            clutchResponseState   = &mEngineDriveState.clutchCommandResponseState;
            differentialState     = &mEngineDriveState.differentialState;
            constraintGroupState  = &mEngineDriveState.wheelConstraintGroupState;
            wheelRigidBody1dStates.setData(mBaseState.wheelRigidBody1dStates);
            engineState  = &mEngineDriveState.engineState;
            gearboxState = &mEngineDriveState.gearboxState;
            clutchState  = &mEngineDriveState.clutchState;
        }
        BaseVehicleParams                            mBaseParams {};
        BaseVehicleState                             mBaseState {};
        DirectDrivetrainParams                       mDirectDriveParams {};
        DirectDrivetrainState                        mDirectDriveState {};
        EngineDrivetrainParams                       mEngineDriveParams {};
        EngineDrivetrainState                        mEngineDriveState {};
        PxVehicleCommandState                        mCommandState {};
        PxVehicleDirectDriveTransmissionCommandState DirectTransmission {};
        PxVehicleEngineDriveTransmissionCommandState mTransmissionCommandState {};
        PxVehicleAntiRollForceParams                 AntiRoll[20] {};
        PxU32                                        AntiRollCount = 0;
        PxVehicleAntiRollTorque                      AntiRollTorque {};
    };
} // namespace Kans::VehicleSDK
