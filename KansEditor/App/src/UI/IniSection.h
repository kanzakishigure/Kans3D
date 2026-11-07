#pragma once
#include <functional>
#include <string>
#include <imgui.h>

namespace Kans
{
    // A "[Type][Settings]" section of imgui.ini owned by an editor object.
    //
    // The ImGui context outlives the editor layer and saves imgui.ini once more
    // when it is destroyed, so Detach() captures the section's current text and
    // ImGui keeps writing that text after the owner is gone.
    class IniSection
    {
    public:
        using Reader = std::function<void(const char* key, const char* value)>;
        using Writer = std::function<void(ImGuiTextBuffer& buffer)>; // Appends "key=value\n" lines.

        // Call before the first frame (imgui.ini is read on the first NewFrame).
        void Install(const char* typeName, Reader read, Writer write);
        void Detach();
        ~IniSection() { Detach(); }

    private:
        std::string m_Type;
    };
} // namespace Kans
