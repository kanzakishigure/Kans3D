#pragma once
#include <imgui.h>
#include <utility>
#include "KansEditorCore/Gizmo/ModalTransform.h"

namespace Kans::Widgets
{
    // This frame's pointer, buttons and keys for a modal transform, from ImGui. `cancel` adds a
    // cancel of the caller's own (the selection went away).
    inline Gizmo::ModalInput ModalTransformInput(bool cancel = false)
    {
        const ImGuiIO&    io = ImGui::GetIO();
        Gizmo::ModalInput input;
        input.Mouse   = {io.MousePos.x, io.MousePos.y};
        input.Shift   = io.KeyShift;
        input.Ctrl    = io.KeyCtrl;
        input.Confirm = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        input.Cancel = cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        input.MiddlePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        input.MiddleDown    = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        static const std::pair<ImGuiKey, char> keys[] = {
            {ImGuiKey_G, 'G'},       {ImGuiKey_R, 'R'},       {ImGuiKey_S, 'S'},       {ImGuiKey_X, 'X'},
            {ImGuiKey_Y, 'Y'},       {ImGuiKey_Z, 'Z'},       {ImGuiKey_Tab, '\t'},    {ImGuiKey_Minus, '-'},
            {ImGuiKey_KeypadSubtract, '-'},                   {ImGuiKey_Period, '.'},  {ImGuiKey_KeypadDecimal, '.'},
            {ImGuiKey_0, '0'},       {ImGuiKey_1, '1'},       {ImGuiKey_2, '2'},       {ImGuiKey_3, '3'},
            {ImGuiKey_4, '4'},       {ImGuiKey_5, '5'},       {ImGuiKey_6, '6'},       {ImGuiKey_7, '7'},
            {ImGuiKey_8, '8'},       {ImGuiKey_9, '9'},       {ImGuiKey_Keypad0, '0'}, {ImGuiKey_Keypad1, '1'},
            {ImGuiKey_Keypad2, '2'}, {ImGuiKey_Keypad3, '3'}, {ImGuiKey_Keypad4, '4'}, {ImGuiKey_Keypad5, '5'},
            {ImGuiKey_Keypad6, '6'}, {ImGuiKey_Keypad7, '7'}, {ImGuiKey_Keypad8, '8'}, {ImGuiKey_Keypad9, '9'}};
        for (const auto& [key, c] : keys)
            if (ImGui::IsKeyPressed(key, false))
                input.Keys.push_back({c, io.KeyShift});
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
            input.Keys.push_back({'\b', io.KeyShift});
        return input;
    }

    // The status line of a modal transform along the bottom of a view, and its value by the cursor.
    inline void DrawModalTransformStatus(const Gizmo::ModalTransform& modal, ImVec2 bottomLeft, float scale = 1.0f)
    {
        if (!modal.IsActive())
            return;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto  box  = [&](const std::string& text, ImVec2 at, bool above) {
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const ImVec2 pad {6.0f * scale, 3.0f * scale};
            if (above)
                at.y -= size.y + pad.y * 2;
            draw->AddRectFilled(at, {at.x + size.x + pad.x * 2, at.y + size.y + pad.y * 2}, IM_COL32(18, 18, 22, 225), 4.0f * scale);
            draw->AddText({at.x + pad.x, at.y + pad.y}, IM_COL32(240, 240, 240, 255), text.c_str());
        };
        const glm::vec2 at = modal.ReadoutPosition();
        box(modal.Readout(), {at.x, at.y}, false);
        box(modal.Status(), bottomLeft, true);
    }
} // namespace Kans::Widgets
