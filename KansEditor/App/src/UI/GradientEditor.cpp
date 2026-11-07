#include "GradientEditor.h"
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include "Kans3D/ImGui/EditorTheme.h"
#include "UI/EditorIcons.h"

namespace Kans::GradientEditor
{
    namespace
    {
        ImU32 Rgba(glm::vec4 c)
        {
            return ImGui::ColorConvertFloat4ToU32({std::clamp(c.r, 0.f, 1.f), std::clamp(c.g, 0.f, 1.f), std::clamp(c.b, 0.f, 1.f),
                                                   std::clamp(c.a, 0.f, 1.f)});
        }
        void Bar(ImDrawList* draw, const Gradient& g, ImVec2 min, ImVec2 max)
        {
            // Checkerboard under the alpha.
            const float cell = std::max(4.0f, (max.y - min.y) * 0.5f);
            for (float y = min.y; y < max.y; y += cell)
                for (float x = min.x; x < max.x; x += cell)
                {
                    const bool dark = (int((x - min.x) / cell) + int((y - min.y) / cell)) % 2;
                    draw->AddRectFilled({x, y}, {std::min(x + cell, max.x), std::min(y + cell, max.y)},
                                        dark ? IM_COL32(90, 90, 90, 255) : IM_COL32(150, 150, 150, 255));
                }
            const int segments = std::max(8, int((max.x - min.x) / 3));
            for (int i = 0; i < segments; ++i)
            {
                const float t0 = float(i) / float(segments), t1 = float(i + 1) / float(segments);
                const ImU32 a = Rgba(g.Evaluate(t0)), b = Rgba(g.Mode == Gradient::Blend::Fixed ? g.Evaluate(t0) : g.Evaluate(t1));
                draw->AddRectFilledMultiColor({min.x + t0 * (max.x - min.x), min.y}, {min.x + t1 * (max.x - min.x), max.y}, a, b, b, a);
            }
            draw->AddRect(min, max, IM_COL32(0, 0, 0, 160));
        }
    } // namespace

    void Preview(const Gradient& gradient, float width, float height)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::Dummy({width, height});
        Bar(ImGui::GetWindowDrawList(), gradient, min, {min.x + width, min.y + height});
    }

    bool Draw(const char* id, Gradient& g)
    {
        using EditorTheme::Px;
        ImGui::PushID(id);
        auto*        storage  = ImGui::GetStateStorage();
        const ImGuiID selID   = ImGui::GetID("##selected");
        // Selection: >= 0 a colour stop, <= -1 the alpha stop -1 - index.
        int          selected = storage->GetInt(selID, 0);
        bool         changed  = false;
        auto*        draw     = ImGui::GetWindowDrawList();
        const float  width    = std::max(Px(120.0f), ImGui::GetContentRegionAvail().x - Px(8.0f));
        const float  marker   = Px(9.0f), barHeight = Px(26.0f);
        const ImVec2 origin   = ImGui::GetCursorScreenPos();
        const ImVec2 barMin {origin.x + Px(4.0f), origin.y + marker + Px(2.0f)};
        const ImVec2 barMax {barMin.x + width, barMin.y + barHeight};
        const auto   toX = [&](float t) { return barMin.x + std::clamp(t, 0.0f, 1.0f) * width; };
        const auto   toT = [&](float x) { return std::clamp((x - barMin.x) / width, 0.0f, 1.0f); };

        // The bar: double-click adds a colour stop (Shift: an alpha stop) with the colour there.
        ImGui::SetCursorScreenPos(barMin);
        ImGui::InvisibleButton("##bar", {width, barHeight});
        Bar(draw, g, barMin, barMax);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            const float     t = toT(ImGui::GetIO().MousePos.x);
            const glm::vec4 v = g.Evaluate(t);
            if (ImGui::GetIO().KeyShift)
            {
                g.Alphas.push_back({t, v.a});
                g.Sort();
                for (size_t k = 0; k < g.Alphas.size(); ++k)
                    if (g.Alphas[k].Time == t)
                        selected = -1 - int(k);
            }
            else
            {
                g.Colors.push_back({t, glm::vec3(v)});
                g.Sort();
                for (size_t k = 0; k < g.Colors.size(); ++k)
                    if (g.Colors[k].Time == t)
                        selected = int(k);
            }
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Double-click: add a colour stop (Shift: an alpha stop)");

        // Stops: alpha above the bar, colour below it.
        const auto stops = [&](bool alpha) {
            const size_t count = alpha ? g.Alphas.size() : g.Colors.size();
            for (size_t k = 0; k < count; ++k)
            {
                const float  t   = alpha ? g.Alphas[k].Time : g.Colors[k].Time;
                const float  x   = toX(t);
                const float  y   = alpha ? barMin.y - marker - Px(1.0f) : barMax.y + Px(1.0f);
                const int    key = alpha ? -1 - int(k) : int(k);
                ImGui::PushID(key);
                ImGui::SetCursorScreenPos({x - marker * 0.5f, y});
                ImGui::InvisibleButton("##stop", {marker, marker});
                if (ImGui::IsItemActivated())
                    selected = key;
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
                {
                    const float nt = toT(ImGui::GetIO().MousePos.x);
                    if (alpha)
                        g.Alphas[k].Time = nt;
                    else
                        g.Colors[k].Time = nt;
                    changed = true;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && count > 1)
                {
                    if (alpha)
                        g.Alphas.erase(g.Alphas.begin() + long(k));
                    else
                        g.Colors.erase(g.Colors.begin() + long(k));
                    selected = 0;
                    changed  = true;
                    ImGui::PopID();
                    break;
                }
                const bool  hot  = selected == key || ImGui::IsItemHovered();
                const ImU32 fill = alpha ? Rgba(glm::vec4(glm::vec3(g.Alphas[k].Alpha), 1)) : Rgba(glm::vec4(g.Colors[k].Color, 1));
                const ImU32 edge = hot ? ImGui::GetColorU32(EditorTheme::Colors().Accent) : IM_COL32(20, 20, 20, 255);
                if (alpha)
                    draw->AddTriangleFilled({x - marker * 0.5f, y}, {x + marker * 0.5f, y}, {x, y + marker}, fill),
                        draw->AddTriangle({x - marker * 0.5f, y}, {x + marker * 0.5f, y}, {x, y + marker}, edge, hot ? 2.0f : 1.0f);
                else
                    draw->AddTriangleFilled({x, y}, {x + marker * 0.5f, y + marker}, {x - marker * 0.5f, y + marker}, fill),
                        draw->AddTriangle({x, y}, {x + marker * 0.5f, y + marker}, {x - marker * 0.5f, y + marker}, edge, hot ? 2.0f : 1.0f);
                ImGui::PopID();
            }
        };
        stops(true);
        stops(false);
        if (changed && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            g.Sort();
        ImGui::SetCursorScreenPos({origin.x, barMax.y + marker + Px(6.0f)});

        // The blend mode and the selected stop.
        int mode = int(g.Mode);
        const char* modes[] = {"Linear", "Fixed"};
        ImGui::SetNextItemWidth(Px(90.0f));
        if (ImGui::Combo("Blend", &mode, modes, 2))
            g.Mode = Gradient::Blend(mode), changed = true;
        if (selected >= int(g.Colors.size()) || -1 - selected >= int(g.Alphas.size()))
            selected = 0;
        if (selected >= 0 && !g.Colors.empty())
        {
            auto& stop = g.Colors[size_t(selected)];
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(70.0f));
            changed |= ImGui::DragFloat("Time", &stop.Time, 0.005f, 0.0f, 1.0f, "%.3f");
            ImGui::SameLine();
            changed |= ImGui::ColorEdit3("Colour", &stop.Color.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        }
        else if (selected < 0)
        {
            auto& stop = g.Alphas[size_t(-1 - selected)];
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(70.0f));
            changed |= ImGui::DragFloat("Time", &stop.Time, 0.005f, 0.0f, 1.0f, "%.3f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(70.0f));
            changed |= ImGui::DragFloat("Alpha", &stop.Alpha, 0.005f, 0.0f, 1.0f, "%.3f");
        }
        ImGui::TextDisabled("%zu colour, %zu alpha stops", g.Colors.size(), g.Alphas.size());
        storage->SetInt(selID, selected);
        ImGui::PopID();
        return changed;
    }
} // namespace Kans::GradientEditor
