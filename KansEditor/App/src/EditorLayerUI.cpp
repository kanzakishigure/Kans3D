#include <imgui.h>
#include <imgui_internal.h>
#include <cstring>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Project/RecentProjects.h"
#include "EditorLayer.h"
#include "KansEditorCore/Launcher/EditorProcess.h"
#include "KansEditorCore/Launcher/ProjectGrid.h"
#include "UI/BloomSettingsWidget.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"
#include "UI/Widgets.h"

// Editor window layout: menu bar, main toolbar, dock space with the panels, and
// the viewport with its overlay toolbar.

namespace Kans
{
    using Widgets::ToolButton;

    namespace
    {
        const ImVec4 AccentColor = EditorTheme::Colors().Accent;

        // Frame padding that makes the menu bar a 36 px band (at scale 1) with centred labels.
        ImVec2 BandPadding() { return {EditorTheme::Px(10.0f), (EditorTheme::Px(36.0f) - ImGui::GetFontSize()) * 0.5f}; }
    } // namespace

    // ------------------------------------------------------------------ panels

    void EditorLayer::RegisterPanels()
    {
        const auto spec = [](const char* id, const char* title, const char* icon, PanelCategory category,
                             PanelDock dock, bool open) {
            PanelSpec value;
            value.ID          = id;
            value.Title       = title;
            value.Icon        = icon;
            value.Category    = category;
            value.Dock        = dock;
            value.DefaultOpen = open;
            return value;
        };
        auto viewport      = spec(PanelID::Viewport, "Viewport", ICON_FA_VIDEO, PanelCategory::Scene, PanelDock::Center, true);
        viewport.Closable  = false;
        viewport.NoPadding = true;
        viewport.Flags     = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        m_Panels.Register(viewport, [this] { m_ViewportPanel.Draw(); });
        m_Panels.Register(spec(PanelID::Hierarchy, "Hierarchy", ICON_FA_LIST, PanelCategory::Scene, PanelDock::Left, true),
                          [this] { m_HierarchyPanel.Draw(); });
        m_Panels.Register(
            spec(PanelID::Properties, "Properties", ICON_FA_SLIDERS, PanelCategory::Scene, PanelDock::Right, true),
            [this] { m_PropertiesPanel.Draw(); });

        auto browser     = spec(PanelID::ContentBrowser, "Content Browser", ICON_FA_FOLDER_TREE, PanelCategory::Assets,
                            PanelDock::Bottom, true);
        browser.Flags    = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        browser.Shortcut = "Ctrl+Space";
        m_Panels.Register(browser, [this] { m_ContentBrowserPanel.onImGuiRender(); });
        m_ContentBrowserPanel.SetProgress(&m_Progress);
        m_ProgressPopup.SetShowConsole([this] { m_Panels.Open(PanelID::Console); });
        m_Panels.Register(spec(PanelID::AssetImporter, "Asset Importer", ICON_FA_FILE_IMPORT, PanelCategory::Assets,
                               PanelDock::Right, false),
                          [this] { m_ProductImportPanel.Draw(); });
        m_Panels.Register(spec(PanelID::MaterialEditor, "Material Editor", ICON_FA_PALETTE, PanelCategory::Assets,
                               PanelDock::Right, false),
                          [this] { m_MaterialEditorPanel.Draw(); });

        m_Panels.Register(spec(PanelID::Console, "Console", ICON_FA_TERMINAL, PanelCategory::Debug, PanelDock::Bottom, true),
                          [this] { m_Console.Draw(); });
        m_Panels.Register(spec(PanelID::Stats, "Stats", ICON_FA_CHART_LINE, PanelCategory::Debug, PanelDock::Left, true),
                          [this] { DrawStats(); });
        auto frameGraph  = spec(PanelID::FrameGraph, "Frame Graph", ICON_FA_DRAW_POLYGON, PanelCategory::Debug,
                               PanelDock::Floating, false);
        frameGraph.Flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        m_Panels.Register(frameGraph, [this] { m_FrameGraphPanel.Draw(); });
        auto animatorGraph  = spec(PanelID::AnimatorGraph, "Animator", ICON_FA_PERSON_RUNNING, PanelCategory::Scene,
                                  PanelDock::Floating, false);
        animatorGraph.Flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        m_Panels.Register(animatorGraph, [this] { m_AnimatorGraphPanel.Draw(); });
        m_PropertiesPanel.SetOpenAnimatorGraph([this] { m_Panels.Open(PanelID::AnimatorGraph); });
        m_Panels.Register(
            spec(PanelID::Timeline, "Timeline", ICON_FA_FILM, PanelCategory::Scene, PanelDock::Bottom, false),
            [this] { m_TimelinePanel.Draw(); });
        BindSecondaryMotion();
        BindRagdolls();
        BindVfxSceneHandles();
        m_PropertiesPanel.SetOpenTimeline([this](Entity owner) {
            m_TimelinePanel.Open(owner);
            m_Panels.Open(PanelID::Timeline);
        });
        m_TimelinePanel.SetViewThroughCamera([this](bool show) { m_ViewportPanel.SetPreviewSceneCamera(show); });
        m_TimelinePanel.SetPlayModeQuery([this] { return m_Play.IsActive(); }); // Follows the running player.
        m_Panels.Register(
            spec(
                PanelID::UIDesigner, "UI Designer", ICON_FA_DESKTOP, PanelCategory::Assets, PanelDock::Floating, false),
            [this] { m_UIDesignerPanel.Draw(); });
        m_Panels.Register(spec(PanelID::ProjectSettings,
                               "Project Settings",
                               ICON_FA_GEAR,
                               PanelCategory::Assets,
                               PanelDock::Floating,
                               false),
                          [this] { m_ProjectSettingsPanel.Draw(); });
        m_Panels.Register(spec(PanelID::PhysicsMaterial,
                               "Physics Material",
                               ICON_FA_ATOM,
                               PanelCategory::Assets,
                               PanelDock::Right,
                               false),
                          [this] { m_PhysicsMaterialPanel.Draw(); });
    }

    void EditorLayer::OpenImporter(const std::filesystem::path& file)
    {
        m_ProductImportPanel.OpenWithFile(file);
        m_Panels.Open(PanelID::AssetImporter);
    }

    void EditorLayer::OpenMaterial(const std::filesystem::path& file)
    {
        m_MaterialEditorPanel.Open(file);
        m_Panels.Open(PanelID::MaterialEditor);
    }

    void EditorLayer::CreateMaterial(const std::filesystem::path& directory)
    {
        m_MaterialEditorPanel.Create(directory);
        m_ContentBrowserPanel.Refresh();
        m_Panels.Open(PanelID::MaterialEditor);
    }

    // ------------------------------------------------------------------ frame

    void EditorLayer::OnImGuiRender()
    {
        PROFILE_FUCTION();
        if (!m_Startup.Finished())
        {
            DrawStartupSplash();
            return;
        }
        m_ThumbnailBudget = 2; // Material thumbnails rendered per frame.
        PollProjectThumbnail();
        HandleShortcuts();

        // Full-window host for the menu bar, the main toolbar and the dock space.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, BandPadding()); // Sets the menu bar (top band) height.
        const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
                                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        // The host's tone is what shows in the gaps between docked panels. In play mode the
        // band turns orange-brown, so it is obvious that edits will not be kept.
        ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::Colors().App);
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg, m_Play.IsActive() ? ImVec4 {0.24f, 0.15f, 0.07f, 1.0f} : EditorTheme::Colors().App);
        ImGui::Begin("##EditorDockHost", nullptr, hostFlags);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
        DrawMenuBar();
        const ImGuiID dockspace = ImGui::GetID("EditorDockSpace");
        m_Panels.PrepareDockSpace(dockspace, ImGui::GetContentRegionAvail());
        ImGui::DockSpace(dockspace);
        DrawSceneFileDialogs();
        DrawProjectBrowser();
        ImGui::End();

        m_ViewportPanel.SetPlaying(m_Play.IsActive());
        m_FrameTimes[m_FrameTimeCursor] = ImGui::GetIO().DeltaTime * 1000.0f;
        m_FrameTimeCursor               = (m_FrameTimeCursor + 1) % m_FrameTimes.size();
        // The editor view captures every pass only while the Frame Graph window is open.
        if (m_Viewport)
        {
            m_FrameGraphPanel.SetClient(m_Viewport.get());
            m_Viewport->SetDebugCapture(m_Panels.IsOpen(PanelID::FrameGraph) ? m_FrameGraphPanel.CaptureSettings() :
                                                                               GraphCaptureSettings {});
        }
        m_Panels.Draw();
        if (m_PlayErrorNotification)
        {
            ImGui::OpenPopup("Play stopped after an error");
            m_PlayErrorNotification = false;
        }
        if (ImGui::BeginPopupModal("Play stopped after an error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextColored(EditorTheme::Colors().Danger, "%s", m_Play.GetLastUpdateError().c_str());
            ImGui::TextUnformatted("The edit scene has been restored.");
            if (ImGui::Button("Dismiss"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        DrawClothInfo();
        m_Panels.DrawStandalone("RagdollEditor", "Ragdoll Editor", [this] { m_RagdollEditor.Draw(); });
        DrawVfxModuleEditors();
        // Bottom right of the editor window, above every panel, wherever the panels are docked.
        m_ProgressPopup.Draw(m_Progress, std::nullopt);
        Application::Get().GetImGuiLayer()->BlockEvents(m_ViewportPanel.IsFocused() || m_ViewportPanel.IsHovered());
        TrackSceneEdits();
    }

    void EditorLayer::DrawStartupSplash()
    {
        using EditorTheme::Px;
        const auto&          theme    = EditorTheme::Colors();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.App);
        ImGui::Begin("##EditorStartup",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoNav);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        // The project's own name once its file is read, its file name until then.
        const std::string name = m_ProjectName.empty() ? m_ProjectFile.stem().u8string() : m_ProjectName;
        if (m_Titlebar.IsCustom())
        {
            auto&             window = Application::Get().GetWindow();
            const std::string title  = name + " \xE2\x80\x94 Kans3D Editor";
            RunTitlebarButton(
                DrawWindowTitlebar(m_Titlebar, title.c_str(), window.IsMaximized(), window.GetTitlebarButtonState()));
        }

        // One column in the middle of the window, a little above the centre.
        const bool   failed = m_Startup.Failed();
        const ImVec2 area   = ImGui::GetCursorScreenPos();
        const ImVec2 avail  = ImGui::GetContentRegionAvail();
        const float  width  = std::max(1.0f, std::min(Px(440.0f), avail.x - Px(EditorTheme::Space4) * 2.0f));
        const float  height = failed ? Px(180.0f) : Px(96.0f);
        const float  left   = IM_ROUND(area.x + (avail.x - width) * 0.5f);
        ImGui::SetCursorScreenPos({left, IM_ROUND(area.y + std::max(0.0f, (avail.y - height) * 0.42f))});
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(left + width);
        auto& fonts = ImGui::GetIO().Fonts->Fonts;
        ImGui::PushFont(fonts.Size > 1 ? fonts[1] : ImGui::GetFont(), 20.0f);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextDim);
        ImGui::TextUnformatted(m_ProjectFile.parent_path().u8string().c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy({0.0f, Px(EditorTheme::Space4)});

        if (!failed)
        {
            // The progress bar of the progress popup: an inset track with the accent fill.
            auto*        draw = ImGui::GetWindowDrawList();
            const float  barH = Px(4.0f);
            const ImVec2 a    = ImGui::GetCursorScreenPos();
            const ImVec2 b {a.x + width, a.y + barH};
            draw->AddRectFilled(a, b, ImGui::GetColorU32(theme.Inset), barH * 0.5f);
            if (const float done = m_Startup.Progress(); done > 0.0f)
                draw->AddRectFilled(a, {a.x + width * done, b.y}, ImGui::GetColorU32(theme.Accent), barH * 0.5f);
            ImGui::Dummy({width, barH});
            ImGui::Dummy({0.0f, Px(EditorTheme::Space1)});
            ImGui::PushStyleColor(ImGuiCol_Text, theme.TextDim);
            const std::string step    = m_Startup.CurrentLabel() + "\xE2\x80\xA6";
            const std::string percent = std::to_string(int(m_Startup.Progress() * 100.0f + 0.5f)) + "%";
            const ImVec2      row     = ImGui::GetCursorScreenPos();
            ImGui::TextUnformatted(step.c_str());
            ImGui::SameLine();
            ImGui::SetCursorScreenPos({left + width - ImGui::CalcTextSize(percent.c_str()).x, row.y});
            ImGui::TextUnformatted(percent.c_str());
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.Danger);
            ImGui::TextUnformatted(ICON_FA_TRIANGLE_EXCLAMATION "  The project could not be opened");
            ImGui::PopStyleColor();
            ImGui::Dummy({0.0f, Px(EditorTheme::Space1)});
            ImGui::TextUnformatted(m_Startup.Error().c_str());
            ImGui::Dummy({0.0f, Px(EditorTheme::Space4)});
            ImGui::BeginDisabled(m_LeavingStartup);
            ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.AccentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.AccentPressed);
            if (ImGui::Button(ICON_FA_ARROW_LEFT "  Back to projects"))
                BackToProjects();
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            if (!m_BackError.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.Danger);
                ImGui::TextUnformatted(m_BackError.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::End();

        // The next stage runs after this frame is shown, so the label names the work in progress.
        if (m_Startup.Running() && !m_StartupStepQueued)
        {
            m_StartupStepQueued = true;
            Application::Get().SubmitPostFrame([this] {
                m_StartupStepQueued = false;
                m_Startup.RunNext();
                if (m_Startup.Finished())
                    CORE_INFO("Editor started: {}", m_ProjectFile.u8string());
            });
        }
    }

    void EditorLayer::BackToProjects()
    {
        if (m_LeavingStartup)
            return;
        auto& app = Application::Get();
        if (m_OpenHubInProcess && EditorProcess::OpensInProcess())
        {
            // After the frame: replacing this layer deletes it.
            m_LeavingStartup = true;
            app.SubmitPostFrame([open = m_OpenHubInProcess, this] { open(this); });
            return;
        }
        if (auto error = EditorProcess::LaunchHub())
        {
            m_BackError = *error;
            return;
        }
        m_LeavingStartup = true;
        app.Close();
    }

    void EditorLayer::TrackSceneEdits()
    {
        if (!m_ActiveScene)
            return;
        // Changes made while playing are discarded on stop, not recorded.
        if (m_Play.IsActive())
        {
            m_EditPending = false;
            return;
        }
        // Edits come from user input; record once the interaction has ended
        // (drags released, text fields left, gizmo let go), so one drag is one step.
        const ImGuiIO& io = ImGui::GetIO();
        for (int button = 0; button < IM_ARRAYSIZE(io.MouseReleased); ++button)
            m_EditPending |= io.MouseReleased[button];
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END && !m_EditPending; ++key)
            m_EditPending |= ImGui::IsKeyReleased(ImGuiKey(key));
        // A timeline preview changes the scene temporarily; it is not an edit.
        if (m_EditPending && !m_TimelinePanel.IsPreviewing() && !ImGui::IsAnyItemActive() && !m_ViewportPanel.IsGizmoDragging() &&
            !ImGui::IsAnyMouseDown())
        {
            m_History.Record(*m_ActiveScene);
            m_EditPending = false;
        }
    }

    VfxSystemEditing::SystemDocument* EditorLayer::ViewportVfxDocument()
    {
        Entity selected = m_HierarchyPanel.GetSelected();
        if (!selected || !selected.HasComponent<ParticleSystemComponent>() || m_VfxSceneHandles.IsDragging())
            return nullptr;
        const auto* choice = m_VfxSceneHandles.Find(selected.GetUUID());
        if (!choice || !choice->Edit)
            return nullptr;
        // Only an open system editor's document: its undo, dirty marker and Save.
        const auto source = m_VfxSceneHandles.Resolve(selected.GetComponent<ParticleSystemComponent>().System, false);
        return source.Editor ? source.Document : nullptr;
    }

    void EditorLayer::UndoScene(int steps, bool routed)
    {
        if (!m_ActiveScene || m_Play.IsActive())
            return;
        m_TimelinePanel.StopPreview();
        // Pending changes become a step first, so they can be undone too.
        m_History.Record(*m_ActiveScene);
        m_EditPending = false;
        // With a VFX asset edited in the viewport, the newer of its step and the scene's goes first.
        auto* asset = routed ? ViewportVfxDocument() : nullptr;
        for (int i = 0; i < steps && UndoRouting::Undo(m_History, *m_ActiveScene, asset) != UndoRouting::Target::None; ++i)
            ;
    }

    void EditorLayer::RedoScene(int steps, bool routed)
    {
        if (!m_ActiveScene || m_Play.IsActive())
            return;
        m_TimelinePanel.StopPreview();
        m_History.Record(*m_ActiveScene);
        m_EditPending = false;
        auto* asset = routed ? ViewportVfxDocument() : nullptr;
        for (int i = 0; i < steps && UndoRouting::Redo(m_History, *m_ActiveScene, asset) != UndoRouting::Target::None; ++i)
            ;
    }

    void EditorLayer::HandleShortcuts()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput)
            return;
        // The material editor has its own undo, redo and save while it has focus.
        const bool material =
            m_MaterialEditorPanel.HasFocus() || m_PhysicsMaterialPanel.HasFocus() || m_RagdollEditor.HasFocus() ||
            std::any_of(m_VfxModuleEditors.begin(), m_VfxModuleEditors.end(), [](const auto& e) { return e->HasFocus(); }) ||
            std::any_of(m_VfxSystemEditors.begin(), m_VfxSystemEditors.end(), [](const auto& e) { return e->HasFocus(); });
        // Cloth painting has its own stroke undo and saves the .cloth file.
        if (io.KeyCtrl && m_ViewportPanel.IsPaintingCloth() && !material && m_ActiveScene)
        {
            auto& paint = m_ViewportPanel.GetClothPaint();
            if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyShift)
                paint.Save();
            else if (ImGui::IsKeyPressed(ImGuiKey_Z, true))
                io.KeyShift ? paint.Redo(*m_ActiveScene) : paint.Undo(*m_ActiveScene);
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, true))
                paint.Redo(*m_ActiveScene);
            if (ImGui::IsKeyPressed(ImGuiKey_S, false) || ImGui::IsKeyPressed(ImGuiKey_Z, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Y, false) || ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_Z) ||
                ImGui::IsKeyDown(ImGuiKey_Y))
                return;
        }
        // A transform in progress (gizmo, handle or modal G/R/S, where Ctrl toggles snapping) holds
        // the keys (the viewport's DragCapture gate): it ends before the scene can be saved, replaced,
        // undone or played.
        const DragCapture capture = m_ViewportPanel.Capture();
        const auto        pressed = [&](ImGuiKey key, bool repeat) {
            return ImGui::IsKeyPressed(key, repeat) && !capture.Captures(key, io.KeyCtrl);
        };
        if (io.KeyCtrl && !capture.Active())
        {
            if (pressed(ImGuiKey_N, false))
                ConfirmDiscard("creating a new scene", [this] { Record(NewScene()); });
            if (pressed(ImGuiKey_O, false))
                ConfirmDiscard("opening another scene", [this] { m_SceneDialog = SceneDialog::Open; });
            if (!material && pressed(ImGuiKey_S, false))
            {
                if (io.KeyShift)
                    m_SceneDialog = SceneDialog::Save;
                else
                    SaveCurrentScene();
            }
            if (pressed(ImGuiKey_Space, false))
                m_Panels.Toggle(PanelID::ContentBrowser);
            // Undo and redo reach the VFX asset edited in the viewport too (UndoRouting).
            if (!material && pressed(ImGuiKey_Z, true))
            {
                if (io.KeyShift)
                    RedoScene();
                else
                    UndoScene();
            }
            if (!material && pressed(ImGuiKey_Y, true))
                RedoScene();
            if (pressed(ImGuiKey_P, false))
            {
                if (io.KeyShift)
                    TogglePause();
                else if (m_Play.IsActive())
                    StopPlay();
                else
                    StartPlay();
            }
        }
    }

    // ------------------------------------------------------------------ play mode

    void EditorLayer::StartPlay()
    {
        if (!m_ActiveScene)
            return;
        m_TimelinePanel.StopPreview(); // Play starts from the scene as saved, not a previewed frame.
        if (!m_Play.IsActive())
        {
            // Play starts clean: no preview state, and painting ends (its strokes saved).
            FinishClothPaint();
            SecondaryMotionEditing::SetPreview(*m_ActiveScene, false);
            // Pending edits become a step first; play itself is not recorded.
            m_History.Record(*m_ActiveScene);
            m_EditPending = false;
            // SceneManager.Load paths are relative to the project root.
            std::error_code error;
            m_Play.SetProjectRoot(m_AssetImports ? m_AssetImports->GetConfig().RootDirectory : std::filesystem::current_path(error));
        }
        m_Play.Play(*m_ActiveScene);
    }

    void EditorLayer::StartSimulate()
    {
        if (!m_ActiveScene || m_Play.IsActive())
            return;
        m_TimelinePanel.StopPreview();
        FinishClothPaint();
        SecondaryMotionEditing::SetPreview(*m_ActiveScene, false);
        m_History.Record(*m_ActiveScene);
        m_EditPending = false;
        m_Play.Simulate(*m_ActiveScene);
    }

    void EditorLayer::StopPlay()
    {
        if (!m_ActiveScene || !m_Play.IsActive())
            return;
        // Restoring keeps UUIDs, so the selection can be found again.
        const UUID selected = m_HierarchyPanel.GetSelected() ? m_HierarchyPanel.GetSelected().GetUUID() : UUID(0);
        m_Play.Stop(*m_ActiveScene);
        m_HierarchyPanel.SetSelected(selected ? m_ActiveScene->GetEntityByUUID(selected) : Entity {});
    }

    void EditorLayer::TogglePause()
    {
        if (!m_ActiveScene || !m_Play.IsActive())
            return;
        if (m_Play.GetState() == ScenePlaySession::State::Paused)
            m_Play.Play(*m_ActiveScene);
        else
            m_Play.Pause();
    }

    float EditorLayer::DrawPlayControls(ImVec2 position)
    {
        using State = ScenePlaySession::State;
        const State state = m_Play.GetState();
        float       right = position.x;
        switch (Widgets::TransportControls(position, state != State::Edit, state == State::Paused, &right))
        {
            case Widgets::Transport::PlayStop:
                if (state == State::Edit)
                    StartPlay();
                else
                    StopPlay();
                break;
            case Widgets::Transport::Pause:
                TogglePause();
                break;
            case Widgets::Transport::Step:
                m_Play.Step();
                break;
            case Widgets::Transport::None:
                break;
        }
        // Simulate (as in Unreal): physics, animation and timelines run without scripts, from
        // the edit state that Stop restores. Selection and editing stay available.
        const bool simulating = m_Play.IsSimulating();
        right                 = Widgets::Pill({right + EditorTheme::Px(EditorTheme::Space2), position.y}, [&] {
            const char* tip = simulating           ? "Stop simulating and restore the scene" :
                              state == State::Edit ? "Simulate: run physics without scripts" :
                                                     "Stop playing to simulate";
            if (ToolButton("##Simulate", ICON_FA_ATOM, tip, simulating, state == State::Edit || simulating))
            {
                if (simulating)
                    StopPlay();
                else
                    StartSimulate();
            }
        });
        return right;
    }

    // ------------------------------------------------------------------ menu bar and main toolbar

    void EditorLayer::DrawProjectMenu()
    {
        const auto openBrowser = [this](ProjectBrowser::Page page) {
            const auto templates = (KansFileSystem::GetResoucesFolder() / ".." / "Templates").lexically_normal();
            m_ProjectBrowser = CreateScope<ProjectBrowser>(templates);
            m_ProjectBrowser->SetPage(page);
            m_ProjectBrowserRequested = true;
        };
        if (Widgets::IconMenuItem(ICON_FA_PLUS, "New Project..."))
            openBrowser(ProjectBrowser::Page::New);
        if (Widgets::IconMenuItem(ICON_FA_FOLDER_OPEN, "Open Project..."))
        {
            openBrowser(ProjectBrowser::Page::Recent);
            m_ProjectBrowser->BrowseForProject();
        }
        if (Widgets::IconMenuItem(ICON_FA_GEAR, "Project Settings..."))
            m_Panels.Open(PanelID::ProjectSettings);
        if (Widgets::BeginIconMenu(ICON_FA_CLOCK_ROTATE_LEFT, "Recent Projects"))
        {
            RecentProjects recent(RecentProjects::DefaultStorageFile());
            (void)recent.Load(); // A broken list just shows fewer entries.
            const auto current = std::filesystem::weakly_canonical(m_ProjectFile);
            int        shown   = 0;
            for (const auto& entry : recent.Entries())
            {
                if (entry.ProjectFile == current || shown == 10)
                    continue;
                ++shown;
                const bool exists = RecentProjects::Exists(entry);
                if (ImGui::MenuItem(entry.Name.c_str(), nullptr, false, exists))
                    SwitchProject(entry.ProjectFile);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s%s", entry.ProjectFile.u8string().c_str(), exists ? "" : "\n(missing)");
            }
            if (shown == 0)
                ImGui::TextDisabled("No other recent projects");
            ImGui::Separator();
            if (ImGui::MenuItem("Show All..."))
                openBrowser(ProjectBrowser::Page::Recent);
            Widgets::EndMenu();
        }
    }

    void EditorLayer::DrawProjectBrowser()
    {
        if (!m_ProjectBrowser)
            return;
        if (m_ProjectBrowserRequested)
        {
            ImGui::OpenPopup("Projects");
            m_ProjectBrowserRequested = false;
        }
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize({std::min(1000.0f, viewport->WorkSize.x - 80.0f), std::min(640.0f, viewport->WorkSize.y - 80.0f)},
                                 ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        bool open = true;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        const bool visible = ImGui::BeginPopupModal("Projects", &open, ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        if (!visible)
        {
            m_ProjectBrowser.reset();
            return;
        }
        if (auto chosen = m_ProjectBrowser->Draw())
        {
            ImGui::CloseCurrentPopup();
            SwitchProject(*chosen);
        }
        ImGui::EndPopup();
    }

    void EditorLayer::SwitchProject(const std::filesystem::path& project)
    {
        std::error_code ec;
        if (std::filesystem::equivalent(project, m_ProjectFile, ec))
            return;
        ConfirmDiscard("switching projects", [this, project] {
            // Before the next editor starts, so it opens where this one is.
            if (m_Placements)
                m_Placements->Remember(Application::Get().GetWindow().GetPlacement());
            if (auto error = EditorProcess::LaunchWithProject(project))
            {
                Record(AssetError {AssetErrorCode::IoError, *error});
                return;
            }
            Application::Get().Close();
        });
    }

    void EditorLayer::DrawMenuBar()
    {
        // The band's padding centres the menu labels; menus and popups use the normal one.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, BandPadding());
        const bool bar = ImGui::BeginMenuBar();
        ImGui::PopStyleVar();
        if (!bar)
            return;
        const ImRect band = ImGui::GetCurrentWindow()->MenuBarRect();
        m_BandRight       = band.Max.x;
        // On Windows the band is the window's titlebar: the icon first, the window buttons
        // last, and everything that is not a control drags the window.
        const bool custom = m_Titlebar.IsCustom();
        if (custom)
        {
            m_Titlebar.Begin(band.Min, band.Max, ImGui::GetMainViewport()->Pos);
            ImGui::GetCurrentWindow()->DC.CursorPos.x =
                m_Titlebar.Icon(band.Min.x + EditorTheme::Px(EditorTheme::Space3)) + EditorTheme::Px(EditorTheme::Space1);
            m_BandRight = m_Titlebar.ButtonsLeft();
        }
        const float menusLeft = ImGui::GetCursorScreenPos().x;
        DrawMenus();
        if (custom)
            m_Titlebar.Client({menusLeft, band.Min.y}, {ImGui::GetCursorScreenPos().x, band.Max.y});
        DrawToolbar();
        if (custom)
        {
            auto& window = Application::Get().GetWindow();
            RunTitlebarButton(m_Titlebar.WindowButtons(window.IsMaximized(), window.GetTitlebarButtonState()));
            m_Titlebar.End();
        }
        ImGui::EndMenuBar();
    }

    void EditorLayer::DrawMenus()
    {
        if (Widgets::BeginMenu("File"))
        {
            Widgets::MenuSection("Project");
            DrawProjectMenu();
            Widgets::MenuSection("Scene");
            if (Widgets::IconMenuItem(ICON_FA_FILE, "New Scene", "Ctrl+N"))
                ConfirmDiscard("creating a new scene", [this] { Record(NewScene()); });
            if (Widgets::IconMenuItem(ICON_FA_FOLDER_OPEN, "Open Scene...", "Ctrl+O"))
                ConfirmDiscard("opening another scene", [this] { m_SceneDialog = SceneDialog::Open; });
            if (Widgets::IconMenuItem(ICON_FA_FLOPPY_DISK, "Save Scene", "Ctrl+S", false, !m_Play.IsActive()))
                SaveCurrentScene();
            if (Widgets::IconMenuItem(ICON_FA_FLOPPY_DISK, "Save Scene As...", "Ctrl+Shift+S", false, !m_Play.IsActive()))
                m_SceneDialog = SceneDialog::Save;
            ImGui::Separator();
            if (Widgets::IconMenuItem(ICON_FA_XMARK, "Exit"))
                ConfirmDiscard("exiting", [] { Application::Get().Close(); });
            Widgets::EndMenu();
        }
        if (Widgets::BeginMenu("Edit"))
        {
            const auto undo     = m_History.UndoLabels();
            const auto redo     = m_History.RedoLabels();
            const auto* asset   = ViewportVfxDocument();
            const auto undoNext = UndoRouting::Describe(m_History, asset, false);
            const auto redoNext = UndoRouting::Describe(m_History, asset, true);
            if (Widgets::IconMenuItem(ICON_FA_UNDO, undoNext.empty() ? "Undo" : ("Undo " + undoNext).c_str(),
                                "Ctrl+Z",
                                false,
                                !undoNext.empty() && !m_Play.IsActive()))
                UndoScene();
            if (Widgets::IconMenuItem(ICON_FA_REDO, redoNext.empty() ? "Redo" : ("Redo " + redoNext).c_str(),
                                "Ctrl+Y",
                                false,
                                !redoNext.empty() && !m_Play.IsActive()))
                RedoScene();
            ImGui::Separator();
            // Jump several steps at once.
            if (Widgets::BeginIconMenu(ICON_FA_CLOCK_ROTATE_LEFT, "History",
                                 (!undo.empty() || !redo.empty()) && !m_Play.IsActive()))
            {
                for (size_t i = redo.size(); i-- > 0;)
                {
                    ImGui::PushID(int(1000 + i));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::MenuItem(redo[i].c_str()))
                        RedoScene(int(i + 1), false); // The scene's own steps.
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                Widgets::IconMenuItem(ICON_FA_CHECK, "Current state", nullptr, true, false);
                for (size_t i = 0; i < undo.size(); ++i)
                {
                    ImGui::PushID(int(i));
                    if (ImGui::MenuItem(undo[i].c_str()))
                        UndoScene(int(i + 1), false);
                    ImGui::PopID();
                }
                Widgets::EndMenu();
            }
            Widgets::EndMenu();
        }
        // Play, pause and step live on the band's play controls (and Ctrl+P / Ctrl+Shift+P).
        if (Widgets::BeginMenu("Assets"))
        {
            if (Widgets::IconMenuItem(ICON_FA_FILE_IMPORT, "Asset Importer"))
                m_Panels.Open(PanelID::AssetImporter);
            if (Widgets::IconMenuItem(ICON_FA_PALETTE, "Create Material"))
                CreateMaterial(m_ContentBrowserPanel.GetCurrentFolder());
            ImGui::Separator();
            if (Widgets::IconMenuItem(ICON_FA_ROTATE_RIGHT, "Refresh Published Products"))
                Record(RefreshProductAssets());
            Widgets::EndMenu();
        }
        DrawScriptsMenu();
        if (Widgets::BeginMenu("Window"))
        {
            m_Panels.DrawWindowMenu();
            Widgets::EndMenu();
        }
    }

    void EditorLayer::DrawToolbar()
    {
        // Runs inside the menu bar: groups of tool buttons on pills, play controls in the
        // middle and the scene status on the right, all centred in the band's height.
        const ImRect bar     = ImGui::GetCurrentWindow()->MenuBarRect();
        const float  buttonH = EditorTheme::Px(26.0f);
        const float  y       = bar.Min.y + (bar.GetHeight() - buttonH) * 0.5f;
        const float  gap     = EditorTheme::Px(EditorTheme::Space2);
        float        x       = ImGui::GetCursorScreenPos().x + EditorTheme::Px(EditorTheme::Space3);

        const bool        custom    = m_Titlebar.IsCustom();
        const std::string sceneName = (m_ScenePath.empty() ? std::string("Untitled scene") : m_ScenePath.filename().u8string()) +
                                      (m_History.IsModified() ? "  " ICON_FA_CIRCLE : "");
        if (custom)
        {
            // The titlebar names the project and the scene; the label drags the window like
            // the rest of the caption (the project commands are in File).
            const float textY = bar.Min.y + (bar.GetHeight() - ImGui::GetTextLineHeight()) * 0.5f;
            auto*       draw  = ImGui::GetWindowDrawList();
            auto& fonts = ImGui::GetIO().Fonts->Fonts;
            ImGui::PushFont(fonts.Size > 1 ? fonts[1] : ImGui::GetFont(), 0.0f); // Bold.
            draw->AddText({x, textY}, ImGui::GetColorU32(EditorTheme::Colors().Text), m_ProjectName.c_str());
            x += ImGui::CalcTextSize(m_ProjectName.c_str()).x;
            ImGui::PopFont();
            const std::string scene = "  \xC2\xB7  " + sceneName;
            draw->AddText({x, textY}, ImGui::GetColorU32(EditorTheme::Colors().TextDim), scene.c_str());
            x += ImGui::CalcTextSize(scene.c_str()).x + EditorTheme::Px(EditorTheme::Space4);
        }
        else
            // The project name opens the project commands (new, open, recent).
            x = Widgets::Pill({x, y}, [&] {
                    if (Widgets::TextButton(ICON_FA_CUBES, m_ProjectName.c_str(), "Projects", false, true))
                        ImGui::OpenPopup("##ProjectMenu");
                    if (ImGui::BeginPopup("##ProjectMenu"))
                    {
                        Widgets::PushMenuStyle();
                        DrawProjectMenu();
                        Widgets::PopMenuStyle();
                        ImGui::EndPopup();
                    }
                }) + gap;
        // Only the actions used all the time; the rest live in the menus and shortcuts.
        const float toolsLeft = x;
        x = Widgets::Pill({x, y}, [&] {
                if (ToolButton("##SaveScene", ICON_FA_FLOPPY_DISK, "Save scene (Ctrl+S)", false, !m_Play.IsActive()))
                    SaveCurrentScene();
                ImGui::SameLine();
                const auto* asset   = ViewportVfxDocument();
                const auto  undo    = UndoRouting::Describe(m_History, asset, false);
                const auto  redo    = UndoRouting::Describe(m_History, asset, true);
                const auto  undoTip = undo.empty() ? std::string("Nothing to undo") : "Undo " + undo + " (Ctrl+Z)";
                const auto  redoTip = redo.empty() ? std::string("Nothing to redo") : "Redo " + redo + " (Ctrl+Y)";
                const bool  editing = !m_Play.IsActive();
                if (ToolButton("##Undo", ICON_FA_UNDO, undoTip.c_str(), false, editing && !undo.empty()))
                    UndoScene();
                ImGui::SameLine();
                if (ToolButton("##Redo", ICON_FA_REDO, redoTip.c_str(), false, editing && !redo.empty()))
                    RedoScene();
            }) + gap;
        if (custom)
            m_Titlebar.Client({toolsLeft, bar.Min.y}, {x - gap, bar.Max.y});

        // Play controls in the middle of the band unless the left side reaches past it.
        // Same height and centre as the pills, so the groups' edges line up.
        const float playTop = bar.Min.y + (bar.GetHeight() - Widgets::BandGroupHeight()) * 0.5f;
        const float playLeft = std::max(x + gap, (bar.Min.x + bar.Max.x - Widgets::TransportWidth()) * 0.5f);
        x = DrawPlayControls({playLeft, playTop}) + gap;
        if (custom)
            m_Titlebar.Client({playLeft, bar.Min.y}, {x - gap, bar.Max.y});
        // Play mode: an accent line under the band (the band itself is tinted by the host).
        if (m_Play.IsActive())
            ImGui::GetWindowDrawList()->AddRectFilled({bar.Min.x, bar.Max.y - EditorTheme::Px(2.0f)}, bar.Max,
                                                      ImGui::GetColorU32(EditorTheme::Colors().TypeModel));

        // Right side: the last error, import progress and the scene name (in the titlebar
        // the scene is named next to the project instead).
        std::string status = custom ? std::string() : ICON_FA_CUBE "  " + sceneName;
        if (m_Play.IsActive())
        {
            char clock[64];
            std::snprintf(clock,
                          sizeof(clock),
                          "%s %.1f s   ",
                          m_Play.GetState() == ScenePlaySession::State::Paused ? "Paused" :
                          m_Play.IsSimulating()                                ? "Simulating" :
                                                                                 "Playing",
                          m_Play.GetPlayTime());
            status = custom ? std::string(clock, std::strlen(clock) - 3) : clock + status; // Without the gap.
        }
        // The first running task, in short; the progress popup has the details.
        const std::string progressText = Launcher::EllipsizeMiddle(
            ProgressUI::StatusText(m_Progress.Tasks()), EditorTheme::Px(420.0f), [](std::string_view text) {
                return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
            });
        const std::string importText   = progressText.empty() ? "" : ICON_FA_SPINNER "  " + progressText;
        // While a VFX asset is edited in the viewport, which document Ctrl+Z changes next.
        std::string undoText;
        if (const auto* asset = m_Play.IsActive() ? nullptr : ViewportVfxDocument())
            if (const auto next = UndoRouting::Describe(m_History, asset, false); !next.empty())
                undoText = ICON_FA_UNDO "  Ctrl+Z: " + next;
        const std::string errorText =
            m_SceneFileError.empty() ? "" : ICON_FA_CIRCLE_XMARK "  " + m_SceneFileError.substr(0, 80);
        const float spacing = EditorTheme::Px(EditorTheme::Space4);
        float       width   = ImGui::CalcTextSize(status.c_str()).x;
        if (!importText.empty())
            width += ImGui::CalcTextSize(importText.c_str()).x + spacing;
        if (!errorText.empty())
            width += ImGui::CalcTextSize(errorText.c_str()).x + spacing;
        if (!undoText.empty())
            width += ImGui::CalcTextSize(undoText.c_str()).x + spacing;
        const float textY = bar.Min.y + (bar.GetHeight() - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorScreenPos({std::max(x, m_BandRight - width - EditorTheme::Px(EditorTheme::Space3)), textY});
        ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0.0f; // textY is already centred.
        if (!errorText.empty())
        {
            ImGui::TextColored(EditorTheme::Colors().Danger, "%s", errorText.c_str());
            if (custom)
                m_Titlebar.ClientItem(); // Clicking it opens the console.
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nClick to open the console and dismiss.", m_SceneFileError.c_str());
            if (ImGui::IsItemClicked())
            {
                m_SceneFileError.clear();
                m_Panels.Open(PanelID::Console);
            }
            ImGui::SameLine(0, spacing);
        }
        if (!importText.empty())
        {
            ImGui::TextColored(AccentColor, "%s", importText.c_str());
            ImGui::SameLine(0, spacing);
        }
        if (!undoText.empty())
        {
            ImGui::TextColored(EditorTheme::Colors().TextDim, "%s", undoText.c_str());
            ImGui::SameLine(0, spacing);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, m_Play.IsActive() ? EditorTheme::Colors().TypeModel : EditorTheme::Colors().TextDim);
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s",
                              m_ScenePath.empty() ? "Not saved yet" : m_ScenePath.u8string().c_str(),
                              m_History.IsModified() ? "\nUnsaved changes" : "");
    }

    // ------------------------------------------------------------------ stats

    void EditorLayer::DrawStats()
    {
        const ImGuiIO& io     = ImGui::GetIO();
        const auto&    result = m_Viewport->GetLastResult();
        const auto&    stats  = result.FrameStats;
        const auto     ms     = [](double value) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.3f ms", value);
            return std::string(text);
        };
        const auto optionalMs = [&](const std::optional<double>& value) { return value ? ms(*value) : "-"; };
        const auto count      = [](uint64_t value) { return std::to_string(value); };
        const auto mib        = [](const std::optional<uint64_t>& bytes) {
            return bytes ? std::to_string(*bytes / (1024 * 1024)) + " MiB" : std::string("unavailable");
        };
        if (!ImGui::BeginChild("##Cards", {0, 0}, ImGuiChildFlags_None))
        {
            ImGui::EndChild();
            return;
        }

        char fps[48];
        std::snprintf(fps, sizeof(fps), "%.0f FPS", io.Framerate);
        PropertyGrid::CardOptions frameCard;
        frameCard.Note = fps;
        if (PropertyGrid::BeginCard("Frame", ICON_FA_CHART_LINE, "Frame", frameCard))
        {
            // Frame time history; the right end is the newest sample.
            std::array<float, 240> ordered;
            float                  peak = 1.0f;
            for (size_t i = 0; i < ordered.size(); ++i)
            {
                ordered[i] = m_FrameTimes[(m_FrameTimeCursor + i) % m_FrameTimes.size()];
                peak       = std::max(peak, ordered[i]);
            }
            const std::string overlay = "max " + std::to_string(int(peak)) + " ms";
            ImGui::PlotLines("##frametimes",
                             ordered.data(),
                             int(ordered.size()),
                             0,
                             overlay.c_str(),
                             0.0f,
                             peak * 1.1f,
                             {-FLT_MIN, EditorTheme::Px(56.0f)});
            PropertyGrid::Begin("Frame");
            PropertyGrid::Text("Frame time", ms(io.Framerate > 0.0f ? 1000.0 / io.Framerate : 0.0));
            const auto viewport = m_Viewport->GetSize();
            PropertyGrid::Text("Viewport", std::to_string(viewport.x) + " x " + std::to_string(viewport.y));
            // The mode the renderer used (MSAA falls back to a supported sample count).
            PropertyGrid::Text("Anti-aliasing", AntiAliasingName(stats.EffectiveAA));
            if (stats.RenderSize != stats.DisplaySize)
                PropertyGrid::Text("Resolution", RenderResolutionText(stats.RenderSize, stats.DisplaySize));
            PropertyGrid::Text("Editor UI",
                               std::to_string(io.MetricsRenderVertices) + " vertices, " +
                                   std::to_string(io.MetricsRenderIndices) + " indices",
                               true);
            PropertyGrid::End();
        }
        PropertyGrid::EndCard();

        if (PropertyGrid::BeginCard("Render", ICON_FA_LAYER_GROUP, "Render"))
        {
            if (PropertyGrid::BeginGroup("CPU"))
            {
                PropertyGrid::Text("Sync / extract", ms(stats.WorldSyncExtractMilliseconds));
                PropertyGrid::Text("Graph build", ms(stats.GraphBuildMilliseconds));
                PropertyGrid::Text("Compile", ms(stats.CompileMilliseconds));
                PropertyGrid::Text("Execute / submit", ms(stats.ExecuteSubmitMilliseconds));
                PropertyGrid::Text("Total", ms(stats.TotalMilliseconds));
                if (stats.Draws)
                {
                    PropertyGrid::Text("Draw calls", count(stats.Draws->DrawCalls));
                    PropertyGrid::Text("Triangles", count(stats.Draws->Triangles));
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Passes"))
            {
                if (stats.Gpu)
                    PropertyGrid::Text("GPU total",
                                       ms(stats.Gpu->PassMilliseconds) + "  (frame " + count(stats.Gpu->SourceFrame) +
                                           ")");
                else
                    PropertyGrid::Text("GPU total", "arrives a few frames later", true);
                PropertyGrid::BeginFree();
                if (ImGui::BeginTable("##passes",
                                      5,
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                    ImGui::TableSetupColumn("CPU");
                    ImGui::TableSetupColumn("GPU");
                    ImGui::TableSetupColumn("Draws");
                    ImGui::TableSetupColumn("Tris");
                    ImGui::TableHeadersRow();
                    for (const auto& pass : stats.Passes)
                    {
                        if (pass.Culled)
                            continue;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(pass.Name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(optionalMs(pass.CpuRecordMilliseconds).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(optionalMs(pass.GpuMilliseconds).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(count(pass.Draws ? pass.Draws->DrawCalls : 0).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(count(pass.Draws ? pass.Draws->Triangles : 0).c_str());
                    }
                    ImGui::EndTable();
                }
                if (ImGui::Button(ICON_FA_DRAW_POLYGON "  Open Frame Graph", {-FLT_MIN, 0.0f}))
                    m_Panels.Open(PanelID::FrameGraph);
                PropertyGrid::EndFree();
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();

        if (PropertyGrid::BeginCard("Scene", ICON_FA_CUBES, "Scene"))
        {
            PropertyGrid::Begin("Scene");
            PropertyGrid::Text("Mesh sections",
                               count(stats.Culling.SectionProxies) + "  (" + count(stats.Culling.InFrustum) +
                                   " in view, " + count(stats.Culling.Drawn) + " drawn)");
            PropertyGrid::Text("Lights",
                               std::to_string(stats.DirectionalLights) + " directional, " +
                                   std::to_string(stats.PointLights) + " point (" +
                                   std::to_string(stats.ShadowedLights) + " shadowed)");
            PropertyGrid::Text("Proxies",
                               "+" + count(stats.ProxiesCreated) + "  ~" + count(stats.ProxiesUpdated) + "  -" +
                                   count(stats.ProxiesRemoved),
                               true);
            if (stats.BloomMipCount)
                PropertyGrid::Text("Bloom mips", std::to_string(stats.BloomMipCount));
            const auto& sprites = result.SpriteStats;
            PropertyGrid::Text(
                "2D", std::to_string(sprites.DrawCalls) + " draws, " + std::to_string(sprites.QuadCount) + " quads");
            PropertyGrid::End();
            if (!stats.Shadows.empty() && PropertyGrid::BeginGroup("Shadow passes", nullptr, nullptr, false))
            {
                for (const auto& shadow : stats.Shadows)
                    PropertyGrid::Text(shadow.Name.c_str(),
                                       count(shadow.CastersDrawn) + " drawn, " + count(shadow.CastersCulled) +
                                           " culled");
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();

        if (PropertyGrid::BeginCard("Memory", ICON_FA_MICROCHIP, "Memory"))
        {
            if (PropertyGrid::BeginGroup("GPU"))
            {
                PropertyGrid::Text("Textures", mib(stats.DeviceTextureBytes));
                PropertyGrid::Text("Buffers", mib(stats.DeviceBufferBytes));
                PropertyGrid::Text("Transient slots", std::to_string(stats.TransientTextureSlots));
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Assets"))
            {
                PropertyGrid::Text("Resident", std::to_string(m_Assets ? m_Assets->GetResidentCount() : size_t(0)));
                if (m_AssetImports)
                    if (auto snapshot = m_AssetImports->GetSnapshot();
                        std::holds_alternative<EditorAssetImportSnapshot>(snapshot))
                    {
                        const char* states[] = {"Idle", "Building", "Succeeded", "Failed", "Cancelled"};
                        PropertyGrid::Text(
                            "Last import",
                            states[static_cast<int>(std::get<EditorAssetImportSnapshot>(snapshot).State)]);
                    }
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();

        if (m_ActiveScene)
        {
            PropertyGrid::CardOptions debugCard;
            debugCard.DefaultOpen = false;
            if (PropertyGrid::BeginCard("DebugRendering", ICON_FA_BUG, "Debug Rendering", debugCard))
            {
                auto  resource = m_ActiveScene->GetRenderWorld().Settings;
                auto& state    = resource.Piplinestate;
                PropertyGrid::Begin("DebugRendering");
                PropertyGrid::Checkbox("Grid", state.EnableGridShader);
                PropertyGrid::Checkbox("Outline", state.EnableOutline);
                PropertyGrid::Checkbox("Stencil", state.EnableStencil);
                PropertyGrid::Checkbox("Debug normals", state.EnableDebugNormal);
                PropertyGrid::End();
                m_ActiveScene->GetRenderWorld().Settings = resource;
            }
            PropertyGrid::EndCard();
        }
        ImGui::EndChild();
    }
} // namespace Kans
