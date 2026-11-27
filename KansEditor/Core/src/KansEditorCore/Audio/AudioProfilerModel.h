#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include "Kans3D/Audio/AudioBusLayout.h"
#include "Kans3D/Audio/AudioProfile.h"

namespace Kans
{
    class AudioEngine;
}

// The Audio Profiler panel's data, kept apart from the drawing: the Record switch, the mixer-load
// history, the bus and effect rows in mixer-tree order, the voice rows with their filter and sort,
// and the lifecycle log read through the engine's event cursor. Everything here reuses its storage:
// once warmed up, polling a steady engine allocates nothing.
namespace Kans::AudioProfiler
{
    enum class TimeWindow
    {
        OneSecond,
        TenSeconds
    };
    double                    WindowSeconds(TimeWindow window);
    const AudioProfileWindow& Pick(const AudioProfileStats& stats, TimeWindow window);
    const AudioProfileWindow& Pick(const AudioProfileNode& node, TimeWindow window);

    // ---- Record: engine profiling on while the panel records, the previous state after ----
    class Recorder
    {
    public:
        // Turns the engine's profiling on, remembering whether it was already on.
        void Start(AudioEngine& engine);
        // Puts back the state Start found (another user, such as a script, may have enabled it).
        void Stop(AudioEngine& engine);
        // The engine was replaced (new instance): profiling is switched on again there while recording.
        void Rebind(AudioEngine& engine);
        bool IsRecording() const { return m_Recording; }

    private:
        bool m_Recording = false, m_WasEnabled = false;
    };

    // ---- Mixer load over time: one sample per new callback, oldest first ----
    struct LoadSample
    {
        double Time = 0; // Seconds on the panel's clock.
        float  Load = 0; // Callback time against its budget, in percent.
    };
    class LoadHistory
    {
    public:
        static constexpr size_t Capacity = 4096; // 10 s at up to 400 UI frames per second.
        void                    Clear() { m_Start = m_Count = 0; }
        void                    Add(double time, float load);
        size_t                  Size() const { return m_Count; }
        const LoadSample&       At(size_t i) const { return m_Samples[(m_Start + i) % Capacity]; }
        // Index of the first sample at or after `time` (Size() when none).
        size_t FirstAtOrAfter(double time) const;

    private:
        std::array<LoadSample, Capacity> m_Samples {};
        size_t                           m_Start = 0, m_Count = 0;
    };

    // ---- Bus and effect timing in mixer-tree order ----
    enum class BusSort
    {
        Tree, // Layout order under each parent, as the mixer draws the strips.
        Name,
        Current,
        Average,
        Peak
    };
    struct BusRow
    {
        size_t             Bus                = 0;
        size_t             Slot               = SIZE_MAX; // Effect slot, or SIZE_MAX for the bus itself.
        int                Depth              = 0;        // Nesting under Master; effects sit one step below their bus.
        uint64_t           CurrentNanoseconds = 0;
        AudioProfileWindow Window;        // The chosen 1 s or 10 s window, in nanoseconds.
        bool               Timed = false; // False when the engine has no node for it (layout changed, limit).
        bool               IsEffect() const { return Slot != SIZE_MAX; }
    };
    // Depth first from the roots (Master), siblings sorted by `sort` (descending times, ascending names),
    // each bus followed by its effect slots in chain order (or sorted the same way). `scratch` is reused.
    void BuildBusRows(const std::vector<AudioBusDefinition>& buses,
                      const std::vector<AudioProfileNode>&   nodes,
                      TimeWindow                             window,
                      BusSort                                sort,
                      bool                                   ascending,
                      std::vector<BusRow>&                   out,
                      std::vector<size_t>&                   scratch);

    // ---- Voices ----
    enum class StateClass : uint8_t
    {
        Real,     // Playing, scheduled, paused or held by the game pause.
        Virtual,  // Tracked but not mixed.
        Stopping, // Fading out (released or stolen).
        Stopped   // Created but not started (a rejected start), or ended.
    };
    StateClass ClassOf(AudioVoiceState state);
    using StateMask                      = uint8_t;
    inline constexpr StateMask AllStates = 0x0F;
    inline StateMask           Bit(StateClass state) { return StateMask(1u << unsigned(state)); }

    // A snapshot row the panel keeps: the engine's borrowed labels copied into fixed buffers, and the
    // owner and asset names resolved by the panel, so a paused view stays valid across layout edits.
    struct VoiceRow
    {
        AudioVoiceInfo Info; // Info.Bus and Info.ConcurrencyGroup are cleared; use the buffers below.
        char           Bus[48] {}, Group[48] {}, Owner[64] {}, Asset[96] {};
        char           Spatializer[24] {}, Occlusion[24] {}, Transmission[40] {}; // As the table shows them.
    };
    // Copies `in` into `out` (reusing its rows) without the borrowed views.
    void CopyVoices(const std::vector<AudioVoiceInfo>& in, std::vector<VoiceRow>& out);

    enum class VoiceColumn
    {
        Owner,
        Asset,
        Bus,
        State,
        Priority,
        Group,
        Audibility,
        Gain,
        Distance,
        Listener,
        Storage,
        Reason,
        Spatializer,  // Panning, or HRTF with its blend.
        Fallback,     // Why a HRTF voice is panned.
        Occlusion,    // The blocked fraction applied.
        Transmission, // Low / mid / high gains through what blocks it.
        Count
    };
    // Rows passing the filter, sorted; ties fall back to the voice slot so the order is stable.
    // `text` matches the owner, asset, bus or group (any case).
    void FilterAndSort(const std::vector<VoiceRow>& rows,
                       std::string_view             text,
                       StateMask                    states,
                       VoiceColumn                  column,
                       bool                         ascending,
                       std::vector<uint32_t>&       order);
    bool ContainsNoCase(std::string_view text, std::string_view part);

    const char* StateLabel(AudioVoiceState state);
    const char* ReasonLabel(AudioVoiceReason reason); // "" for None.
    // Why a HRTF voice is panned, short for the table ("" for None).
    const char* FallbackLabel(AudioHRTFFallback fallback);
    // "Panning", "HRTF 0.75" (with the spatial blend).
    void        FormatSpatializer(const AudioVoiceInfo& voice, char* out, size_t size);
    const char* OwnerKindLabel(AudioVoiceOwnerKind kind);
    const char* StorageLabel(AudioVoiceStorage storage);
    const char* EventLabel(AudioProfileEventType type);
    // Why the engine holds the voice this way, for the row's tooltip ("" when nothing to explain).
    const char* ReasonDetail(AudioVoiceState state, AudioVoiceReason reason);

    // ---- Lifecycle log: newest first, with markers where the engine's ring overwrote entries ----
    enum class LogKind : uint8_t
    {
        Event,
        Dropped, // `Dropped` entries the cursor missed (the engine's 256-entry ring overflowed).
        Restart  // The engine was rebuilt: a fresh cursor starts here.
    };
    struct LogEntry
    {
        LogKind           Kind    = LogKind::Event;
        uint64_t          Dropped = 0;
        AudioProfileEvent Event;
    };
    using EventMask                       = uint16_t;
    inline constexpr EventMask AllEvents  = 0xFF;
    inline constexpr size_t    EventKinds = 8;
    inline EventMask           Bit(AudioProfileEventType type) { return EventMask(1u << unsigned(type)); }

    class EventLog
    {
    public:
        static constexpr size_t Capacity = 1024;
        // Reads what the engine logged since the last poll; returns the entries added. A different
        // engine instance (Init again) starts a fresh cursor, after a Restart marker when entries exist.
        size_t Poll(AudioEngine& engine);
        // Forgets the shown entries; the cursor stays, so nothing read before comes back.
        void Clear() { m_Start = m_Count = 0; }
        // Forgets the entries and the cursor (the next Poll reads the engine's ring from its start).
        void   Reset();
        size_t Size() const { return m_Count; }
        // 0 is the newest entry.
        const LogEntry& Newest(size_t i) const { return m_Entries[(m_Start + m_Count - 1 - i) % Capacity]; }
        uint64_t        TotalDropped() const { return m_Cursor.Dropped; }

        // For tests and replays: adds entries as Poll would after reading them.
        void Append(const LogEntry& entry);

    private:
        static constexpr size_t ReadBatch = 128;

        std::array<LogEntry, Capacity>           m_Entries {};
        std::array<AudioProfileEvent, ReadBatch> m_Read {};
        size_t                                   m_Start = 0, m_Count = 0;
        AudioProfileEventCursor                  m_Cursor;
        uint64_t                                 m_Instance = 0;
    };

    // ---- Text into fixed buffers ----
    // "1.5 KiB", "12.0 MiB".
    void FormatBytes(uint64_t bytes, char* out, size_t size);
    // "-6.0 dB", "-inf dB".
    void FormatDb(float db, char* out, size_t size);
    // "120 us", "1.25 ms".
    void FormatNanoseconds(double nanoseconds, char* out, size_t size);
} // namespace Kans::AudioProfiler
