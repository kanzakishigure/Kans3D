#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "KansEditorCore/Audio/AudioSpatialEditing.h"
#include "KansEditorCore/EditorAssetChoice.h"
#include "KansEditorCore/Physics/PhysicsEditing.h"

namespace Kans
{
    class AudioEngine;
    class AssetManager;

    // Project-wide settings, a page per area (Physics, Audio). Edits apply at once (the runtime
    // copy, and the editor's audio engine) and are written to the project file when the interaction ends.
    class ProjectSettingsPanel
    {
    public:
        enum class Page
        {
            Physics,
            Audio
        };

        // Reads the settings of `projectFile`; empty unbinds.
        void Bind(const std::filesystem::path& projectFile);
        // The engine the Audio page applies to and the assets it loads a HRTF from (either may be null).
        void SetAudio(AudioEngine* engine, AssetManager* assets)
        {
            m_Engine = engine;
            m_Assets = assets;
        }
        // The published assets; the AudioHRTFs are offered as the active HRTF.
        void SetAssetChoices(const std::vector<EditorAssetChoice>& choices);
        // Called after the Audio page changed a value (the editor keeps its project config in step).
        void SetAudioChangedCallback(std::function<void(const AudioSpatialEditing::ProjectAudioValues&)> callback)
        {
            m_AudioChanged = std::move(callback);
        }
        void ShowPage(Page page) { m_Page = page; }
        void Draw();

        const AudioSpatialEditing::ProjectAudioSettings& GetAudioSettings() const { return m_Audio; }
        // Where the Audio page drew its controls in the last frame, for UI tests.
        struct AudioBoxes
        {
            ImRect                                  Spatializer, HRTF, Rays, Stale;
            std::vector<std::pair<int, ImRect>>     SpatializerItems;
            std::vector<std::pair<AssetID, ImRect>> HRTFItems; // Invalid: Built-in.
            std::string                             HRTFNote;  // The availability or load-failure note.
        };
        const AudioBoxes& GetAudioBoxes() const { return m_AudioBoxes; }

    private:
        void DrawPhysics();
        void DrawAudio();
        void DrawCollisionMatrix(PhysicsSettings& settings, bool& changed);

        PhysicsEditing::ProjectPhysicsSettings                              m_Physics;
        AudioSpatialEditing::ProjectAudioSettings                           m_Audio;
        AudioEngine*                                                        m_Engine = nullptr;
        AssetManager*                                                       m_Assets = nullptr;
        std::vector<EditorAssetChoice>                                      m_HRTFs;
        std::function<void(const AudioSpatialEditing::ProjectAudioValues&)> m_AudioChanged;
        AudioBoxes                                                          m_AudioBoxes;
        Page                                                                m_Page  = Page::Physics;
        bool                                                                m_Bound = false;
        bool        m_AllLayers = false, m_SavePending = false, m_AudioSavePending = false;
        std::string m_Status, m_AudioStatus;
    };
} // namespace Kans
