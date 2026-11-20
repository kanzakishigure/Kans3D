#pragma once
#include <filesystem>
#include "Kans3D/Asset/Runtime/MaterialParameterCollection.h"

namespace Kans
{
    class EditorAssetImportService;

    class MaterialParameterCollectionPanel
    {
    public:
        void Bind(EditorAssetImportService* service);
        bool Open(const std::filesystem::path& file);
        bool Save();
        void Draw();
        bool HasFocus() const { return m_Focused; }

    private:
        EditorAssetImportService*   m_Service = nullptr;
        std::filesystem::path       m_File;
        MaterialParameterCollection m_Data, m_Saved;
        std::string                 m_Status;
        bool                        m_Dirty = false, m_Focused = false;
    };
} // namespace Kans
