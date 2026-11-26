#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Animation/AnimationData.h"
#include "KansEditorCore/EditorAssetChoice.h"

// The Action Markers window's model, without ImGui: an action's markers (its animation events),
// the Audio marker's payload check, the .action text and the document with its undo history.
namespace Kans::ActionMarkerEditing
{
    // A marker with this name plays the AudioClip or AudioEvent its payload names, at the entity.
    inline constexpr char AudioMarker[] = "Audio";

    // The marker name dropdown: Audio first, then the other names the action already uses.
    std::vector<std::string> QuickNames(const ActionData& action);

    // What an Audio marker's payload names.
    enum class AudioPayload
    {
        Empty,
        NotAnAssetID, // Not an asset ID: the engine warns and plays nothing.
        Missing,      // No published asset has this ID.
        WrongType,    // An asset that is neither an AudioClip nor an AudioEvent.
        Valid
    };
    bool IsAudioChoice(const EditorAssetChoice& choice);
    // `found` receives the asset when the ID names a published one (also of the wrong type).
    AudioPayload CheckAudioPayload(const std::string&                    payload,
                                   const std::vector<EditorAssetChoice>& choices,
                                   const EditorAssetChoice**             found = nullptr);
    // The warning the payload row shows; null when the payload is valid.
    const char* AudioPayloadWarning(AudioPayload status);

    // A new marker at `time` named `name`; returns its index. Markers keep the order they were
    // added in while editing; the text is written in time order.
    size_t AddMarker(ActionData& action, double time, const std::string& name);
    bool   RemoveMarker(ActionData& action, size_t index);

    // The .action text (markers in time order) and back. A malformed file returns the reason.
    std::string                           ToText(const ActionData& action);
    std::variant<ActionData, std::string> ParseAction(const std::string& text);
    // ValidateAction's reason for the markers in time order (null when the action can be saved).
    std::optional<std::string> ValidationError(const ActionData& action);

    // The edited action and its undo history. A continuous edit (a field being dragged) joins the
    // open gesture, so a drag is one undo step, closed by EndGesture.
    class ActionDocument
    {
    public:
        static constexpr size_t MaxHistory = 200;

        void              Reset(ActionData action, bool saved = true);
        const ActionData& Get() const { return m_Action; }
        uint64_t          GetRevision() const { return m_Revision; }
        void              Commit(ActionData next, bool continuous = false);
        void              EndGesture() { m_GestureOpen = false; }
        bool              CanUndo() const { return !m_Undo.empty(); }
        bool              CanRedo() const { return !m_Redo.empty(); }
        bool              Undo();
        bool              Redo();
        bool              IsDirty() const { return ToText(m_Action) != m_SavedText; }
        void              MarkSaved() { m_SavedText = ToText(m_Action); }

    private:
        ActionData              m_Action;
        std::string             m_SavedText;
        std::vector<ActionData> m_Undo, m_Redo;
        bool                    m_GestureOpen = false;
        uint64_t                m_Revision    = 0;
    };
} // namespace Kans::ActionMarkerEditing
