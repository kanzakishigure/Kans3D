#include "Titlebar.h"
#include <filesystem>
#include <imgui_internal.h>
#include "Kans3D/Core/Application.h"
#include "Kans3D/Core/Events/ApplicationEvent.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Core/Window.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Renderer/Resource/Texture.h"
#include "EditorIcons.h"
#include "Widgets.h"

namespace Kans
{
    namespace
    {
        // Windows 11 caption button colours: the close button turns red under the pointer.
        constexpr ImU32 CloseHover   = IM_COL32(196, 43, 28, 255);
        constexpr ImU32 ClosePressed = IM_COL32(176, 40, 26, 255);
    } // namespace

    Titlebar::Titlebar() = default;
    Titlebar::~Titlebar() { Detach(); }

    bool Titlebar::Attach(Window& window)
    {
        Detach();
        if (!window.HasCustomTitlebar())
            return false;
        m_Window = &window;
        // Runs inside the window procedure: reads the last published layout only.
        window.SetTitlebarHitTest([this](int x, int y) { return m_Regions.HitTest(float(x), float(y)); });
        return true;
    }

    void Titlebar::Detach()
    {
        if (m_Window)
            m_Window->SetTitlebarHitTest({});
        m_Window = nullptr;
        m_Regions.Clear();
    }

    TitlebarRect Titlebar::Local(ImVec2 min, ImVec2 max) const
    {
        return {min.x - m_Origin.x, min.y - m_Origin.y, max.x - m_Origin.x, max.y - m_Origin.y};
    }

    void Titlebar::Begin(ImVec2 min, ImVec2 max, ImVec2 origin)
    {
        m_Min    = min;
        m_Max    = max;
        m_Origin = origin;
        m_Regions.Begin(Local(min, max));
    }

    void Titlebar::Client(ImVec2 min, ImVec2 max) { m_Regions.AddClient(Local(min, max)); }

    float Titlebar::Icon(float x)
    {
        if (!m_IconLoaded)
        {
            m_IconLoaded = true;
            std::error_code ec;
            const auto      file = KansFileSystem::GetResoucesFolder() / "Icon" / "KansTitlebar.png";
            if (std::filesystem::is_regular_file(file, ec))
            {
                TextureSpecification spec;
                spec.Format       = RenderHardware::Format::RGBA8Unorm;
                spec.Minf         = RenderHardware::Filter::Linear;
                spec.Maxf         = RenderHardware::Filter::Linear;
                spec.Wrap         = RenderHardware::AddressMode::ClampToEdge;
                spec.GenerateMips = true;
                m_Icon            = Texture2D::Create(spec, file);
            }
        }
        const float  size = EditorTheme::Px(18.0f);
        const ImVec2 min {x, IM_ROUND((m_Min.y + m_Max.y - size) * 0.5f)};
        const ImVec2 max {x + size, min.y + size};
        auto*        draw = ImGui::GetWindowDrawList();
        if (m_Icon)
            draw->AddImage(static_cast<ImTextureID>(uintptr_t(m_Icon->GetImGuiTextureID())), min, max, {0, 1}, {1, 0});
        else
            Widgets::CenterGlyph(ICON_FA_CUBE, min, max, ImGui::GetColorU32(EditorTheme::Colors().Accent));
        Client({x, m_Min.y}, {max.x, m_Max.y});
        return max.x;
    }

    ImVec2 Titlebar::ButtonSize() { return {EditorTheme::Px(46.0f), EditorTheme::Px(32.0f)}; }
    float  Titlebar::ButtonsWidth() { return ButtonSize().x * 3.0f; }

    Titlebar::Button Titlebar::WindowButtons(bool maximized, TitlebarButtonState state)
    {
        const ImVec2 size  = ButtonSize();
        const float  glyph = EditorTheme::Px(10.0f);
        const float  line  = std::max(1.0f, IM_ROUND(EditorTheme::Px(1.0f)));
        auto*        draw  = ImGui::GetWindowDrawList();
        const auto&  theme = EditorTheme::Colors();
        Button       clicked = Button::None;
        // Anchored to the top edge, so a maximized window's buttons reach the screen edge.
        float x = ButtonsLeft();
        const auto cell = [&](float left) {
            const ImVec2 min {IM_ROUND(left), m_Min.y};
            return std::pair<ImVec2, ImVec2> {min, {IM_ROUND(left + size.x), m_Min.y + size.y}};
        };
        const auto centre = [&](ImVec2 min, ImVec2 max) {
            return ImVec2 {IM_ROUND((min.x + max.x - glyph) * 0.5f), IM_ROUND((min.y + max.y - glyph) * 0.5f)};
        };
        const auto button = [&](const char* id, ImVec2 min, ImVec2 max, bool& hovered, bool& held) {
            ImGui::SetCursorScreenPos(min);
            const bool pressed = ImGui::InvisibleButton(id, {max.x - min.x, max.y - min.y});
            hovered            = ImGui::IsItemHovered();
            held               = ImGui::IsItemActive();
            Client(min, max);
            return pressed;
        };

        // Minimize: a horizontal stroke.
        {
            auto [min, max] = cell(x);
            bool hovered = false, held = false;
            if (button("##TitlebarMinimize", min, max, hovered, held))
                clicked = Button::Minimize;
            if (hovered || held)
                draw->AddRectFilled(min, max, ImGui::GetColorU32(held ? theme.Pressed : theme.Hover));
            const ImVec2 at = centre(min, max);
            draw->AddRectFilled({at.x, at.y + IM_ROUND(glyph * 0.5f)},
                                {at.x + glyph, at.y + IM_ROUND(glyph * 0.5f) + line},
                                ImGui::GetColorU32(theme.Text));
            x += size.x;
        }
        // Maximize / restore: Windows reports its hover and press.
        {
            auto [min, max] = cell(x);
            m_Regions.SetMaximize(Local(min, max));
            if (state.MaximizeHovered || state.MaximizePressed)
                draw->AddRectFilled(min, max, ImGui::GetColorU32(state.MaximizePressed ? theme.Pressed : theme.Hover));
            const ImVec2 at    = centre(min, max);
            const ImU32  color = ImGui::GetColorU32(theme.Text);
            if (maximized)
            {
                // Restore: a square with a second one offset behind it.
                const float offset = IM_ROUND(glyph * 0.2f);
                draw->AddRect({at.x, at.y + offset}, {at.x + glyph - offset, at.y + glyph}, color, 0.0f, 0, line);
                draw->AddRectFilled({at.x + offset, at.y}, {at.x + glyph, at.y + line}, color);
                draw->AddRectFilled({at.x + glyph - line, at.y}, {at.x + glyph, at.y + glyph - offset}, color);
            }
            else
                draw->AddRect(at, {at.x + glyph, at.y + glyph}, color, 0.0f, 0, line);
            x += size.x;
        }
        // Close: an X that turns white on red.
        {
            auto [min, max] = cell(x);
            max.x           = std::max(max.x, m_Max.x); // No gap at the window's right edge.
            bool hovered = false, held = false;
            if (button("##TitlebarClose", min, max, hovered, held))
                clicked = Button::Close;
            if (hovered || held)
                draw->AddRectFilled(min, max, held ? ClosePressed : CloseHover);
            const ImVec2 at    = centre(min, max);
            const ImU32  color = hovered || held ? IM_COL32_WHITE : ImGui::GetColorU32(theme.Text);
            draw->AddLine(at, {at.x + glyph, at.y + glyph}, color, line);
            draw->AddLine({at.x + glyph, at.y}, {at.x, at.y + glyph}, color, line);
        }
        return clicked;
    }

    void Titlebar::End()
    {
        m_Regions.Publish();
        // TextOff gives both the DWM border and the fallback a quiet, visible edge against App.
        const auto& edge = EditorTheme::Colors().TextOff;
        if (m_Window)
            m_Window->SetWindowEdgeColor(edge.x, edge.y, edge.z);
        // Above every window, inside the client rectangle; resize hit-testing is the window
        // procedure's and is unaffected.
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!ShowsWindowEdge(IsCustom(),
                             m_Window && m_Window->IsMaximized(),
                             m_Window && m_Window->HasNativeWindowEdge(),
                             viewport->Size.x,
                             viewport->Size.y))
            return;
        const ImVec2 max {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
        ImGui::GetForegroundDrawList(viewport)->AddRect(
            viewport->Pos, max, ImGui::GetColorU32(edge), 0.0f, 0, std::max(1.0f, IM_ROUND(EditorTheme::Px(1.0f))));
    }

    Titlebar::Button
    DrawWindowTitlebar(Titlebar& titlebar, const char* title, bool maximized, TitlebarButtonState state)
    {
        const ImVec2 min    = ImGui::GetCursorScreenPos();
        const float  height = Titlebar::ButtonSize().y;
        const ImVec2 max {min.x + ImGui::GetContentRegionAvail().x, min.y + height};
        auto*        draw   = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, ImGui::GetColorU32(EditorTheme::Colors().App));
        titlebar.Begin(min, max, ImGui::GetMainViewport()->Pos);
        const float x = titlebar.Icon(min.x + EditorTheme::Px(EditorTheme::Space3)) + EditorTheme::Px(EditorTheme::Space2);
        // The title is part of the drag area.
        draw->AddText({x, IM_ROUND((min.y + max.y - ImGui::GetTextLineHeight()) * 0.5f)},
                      ImGui::GetColorU32(EditorTheme::Colors().TextDim),
                      title);
        const auto clicked = titlebar.WindowButtons(maximized, state);
        titlebar.End();
        ImGui::SetCursorScreenPos({min.x, max.y}); // The caller's content follows directly.
        return clicked;
    }

    void RunTitlebarButton(Titlebar::Button button)
    {
        if (button == Titlebar::Button::None)
            return;
        auto& app = Application::Get();
        if (button == Titlebar::Button::Minimize)
            app.GetWindow().Minimize();
        else if (button == Titlebar::Button::Close)
        {
            // The same path as the native close button, so unsaved changes are asked about;
            // after the frame, as layers are being drawn now.
            app.SubmitPostFrame([] {
                WindowCloseEvent event;
                Application::Get().OnEvent(event);
            });
        }
    }
} // namespace Kans
