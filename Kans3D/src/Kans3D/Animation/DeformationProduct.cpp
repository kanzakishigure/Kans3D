#include "kspch.h"
#include "DeformationProduct.h"
#include <stdexcept>
#include "AnimationProduct.h"
namespace Kans
{
    namespace
    {
        YAML::Node Write(const CopyTransformIK& ik)
        {
            YAML::Node n;
            n["Source"]       = ik.Source;
            n["Joint"]        = ik.Joint;
            n["CopyRotation"] = ik.CopyRotation;
            n["CopyLocation"] = ik.CopyLocation;
            n["WorldSpace"]   = ik.WorldSpace;
            if (!std::isfinite(ik.Weight) || ik.Weight < 0 || ik.Weight > 1)
                throw std::runtime_error("Invalid IK Weight");
            n["Weight"] = ik.Weight;
            return n;
        }
        CopyTransformIK ReadCopyTransformIK(const YAML::Node& n)
        {
            CopyTransformIK ik;
            if (n["Source"])
                ik.Source = n["Source"].as<std::string>();
            if (n["Joint"])
                ik.Joint = n["Joint"].as<std::string>();
            if (n["CopyRotation"])
                ik.CopyRotation = n["CopyRotation"].as<bool>();
            if (n["CopyLocation"])
                ik.CopyLocation = n["CopyLocation"].as<bool>();
            if (n["WorldSpace"])
                ik.WorldSpace = n["WorldSpace"].as<bool>();
            if (n["Weight"])
                ik.Weight = n["Weight"].as<float>();
            Write(ik);
            return ik;
        }
        YAML::Node Write(const FootPlacementLeg& ik)
        {
            YAML::Node n;
            n["Hip"]              = ik.Hip;
            n["Knee"]             = ik.Knee;
            n["Ankle"]            = ik.Ankle;
            n["Toe"]              = ik.Toe;
            n["DrivenConstraint"] = ik.DrivenConstraint;
            if (!std::isfinite(ik.FootHeight) || ik.FootHeight < 0)
                throw std::runtime_error("Invalid IK FootHeight");
            n["FootHeight"] = ik.FootHeight;
            if (!std::isfinite(ik.FootLength) || ik.FootLength < 0)
                throw std::runtime_error("Invalid IK FootLength");
            n["FootLength"] = ik.FootLength;
            n["PoleEntity"] = ik.PoleEntity;
            if (!std::isfinite(ik.LockWeight) || ik.LockWeight < 0 || ik.LockWeight > 1)
                throw std::runtime_error("Invalid IK LockWeight");
            n["LockWeight"] = ik.LockWeight;
            if (!std::isfinite(ik.PoleDirection.x) || !std::isfinite(ik.PoleDirection.y) ||
                !std::isfinite(ik.PoleDirection.z))
                throw std::runtime_error("Invalid IK pole");
            n["PoleDirection"].push_back(ik.PoleDirection.x);
            n["PoleDirection"].push_back(ik.PoleDirection.y);
            n["PoleDirection"].push_back(ik.PoleDirection.z);
            return n;
        }
        FootPlacementLeg ReadFootPlacementLeg(const YAML::Node& n)
        {
            FootPlacementLeg ik;
            if (n["Hip"])
                ik.Hip = n["Hip"].as<std::string>();
            if (n["Knee"])
                ik.Knee = n["Knee"].as<std::string>();
            if (n["Ankle"])
                ik.Ankle = n["Ankle"].as<std::string>();
            if (n["Toe"])
                ik.Toe = n["Toe"].as<std::string>();
            if (n["DrivenConstraint"])
                ik.DrivenConstraint = n["DrivenConstraint"].as<std::string>();
            if (n["FootHeight"])
                ik.FootHeight = n["FootHeight"].as<float>();
            if (n["FootLength"])
                ik.FootLength = n["FootLength"].as<float>();
            if (n["PoleEntity"])
                ik.PoleEntity = n["PoleEntity"].as<uint64_t>();
            if (n["LockWeight"])
                ik.LockWeight = n["LockWeight"].as<float>();
            if (n["PoleDirection"])
            {
                const auto a = n["PoleDirection"];
                if (!a.IsSequence() || a.size() != 3)
                    throw std::runtime_error("Invalid pole");
                ik.PoleDirection = {a[0].as<float>(), a[1].as<float>(), a[2].as<float>()};
            }
            Write(ik);
            return ik;
        }
        YAML::Node Write(const FootPlacementIK& ik)
        {
            YAML::Node n;
            n["Pelvis"]          = ik.Pelvis;
            n["GroundLayerMask"] = ik.GroundLayerMask;
            if (!std::isfinite(ik.RayStartHeight) || ik.RayStartHeight < 0)
                throw std::runtime_error("Invalid IK RayStartHeight");
            n["RayStartHeight"] = ik.RayStartHeight;
            if (!std::isfinite(ik.MaxStepDown) || ik.MaxStepDown < 0)
                throw std::runtime_error("Invalid IK MaxStepDown");
            n["MaxStepDown"] = ik.MaxStepDown;
            if (!std::isfinite(ik.ContactBand) || ik.ContactBand < 0)
                throw std::runtime_error("Invalid IK ContactBand");
            n["ContactBand"] = ik.ContactBand;
            if (!std::isfinite(ik.PelvisAdjustSpeed) || ik.PelvisAdjustSpeed < 0)
                throw std::runtime_error("Invalid IK PelvisAdjustSpeed");
            n["PelvisAdjustSpeed"] = ik.PelvisAdjustSpeed;
            if (!std::isfinite(ik.PelvisMaxLower) || ik.PelvisMaxLower < 0)
                throw std::runtime_error("Invalid IK PelvisMaxLower");
            n["PelvisMaxLower"] = ik.PelvisMaxLower;
            if (!std::isfinite(ik.PelvisMaxRaise) || ik.PelvisMaxRaise < 0)
                throw std::runtime_error("Invalid IK PelvisMaxRaise");
            n["PelvisMaxRaise"] = ik.PelvisMaxRaise;
            if (!std::isfinite(ik.RotationWeight) || ik.RotationWeight < 0 || ik.RotationWeight > 1)
                throw std::runtime_error("Invalid IK RotationWeight");
            n["RotationWeight"] = ik.RotationWeight;
            if (!std::isfinite(ik.RotationLimit) || ik.RotationLimit < 0)
                throw std::runtime_error("Invalid IK RotationLimit");
            n["RotationLimit"] = ik.RotationLimit;
            if (!std::isfinite(ik.LockSpeed) || ik.LockSpeed < 0)
                throw std::runtime_error("Invalid IK LockSpeed");
            n["LockSpeed"] = ik.LockSpeed;
            if (!std::isfinite(ik.LockMaxDistance) || ik.LockMaxDistance < 0)
                throw std::runtime_error("Invalid IK LockMaxDistance");
            n["LockMaxDistance"] = ik.LockMaxDistance;
            if (ik.ContactSource != FootContactSource::Velocity && ik.ContactSource != FootContactSource::Tracks)
                throw std::runtime_error("Invalid foot contact source");
            n["ContactSource"] = ik.ContactSource == FootContactSource::Tracks ? "Tracks" : "Velocity";
            if (!std::isfinite(ik.TrackMaxReach) || ik.TrackMaxReach <= 0 || ik.TrackMaxReach > 1)
                throw std::runtime_error("Invalid foot track reach");
            n["TrackMaxReach"] = ik.TrackMaxReach;
            if (!std::isfinite(ik.ReleaseTime) || ik.ReleaseTime < 0)
                throw std::runtime_error("Invalid IK ReleaseTime");
            n["ReleaseTime"] = ik.ReleaseTime;
            if (!std::isfinite(ik.Weight) || ik.Weight < 0 || ik.Weight > 1)
                throw std::runtime_error("Invalid IK Weight");
            n["Weight"]  = ik.Weight;
            n["Locking"] = ik.Locking;
            n["LockYaw"] = ik.LockYaw;
            if (ik.Legs.size() > 16)
                throw std::runtime_error("Too many legs");
            n["Legs"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& leg : ik.Legs)
                n["Legs"].push_back(Write(leg));
            return n;
        }
        FootPlacementIK ReadFootPlacementIK(const YAML::Node& n)
        {
            FootPlacementIK ik;
            if (n["Pelvis"])
                ik.Pelvis = n["Pelvis"].as<std::string>();
            if (n["GroundLayerMask"])
                ik.GroundLayerMask = n["GroundLayerMask"].as<uint32_t>();
            if (n["RayStartHeight"])
                ik.RayStartHeight = n["RayStartHeight"].as<float>();
            if (n["MaxStepDown"])
                ik.MaxStepDown = n["MaxStepDown"].as<float>();
            if (n["ContactBand"])
                ik.ContactBand = n["ContactBand"].as<float>();
            if (n["PelvisAdjustSpeed"])
                ik.PelvisAdjustSpeed = n["PelvisAdjustSpeed"].as<float>();
            if (n["PelvisMaxLower"])
                ik.PelvisMaxLower = n["PelvisMaxLower"].as<float>();
            if (n["PelvisMaxRaise"])
                ik.PelvisMaxRaise = n["PelvisMaxRaise"].as<float>();
            if (n["RotationWeight"])
                ik.RotationWeight = n["RotationWeight"].as<float>();
            if (n["RotationLimit"])
                ik.RotationLimit = n["RotationLimit"].as<float>();
            if (n["ContactSource"])
            {
                const auto source = n["ContactSource"].as<std::string>();
                if (source != "Velocity" && source != "Tracks")
                    throw std::runtime_error("Invalid foot contact source");
                ik.ContactSource = source == "Tracks" ? FootContactSource::Tracks : FootContactSource::Velocity;
            }
            if (n["TrackMaxReach"])
                ik.TrackMaxReach = n["TrackMaxReach"].as<float>();
            if (n["LockSpeed"])
                ik.LockSpeed = n["LockSpeed"].as<float>();
            if (n["LockMaxDistance"])
                ik.LockMaxDistance = n["LockMaxDistance"].as<float>();
            if (n["ReleaseTime"])
                ik.ReleaseTime = n["ReleaseTime"].as<float>();
            if (n["Weight"])
                ik.Weight = n["Weight"].as<float>();
            if (n["Locking"])
                ik.Locking = n["Locking"].as<bool>();
            if (n["LockYaw"])
                ik.LockYaw = n["LockYaw"].as<bool>();
            if (n["Legs"])
            {
                if (!n["Legs"].IsSequence() || n["Legs"].size() > 16)
                    throw std::runtime_error("Invalid legs");
                for (const auto& l : n["Legs"])
                    ik.Legs.push_back(ReadFootPlacementLeg(l));
            }
            Write(ik);
            return ik;
        }
        AnimationControllerData EmptyController()
        {
            AnimationControllerData c;
            c.Motions.emplace_back().InlineClip.emplace();
            c.Layers.emplace_back().States.push_back({"Rest", 0});
            return c;
        }
    } // namespace
    YAML::Node EncodeIKComponent(const IKComponent& component)
    {
        if (component.Constraints.size() > 512)
            throw std::runtime_error("Too many IK constraints");
        YAML::Node n;
        n["Skeleton"]    = component.Skeleton.ToString();
        n["Constraints"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& constraint : component.Constraints)
        {
            if (constraint.Name.empty() || !std::isfinite(constraint.Weight) || constraint.Weight < 0 ||
                constraint.Weight > 1)
                throw std::runtime_error("Invalid IK name/weight");
            auto       c = EmptyController();
            YAML::Node v;
            v["Name"]    = constraint.Name;
            v["Enabled"] = constraint.Enabled;
            v["Weight"]  = constraint.Weight;
            std::visit(
                [&](auto ik) {
                    ik.Name = constraint.Name;
                    if constexpr (std::is_same_v<decltype(ik), TwoBoneIK>)
                    {
                        c.TwoBoneConstraints.push_back(ik);
                        v["Type"] = "TwoBone";
                        v["Data"] = EncodeAnimationControllerNode(c)["two_bone_ik"][0];
                    }
                    else if constexpr (std::is_same_v<decltype(ik), LookAtIK>)
                    {
                        c.LookAtConstraints.push_back(ik);
                        v["Type"] = "LookAt";
                        v["Data"] = EncodeAnimationControllerNode(c)["look_at_ik"][0];
                    }
                    else
                    {
                        v["Type"] = std::is_same_v<decltype(ik), CopyTransformIK> ? "CopyTransform" : "FootPlacement";
                        v["Data"] = Write(ik);
                    }
                },
                constraint.Data);
            n["Constraints"].push_back(v);
        }
        return n;
    }
    IKComponent DecodeIKComponent(const YAML::Node& n)
    {
        if (!n.IsMap() || !n["Constraints"].IsSequence() || n["Constraints"].size() > 512)
            throw std::runtime_error("Invalid IKComponent");
        IKComponent result;
        if (n["Skeleton"])
            result.Skeleton = AssetID::FromString(n["Skeleton"].as<std::string>());
        for (const auto& v : n["Constraints"])
        {
            auto       c    = EncodeAnimationControllerNode(EmptyController());
            const auto type = v["Type"].as<std::string>();
            if (type != "TwoBone" && type != "LookAt" && type != "CopyTransform" && type != "FootPlacement")
                throw std::runtime_error("Unknown IK constraint type");
            if (type == "TwoBone" || type == "LookAt")
                c[type == "TwoBone" ? "two_bone_ik" : "look_at_ik"].push_back(v["Data"]);
            const auto   decoded = DecodeAnimationControllerNode(c);
            IKConstraint constraint;
            constraint.Name    = v["Name"].as<std::string>();
            constraint.Enabled = v["Enabled"] ? v["Enabled"].as<bool>() : true;
            constraint.Weight  = v["Weight"] ? v["Weight"].as<float>() : 1.f;
            if (type == "TwoBone")
                constraint.Data = decoded.TwoBoneConstraints[0];
            else if (type == "LookAt")
                constraint.Data = decoded.LookAtConstraints[0];
            else if (type == "CopyTransform")
                constraint.Data = ReadCopyTransformIK(v["Data"]);
            else
                constraint.Data = ReadFootPlacementIK(v["Data"]);
            result.Constraints.push_back(std::move(constraint));
        }
        EncodeIKComponent(result);
        return result;
    }
} // namespace Kans
