#include "KansEditorCore/Animation/ActionMarkerEditing.h"
#include <algorithm>
#include <exception>
#include "Kans3D/Animation/AnimationProduct.h"

namespace Kans::ActionMarkerEditing
{
    namespace
    {
        ActionData Sorted(ActionData action)
        {
            std::stable_sort(action.Events.begin(), action.Events.end(), [](const auto& a, const auto& b) {
                return a.Time < b.Time;
            });
            return action;
        }
    } // namespace

    std::vector<std::string> QuickNames(const ActionData& action)
    {
        std::vector<std::string> names {AudioMarker};
        for (const auto& marker : action.Events)
            if (!marker.Name.empty() && std::find(names.begin(), names.end(), marker.Name) == names.end())
                names.push_back(marker.Name);
        std::sort(names.begin() + 1, names.end());
        return names;
    }

    bool IsAudioChoice(const EditorAssetChoice& choice)
    {
        return choice.Type == AssetType::AudioClip || choice.Type == AssetType::AudioEvent;
    }

    AudioPayload CheckAudioPayload(const std::string&                    payload,
                                   const std::vector<EditorAssetChoice>& choices,
                                   const EditorAssetChoice**             found)
    {
        if (found)
            *found = nullptr;
        if (payload.empty())
            return AudioPayload::Empty;
        const AssetID id = AssetID::FromString(payload);
        if (!id.IsValid())
            return AudioPayload::NotAnAssetID;
        for (const auto& choice : choices)
            if (choice.ID == id)
            {
                if (found)
                    *found = &choice;
                return IsAudioChoice(choice) ? AudioPayload::Valid : AudioPayload::WrongType;
            }
        return AudioPayload::Missing;
    }

    const char* AudioPayloadWarning(AudioPayload status)
    {
        switch (status)
        {
            case AudioPayload::Empty:
                return "Pick an AudioClip or AudioEvent: this marker plays nothing.";
            case AudioPayload::NotAnAssetID:
                return "The payload is not an asset ID: this marker plays nothing.";
            case AudioPayload::Missing:
                return "Missing asset: no AudioClip or AudioEvent has this ID.";
            case AudioPayload::WrongType:
                return "Not audio: the payload names an asset that is neither an AudioClip nor an AudioEvent.";
            case AudioPayload::Valid:
                break;
        }
        return nullptr;
    }

    size_t AddMarker(ActionData& action, double time, const std::string& name)
    {
        action.Events.push_back({std::max(0.0, time), name, {}});
        return action.Events.size() - 1;
    }

    bool RemoveMarker(ActionData& action, size_t index)
    {
        if (index >= action.Events.size())
            return false;
        action.Events.erase(action.Events.begin() + std::ptrdiff_t(index));
        return true;
    }

    std::string ToText(const ActionData& action)
    {
        try
        {
            YAML::Emitter out;
            out << EncodeActionNode(Sorted(action));
            return std::string(out.c_str()) + "\n";
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    std::variant<ActionData, std::string> ParseAction(const std::string& text)
    {
        try
        {
            return DecodeActionNode(YAML::Load(text));
        }
        catch (const std::exception& error)
        {
            return std::string(error.what());
        }
    }

    std::optional<std::string> ValidationError(const ActionData& action)
    {
        const auto result = ValidateAction(Sorted(action));
        if (const auto* error = std::get_if<AssetError>(&result))
            return error->Message;
        return std::nullopt;
    }

    void ActionDocument::Reset(ActionData action, bool saved)
    {
        m_Undo.clear();
        m_Redo.clear();
        m_GestureOpen = false;
        m_Action      = std::move(action);
        m_SavedText   = saved ? ToText(m_Action) : std::string();
        ++m_Revision;
    }

    void ActionDocument::Commit(ActionData next, bool continuous)
    {
        if (ToText(next) == ToText(m_Action))
            return; // Nothing the file would see changed (a field re-typed with its value).
        if (!(continuous && m_GestureOpen))
        {
            m_Undo.push_back(m_Action);
            if (m_Undo.size() > MaxHistory)
                m_Undo.erase(m_Undo.begin());
        }
        m_Redo.clear();
        m_GestureOpen = continuous;
        m_Action      = std::move(next);
        ++m_Revision;
    }

    bool ActionDocument::Undo()
    {
        if (m_Undo.empty())
            return false;
        m_Redo.push_back(std::move(m_Action));
        m_Action = std::move(m_Undo.back());
        m_Undo.pop_back();
        m_GestureOpen = false;
        ++m_Revision;
        return true;
    }

    bool ActionDocument::Redo()
    {
        if (m_Redo.empty())
            return false;
        m_Undo.push_back(std::move(m_Action));
        m_Action = std::move(m_Redo.back());
        m_Redo.pop_back();
        m_GestureOpen = false;
        ++m_Revision;
        return true;
    }
} // namespace Kans::ActionMarkerEditing
