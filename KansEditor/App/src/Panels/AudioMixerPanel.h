#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Audio/AudioEngine.h"
#include "AudioOutputSettingsPanel.h"
#include "KansEditorCore/Audio/AudioMixerEditing.h"

namespace Kans
{
    class EditorAssetImportService;

    // The Audio Mixer: one strip per bus of the edited .audiobus layout (name, parent, fader with
    // per-channel meters, Solo/Mute/Bypass, sends and the effect chain), and a header to pick, create,
    // save and make the project's layout. Every edit plays at once through the bound engine and
    // goes into the panel's own undo history; Save writes the .audiobus source and rebuilds it, so
    // the project's hot reload applies it. The layout is an asset, not scene data, so edits made in
    // Play mode stay after Play stops.
    class AudioMixerPanel
    {
    public:
        // The editor's engine (may be null), the project's import service and .kproj (may be empty).
        void Bind(AudioEngine* engine, EditorAssetImportService* service, std::filesystem::path projectFile);
        // Called after "Set as project layout" wrote the project file, with the layout's asset.
        void SetProjectLayoutCallback(std::function<void(AssetID)> callback)
        {
            m_OnProjectLayout = std::move(callback);
        }
        // The layout the project plays (from its .kproj); the panel opens it the first time it draws.
        void SetProjectLayout(AssetID layout) { m_ProjectLayout = layout; }

        // Loads and plays a .audiobus file; false (with the reason in the status) when it cannot be read.
        bool Open(const std::filesystem::path& file);
        // Edits `layout` without a file (Save asks for one through New layout).
        void OpenUnsaved(AudioBusLayout layout);
        // Writes a new .audiobus with the default buses into `directory` and opens it.
        std::optional<std::filesystem::path> CreateLayout(const std::filesystem::path& directory);
        bool                                 Save();
        // Writes the open file's asset into the project file as its layout (once it is imported).
        std::optional<std::string> SetAsProjectLayout();

        void Draw();
        bool Undo() { return m_Pending.reset(), m_Document.Undo(); }
        bool Redo() { return m_Pending.reset(), m_Document.Redo(); }
        bool HasFocus() const { return m_Focused; }

        AudioMixerEditing::MixerDocument&       GetDocument() { return m_Document; }
        const AudioMixerEditing::MixerDocument& GetDocument() const { return m_Document; }
        const std::filesystem::path&            GetFile() const { return m_File; }
        const std::string&                      GetStatus() const { return m_Status; }

        // Where the last frame drew each control (screen space), for tooltips and UI tests.
        struct EffectBoxes
        {
            ImRect Heading, Enable, Grip, Remove, Reduction;
        };
        struct StripBoxes
        {
            std::string              Bus;
            ImRect                   Strip, Name, Parent, Fader, Volume, Solo, Mute, Bypass, Delete, PinToCenter;
            ImRect                   FollowTimeScale, PausesWithGame;
            std::string              PlayStatus; // "Paused", "Time scale 0.50x" or empty, as drawn.
            ImRect                   AddSend, AddEffect;
            size_t                   Meters = 0; // Meter bars drawn (the engine's output channels).
            std::vector<EffectBoxes> Effects;
        };
        const std::vector<StripBoxes>& GetStripBoxes() const { return m_Boxes; }
        // The Output pane (device, speakers, player volume), shown beside the strips by the header's
        // Output toggle.
        AudioOutputSettingsPanel& GetOutputSettings() { return m_Output; }
        bool                      IsOutputShown() const { return m_ShowOutput; }
        void                      ShowOutput(bool show) { m_ShowOutput = show; }
        const ImRect&             GetOutputToggleBox() const { return m_OutputToggle; }
        // Items of the open add-effect or add-send menu, by label.
        const std::vector<std::pair<std::string, ImRect>>& GetMenuBoxes() const { return m_MenuBoxes; }

    private:
        void EnsureDocument();
        void DrawHeader();
        void DrawStrip(size_t index, int depth, StripBoxes& boxes);
        void DrawFader(size_t index, StripBoxes& boxes);
        void DrawSends(size_t index, StripBoxes& boxes);
        void DrawEffects(size_t index, StripBoxes& boxes);
        void DrawEffectParameters(size_t bus, size_t effect);
        // Changes a copy of the layout, committed at the end of the frame (the strips keep reading the
        // layout as it was); a refused edit shows its reason on the bus's strip.
        void    Edit(const std::function<std::optional<std::string>(AudioBusLayout&)>& change, size_t bus = SIZE_MAX);
        void    CommitPending();
        AssetID PublishedLayoutCached();
        std::vector<std::filesystem::path> ProjectLayouts() const;
        AssetID                            PublishedLayout(const std::filesystem::path& file) const;
        void                               ReloadIfChangedOnDisk();

        AudioEngine*                     m_Engine  = nullptr;
        EditorAssetImportService*        m_Service = nullptr;
        std::filesystem::path            m_ProjectFile, m_File;
        AssetID                          m_ProjectLayout;
        std::function<void(AssetID)>     m_OnProjectLayout;
        AudioMixerEditing::MixerDocument m_Document;
        bool                             m_Opened = false, m_Focused = false, m_PendingProjectLayout = false;
        std::string                      m_Status;
        // A refused edit's reason, shown on the strip it came from.
        std::string m_Error;
        size_t      m_ErrorBus = SIZE_MAX;
        // Name field being edited (bus index) and its text.
        size_t                                    m_Renaming = SIZE_MAX;
        std::string                               m_RenameText;
        std::string                               m_CustomName; // Add effect: a custom DSP name typed in the menu.
        std::filesystem::file_time_type           m_FileTime {};
        float                                     m_DiskCheck = 0.0f;
        std::optional<AudioBusLayout>             m_Pending;
        bool                                      m_PendingContinuous = false;
        size_t                                    m_PendingBus        = SIZE_MAX;
        std::filesystem::path                     m_PublishedFor;
        const void*                               m_PublishedRegistry = nullptr;
        AssetID                                   m_Published;
        uint64_t                                  m_OrderRevision = UINT64_MAX;
        std::vector<AudioMixerEditing::TreeEntry> m_Order;
        std::vector<AudioMixerEditing::MeterBallistics> m_Meters;
        AudioBusMeter                                   m_MeterRead;
        std::vector<StripBoxes>                         m_Boxes;
        std::vector<std::pair<std::string, ImRect>>     m_MenuBoxes;
        std::vector<std::filesystem::path>              m_LayoutChoices;
        AudioOutputSettingsPanel                        m_Output;
        bool                                            m_ShowOutput = false;
        ImRect                                          m_OutputToggle;
    };
} // namespace Kans
