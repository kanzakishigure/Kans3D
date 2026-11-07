#include "UndoRouting.h"

namespace Kans::UndoRouting
{
    Target NextUndo(std::optional<uint64_t> scene, std::optional<uint64_t> asset)
    {
        if (scene && (!asset || *scene > *asset))
            return Target::Scene;
        return asset ? Target::Asset : Target::None;
    }

    Target NextRedo(std::optional<uint64_t> scene, std::optional<uint64_t> asset)
    {
        if (scene && (!asset || *scene < *asset))
            return Target::Scene;
        return asset ? Target::Asset : Target::None;
    }

    Target NextUndo(const SceneHistory& history, const VfxSystemEditing::SystemDocument* asset)
    {
        return NextUndo(history.UndoStamp(), asset ? asset->UndoStamp() : std::nullopt);
    }

    Target NextRedo(const SceneHistory& history, const VfxSystemEditing::SystemDocument* asset)
    {
        return NextRedo(history.RedoStamp(), asset ? asset->RedoStamp() : std::nullopt);
    }

    Target Undo(SceneHistory& history, Scene& scene, VfxSystemEditing::SystemDocument* asset)
    {
        const auto target = NextUndo(history, asset);
        if (target == Target::Scene)
            return history.Undo(scene) ? target : Target::None;
        if (target == Target::Asset)
            return asset->Undo() ? target : Target::None;
        return Target::None;
    }

    Target Redo(SceneHistory& history, Scene& scene, VfxSystemEditing::SystemDocument* asset)
    {
        const auto target = NextRedo(history, asset);
        if (target == Target::Scene)
            return history.Redo(scene) ? target : Target::None;
        if (target == Target::Asset)
            return asset->Redo() ? target : Target::None;
        return Target::None;
    }

    std::string Describe(const SceneHistory& history, const VfxSystemEditing::SystemDocument* asset, bool redo)
    {
        switch (redo ? NextRedo(history, asset) : NextUndo(history, asset))
        {
            case Target::Scene:
            {
                const auto labels = redo ? history.RedoLabels() : history.UndoLabels();
                return (labels.empty() ? std::string("Scene edit") : labels.front()) + " (scene)";
            }
            case Target::Asset:
            {
                const auto name = asset->File().empty() ? std::string("VFX asset") : asset->File().filename().u8string();
                return name + " (VFX asset)";
            }
            case Target::None:
                break;
        }
        return {};
    }
} // namespace Kans::UndoRouting
