#include "kspch.h"
#include "AnimatorComponent.h"
#include "AnimationProduct.h"
namespace Kans
{
    AnimatorComponent::AnimatorComponent(const AnimatorComponent& c) :
        Skeleton(c.Skeleton), Controller(c.Controller), InlineController(c.InlineController), Enabled(c.Enabled),
        ApplyRootMotion(c.ApplyRootMotion), Speed(c.Speed), Retargets(c.Retargets), Parameters(c.Parameters),
        SkeletonSource(c.SkeletonSource), ControllerSource(c.ControllerSource), ClipSources(c.ClipSources),
        ActionSources(c.ActionSources)
    {}
    AnimatorComponent& AnimatorComponent::operator=(const AnimatorComponent& c)
    {
        if (this != &c)
        {
            AnimatorComponent copy(c);
            *this = std::move(copy);
        }
        return *this;
    }
    bool operator==(const AnimatorComponent& a, const AnimatorComponent& b)
    {
        if (a.Skeleton != b.Skeleton || a.Controller != b.Controller || a.Enabled != b.Enabled ||
            a.ApplyRootMotion != b.ApplyRootMotion || a.Speed != b.Speed || a.Parameters != b.Parameters ||
            a.InlineController.has_value() != b.InlineController.has_value())
            return false;
        // Equality is also used by editor undo while joint pickers contain incomplete or duplicate draft rows.
        // Compare authoring data directly; serialization validation belongs to saving/loading.
        if (a.Retargets.size() != b.Retargets.size())
            return false;
        for (size_t i = 0; i < a.Retargets.size(); ++i)
        {
            const auto& x = a.Retargets[i];
            const auto& y = b.Retargets[i];
            if (x.Clip != y.Clip || x.RootJoint != y.RootJoint || x.RootTranslationScale != y.RootTranslationScale ||
                x.Edited != y.Edited || x.Analysis.HeightBand != y.Analysis.HeightBand ||
                x.Analysis.VerticalSpeed != y.Analysis.VerticalSpeed ||
                x.Analysis.MinimumDuration != y.Analysis.MinimumDuration ||
                x.Analysis.MergeGap != y.Analysis.MergeGap || x.Analysis.SampleRate != y.Analysis.SampleRate ||
                x.Tracks.size() != y.Tracks.size())
                return false;
            for (size_t t = 0; t < x.Tracks.size(); ++t)
            {
                const auto& u = x.Tracks[t];
                const auto& v = y.Tracks[t];
                if (u.Foot != v.Foot || u.Heel != v.Heel || u.Toe != v.Toe || u.Windows.size() != v.Windows.size())
                    return false;
                for (size_t w = 0; w < u.Windows.size(); ++w)
                    if (u.Windows[w].Start != v.Windows[w].Start || u.Windows[w].End != v.Windows[w].End ||
                        u.Windows[w].ToePivot != v.Windows[w].ToePivot)
                        return false;
            }
        }
        if (!a.InlineController)
            return true;
        auto        x  = EncodeAnimationControllerProduct(*a.InlineController),
                    y  = EncodeAnimationControllerProduct(*b.InlineController);
        const auto* xb = std::get_if<std::vector<std::byte>>(&x);
        const auto* yb = std::get_if<std::vector<std::byte>>(&y);
        return xb && yb && *xb == *yb;
    }
} // namespace Kans
