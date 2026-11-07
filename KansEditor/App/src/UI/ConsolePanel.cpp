#include "ConsolePanel.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <imgui.h>
#include "Kans3D/Core/Log/Log.h"
#include "EditorIcons.h"

namespace Kans
{
    namespace
    {
        ConsoleSink::Severity SeverityOf(spdlog::level::level_enum level)
        {
            switch (level)
            {
                case spdlog::level::trace:
                case spdlog::level::debug:
                    return ConsoleSink::Severity::Trace;
                case spdlog::level::info:
                    return ConsoleSink::Severity::Info;
                case spdlog::level::warn:
                    return ConsoleSink::Severity::Warning;
                default:
                    return ConsoleSink::Severity::Error;
            }
        }

        ImVec4 ColorOf(ConsoleSink::Severity severity)
        {
            switch (severity)
            {
                case ConsoleSink::Severity::Trace:
                    return {0.55f, 0.55f, 0.55f, 1.0f};
                case ConsoleSink::Severity::Info:
                    return {0.85f, 0.85f, 0.85f, 1.0f};
                case ConsoleSink::Severity::Warning:
                    return {0.95f, 0.75f, 0.30f, 1.0f};
                default:
                    return {0.95f, 0.38f, 0.35f, 1.0f};
            }
        }

        const char* IconOf(ConsoleSink::Severity severity)
        {
            switch (severity)
            {
                case ConsoleSink::Severity::Trace:
                    return ICON_FA_BUG;
                case ConsoleSink::Severity::Info:
                    return ICON_FA_CIRCLE_INFO;
                case ConsoleSink::Severity::Warning:
                    return ICON_FA_TRIANGLE_EXCLAMATION;
                default:
                    return ICON_FA_CIRCLE_XMARK;
            }
        }

        bool ContainsIgnoreCase(const std::string& text, const char* needle)
        {
            if (!*needle)
                return true;
            const auto end = text.end();
            return std::search(text.begin(), end, needle, needle + std::strlen(needle), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                   }) != end;
        }
    } // namespace

    void ConsoleSink::sink_it_(const spdlog::details::log_msg& message)
    {
        const std::time_t time = std::chrono::system_clock::to_time_t(message.time);
        std::tm           local {};
#ifdef _WIN32
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char stamp[16];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
        const auto severity = SeverityOf(message.level);
        m_Messages.push_back({severity,
                              stamp,
                              std::string(message.logger_name.begin(), message.logger_name.end()),
                              std::string(message.payload.begin(), message.payload.end())});
        ++m_Counts[size_t(severity)];
        if (m_Messages.size() > Capacity)
            m_Messages.pop_front();
    }

    void ConsoleSink::Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        m_Messages.clear();
        std::fill(std::begin(m_Counts), std::end(m_Counts), size_t(0));
    }

    void ConsolePanel::Attach()
    {
        if (m_Sink)
            return;
        m_Sink = std::make_shared<ConsoleSink>();
        for (auto* logger : {&Log::GetCoreLogger(), &Log::GetClientLogger()})
            if (*logger)
                (*logger)->sinks().push_back(m_Sink);
    }

    void ConsolePanel::Detach()
    {
        if (!m_Sink)
            return;
        for (auto* logger : {&Log::GetCoreLogger(), &Log::GetClientLogger()})
            if (*logger)
            {
                auto& sinks = (*logger)->sinks();
                sinks.erase(std::remove(sinks.begin(), sinks.end(), m_Sink), sinks.end());
            }
        m_Sink.reset();
    }

    size_t ConsolePanel::ErrorCount()
    {
        size_t errors = 0;
        if (m_Sink)
            m_Sink->Read([&](const auto&, const size_t* counts) { errors = counts[size_t(ConsoleSink::Severity::Error)]; });
        return errors;
    }

    void ConsolePanel::Draw()
    {
        if (!m_Sink)
        {
            ImGui::TextDisabled("Logging is not connected.");
            return;
        }
        // Toolbar: clear, severity filters with counts, search, auto-scroll.
        if (ImGui::Button(ICON_FA_BROOM " Clear"))
            m_Sink->Clear();
        size_t counts[4] = {};
        m_Sink->Read([&](const auto&, const size_t* values) { std::copy(values, values + 4, counts); });
        const char* names[] = {"Trace", "Info", "Warnings", "Errors"};
        for (int i = 0; i < 4; ++i)
        {
            ImGui::SameLine();
            const auto severity = ConsoleSink::Severity(i);
            ImGui::PushStyleColor(ImGuiCol_Text, m_Show[i] ? ColorOf(severity) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
            const std::string label =
                std::string(IconOf(severity)) + " " + std::to_string(counts[i]) + "###filter" + std::to_string(i);
            if (ImGui::Button(label.c_str()))
                m_Show[i] = !m_Show[i];
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s %s", m_Show[i] ? "Hide" : "Show", names[i]);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 210.0f));
        ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS " Search", m_Filter, sizeof(m_Filter));
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_AutoScroll);
        ImGui::SameLine();
        const bool copy = ImGui::Button(ICON_FA_COPY);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy the visible messages");
        ImGui::Separator();

        ImGui::BeginChild("ConsoleMessages", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        // Follow new messages only while the view is already at the bottom.
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        size_t     total    = 0;
        m_Sink->Read([&](const std::deque<ConsoleSink::Message>& messages, const size_t* received) {
            std::vector<const ConsoleSink::Message*> visible;
            visible.reserve(messages.size());
            for (const auto& message : messages)
                if (m_Show[size_t(message.Level)] &&
                    (ContainsIgnoreCase(message.Text, m_Filter) || ContainsIgnoreCase(message.Logger, m_Filter)))
                    visible.push_back(&message);
            // Counts keep growing when old messages are dropped.
            total = received[0] + received[1] + received[2] + received[3];
            if (copy)
            {
                std::string text;
                for (const auto* message : visible)
                    text += message->Time + " [" + message->Logger + "] " + message->Text + "\n";
                ImGui::SetClipboardText(text.c_str());
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& message = *visible[i];
                    ImGui::PushStyleColor(ImGuiCol_Text, ColorOf(message.Level));
                    ImGui::TextUnformatted(IconOf(message.Level));
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", message.Time.c_str());
                    ImGui::SameLine();
                    ImGui::TextUnformatted(message.Text.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s - right-click to copy", message.Logger.c_str());
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                        ImGui::SetClipboardText(message.Text.c_str());
                }
        });
        if (m_AutoScroll && atBottom && total != m_LastCount)
            ImGui::SetScrollHereY(1.0f);
        m_LastCount = total;
        ImGui::EndChild();
    }
} // namespace Kans
