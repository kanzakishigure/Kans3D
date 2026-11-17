#pragma once
#include <yaml-cpp/yaml.h>
#include "Animation.h"
namespace Kans
{
    const ClipRetargetSettings* FindClipRetarget(const std::vector<ClipRetargetSettings>&, const AnimationClipData*);
    void                        ApplyClipRetarget(const SkeletonData&,
                                                  const BoundAnimationClip&,
                                                  const ClipRetargetSettings&,
                                                  double time,
                                                  AnimationPose&);
    void AnalyzeFootContacts(const SkeletonData&, const BoundAnimationClip&, ClipRetargetSettings&);
    // Horizontal least squares; vertical scale is retained. Unobservable stride returns the existing scale.
    glm::vec3 EstimateRootTranslationScale(const SkeletonData&, const BoundAnimationClip&, const ClipRetargetSettings&);
    YAML::Node                        EncodeClipRetargets(const std::vector<ClipRetargetSettings>&);
    std::vector<ClipRetargetSettings> DecodeClipRetargets(const YAML::Node&);
} // namespace Kans
