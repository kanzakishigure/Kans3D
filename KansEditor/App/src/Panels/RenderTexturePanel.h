#pragma once
#include <filesystem>
#include "Kans3D/Asset/Runtime/RenderTexture.h"
#include "Kans3D/Renderer/Resource/RenderAssetCache.h"
namespace Kans
{
    class EditorAssetImportService;
    class RenderTexturePanel
    {
    public:
        void Bind(EditorAssetImportService* service) { m_Service = service; }
        void SetResources(std::shared_ptr<RenderAssetCache> cache) { m_Cache = std::move(cache); }
        bool Open(const std::filesystem::path&);
        void Draw();
        void Save();
        bool IsDirty() const { return m_Dirty; }
        bool HasFocus() const { return m_Focused; }
        const std::filesystem::path&      GetFile() const { return m_File; }
        const RenderTextureSpecification& GetValues() const { return m_Values; }

    private:
        EditorAssetImportService*         m_Service = nullptr;
        std::shared_ptr<RenderAssetCache> m_Cache;
        std::filesystem::path             m_File;
        RenderTextureSpecification        m_Values, m_Saved;
        std::string                       m_Status;
        bool                              m_Dirty = false, m_Focused = false;
    };
} // namespace Kans
