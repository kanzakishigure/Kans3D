#pragma once
#include <imgui.h>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// The peak envelope of an audio clip drawn as a mirrored waveform (Content Browser tiles, the clip
// preview and the Audio Source card).
namespace Kans::AudioWaveform
{
    struct Style
    {
        bool                    Background = true;
        std::pair<float, float> Loop {0.0f, 1.0f}; // Shaded when narrower than the clip (0..1).
        std::optional<float>    Playhead;          // 0..1 of the clip.
    };
    // One bar per pixel column between `min` and `max` on the window's draw list.
    void Draw(const std::vector<uint8_t>& peaks, ImVec2 min, ImVec2 max, const Style& style = {});

    // The waveform as an item `size` wide and high: clicking or dragging on it reports the point
    // (0..1 of the clip) in `seek`. True on that frame.
    bool Scrubber(const char* id, const std::vector<uint8_t>& peaks, ImVec2 size, const Style& style, float& seek);
} // namespace Kans::AudioWaveform
