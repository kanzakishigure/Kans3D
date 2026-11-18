#pragma once
#include "KansEditorCore/Plugin/PluginSettings.h"
namespace Kans
{
    class PluginsPanel
    {
    public:
        void Bind(const std::filesystem::path& file);
        void Draw();

    private:
        PluginSettings m_Settings;
        std::string    m_Status;
    };
} // namespace Kans
