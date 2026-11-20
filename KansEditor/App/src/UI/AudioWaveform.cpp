#include "AudioWaveform.h"
#include <algorithm>
#include "Kans3D/ImGui/EditorTheme.h"
#include "KansEditorCore/Audio/AudioPreviewCache.h"

namespace Kans::AudioWaveform
{
    void Draw(const std::vector<uint8_t>& peaks, ImVec2 min, ImVec2 max, const Style& style)
    {
        const auto& p     = EditorTheme::Colors();
        ImDrawList* draw  = ImGui::GetWindowDrawList();
        const float width = max.x - min.x, half = (max.y - min.y) * 0.5f, middle = min.y + half;
        if (style.Background)
            draw->AddRectFilled(min, max, ImGui::GetColorU32(p.Inset), EditorTheme::Px(3.0f));
        if (width < 1.0f || half <= 0.0f)
            return;
        if (style.Loop.first > 0.0f || style.Loop.second < 1.0f)
        {
            ImVec4 loop = p.Accent;
            loop.w      = 0.16f;
            draw->AddRectFilled({min.x + std::clamp(style.Loop.first, 0.0f, 1.0f) * width, min.y},
                                {min.x + std::clamp(style.Loop.second, 0.0f, 1.0f) * width, max.y},
                                ImGui::GetColorU32(loop));
        }
        const ImU32 wave    = ImGui::GetColorU32(p.TypeAudio);
        const auto  columns = AudioPeakColumns(peaks, size_t(width));
        for (size_t x = 0; x < columns.size(); ++x)
        {
            // Silence stays visible as a hairline at the centre.
            const float height = std::max(0.5f, columns[x] / 255.0f * (half - 1.0f));
            draw->AddRectFilled({min.x + x, middle - height}, {min.x + x + 1, middle + height}, wave);
        }
        if (style.Playhead)
        {
            const float at = min.x + std::clamp(*style.Playhead, 0.0f, 1.0f) * width;
            draw->AddLine({at, min.y}, {at, max.y}, ImGui::GetColorU32(p.Warning), EditorTheme::Px(1.5f));
        }
    }

    bool Scrubber(const char* id, const std::vector<uint8_t>& peaks, ImVec2 size, const Style& style, float& seek)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, size);
        const bool held = ImGui::IsItemActive();
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 max {min.x + size.x, min.y + size.y};
        Draw(peaks, min, max, style);
        if (ImGui::IsItemHovered() || held)
            ImGui::GetWindowDrawList()->AddRect(
                min, max, ImGui::GetColorU32(EditorTheme::Colors().Accent), EditorTheme::Px(3.0f));
        if (!held || size.x <= 0.0f)
            return false;
        seek = std::clamp((ImGui::GetIO().MousePos.x - min.x) / size.x, 0.0f, 1.0f);
        return true;
    }
} // namespace Kans::AudioWaveform
