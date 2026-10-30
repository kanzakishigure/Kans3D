#pragma once
#include <yaml-cpp/yaml.h>
#include "PhysicsComponents.h"
namespace Kans::PhysicsYaml
{
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
    YAML::Node Encode(const GlobalWindComponent&);
    void       Decode(const YAML::Node&, GlobalWindComponent&);
    YAML::Node Encode(const PhysicsSettings&);
    void       Decode(const YAML::Node&, PhysicsSettings&);
} // namespace Kans::PhysicsYaml
