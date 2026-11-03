#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Project/RecentProjects.h"
#include "EditorLayer.h"
#include "Launcher/EditorProcess.h"
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

        std::string MenuLabel(const char* icon, const char* text) { return std::string(icon) + "  " + text; }
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
        m_PropertiesPanel.SetOpenTimeline([this](Entity owner) {
            m_TimelinePanel.Open(owner);
            m_Panels.Open(PanelID::Timeline);
        });
        m_TimelinePanel.SetViewThroughCamera([this](bool show) { m_ViewportPanel.SetPreviewSceneCamera(show); });
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
        m_ThumbnailBudget = 2; // Material thumbnails rendered per frame.
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
        DrawClothInfo();
        m_RagdollEditor.Draw();
        Application::Get().GetImGuiLayer()->BlockEvents(m_ViewportPanel.IsFocused() || m_ViewportPanel.IsHovered());
        TrackSceneEdits();
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

    void EditorLayer::UndoScene(int steps)
    {
        if (!m_ActiveScene || m_Play.IsActive())
            return;
        m_TimelinePanel.StopPreview();
        // Pending changes become a step first, so they can be undone too.
        m_History.Record(*m_ActiveScene);
        m_EditPending = false;
        for (int i = 0; i < steps && m_History.Undo(*m_ActiveScene); ++i)
            ;
    }

    void EditorLayer::RedoScene(int steps)
    {
        if (!m_ActiveScene || m_Play.IsActive())
            return;
        m_TimelinePanel.StopPreview();
        m_History.Record(*m_ActiveScene);
        m_EditPending = false;
        for (int i = 0; i < steps && m_History.Redo(*m_ActiveScene); ++i)
            ;
    }

    void EditorLayer::HandleShortcuts()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput)
            return;
        // The material editor has its own undo, redo and save while it has focus.
        const bool material =
            m_MaterialEditorPanel.HasFocus() || m_PhysicsMaterialPanel.HasFocus() || m_RagdollEditor.HasFocus();
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
        if (io.KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_N, false))
                ConfirmDiscard("creating a new scene", [this] { Record(NewScene()); });
            if (ImGui::IsKeyPressed(ImGuiKey_O, false))
                ConfirmDiscard("opening another scene", [this] { m_SceneDialog = SceneDialog::Open; });
            if (!material && ImGui::IsKeyPressed(ImGuiKey_S, false))
            {
                if (io.KeyShift)
                    m_SceneDialog = SceneDialog::Save;
                else
                    SaveCurrentScene();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
                m_Panels.Toggle(PanelID::ContentBrowser);
            if (!material && ImGui::IsKeyPressed(ImGuiKey_Z, true))
            {
                if (io.KeyShift)
                    RedoScene();
                else
                    UndoScene();
            }
            if (!material && ImGui::IsKeyPressed(ImGuiKey_Y, true))
                RedoScene();
            if (ImGui::IsKeyPressed(ImGuiKey_P, false))
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
            if (ToolButton(ICON_FA_ATOM, tip, simulating, state == State::Edit || simulating))
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
        if (ImGui::MenuItem(MenuLabel(ICON_FA_PLUS, "New Project...").c_str()))
            openBrowser(ProjectBrowser::Page::New);
        if (ImGui::MenuItem(MenuLabel(ICON_FA_FOLDER_OPEN, "Open Project...").c_str()))
        {
            openBrowser(ProjectBrowser::Page::Recent);
            m_ProjectBrowser->BrowseForProject();
        }
        if (ImGui::MenuItem(MenuLabel(ICON_FA_GEAR, "Project Settings...").c_str()))
            m_Panels.Open(PanelID::ProjectSettings);
        if (ImGui::BeginMenu(MenuLabel(ICON_FA_CLOCK_ROTATE_LEFT, "Recent Projects").c_str()))
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
            ImGui::EndMenu();
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
        if (ImGui::BeginMenu("File"))
        {
            Widgets::MenuSection("Project");
            DrawProjectMenu();
            Widgets::MenuSection("Scene");
            if (ImGui::MenuItem(MenuLabel(ICON_FA_FILE, "New Scene").c_str(), "Ctrl+N"))
                ConfirmDiscard("creating a new scene", [this] { Record(NewScene()); });
            if (ImGui::MenuItem(MenuLabel(ICON_FA_FOLDER_OPEN, "Open Scene...").c_str(), "Ctrl+O"))
                ConfirmDiscard("opening another scene", [this] { m_SceneDialog = SceneDialog::Open; });
            if (ImGui::MenuItem(MenuLabel(ICON_FA_FLOPPY_DISK, "Save Scene").c_str(), "Ctrl+S", false, !m_Play.IsActive()))
                SaveCurrentScene();
            if (ImGui::MenuItem(MenuLabel(ICON_FA_FLOPPY_DISK, "Save Scene As...").c_str(), "Ctrl+Shift+S", false, !m_Play.IsActive()))
                m_SceneDialog = SceneDialog::Save;
            ImGui::Separator();
            if (ImGui::MenuItem(MenuLabel(ICON_FA_XMARK, "Exit").c_str()))
                ConfirmDiscard("exiting", [] { Application::Get().Close(); });
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            const auto undo = m_History.UndoLabels();
            const auto redo = m_History.RedoLabels();
            if (ImGui::MenuItem(MenuLabel(ICON_FA_UNDO, undo.empty() ? "Undo" : ("Undo " + undo.front()).c_str()).c_str(),
                                "Ctrl+Z",
                                false,
                                !undo.empty() && !m_Play.IsActive()))
                UndoScene();
            if (ImGui::MenuItem(MenuLabel(ICON_FA_REDO, redo.empty() ? "Redo" : ("Redo " + redo.front()).c_str()).c_str(),
                                "Ctrl+Y",
                                false,
                                !redo.empty() && !m_Play.IsActive()))
                RedoScene();
            ImGui::Separator();
            // Jump several steps at once.
            if (ImGui::BeginMenu(MenuLabel(ICON_FA_CLOCK_ROTATE_LEFT, "History").c_str(),
                                 (!undo.empty() || !redo.empty()) && !m_Play.IsActive()))
            {
                for (size_t i = redo.size(); i-- > 0;)
                {
                    ImGui::PushID(int(1000 + i));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::MenuItem(redo[i].c_str()))
                        RedoScene(int(i + 1));
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::MenuItem(ICON_FA_CHECK "  Current state", nullptr, true, false);
                for (size_t i = 0; i < undo.size(); ++i)
                {
                    ImGui::PushID(int(i));
                    if (ImGui::MenuItem(undo[i].c_str()))
                        UndoScene(int(i + 1));
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        // Play, pause and step live on the band's play controls (and Ctrl+P / Ctrl+Shift+P).
        if (ImGui::BeginMenu("Assets"))
        {
            if (ImGui::MenuItem(MenuLabel(ICON_FA_FILE_IMPORT, "Asset Importer").c_str()))
                m_Panels.Open(PanelID::AssetImporter);
            if (ImGui::MenuItem(MenuLabel(ICON_FA_PALETTE, "Create Material").c_str()))
                CreateMaterial(m_ContentBrowserPanel.GetCurrentFolder());
            ImGui::Separator();
            if (ImGui::MenuItem(MenuLabel(ICON_FA_ROTATE_RIGHT, "Refresh Published Products").c_str()))
                Record(RefreshProductAssets());
            ImGui::EndMenu();
        }
        DrawScriptsMenu();
        if (ImGui::BeginMenu("Window"))
        {
            m_Panels.DrawWindowMenu();
            ImGui::EndMenu();
        }
        DrawToolbar();
        ImGui::EndMenuBar();
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

        // The project name opens the project commands (new, open, recent).
        x = Widgets::Pill({x, y}, [&] {
                if (Widgets::TextButton(ICON_FA_CUBES, m_ProjectName.c_str(), "Projects", false, true))
                    ImGui::OpenPopup("##ProjectMenu");
                if (ImGui::BeginPopup("##ProjectMenu"))
                {
                    DrawProjectMenu();
                    ImGui::EndPopup();
                }
            }) + gap;
        // Only the actions used all the time; the rest live in the menus and shortcuts.
        x = Widgets::Pill({x, y}, [&] {
                if (ToolButton(ICON_FA_FLOPPY_DISK, "Save scene (Ctrl+S)", false, !m_Play.IsActive()))
                    SaveCurrentScene();
                ImGui::SameLine();
                const auto undo    = m_History.UndoLabels();
                const auto redo    = m_History.RedoLabels();
                const auto undoTip = undo.empty() ? std::string("Nothing to undo") : "Undo " + undo.front() + " (Ctrl+Z)";
                const auto redoTip = redo.empty() ? std::string("Nothing to redo") : "Redo " + redo.front() + " (Ctrl+Y)";
                const bool editing = !m_Play.IsActive();
                if (ToolButton(ICON_FA_UNDO, undoTip.c_str(), false, editing && !undo.empty()))
                    UndoScene();
                ImGui::SameLine();
                if (ToolButton(ICON_FA_REDO, redoTip.c_str(), false, editing && !redo.empty()))
                    RedoScene();
            }) + gap;

        // Play controls in the middle of the band unless the left side reaches past it.
        // Same height and centre as the pills, so the groups' edges line up.
        const float playTop = bar.Min.y + (bar.GetHeight() - Widgets::BandGroupHeight()) * 0.5f;
        x = DrawPlayControls({std::max(x + gap, (bar.Min.x + bar.Max.x - Widgets::TransportWidth()) * 0.5f), playTop}) + gap;
        // Play mode: an accent line under the band (the band itself is tinted by the host).
        if (m_Play.IsActive())
            ImGui::GetWindowDrawList()->AddRectFilled({bar.Min.x, bar.Max.y - EditorTheme::Px(2.0f)}, bar.Max,
                                                      ImGui::GetColorU32(EditorTheme::Colors().TypeModel));

        // Right side: the last error, import progress and the scene name.
        std::string status = ICON_FA_CUBE "  " + (m_ScenePath.empty() ? std::string("Untitled scene") :
                                                                         m_ScenePath.filename().u8string());
        if (m_History.IsModified())
            status += "  " ICON_FA_CIRCLE;
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
            status = clock + status;
        }
        bool importing = false;
        if (m_AssetImports)
            if (auto snapshot = m_AssetImports->GetSnapshot();
                std::holds_alternative<EditorAssetImportSnapshot>(snapshot) &&
                std::get<EditorAssetImportSnapshot>(snapshot).State == EditorAssetImportState::Building)
            {
                const auto source = std::get<EditorAssetImportSnapshot>(snapshot).Source;
                if (source != m_StatusSource)
                {
                    m_StatusSource = source;
                    m_StatusName   = source.ToString();
                    if (auto  sources = m_AssetImports->GetSources();
                        auto* list    = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
                        for (const auto& metadata : *list)
                            if (metadata.sourceAssetID == source)
                                m_StatusName = metadata.Path.GetPath().filename().u8string();
                }
                importing = true;
            }
        const std::string importText = importing ? ICON_FA_SPINNER "  Importing " + m_StatusName + "..." : "";
        const std::string errorText =
            m_SceneFileError.empty() ? "" : ICON_FA_CIRCLE_XMARK "  " + m_SceneFileError.substr(0, 80);
        const float spacing = EditorTheme::Px(EditorTheme::Space4);
        float       width   = ImGui::CalcTextSize(status.c_str()).x;
        if (!importText.empty())
            width += ImGui::CalcTextSize(importText.c_str()).x + spacing;
        if (!errorText.empty())
            width += ImGui::CalcTextSize(errorText.c_str()).x + spacing;
        const float textY = bar.Min.y + (bar.GetHeight() - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorScreenPos({std::max(x, bar.Max.x - width - EditorTheme::Px(EditorTheme::Space3)), textY});
        ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0.0f; // textY is already centred.
        if (!errorText.empty())
        {
            ImGui::TextColored(EditorTheme::Colors().Danger, "%s", errorText.c_str());
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
