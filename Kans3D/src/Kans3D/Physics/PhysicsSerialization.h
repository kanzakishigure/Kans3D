#pragma once
#include <yaml-cpp/yaml.h>
#include "Kans3D/Animation/SpringBoneComponent.h"
#include "ClothComponent.h"
#include "PhysicsComponents.h"
#include "RagdollComponent.h"
#include "VehicleComponent.h"
namespace Kans::PhysicsYaml
{
    YAML::Node Encode(const VehicleComponent&);
    void       Decode(const YAML::Node&, VehicleComponent&);
    YAML::Node Encode(const RagdollComponent&);
    void       Decode(const YAML::Node&, RagdollComponent&);
    YAML::Node Encode(const ClothComponent&);
    void       Decode(const YAML::Node&, ClothComponent&);
    YAML::Node Encode(const SpringBoneComponent&);
    void       Decode(const YAML::Node&, SpringBoneComponent&);
    YAML::Node Encode(const MotionColliderComponent&);
    void       Decode(const YAML::Node&, MotionColliderComponent&);
    YAML::Node Encode(const CharacterControllerComponent&);
    void       Decode(const YAML::Node&, CharacterControllerComponent&);
    YAML::Node Encode(const JointComponent&);
    void       Decode(const YAML::Node&, JointComponent&);
    YAML::Node Encode(const RigidBodyComponent&);
    void       Decode(const YAML::Node&, RigidBodyComponent&);
    YAML::Node Encode(const BoxColliderComponent&);
    void       Decode(const YAML::Node&, BoxColliderComponent&);
    YAML::Node Encode(const SphereColliderComponent&);
    void       Decode(const YAML::Node&, SphereColliderComponent&);
    YAML::Node Encode(const CapsuleColliderComponent&);
    void       Decode(const YAML::Node&, CapsuleColliderComponent&);
    YAML::Node Encode(const MeshColliderComponent&);
    void       Decode(const YAML::Node&, MeshColliderComponent&);
    YAML::Node Encode(const ForceFieldComponent&);
    void       Decode(const YAML::Node&, ForceFieldComponent&);
    YAML::Node Encode(const WindZoneComponent&);
    void       Decode(const YAML::Node&, WindZoneComponent&);
    YAML::Node Encode(const PhysicsSettings&);
    void       Decode(const YAML::Node&, PhysicsSettings&);
} // namespace Kans::PhysicsYaml
