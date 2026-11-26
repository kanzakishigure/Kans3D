#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "KansEditorCore/Animation/ActionMarkerEditing.h"
#include "KansEditorCore/EditorAssetChoice.h"

namespace Kans
{
    class EditorAssetImportService;

    // The Action Markers window: the markers (animation events) of the open .action, each with its
    // time, name (typed or picked from the quick names, Audio first) and payload. A marker named
    // Audio picks its payload from the project's AudioClips and AudioEvents (also by dropping one
    // from the Content Browser) and warns when the asset is missing or not audio. Edits go into the
    // window's own undo history (a drag is one step); Save writes the .action and rebuilds it.
    class ActionMarkersPanel
    {
    public:
        void Bind(EditorAssetImportService* service) { m_Service = service; }
        // The published assets; the AudioClips and AudioEvents are offered to Audio markers.
        void SetAssetChoices(const std::vector<EditorAssetChoice>& choices) { m_Choices = choices; }

        // Loads a .action; false (with the reason in the status) when it cannot be read.
        bool Open(const std::filesystem::path& file);
        bool Save();
        void Draw();
        bool Undo() { return m_Pending.reset(), m_Document.Undo(); }
        bool Redo() { return m_Pending.reset(), m_Document.Redo(); }
        bool HasFocus() const { return m_Focused; }

        ActionMarkerEditing::ActionDocument&       GetDocument() { return m_Document; }
        const ActionMarkerEditing::ActionDocument& GetDocument() const { return m_Document; }
        const std::filesystem::path&               GetFile() const { return m_File; }
        const std::string&                         GetStatus() const { return m_Status; }

        // Where the last frame drew each control (screen space), for tooltips and UI tests.
        struct MarkerBoxes
        {
            ImRect      Time, Name, Names, Payload, Remove;
            std::string Warning; // The payload warning drawn, if any.
        };
        struct Boxes
        {
            ImRect                   Save, Undo, Redo, Add, Validation;
            std::vector<MarkerBoxes> Markers;
            // Items of the open name or payload dropdown, by label.
            std::vector<std::pair<std::string, ImRect>> Menu;
        };
        const Boxes& GetBoxes() const { return m_Boxes; }

    private:
        void DrawHeader();
        void DrawMarker(size_t index, MarkerBoxes& boxes);
        bool AudioPayloadCombo(std::string& payload, MarkerBoxes& boxes);
        void Edit(const std::function<void(ActionData&)>& change);
        void CommitPending();

        EditorAssetImportService*           m_Service = nullptr;
        std::vector<EditorAssetChoice>      m_Choices;
        ActionMarkerEditing::ActionDocument m_Document;
        std::optional<ActionData>           m_Pending;
        bool                                m_PendingContinuous = false;
        std::filesystem::path               m_File;
        bool                                m_Opened = false, m_Focused = false;
        std::string                         m_Status;
        Boxes                               m_Boxes;
    };
} // namespace Kans
