#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "KansEditorCore/SceneHistory.h"
#include "Panels/VfxSystemEditing.h"

// Undo and redo in the scene viewport while a VFX asset is edited there ("Edit asset in viewport"):
// the scene's history and the asset's document are two undo stacks, interleaved by when each step
// was made (NextEditStamp).
//
// The rule: Ctrl+Z undoes the newest step of the two, so the last edit goes first whichever
// document it changed; Ctrl+Y redoes the undone step that was made first, so a run of undos is
// replayed in the order the edits were made. Without an asset in edit mode both act on the scene.
namespace Kans::UndoRouting
{
    enum class Target
    {
        None,
        Scene,
        Asset
    };
    Target NextUndo(std::optional<uint64_t> scene, std::optional<uint64_t> asset);
    Target NextRedo(std::optional<uint64_t> scene, std::optional<uint64_t> asset);
    Target NextUndo(const SceneHistory&, const VfxSystemEditing::SystemDocument* asset);
    Target NextRedo(const SceneHistory&, const VfxSystemEditing::SystemDocument* asset);
    // Applies one step to the chosen document (`asset` may be null) and says which one it was.
    Target Undo(SceneHistory&, Scene&, VfxSystemEditing::SystemDocument* asset);
    Target Redo(SceneHistory&, Scene&, VfxSystemEditing::SystemDocument* asset);
    // What Undo (or Redo) would change, for the status line and tooltips: "Move Cube (scene)",
    // "Fire.vfxsystem (VFX asset)", or empty when there is nothing.
    std::string Describe(const SceneHistory&, const VfxSystemEditing::SystemDocument* asset, bool redo);
} // namespace Kans::UndoRouting
