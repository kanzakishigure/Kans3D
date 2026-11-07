#include "IniSection.h"
#include <cstring>
#include <unordered_map>
#include <imgui_internal.h>

namespace Kans
{
    namespace
    {
        // Lives as long as the program, so handlers stay valid after their owners.
        struct State
        {
            IniSection::Reader Read;
            IniSection::Writer Write;
            std::string        Captured;
            bool               Live = false;
        };
        std::unordered_map<std::string, State>& States()
        {
            static std::unordered_map<std::string, State> states;
            return states;
        }
    } // namespace

    void IniSection::Install(const char* typeName, Reader read, Writer write)
    {
        m_Type      = typeName;
        auto& state = States()[m_Type];
        state.Read  = std::move(read);
        state.Write = std::move(write);
        state.Live  = true;
        auto& handlers = ImGui::GetCurrentContext()->SettingsHandlers;
        for (const auto& handler : handlers)
            if (handler.TypeHash == ImHashStr(typeName))
                return; // Re-installed by a new owner: the handler already points at the state.

        ImGuiSettingsHandler handler;
        handler.TypeName   = States().find(m_Type)->first.c_str();
        handler.TypeHash   = ImHashStr(typeName);
        handler.UserData   = &state;
        handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
            return std::strcmp(name, "Settings") == 0 ? reinterpret_cast<void*>(1) : nullptr;
        };
        handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, void*, const char* line) {
            auto*       state = static_cast<State*>(handler->UserData);
            const char* split = std::strchr(line, '=');
            if (!state->Live || !split || !state->Read)
                return;
            const std::string key(line, split);
            state->Read(key.c_str(), split + 1);
        };
        handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer) {
            auto* state = static_cast<State*>(handler->UserData);
            buffer->appendf("[%s][Settings]\n", handler->TypeName);
            if (state->Live && state->Write)
                state->Write(*buffer);
            else
                buffer->append(state->Captured.c_str());
            buffer->append("\n");
        };
        handlers.push_back(handler);
    }

    void IniSection::Detach()
    {
        if (m_Type.empty())
            return;
        auto& state = States()[m_Type];
        if (state.Live && state.Write)
        {
            ImGuiTextBuffer buffer;
            state.Write(buffer);
            state.Captured = buffer.c_str();
        }
        state.Live  = false;
        state.Read  = nullptr;
        state.Write = nullptr;
        m_Type.clear();
    }
} // namespace Kans
