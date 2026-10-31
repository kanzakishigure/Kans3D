#include "CurveEditor.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Physics/SecondaryMotionEditing.h"
#include "EditorIcons.h"

namespace Kans::CurveEditor
{
    bool Draw(const char* id, TimelineCurve& curve, int components, glm::vec4 fallback, float speed)
    {
        namespace Edit = SecondaryMotionEditing;
        using EditorTheme::Px;
        components = std::clamp(components, 1, 4);
        bool changed = false;
        ImGui::PushID(id);
        const auto& p      = EditorTheme::Colors();
        auto*       draw   = ImGui::GetWindowDrawList();
        const float width  = std::max(Px(80.0f), ImGui::GetContentRegionAvail().x);
        const float height = Px(72.0f);
        const ImVec2 min   = ImGui::GetCursorScreenPos();
        const ImVec2 max {min.x + width, min.y + height};
        ImGui::InvisibleButton("##graph", {width, height});
        const bool hovered = ImGui::IsItemHovered();

        // The value range: every key and the fallback, padded.
        float low = fallback.x, high = fallback.x;
        for (const auto& key : curve.Keys)
            for (int c = 0; c < components; ++c)
                low = std::min(low, key.Value[c]), high = std::max(high, key.Value[c]);
        if (high - low < 1e-4f)
            low -= 0.5f, high += 0.5f;
        const float pad = (high - low) * 0.15f;
        low -= pad, high += pad;
        const auto toScreen = [&](double t, float v) {
            return ImVec2 {min.x + float(t) * width, max.y - (v - low) / (high - low) * height};
        };

        draw->AddRectFilled(min, max, ImGui::GetColorU32(p.App), Px(3.0f));
        for (float t : {0.25f, 0.5f, 0.75f})
            draw->AddLine({min.x + t * width, min.y}, {min.x + t * width, max.y}, ImGui::GetColorU32(p.Panel), 1.0f);
        const ImU32 colors[4] = {ImGui::GetColorU32(p.Accent), IM_COL32(110, 220, 120, 255), IM_COL32(110, 160, 255, 255),
                                 IM_COL32(200, 200, 200, 255)};
        for (int c = components - 1; c >= 0; --c)
        {
            ImVec2 last;
            for (int i = 0; i <= 48; ++i)
            {
                const double t     = double(i) / 48.0;
                const ImVec2 point = toScreen(t, curve.Sample(t, fallback)[c]);
                if (i)
                    draw->AddLine(last, point, colors[c], 1.5f);
                last = point;
            }
        }
        draw->AddText({min.x + Px(3.0f), min.y + Px(1.0f)}, ImGui::GetColorU32(p.TextDim), "Root");
        const char* tip = "Tip";
        draw->AddText({max.x - ImGui::CalcTextSize(tip).x - Px(3.0f), min.y + Px(1.0f)}, ImGui::GetColorU32(p.TextDim), tip);

        // Keys: drag the first channel, double-click to add, right-click to remove.
        static ImGuiID dragging = 0;
        static size_t  dragKey  = 0;
        const ImGuiID  graphID  = ImGui::GetID("##graph");
        const ImVec2   mouse    = ImGui::GetIO().MousePos;
        int            hoveredKey = -1;
        for (size_t i = 0; i < curve.Keys.size(); ++i)
        {
            const ImVec2 at = toScreen(curve.Keys[i].Time, curve.Keys[i].Value[0]);
            const float  dx = mouse.x - at.x, dy = mouse.y - at.y;
            if (hovered && dx * dx + dy * dy < Px(36.0f))
                hoveredKey = int(i);
            const bool hot = hoveredKey == int(i) || (dragging == graphID && dragKey == i);
            draw->AddCircleFilled(at, Px(hot ? 4.5f : 3.5f), hot ? IM_COL32(255, 255, 255, 255) : colors[0]);
        }
        if (hovered && hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            dragging = graphID, dragKey = size_t(hoveredKey);
        if (dragging == graphID)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragKey < curve.Keys.size())
            {
                const double t     = std::clamp(double((mouse.x - min.x) / width), 0.0, 1.0);
                glm::vec4    value = curve.Keys[dragKey].Value;
                value[0]           = low + (max.y - mouse.y) / height * (high - low);
                if (components == 1)
                    value = glm::vec4(value[0]);
                Edit::MoveCurveKey(curve, dragKey, t, value);
                changed = true;
            }
            else
                dragging = 0;
        }
        if (hovered && hoveredKey < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            Edit::InsertCurveKey(curve, double((mouse.x - min.x) / width), fallback);
            changed = true;
        }
        if (hovered && hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            changed |= Edit::RemoveCurveKey(curve, size_t(hoveredKey));
        if (hovered && hoveredKey < 0)
            ImGui::SetTooltip("Drag a key to move it, double-click to add one, right-click a key to remove it");

        // The exact values.
        for (size_t i = 0; i < curve.Keys.size(); ++i)
        {
            ImGui::PushID(int(i));
            auto&       key  = curve.Keys[i];
            float       time = float(key.Time);
            glm::vec4   value = key.Value;
            const float timeWidth = Px(56.0f), button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(timeWidth);
            const bool ends = i == 0 || i + 1 == curve.Keys.size();
            ImGui::BeginDisabled(ends);
            bool edited = ImGui::DragFloat("##time", &time, 0.005f, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
            if (components == 1)
            {
                if (ImGui::DragFloat("##value", &value.x, speed, 0.0f, 0.0f, "%.3f"))
                    value = glm::vec4(value.x), edited = true;
            }
            else
                edited |= ImGui::DragScalarN("##value", ImGuiDataType_Float, &value.x, components, speed);
            if (edited)
            {
                Edit::MoveCurveKey(curve, i, time, value);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(ends);
            if (ImGui::Button(ICON_FA_XMARK, {button, button}) && Edit::RemoveCurveKey(curve, i))
                changed = true;
            ImGui::EndDisabled();
            ImGui::PopID();
            if (changed && i >= curve.Keys.size())
                break;
        }
        ImGui::PopID();
        return changed;
    }
} // namespace Kans::CurveEditor
