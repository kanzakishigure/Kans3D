#include "BenchmarkWindow.h"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Project/ProjectFile.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"
#include "UI/Widgets.h"

namespace Kans
{
    namespace fs = std::filesystem;
    using EditorTheme::Px;
    using State = Launcher::BenchmarkSession::State;
    namespace
    {
        struct Preset
        {
            const char* Label;
            uint32_t    Width, Height;
        };
        constexpr Preset      Presets[] {{"1920 x 1080", 1920, 1080},
                                         {"2560 x 1440", 2560, 1440},
                                         {"3840 x 2160", 3840, 2160}};
        constexpr const char* ComplexLabel = "Built-in: complex (procedural stress scene)";

        ImFont* BoldFont()
        {
            auto& fonts = ImGui::GetIO().Fonts->Fonts;
            return fonts.Size > 1 ? fonts[1] : ImGui::GetFont();
        }

        void Heading(const char* text, float size)
        {
            ImGui::PushFont(BoldFont(), size);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
        }

        bool PrimaryButton(const char* label, bool enabled, ImVec2 size = {0, 0})
        {
            const auto& p = EditorTheme::Colors();
            ImGui::BeginDisabled(!enabled);
            ImGui::PushStyleColor(ImGuiCol_Button, p.Accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.AccentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.AccentPressed);
            const bool pressed = ImGui::Button(label, size);
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            return pressed;
        }

        ImVec4 Alpha(ImVec4 color, float alpha) { return {color.x, color.y, color.z, alpha}; }

        std::string Duration(double seconds)
        {
            const auto total = int64_t(seconds + 0.5);
            char       text[32];
            if (total >= 3600)
                std::snprintf(
                    text, sizeof(text), "%d:%02d:%02d", int(total / 3600), int(total / 60 % 60), int(total % 60));
            else
                std::snprintf(text, sizeof(text), "%d:%02d", int(total / 60), int(total % 60));
            return text;
        }

        std::string Date(int64_t unixTime)
        {
            if (unixTime <= 0)
                return "-";
            const std::time_t time = static_cast<std::time_t>(unixTime);
            std::tm           local {};
#ifdef _WIN32
            localtime_s(&local, &time);
#else
            localtime_r(&time, &local);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
            return text;
        }

        std::string Ms(std::optional<double> value, const char* format = "%.3f")
        {
            if (!value)
                return "-";
            char text[32];
            std::snprintf(text, sizeof(text), format, *value);
            return text;
        }

        std::string SceneLabel(const std::string& scene)
        {
            return scene == Benchmark::ComplexScene ? ComplexLabel : scene;
        }

        const char* MetricLabel(const std::string& metric)
        {
            if (metric == "cpu_ms")
                return "CPU wall";
            if (metric == "cpu_busy_ms")
                return "CPU busy";
            if (metric == "gpu_frame_ms")
                return "GPU frame";
            if (metric == "gpu_ms")
                return "GPU pass sum";
            if (metric == "cpu_record_ms")
                return "CPU record";
            return metric.c_str();
        }

        ImVec4 BottleneckColor(const std::string& label)
        {
            const auto& p = EditorTheme::Colors();
            if (label == "Shader compile stutter")
                return p.Danger;
            if (label == "GPU-bound" || label == "CPU-bound")
                return p.Warning;
            if (label == "Present/VSync-limited")
                return p.Success;
            return p.TextDim;
        }

        // Opens the system file manager at `folder`.
        void Reveal(const fs::path& folder)
        {
#ifdef _WIN32
            const std::string command = "explorer \"" + folder.string() + "\"";
#else
            std::string quoted = "'";
            for (char c : folder.u8string())
                quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
            quoted += "'";
#ifdef __APPLE__
            const std::string command = "open " + quoted + " &";
#else
            const std::string command = "xdg-open " + quoted + " >/dev/null 2>&1 &";
#endif
#endif
            std::system(command.c_str());
        }

        void CellText(const std::string& text, ImVec4 color)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopStyleColor();
        }
    } // namespace

    BenchmarkWindow::BenchmarkWindow(fs::path program) : m_Program(std::move(program)) {}

    void BenchmarkWindow::Open(const fs::path& projectFile)
    {
        if (IsRunning())
            return;
        m_Error.clear();
        m_ProjectFile     = projectFile;
        m_ProjectRoot     = projectFile.parent_path();
        auto       loaded = LoadProjectFile(projectFile);
        const auto project =
            std::holds_alternative<ProjectFile>(loaded) ? std::get<ProjectFile>(loaded) : ProjectFile {};
        if (const auto* error = std::get_if<AssetError>(&loaded))
            m_Error = error->Message;
        m_ProjectName  = project.Name.empty() ? projectFile.stem().u8string() : project.Name;
        m_SettingsFile = Benchmark::HubSettingsFile(m_ProjectRoot, project.CacheDirectory);
        m_Settings     = Benchmark::LoadHubSettings(m_SettingsFile);
        m_Scenes       = Benchmark::ListScenes(m_ProjectRoot, project.AssetDirectory);
        if (m_Settings.Scene != Benchmark::ComplexScene &&
            std::find(m_Scenes.begin(), m_Scenes.end(), m_Settings.Scene) == m_Scenes.end())
            m_Settings.Scene = Benchmark::ComplexScene;
        m_CustomResolution = std::none_of(std::begin(Presets), std::end(Presets), [&](const Preset& preset) {
            return preset.Width == m_Settings.Width && preset.Height == m_Settings.Height;
        });
        m_Result.reset();
        m_ResultFile.clear();
        m_SeenState = m_Session.GetState();
        RefreshHistory();
        m_Open          = true;
        m_OpenRequested = true;
    }

    void BenchmarkWindow::Close()
    {
        m_Session.Cancel();
        m_Open = m_OpenRequested = false;
    }

    void BenchmarkWindow::RefreshHistory()
    {
        m_History = Benchmark::ListHistory(Benchmark::BenchmarksDirectory(m_ProjectRoot));
    }

    void BenchmarkWindow::SaveSettings()
    {
        if (auto error = Benchmark::SaveHubSettings(m_SettingsFile, m_Settings))
            m_Error = *error;
    }

    void BenchmarkWindow::ShowResult(const fs::path& summary)
    {
        try
        {
            m_Result     = Benchmark::MakeResultView(Benchmark::ReadReport(summary));
            m_ResultFile = summary;
        }
        catch (const std::exception& error)
        {
            m_Result.reset();
            m_ResultFile.clear();
            m_Error = error.what();
        }
    }

    void BenchmarkWindow::Start()
    {
        SaveSettings();
        m_Error.clear();
        if (auto error = m_Session.Start({m_Program, m_ProjectFile, m_Settings}))
            m_Error = *error;
        else
        {
            m_Result.reset();
            m_ResultFile.clear();
        }
        m_SeenState = m_Session.GetState();
    }

    std::string BenchmarkWindow::BaselineLabel() const
    {
        if (m_Settings.Baseline.empty())
            return "None";
        for (const auto& entry : m_History)
            if (entry.Summary == m_Settings.Baseline)
                return Date(entry.Created) + "  " + SceneLabel(entry.Scene);
        return m_Settings.Baseline.filename().u8string();
    }

    void BenchmarkWindow::Draw()
    {
        if (!m_Open)
            return;
        if (IsRunning())
            m_Session.Poll();
        if (const auto state = m_Session.GetState(); state != m_SeenState)
        {
            m_SeenState = state;
            if (state == State::Succeeded)
            {
                RefreshHistory();
                ShowResult(m_Session.SummaryFile());
            }
        }
        if (m_OpenRequested)
        {
            ImGui::OpenPopup("###BenchmarkWindow");
            m_OpenRequested = false;
        }
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize({(std::min)(Px(1240.0f), viewport->WorkSize.x - Px(48.0f)),
                                  (std::min)(Px(860.0f), viewport->WorkSize.y - Px(48.0f))},
                                 ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        const std::string title = "Benchmark \xE2\x80\x94 " + m_ProjectName + "###BenchmarkWindow";
        if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings))
        {
            Close();
            return;
        }

        Heading(ICON_FA_GAUGE_HIGH "  Benchmark", Px(20.0f));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", m_ProjectFile.u8string().c_str());
        const char* closeLabel = "Close";
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                             ImGui::CalcTextSize(closeLabel).x - ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::Button(closeLabel))
        {
            SaveSettings();
            Close();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::Dummy({0, Px(EditorTheme::Space1)});

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float  left      = (std::max)(Px(400.0f), available.x * 0.40f);
        if (ImGui::BeginChild("##BenchmarkLeft", {left, 0}, ImGuiChildFlags_None))
        {
            DrawSetup(available.y);
            DrawHistory();
        }
        ImGui::EndChild();
        ImGui::SameLine(0, Px(EditorTheme::Space3));
        if (ImGui::BeginChild("##BenchmarkRight", {0, 0}, ImGuiChildFlags_None))
            DrawStatus();
        ImGui::EndChild();
        DrawDialogs();
        ImGui::EndPopup();
    }

    void BenchmarkWindow::DrawSetup(float)
    {
        const auto& p       = EditorTheme::Colors();
        const bool  running = IsRunning() || m_Preview;
        ImGui::BeginDisabled(running);
        if (PropertyGrid::BeginCard("BenchmarkSetup", ICON_FA_GEAR, "Setup"))
        {
            if (PropertyGrid::BeginGroup("Scene"))
            {
                if (PropertyGrid::Row("Scene", "One of this project's scenes, or the built-in procedural stress scene"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##Scene", SceneLabel(m_Settings.Scene).c_str()))
                    {
                        if (ImGui::Selectable(ComplexLabel, m_Settings.Scene == Benchmark::ComplexScene))
                            m_Settings.Scene = Benchmark::ComplexScene;
                        if (!m_Scenes.empty())
                            ImGui::Separator();
                        for (const auto& scene : m_Scenes)
                            if (ImGui::Selectable(scene.c_str(), m_Settings.Scene == scene))
                                m_Settings.Scene = scene;
                        if (m_Scenes.empty())
                            ImGui::TextDisabled("This project has no .kans scenes.");
                        ImGui::EndCombo();
                    }
                }
                if (m_Settings.Scene == Benchmark::ComplexScene)
                {
                    const Benchmark::ComplexOptions defaults;
                    const auto field = [](const char* label, uint32_t& value, int lower, int upper, uint32_t reset) {
                        int       number = int(value);
                        const int base   = int(reset);
                        if (PropertyGrid::Int(label, number, lower, upper, &base))
                            value = uint32_t(std::clamp(number, lower, upper));
                    };
                    field("Instances", m_Settings.Complex.Instances, 1, 100000, defaults.Instances);
                    field("Lights", m_Settings.Complex.Lights, 0, 256, defaults.Lights);
                    field("Shadowed lights",
                          m_Settings.Complex.ShadowedLights,
                          0,
                          int((std::min)(m_Settings.Complex.Lights, 8u)),
                          defaults.ShadowedLights);
                    field("Seed", m_Settings.Complex.Seed, 0, INT32_MAX, defaults.Seed);
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Resolution"))
            {
                const char* items[std::size(Presets) + 1];
                int         index = int(std::size(Presets));
                for (size_t i = 0; i < std::size(Presets); ++i)
                {
                    items[i] = Presets[i].Label;
                    if (!m_CustomResolution && Presets[i].Width == m_Settings.Width &&
                        Presets[i].Height == m_Settings.Height)
                        index = int(i);
                }
                items[std::size(Presets)] = "Custom";
                if (PropertyGrid::Combo("Resolution", items, int(std::size(items)), index))
                {
                    m_CustomResolution = index == int(std::size(Presets));
                    if (!m_CustomResolution)
                    {
                        m_Settings.Width  = Presets[index].Width;
                        m_Settings.Height = Presets[index].Height;
                    }
                }
                if (m_CustomResolution)
                {
                    int width = int(m_Settings.Width), height = int(m_Settings.Height);
                    if (PropertyGrid::Int("Width", width, 1, 16384))
                        m_Settings.Width = uint32_t(std::clamp(width, 1, 16384));
                    if (PropertyGrid::Int("Height", height, 1, 16384))
                        m_Settings.Height = uint32_t(std::clamp(height, 1, 16384));
                }
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Frames"))
            {
                const Benchmark::HubSettings defaults;
                int frames = int(m_Settings.Frames), warmup = int(m_Settings.Warmup), repeats = int(m_Settings.Repeats);
                const int frameReset = int(defaults.Frames), warmupReset = int(defaults.Warmup),
                          repeatReset = int(defaults.Repeats);
                if (PropertyGrid::Int("Measured frames", frames, 1, 10000, &frameReset))
                    m_Settings.Frames = uint32_t(std::clamp(frames, 1, 10000));
                if (PropertyGrid::Int("Warmup frames", warmup, 0, 10000, &warmupReset))
                    m_Settings.Warmup = uint32_t(std::clamp(warmup, 0, 10000));
                int       slots     = int(m_Settings.FramesInFlight);
                const int slotReset = int(defaults.FramesInFlight);
                if (PropertyGrid::Int("Frames in flight", slots, 1, int(Benchmark::MaxFramesInFlight), &slotReset))
                    m_Settings.FramesInFlight = uint32_t(std::clamp(slots, 1, int(Benchmark::MaxFramesInFlight)));
                if (PropertyGrid::Int("Repeats", repeats, 1, int(Benchmark::MaxRepeats), &repeatReset))
                    m_Settings.Repeats = uint32_t(std::clamp(repeats, 1, int(Benchmark::MaxRepeats)));
                PropertyGrid::EndGroup();
            }
            if (PropertyGrid::BeginGroup("Comparison"))
            {
                if (PropertyGrid::Row("Baseline",
                                      "Compare the result with an earlier one of this project or a report file"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##Baseline", BaselineLabel().c_str()))
                    {
                        if (ImGui::Selectable("None", m_Settings.Baseline.empty()))
                            m_Settings.Baseline.clear();
                        if (!m_History.empty())
                            Widgets::MenuSection("History");
                        for (const auto& entry : m_History)
                        {
                            const auto label = Date(entry.Created) + "  " + SceneLabel(entry.Scene) + "  " +
                                               std::to_string(entry.Width) + "x" + std::to_string(entry.Height) + "##" +
                                               entry.Folder.filename().u8string();
                            if (ImGui::Selectable(label.c_str(), m_Settings.Baseline == entry.Summary))
                                m_Settings.Baseline = entry.Summary;
                        }
                        ImGui::Separator();
                        if (ImGui::Selectable(ICON_FA_FOLDER_OPEN "  Browse for a file..."))
                            m_Browser.Open(FolderBrowser::Mode::File,
                                           m_Settings.Baseline.empty() ? Benchmark::BenchmarksDirectory(m_ProjectRoot) :
                                                                         m_Settings.Baseline.parent_path(),
                                           "Choose Baseline",
                                           ".json");
                        ImGui::EndCombo();
                    }
                }
                if (!m_Settings.Baseline.empty())
                    PropertyGrid::Text("File", m_Settings.Baseline.u8string(), true);
                float      threshold = float(m_Settings.ThresholdPercent);
                const auto reset     = float(Benchmark::HubSettings {}.ThresholdPercent);
                if (PropertyGrid::Float("Threshold %", threshold, 0.1f, 0.0f, 1000.0f, "%.1f", &reset))
                    m_Settings.ThresholdPercent = std::clamp(double(threshold), 0.0, 1000.0);
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();
        ImGui::EndDisabled();

        ImGui::PushStyleColor(ImGuiCol_Text, p.TextDim);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(ICON_FA_CIRCLE_INFO "  Close other heavy programs (games, browsers with video, "
                                                   "builds) first: they share the GPU and CPU and skew the result. "
                                                   "Each repeat runs in its own editor process; this window stays "
                                                   "idle meanwhile.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        const auto problem = Benchmark::SettingsProblem(m_Settings, m_ProjectRoot);
        if (problem)
            ImGui::TextColored(p.Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %s", problem->c_str());
        if (!m_Error.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(p.Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_Error.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy({0, Px(EditorTheme::Space1)});
        if (PrimaryButton(ICON_FA_PLAY "  Run Benchmark", !problem && !running, {-FLT_MIN, 0}))
            Start();
        ImGui::Dummy({0, Px(EditorTheme::Space2)});
    }

    void BenchmarkWindow::DrawHistory()
    {
        const auto& p = EditorTheme::Colors();
        if (PropertyGrid::BeginCard("BenchmarkHistory", ICON_FA_CLOCK_ROTATE_LEFT, "History"))
        {
            PropertyGrid::Begin("BenchmarkHistoryGrid");
            PropertyGrid::BeginFree();
            if (m_History.empty())
                ImGui::TextDisabled("No results yet. They are kept in %s.",
                                    Benchmark::BenchmarksDirectory(m_ProjectRoot).u8string().c_str());
            else if (ImGui::BeginTable("##history",
                                       5,
                                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                       {0, (std::max)(Px(140.0f), ImGui::GetContentRegionAvail().y - Px(8.0f))}))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableSetupColumn("Scene", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.1f);
                ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableHeadersRow();
                for (const auto& entry : m_History)
                {
                    ImGui::PushID(entry.Folder.filename().u8string().c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool shown = m_ResultFile == entry.Summary;
                    if (ImGui::Selectable(Date(entry.Created).c_str(),
                                          shown,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) &&
                        !IsRunning())
                        ShowResult(entry.Summary);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s", entry.Gpu.c_str(), entry.Folder.u8string().c_str());
                    if (ImGui::BeginPopupContextItem("##entry"))
                    {
                        Widgets::PushMenuStyle();
                        if (Widgets::IconMenuItem(ICON_FA_CHART_LINE, "View", nullptr, false, !IsRunning()))
                            ShowResult(entry.Summary);
                        if (Widgets::IconMenuItem(ICON_FA_FLAG,
                                                  "Use as Baseline",
                                                  nullptr,
                                                  m_Settings.Baseline == entry.Summary,
                                                  !IsRunning()))
                        {
                            m_Settings.Baseline = entry.Summary;
                            SaveSettings();
                        }
                        if (Widgets::IconMenuItem(ICON_FA_TRASH_CAN, "Delete...", nullptr, false, !IsRunning()))
                        {
                            m_PendingDelete = entry.Folder;
                            m_AskDelete     = true;
                        }
                        Widgets::PopMenuStyle();
                        ImGui::EndPopup();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(entry.Scene == Benchmark::ComplexScene ?
                                               "builtin:complex" :
                                               fs::u8path(entry.Scene).stem().u8string().c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%ux%u", entry.Width, entry.Height);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Ms(entry.GpuMedianMs).c_str());
                    ImGui::TableNextColumn();
                    if (entry.Runs < 2)
                        CellText("1 run", p.TextDim);
                    else if (entry.Stable)
                        CellText("Stable", p.Success);
                    else
                        CellText("Unstable", p.Warning);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            PropertyGrid::EndFree();
            PropertyGrid::End();
        }
        PropertyGrid::EndCard();
    }

    void BenchmarkWindow::DrawStatus()
    {
        const auto& p = EditorTheme::Colors();
        if (m_Preview)
            DrawRunning(*m_Preview);
        else if (IsRunning())
        {
            RunStatus status;
            status.Run              = m_Session.CurrentRun();
            status.Runs             = m_Session.Runs();
            status.Progress         = m_Session.GetProgress();
            status.Warmup           = m_Settings.Warmup;
            status.Frames           = m_Settings.Frames;
            status.Fraction         = m_Session.CompletedFraction();
            status.ElapsedSeconds   = m_Session.ElapsedSeconds();
            status.RemainingSeconds = m_Session.RemainingSeconds();
            DrawRunning(status);
            return;
        }
        else if (m_Session.GetState() == State::Failed && !m_Result)
            DrawFailure();
        else if (m_Session.GetState() == State::Cancelled && !m_Result)
            ImGui::TextColored(p.TextDim, ICON_FA_CIRCLE_INFO "  The run was cancelled; nothing was kept.");
        if (m_Preview)
            return;
        if (m_Result)
            DrawResult();
        else if (m_Session.GetState() == State::Idle)
        {
            ImGui::Dummy({0, Px(40.0f)});
            ImGui::TextDisabled("Set up a run and press Run Benchmark, or pick a result from History.");
        }
    }

    void BenchmarkWindow::DrawRunning(const RunStatus& status)
    {
        const auto& p = EditorTheme::Colors();
        if (PropertyGrid::BeginCard("BenchmarkRun", ICON_FA_GAUGE_HIGH, "Running"))
        {
            PropertyGrid::Begin("BenchmarkRunGrid");
            PropertyGrid::BeginFree();
            const auto progress = status.Progress.value_or(Benchmark::Progress {});
            ImGui::PushFont(BoldFont(), 0.0f);
            ImGui::Text("Run %u of %u", status.Run, status.Runs);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextColored(p.TextDim, "%s", Benchmark::PhaseLabel(progress.Stage));
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%.0f%%", status.Fraction * 100.0);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, p.Accent);
            ImGui::ProgressBar(float(status.Fraction), {-FLT_MIN, Px(22.0f)}, overlay);
            ImGui::PopStyleColor();

            // The current phase of the current run.
            float       phase = 0;
            std::string detail;
            switch (progress.Stage)
            {
                case Benchmark::Phase::Loading:
                    detail = "Opening the project, building assets and compiling shaders";
                    break;
                case Benchmark::Phase::Warmup:
                case Benchmark::Phase::Measuring:
                    phase  = progress.Total ? float(progress.Frame) / float(progress.Total) : 0.0f;
                    detail = std::string(progress.Stage == Benchmark::Phase::Warmup ? "Warmup" : "Measured") +
                             " frame " + std::to_string(progress.Frame) + " / " + std::to_string(progress.Total);
                    break;
                case Benchmark::Phase::Writing:
                case Benchmark::Phase::Done:
                    phase  = 1;
                    detail = "Writing the report";
                    break;
            }
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Alpha(p.Accent, 0.55f));
            ImGui::ProgressBar(phase, {-FLT_MIN, Px(6.0f)}, "");
            ImGui::PopStyleColor();
            ImGui::TextUnformatted(detail.c_str());
            ImGui::TextColored(p.TextDim,
                               "Elapsed %s    %s",
                               Duration(status.ElapsedSeconds).c_str(),
                               status.RemainingSeconds ?
                                   ("about " + Duration(*status.RemainingSeconds) + " remaining").c_str() :
                                   "estimating the remaining time");
            ImGui::Dummy({0, Px(EditorTheme::Space1)});
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(p.TextOff,
                               "Warmup %u and %u measured frames per run. This window redraws only when the "
                               "progress changes or on input.",
                               status.Warmup,
                               status.Frames);
            ImGui::PopTextWrapPos();
            ImGui::Dummy({0, Px(EditorTheme::Space2)});
            ImGui::PushStyleColor(ImGuiCol_Button, Alpha(p.Danger, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.Danger);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Alpha(p.Danger, 0.9f));
            if (ImGui::Button(ICON_FA_STOP "  Cancel") && !m_Preview)
                m_Session.Cancel();
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Stops the running process and keeps nothing of this run");
            PropertyGrid::EndFree();
            PropertyGrid::End();
        }
        PropertyGrid::EndCard();
    }

    void BenchmarkWindow::DrawFailure()
    {
        const auto& p = EditorTheme::Colors();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(p.Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_Session.GetError().c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("Nothing of this run was kept.");
        if (PropertyGrid::BeginCard("BenchmarkLog", ICON_FA_TERMINAL, "Last log lines"))
        {
            PropertyGrid::Begin("BenchmarkLogGrid");
            PropertyGrid::BeginFree();
            if (m_Session.LogTail().empty())
                ImGui::TextDisabled("The process wrote nothing.");
            ImGui::PushTextWrapPos(0.0f);
            for (const auto& line : m_Session.LogTail())
            {
                const bool error =
                    line.find("[error]") != std::string::npos || line.find("failed") != std::string::npos;
                CellText(line, error ? p.Danger : p.TextDim);
            }
            ImGui::PopTextWrapPos();
            PropertyGrid::EndFree();
            PropertyGrid::End();
        }
        PropertyGrid::EndCard();
    }

    void BenchmarkWindow::DrawResult()
    {
        const auto& p      = EditorTheme::Colors();
        const auto& result = *m_Result;
        ImGui::PushFont(BoldFont(), Px(17.0f));
        ImGui::TextUnformatted(SceneLabel(result.Scene).c_str());
        ImGui::PopFont();
        ImGui::TextColored(p.TextDim,
                           "%ux%u    %s    %s    %zu run%s",
                           result.Width,
                           result.Height,
                           result.Gpu.c_str(),
                           Date(result.Created).c_str(),
                           result.Runs.size(),
                           result.Runs.size() == 1 ? "" : "s");
        const auto folder = m_ResultFile.parent_path();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder"))
            Reveal(folder);
        ImGui::SameLine();
        ImGui::BeginDisabled(IsRunning() || m_Settings.Baseline == m_ResultFile);
        if (ImGui::Button(ICON_FA_FLAG "  Set as Baseline"))
        {
            m_Settings.Baseline = m_ResultFile;
            SaveSettings();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(m_Settings.Baseline == m_ResultFile ? "This result is the baseline for the next runs" :
                                                                    "Compare the next runs with this result");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COPY "  Copy Summary Path"))
            ImGui::SetClipboardText(m_ResultFile.u8string().c_str());
        ImGui::Dummy({0, Px(EditorTheme::Space1)});

        ImGui::PushTextWrapPos(0.0f);
        if (!result.Stable)
        {
            std::string metrics;
            for (const auto& metric : result.UnstableMetrics)
            {
                if (!metrics.empty())
                    metrics += ", ";
                metrics += MetricLabel(metric);
            }
            ImGui::TextColored(p.Warning,
                               ICON_FA_TRIANGLE_EXCLAMATION "  Unstable run-to-run spread%s%s.",
                               metrics.empty() ? "" : " in ",
                               metrics.c_str());
        }
        if (result.CompileFrames)
            ImGui::TextColored(p.Warning,
                               ICON_FA_TRIANGLE_EXCLAMATION "  %llu compile hitch frames (worst CPU wall %s ms).",
                               static_cast<unsigned long long>(result.CompileFrames),
                               Ms(result.WorstCompileMs).c_str());
        else if (result.ShaderCompiles || result.PipelineCompiles)
            ImGui::TextColored(p.Warning,
                               ICON_FA_TRIANGLE_EXCLAMATION "  %llu shader-stage and %llu pipeline compiles.",
                               static_cast<unsigned long long>(result.ShaderCompiles),
                               static_cast<unsigned long long>(result.PipelineCompiles));
        if (!result.ComparisonError.empty())
            ImGui::TextColored(
                p.Warning, ICON_FA_TRIANGLE_EXCLAMATION "  No comparison: %s", result.ComparisonError.c_str());
        ImGui::PopTextWrapPos();

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Bottleneck");
        ImGui::SameLine();
        std::string bottleneck = result.Bottleneck.empty() ? "No data" : result.Bottleneck;
        if (result.Bottleneck == "GPU-bound" && result.TopPass)
            bottleneck += " \xC2\xB7 slowest pass " + *result.TopPass + " (" + Ms(result.TopPassMs) + " ms)";
        ImGui::TextColored(BottleneckColor(result.Bottleneck), "%s", bottleneck.c_str());
        ImGui::Dummy({0, Px(EditorTheme::Space1)});

        if (PropertyGrid::BeginCard("BenchmarkFrame", ICON_FA_CHART_LINE, "Frame"))
        {
            PropertyGrid::Begin("BenchmarkFrameGrid");
            PropertyGrid::BeginFree();
            if (ImGui::BeginTable("##frame",
                                  5,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("Median");
                ImGui::TableSetupColumn("P95");
                ImGui::TableSetupColumn("P99");
                ImGui::TableSetupColumn("Mean");
                ImGui::TableHeadersRow();
                const auto row = [&](const char* label, const std::optional<Benchmark::Distribution>& value) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(label);
                    if (!value)
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("Unavailable");
                        return;
                    }
                    for (const double number : {value->Median, value->P95, value->P99, value->Mean})
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(Ms(number).c_str());
                    }
                };
                row("CPU wall", result.CpuFrame);
                row("CPU busy", result.CpuBusy);
                row("GPU pass sum", result.GpuFrame);
                row("GPU frame", result.GpuWholeFrame);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Frames in flight %s",
                                result.FramesInFlight ? std::to_string(*result.FramesInFlight).c_str() : "Unavailable");
            PropertyGrid::EndFree();
            PropertyGrid::End();
            if (result.Runs.size() > 1 &&
                PropertyGrid::BeginGroup(result.GpuStabilityMetric.empty() ? "Runs (means)" : "Runs (medians)"))
            {
                for (size_t i = 0; i < result.Runs.size(); ++i)
                {
                    const auto& run = result.Runs[i];
                    const bool  legacy   = result.GpuStabilityMetric.empty();
                    const auto* gpuLabel = legacy ? "GPU" : MetricLabel(result.GpuStabilityMetric);
                    PropertyGrid::Text(("Run " + std::to_string(i + 1)).c_str(),
                                       "CPU " + Ms(legacy ? run.CpuMs : run.CpuMedianMs) + " ms    " + gpuLabel + " " +
                                           Ms(legacy ? run.GpuMs : run.GpuMedianMs) + " ms");
                }
                PropertyGrid::Text("Spread",
                                   "CPU " + Ms(result.CpuSpreadPercent, "%.1f") + "%    GPU " +
                                       Ms(result.GpuSpreadPercent, "%.1f") + "%",
                                   false);
                PropertyGrid::EndGroup();
            }
        }
        PropertyGrid::EndCard();

        if (PropertyGrid::BeginCard("BenchmarkCpu", ICON_FA_CHART_LINE, "CPU"))
        {
            PropertyGrid::Begin("BenchmarkCpuGrid");
            PropertyGrid::BeginFree();
            if (ImGui::BeginTable("##cpu",
                                  3,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 2.f);
                ImGui::TableSetupColumn("Median ms");
                ImGui::TableSetupColumn("P95 ms");
                ImGui::TableHeadersRow();
                for (size_t stage = 0; stage < result.CpuBreakdown.size(); ++stage)
                {
                    const auto& value = result.CpuBreakdown[stage];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Benchmark::CpuMetrics[stage].Label);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(value ? Ms(value->Median).c_str() : "Unavailable");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(value ? Ms(value->P95).c_str() : "-");
                }
                ImGui::EndTable();
            }
            PropertyGrid::EndFree();
            PropertyGrid::End();
        }
        PropertyGrid::EndCard();

        if (!result.Hitches.empty())
        {
            if (PropertyGrid::BeginCard("BenchmarkHitches", ICON_FA_TRIANGLE_EXCLAMATION, "Hitches"))
            {
                PropertyGrid::Begin("BenchmarkHitchGrid");
                PropertyGrid::BeginFree();
                if (ImGui::BeginTable("##hitches",
                                      4,
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Frame");
                    ImGui::TableSetupColumn("CPU ms");
                    ImGui::TableSetupColumn("GPU ms");
                    ImGui::TableSetupColumn("Reason / compiled names", ImGuiTableColumnFlags_WidthStretch, 3.f);
                    ImGui::TableHeadersRow();
                    for (const auto& hitch : result.Hitches)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (hitch.Run)
                            ImGui::Text("Run %u / %llu", hitch.Run, static_cast<unsigned long long>(hitch.Frame));
                        else
                            ImGui::Text("%llu", static_cast<unsigned long long>(hitch.Frame));
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(Ms(hitch.CpuMs).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(Ms(hitch.GpuMs).c_str());
                        ImGui::TableNextColumn();
                        if (hitch.Reason == "compile")
                            ImGui::Text("%llu pipelines, %llu shader stages",
                                        static_cast<unsigned long long>(hitch.PipelineCompiles),
                                        static_cast<unsigned long long>(hitch.ShaderCompiles));
                        else
                            ImGui::TextUnformatted("Spike");
                        for (const auto& name : hitch.Pipelines)
                            ImGui::TextWrapped("%s", name.c_str());
                        for (const auto& name : hitch.Shaders)
                            ImGui::TextWrapped("%s", name.c_str());
                        if (hitch.OmittedNames)
                            ImGui::TextDisabled("%llu names omitted",
                                                static_cast<unsigned long long>(hitch.OmittedNames));
                    }
                    ImGui::EndTable();
                }
                PropertyGrid::EndFree();
                PropertyGrid::End();
            }
            PropertyGrid::EndCard();
        }

        if (PropertyGrid::BeginCard("BenchmarkPasses", ICON_FA_LAYER_GROUP, "Passes"))
        {
            PropertyGrid::Begin("BenchmarkPassesGrid");
            PropertyGrid::BeginFree();
            DrawResultPasses();
            PropertyGrid::EndFree();
            PropertyGrid::End();
        }
        PropertyGrid::EndCard();

        if (result.Compared)
        {
            if (PropertyGrid::BeginCard("BenchmarkComparison", ICON_FA_FLAG, "Comparison with the baseline"))
            {
                PropertyGrid::Begin("BenchmarkComparisonGrid");
                PropertyGrid::BeginFree();
                DrawComparison();
                PropertyGrid::EndFree();
                PropertyGrid::End();
            }
            PropertyGrid::EndCard();
        }
    }

    void BenchmarkWindow::DrawResultPasses()
    {
        const auto& p      = EditorTheme::Colors();
        const auto& passes = m_Result->Passes;
        if (passes.empty())
        {
            ImGui::TextDisabled("No passes were recorded.");
            return;
        }
        if (!ImGui::BeginTable("##passes",
                               3,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable |
                                   ImGuiTableFlags_SizingStretchProp))
            return;
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 3.0f, ImGuiID(PassSort::Name));
        ImGui::TableSetupColumn(
            "CPU record ms", ImGuiTableColumnFlags_PreferSortDescending, 1.0f, ImGuiID(PassSort::Cpu));
        ImGui::TableSetupColumn("GPU ms",
                                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
                                1.0f,
                                ImGuiID(PassSort::Gpu));
        ImGui::TableHeadersRow();
        if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0)
        {
            m_Sort      = PassSort(specs->Specs[0].ColumnUserID);
            m_Ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
        }
        std::vector<size_t> order(passes.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        const auto sort      = m_Sort;
        const bool ascending = m_Ascending;
        std::stable_sort(order.begin(), order.end(), [&passes, sort, ascending](size_t a, size_t b) {
            const auto& x = passes[a];
            const auto& y = passes[b];
            if (sort == PassSort::Name)
                return ascending ? x.Name < y.Name : y.Name < x.Name;
            const double left  = (sort == PassSort::Cpu ? x.CpuMs : x.GpuMs).value_or(-1.0);
            const double right = (sort == PassSort::Cpu ? y.CpuMs : y.GpuMs).value_or(-1.0);
            return ascending ? left < right : right < left;
        });
        // The three slowest GPU passes stand out, as in the Profiler panel.
        std::vector<size_t> slowest(passes.size());
        for (size_t i = 0; i < slowest.size(); ++i)
            slowest[i] = i;
        std::stable_sort(slowest.begin(), slowest.end(), [&passes](size_t a, size_t b) {
            return passes[a].GpuMs.value_or(-1.0) > passes[b].GpuMs.value_or(-1.0);
        });
        for (const size_t i : order)
        {
            const auto& pass = passes[i];
            int         rank = -1;
            for (int r = 0; r < 3 && r < int(slowest.size()); ++r)
                if (slowest[r] == i && pass.GpuMs)
                    rank = r;
            ImGui::TableNextRow();
            if (rank >= 0)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                       ImGui::GetColorU32(Alpha(p.Warning, rank == 0 ? 0.24f : 0.13f)));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pass.Name.c_str());
            ImGui::TableNextColumn();
            CellText(Ms(pass.CpuMs), p.TextDim);
            ImGui::TableNextColumn();
            if (rank >= 0)
                ImGui::TextColored(p.Warning, "%s  #%d", Ms(pass.GpuMs).c_str(), rank + 1);
            else
                ImGui::TextUnformatted(Ms(pass.GpuMs).c_str());
        }
        ImGui::EndTable();
    }

    void BenchmarkWindow::DrawComparison()
    {
        const auto& p           = EditorTheme::Colors();
        const auto& result      = *m_Result;
        const auto& comparison  = *result.Compared;
        size_t      regressions = 0, improvements = 0;
        for (const auto& row : comparison.Rows)
        {
            regressions += row.Status == "regression";
            improvements += row.Status == "improvement";
        }
        ImGui::TextColored(p.TextDim,
                           "Baseline %s    threshold %.1f%% and %.2f ms",
                           fs::u8path(result.BaselineFile).parent_path().filename().u8string().c_str(),
                           result.ThresholdPercent,
                           Benchmark::AbsoluteFloorMs);
        if (comparison.Regressed)
            ImGui::TextColored(
                p.Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %zu regression%s", regressions, regressions == 1 ? "" : "s");
        else
            ImGui::TextColored(p.Success, ICON_FA_CIRCLE_CHECK "  No regressions");
        if (improvements)
        {
            ImGui::SameLine(0, Px(EditorTheme::Space4));
            ImGui::TextColored(p.Success, "%zu improvement%s", improvements, improvements == 1 ? "" : "s");
        }
        if (!comparison.Mismatches.empty())
        {
            std::string names;
            for (const auto& mismatch : comparison.Mismatches)
                names += (names.empty() ? "" : ", ") + mismatch;
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(p.Warning,
                               ICON_FA_TRIANGLE_EXCLAMATION "  Not like for like: %s differ%s.",
                               names.c_str(),
                               comparison.Mismatches.size() == 1 ? "s" : "");
            ImGui::PopTextWrapPos();
        }
        if (!ImGui::BeginTable("##comparison",
                               7,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_SizingStretchProp))
            return;
        ImGui::TableSetupColumn("Frame / pass", ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Baseline");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Change ms");
        ImGui::TableSetupColumn("Change %");
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.7f);
        ImGui::TableHeadersRow();
        // Regressions first, then improvements, then the rest, each in report order.
        const auto rank = [](const std::string& status) {
            return status == "regression" ? 0 : status == "improvement" ? 1 : 2;
        };
        std::vector<size_t> order(comparison.Rows.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return rank(comparison.Rows[a].Status) < rank(comparison.Rows[b].Status);
        });
        for (const size_t i : order)
        {
            const auto&  row   = comparison.Rows[i];
            const ImVec4 color = row.Status == "regression"       ? p.Danger :
                                 row.Status == "improvement"      ? p.Success :
                                 row.Status == "within_threshold" ? p.Text :
                                                                    p.TextDim;
            ImGui::TableNextRow();
            if (row.Status == "regression")
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(Alpha(p.Danger, 0.16f)));
            ImGui::TableNextColumn();
            CellText(row.Name, color);
            ImGui::TableNextColumn();
            CellText(MetricLabel(row.Metric), p.TextDim);
            ImGui::TableNextColumn();
            CellText(Ms(row.BaselineMs), color);
            ImGui::TableNextColumn();
            CellText(Ms(row.CurrentMs), color);
            ImGui::TableNextColumn();
            CellText(Ms(row.DeltaMs, "%+.3f"), color);
            ImGui::TableNextColumn();
            CellText(Ms(row.DeltaPercent, "%+.1f"), color);
            ImGui::TableNextColumn();
            const char* status = row.Status == "regression"       ? "Regression" :
                                 row.Status == "improvement"      ? "Improvement" :
                                 row.Status == "within_threshold" ? "Within threshold" :
                                 row.Status == "new"              ? "New" :
                                 row.Status == "missing"          ? "Missing" :
                                                                    "Unavailable";
            CellText(status, color);
        }
        ImGui::EndTable();
    }

    void BenchmarkWindow::DrawDialogs()
    {
        if (auto chosen = m_Browser.Draw())
            m_Settings.Baseline = *chosen;
        if (m_AskDelete)
        {
            ImGui::OpenPopup("Delete Result?");
            m_AskDelete = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal(
                "Delete Result?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::Text("Delete %s and its runs?", m_PendingDelete.filename().u8string().c_str());
            ImGui::TextDisabled("%s", m_PendingDelete.u8string().c_str());
            ImGui::Dummy({0, Px(6.0f)});
            ImGui::PushStyleColor(ImGuiCol_Button, Alpha(EditorTheme::Colors().Danger, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Colors().Danger);
            const bool confirmed = ImGui::Button(ICON_FA_TRASH_CAN "  Delete");
            ImGui::PopStyleColor(2);
            if (confirmed)
            {
                // Only a folder of this project's Benchmarks directory is ever removed.
                const auto      root = Benchmark::BenchmarksDirectory(m_ProjectRoot);
                std::error_code error;
                if (m_PendingDelete.parent_path() == root && !m_PendingDelete.filename().empty())
                {
                    const auto summary = m_PendingDelete / "summary.json";
                    if (m_ResultFile == summary)
                    {
                        m_Result.reset();
                        m_ResultFile.clear();
                    }
                    if (m_Settings.Baseline == summary)
                    {
                        m_Settings.Baseline.clear();
                        SaveSettings();
                    }
                    fs::remove_all(m_PendingDelete, error);
                    if (error)
                        m_Error = "Cannot delete " + m_PendingDelete.u8string() + ": " + error.message();
                }
                RefreshHistory();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
} // namespace Kans
