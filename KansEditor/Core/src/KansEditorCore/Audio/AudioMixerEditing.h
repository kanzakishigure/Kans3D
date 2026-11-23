#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "Kans3D/Audio/AudioBusLayout.h"

namespace Kans
{
    class AudioEngine;
    struct AudioBusMeter;
} // namespace Kans

// The Audio Mixer panel's model, without ImGui: fader and meter scales, meter ballistics, edits of a
// bus layout, and the document that keeps the layout, its undo history and the live engine in step.
namespace Kans::AudioMixerEditing
{
    // ---- Fader and meter scales ----
    inline constexpr float FaderMinDb = -80.0f, FaderMaxDb = 6.0f;
    // Fader travel 0..1 for a level in dB: a fourth-root gain law, so the top of the travel has the
    // finest steps. -inf is 0; levels outside -80..+6 dB are clamped.
    float DbToFader(float db);
    // The inverse; the bottom of the travel (below half of the -80 dB position) is -inf.
    float FaderToDb(float position);
    // Meter height 0..1 for a level in dBFS, on the fader's scale (so one dB scale reads for both);
    // levels at or below -80 dB are empty.
    float MeterPosition(float db);
    // Linear full-scale amplitude to dBFS (-inf for 0).
    float AmplitudeToDb(float amplitude);

    // Display state of one bus meter: up to 8 output channels, updated once per frame from the
    // engine's per-block peaks. The bar falls at FallDbPerSecond, the peak-hold mark stays for
    // HoldSeconds before falling, and a clip latch lights when a block peaks above 0 dBFS and stays
    // for ClipSeconds (or until Reset). Fixed storage: updating never allocates.
    class MeterBallistics
    {
    public:
        static constexpr size_t MaxChannels     = 8;
        static constexpr float  FallDbPerSecond = 24.0f, HoldSeconds = 1.5f, HoldFallDbPerSecond = 20.0f;
        static constexpr float  ClipSeconds = 3.0f;
        struct Channel
        {
            float LevelDb = -1e9f, HoldDb = -1e9f, HoldAge = 0.0f, ClipAge = 0.0f;
            bool  Clip = false;
        };

        // `meter` may have 0..8 channels; extra channels are ignored, missing ones fall silent.
        void           Update(const AudioBusMeter& meter, float deltaSeconds);
        void           Reset();
        void           ClearClip();
        size_t         GetChannelCount() const { return m_Count; }
        const Channel& GetChannel(size_t index) const { return m_Channels[index]; }
        bool           AnyClip() const;

    private:
        std::array<Channel, MaxChannels> m_Channels {};
        size_t                           m_Count = 0;
    };

    // ---- Layout edits (the layout is only changed when they succeed) ----
    // Buses in tree order (depth first from Master, children in layout order), with their depth.
    struct TreeEntry
    {
        size_t Index = 0;
        int    Depth = 0;
    };
    std::vector<TreeEntry> TreeOrder(const AudioBusLayout& layout);
    // The bus at `index` and every bus below it in the parent tree.
    std::vector<size_t> Subtree(const AudioBusLayout& layout, size_t index);
    int                 FindBus(const AudioBusLayout& layout, const std::string& name);
    // `base`, or "base 2", "base 3"... whichever no bus uses yet.
    std::string UniqueBusName(const AudioBusLayout& layout, const std::string& base);

    // Renames the bus and every reference to it (children's parent, send targets, ducking sources).
    std::optional<std::string> RenameBus(AudioBusLayout& layout, size_t index, const std::string& name);
    std::optional<std::string> SetParent(AudioBusLayout& layout, size_t index, const std::string& parent);
    // Appends a bus under `parent`; returns its index.
    size_t AddBus(AudioBusLayout& layout, const std::string& parent = MasterAudioBus);
    // Master cannot be removed. Children move to the removed bus's parent, sends to it are dropped
    // and ducking effects that listened to it listen to Master.
    std::optional<std::string> RemoveBus(AudioBusLayout& layout, size_t index);
    // Moves an effect within one bus's chain (to: the index it ends up at).
    std::optional<std::string> MoveEffect(AudioBusLayout& layout, size_t bus, size_t from, size_t to);
    // Default parameters of a built-in effect type (AudioEffectType order), or a custom DSP slot.
    AudioBusEffect MakeEffect(size_t type, const std::string& customName = {});

    // The edited layout, its undo history and the engine it plays through. Every accepted edit is
    // applied to the engine at once, so it is heard while editing (also in Play mode: the layout is
    // an asset, not part of the scene, so the edits stay after Play stops).
    class MixerDocument
    {
    public:
        static constexpr size_t MaxHistory = 200;

        void         Bind(AudioEngine* engine) { m_Engine = engine; }
        AudioEngine* GetEngine() const { return m_Engine; }
        // Starts editing `layout` (a loaded or new file): clears the history and plays it.
        void                  Reset(AudioBusLayout layout, bool saved = true);
        const AudioBusLayout& Get() const { return m_Layout; }
        // Changes whenever the layout does (an edit, undo, redo or reset).
        uint64_t GetRevision() const { return m_Revision; }

        // Replaces the layout when it is valid; otherwise leaves everything as it was and returns why.
        // A continuous edit (a fader or field being dragged) joins the open gesture: the whole drag is
        // one undo step, closed by EndGesture (the panel calls it once nothing is active).
        std::optional<std::string> Commit(AudioBusLayout next, bool continuous = false);
        void                       EndGesture() { m_GestureOpen = false; }

        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        bool Undo();
        bool Redo();

        // Whether the layout differs from the one last loaded or saved.
        bool IsDirty() const { return m_Text != m_SavedText; }
        void MarkSaved() { m_SavedText = m_Text; }
        // Plays the current layout again (after the engine was re-created or another layout ran).
        std::optional<std::string> ApplyToEngine() const;

    private:
        void Set(AudioBusLayout layout);

        AudioEngine*                m_Engine = nullptr;
        AudioBusLayout              m_Layout;
        std::string                 m_Text, m_SavedText; // Serialized: compares layouts field by field.
        std::vector<AudioBusLayout> m_Undo, m_Redo;
        bool                        m_GestureOpen = false;
        uint64_t                    m_Revision    = 0;
    };

    // The layout's .audiobus text, or empty when it cannot be serialized.
    std::string ToText(const AudioBusLayout& layout);
} // namespace Kans::AudioMixerEditing
