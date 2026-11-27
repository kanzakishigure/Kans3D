#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Kans3D/Audio/AudioEngine.h"
#include "Kans3D/Scene/Entity.h"
#include "KansEditorCore/Audio/AudioProfilerModel.h"
#include "KansEditorCore/EditorAssetChoice.h"

namespace Kans
{
    class Scene;

    // The Audio Profiler (Window > Debug): what the audio engine is doing right now.
    //  - Toolbar: Record (engine profiling on while recording; the previous state comes back when it
    //    stops), Pause view (freezes everything shown), Clear (history and log) and the 1 s / 10 s window.
    //  - Mixer load: callback time against its budget as a scrolling graph with the window's minimum,
    //    average and peak, and the overrun count (red once any callback missed its budget).
    //  - Tabs: Voices (one row per engine voice, filterable by text and state, sortable; a click selects
    //    the owning entity), Buses (per-bus and per-effect-slot time in the mixer's tree), Streams and
    //    memory (stream buffer fill, decode time and starvation, generator underruns, PCM memory) and
    //    Event log (newest first, a marker where the engine's ring dropped entries).
    // The panel reads the engine once per drawn frame and reuses its storage, so a steady engine costs
    // no allocations; nothing is read while the panel is closed or its view is paused.
    class AudioProfilerPanel
    {
    public:
        enum class Tab
        {
            Voices,
            Buses,
            Streams,
            Events,
            Count
        };

        // The editor's engine (may be null). Stops recording on the previous engine first.
        void Bind(AudioEngine* engine);
        // The scene whose entities own the voices (for names and selection).
        void SetSceneProvider(std::function<Scene*()> provider) { m_Scene = std::move(provider); }
        // Called with the owning entity when a voice row is clicked.
        void SetSelectEntity(std::function<void(Entity)> select) { m_Select = std::move(select); }
        // Published assets, for clip and event names.
        void SetAssetChoices(const std::vector<EditorAssetChoice>& choices);

        void Draw();

        void SetRecording(bool record);
        bool IsRecording() const { return m_Recorder.IsRecording(); }
        void SetPaused(bool paused) { m_Paused = paused; }
        bool IsPaused() const { return m_Paused; }
        void Clear();
        void SetWindow(AudioProfiler::TimeWindow window) { m_Window = window; }
        // Selects a tab on the next frame.
        void ShowTab(Tab tab) { m_ShowTab = tab; }
        Tab  GetTab() const { return m_Tab; }
        void SetAutoScroll(bool on) { m_AutoScroll = on; }

        // What the panel read last (unchanged while paused).
        const AudioProfileStats&                    GetStats() const { return m_Stats; }
        const AudioProfiler::LoadHistory&           GetHistory() const { return m_History; }
        const std::vector<AudioProfiler::VoiceRow>& GetVoiceRows() const { return m_Voices; }
        const std::vector<uint32_t>&                GetVoiceOrder() const { return m_Order; } // Shown rows.
        const std::vector<AudioProfiler::BusRow>&   GetBusRows() const { return m_BusRows; }
        const std::vector<AudioBusDefinition>&      GetBuses() const { return m_Buses; }
        const AudioProfiler::EventLog&              GetLog() const { return m_Log; }
        AudioProfiler::StateMask                    GetStateFilter() const { return m_States; }

        // Where the last frame drew each control and row (screen space), for UI tests.
        struct RowBox
        {
            size_t Row = 0; // Voice row, bus row or log entry (0 = newest), by table.
            ImRect Box;
            float  TextX = 0; // Where the first column's text starts (bus indentation).
        };
        struct Boxes
        {
            ImRect                                        Record, Pause, Clear, OneSecond, TenSeconds;
            ImRect                                        Graph, Overruns, Filter, AutoScroll;
            std::array<ImRect, size_t(Tab::Count)>        Tabs;
            std::array<ImRect, 4>                         States; // AudioProfiler::StateClass order.
            std::array<ImRect, AudioProfiler::EventKinds> Kinds;  // AudioProfileEventType order.
            std::vector<RowBox>                           Voices, Buses, Streams, Generators, Events;
            bool                                          Unavailable = false;
        };
        const Boxes& GetBoxes() const { return m_Boxes; }

    private:
        void Poll();
        void ResolveNames(AudioProfiler::VoiceRow& row) const;
        void DrawToolbar();
        void DrawLoad();
        void DrawVoices();
        void DrawBuses();
        void DrawStreams();
        void DrawEvents();
        // Writes the owner of an event or voice: the entity's name, else the owner kind.
        void OwnerText(const AudioVoiceOwner& owner, char* out, size_t size) const;
        void AssetText(AssetID clip, AssetID owner, AudioSoundID sound, char* out, size_t size) const;

        AudioEngine*                             m_Engine   = nullptr;
        uint64_t                                 m_Instance = 0;
        std::function<Scene*()>                  m_Scene;
        std::function<void(Entity)>              m_Select;
        std::unordered_map<AssetID, std::string> m_AssetNames;
        AudioProfiler::Recorder                  m_Recorder;
        AudioProfiler::TimeWindow                m_Window = AudioProfiler::TimeWindow::TenSeconds;
        bool                                     m_Paused = false, m_AutoScroll = true, m_ScrollToNewest = false;
        double                                   m_Now = 0; // Panel clock (ImGui time) at the last read.
        Tab                                      m_Tab = Tab::Voices, m_ShowTab = Tab::Count;

        AudioProfileStats                    m_Stats;
        uint64_t                             m_LastCallbacks   = 0;
        uint32_t                             m_AllocatedVoices = 0;
        uint64_t                             m_StolenThisFrame = 0, m_TotalStolen = 0, m_Rejected = 0;
        AudioProfiler::LoadHistory           m_History;
        std::vector<AudioProfileNode>        m_Nodes;
        std::vector<AudioBusDefinition>      m_Buses; // Copied when the engine's layout changes.
        std::vector<AudioProfiler::BusRow>   m_BusRows;
        std::vector<size_t>                  m_BusScratch;
        AudioProfiler::BusSort               m_BusSort      = AudioProfiler::BusSort::Tree;
        bool                                 m_BusAscending = false;
        std::vector<AudioVoiceInfo>          m_Snapshot;
        std::vector<AudioProfiler::VoiceRow> m_Voices;
        std::vector<uint32_t>                m_Order;
        AudioProfiler::VoiceColumn           m_VoiceSort      = AudioProfiler::VoiceColumn::Audibility;
        bool                                 m_VoiceAscending = false;
        AudioProfiler::StateMask             m_States         = AudioProfiler::AllStates;
        char                                 m_FilterText[96] {};
        AudioVoiceHandle                     m_SelectedVoice;
        AudioProfiler::EventLog              m_Log;
        AudioProfiler::EventMask             m_Kinds = AudioProfiler::AllEvents;
        std::vector<uint32_t>                m_EventOrder; // Shown log entries (0 = newest).
        std::array<ImVec2, AudioProfiler::LoadHistory::Capacity> m_Points;
        Boxes                                                    m_Boxes;
    };
} // namespace Kans
