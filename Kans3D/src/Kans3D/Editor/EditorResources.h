#pragma once

#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Renderer/Resource/Texture.h"

namespace Kans
{
    class EditorResources
    {
    public:
        inline static Ref<Texture2D> FbxFileIcon;
        inline static Ref<Texture2D> FolderIcon;
        inline static Ref<Texture2D> FolderOpenIcon;
        inline static Ref<Texture2D> ObjFileIcon;
        inline static Ref<Texture2D> GltfFileIcon;
        inline static Ref<Texture2D> GlbFileIcon;
        inline static Ref<Texture2D> BlendFileIcon;
        inline static Ref<Texture2D> DaeFileIcon;
        inline static Ref<Texture2D> MeshFileIcon;
        inline static Ref<Texture2D> ShaderFileIcon;
        inline static Ref<Texture2D> TextureFileIcon;
        inline static Ref<Texture2D> ModelFileIcon;
        inline static Ref<Texture2D> SceneFileIcon;
        inline static Ref<Texture2D> MaterialFileIcon;
        inline static Ref<Texture2D> ScriptFileIcon;
        inline static Ref<Texture2D> AudioFileIcon;
        inline static Ref<Texture2D> FontFileIcon;
        inline static Ref<Texture2D> UIFileIcon;
        inline static Ref<Texture2D> TimelineFileIcon;
        inline static Ref<Texture2D> ActionFileIcon;
        inline static Ref<Texture2D> ExpressionFileIcon;
        inline static Ref<Texture2D> FileIcon;

        inline static Ref<Texture2D> BorderShadow;

        static void           Init();
        static void           ShutDown();
        static Ref<Texture2D> GetFileIcon(const std::filesystem::path& path);

    private:
        static Ref<Texture2D> LoadTexture(const std::string& relativePath);
    };
} // namespace Kans
