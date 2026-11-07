#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "Kans3D/Renderer/Resource/Texture.h"

namespace Kans::Browser
{
    namespace fs = std::filesystem;

    // Image thumbnails decoded on a worker thread and uploaded on the render
    // thread. Recently used thumbnails are kept up to a fixed count.
    class ThumbnailCache
    {
    public:
        ThumbnailCache();
        ~ThumbnailCache();
        ThumbnailCache(const ThumbnailCache&)            = delete;
        ThumbnailCache& operator=(const ThumbnailCache&) = delete;

        static bool Supports(const fs::path& file);

        // The thumbnail (longest side at most `size` pixels) when ready; queues
        // it otherwise. A changed `modified` time re-decodes the file. Textures
        // store the bottom row first, like other editor icons.
        Ref<Texture2D> Get(const fs::path& file, fs::file_time_type modified, uint32_t size);
        // Uploads finished thumbnails; call once per frame on the render thread.
        void Update();

    private:
        struct Request
        {
            fs::path           File;
            fs::file_time_type Modified;
            uint32_t           Size;
            std::string        Key;
        };
        struct Decoded
        {
            std::string            Key;
            fs::file_time_type     Modified;
            uint32_t               Width = 0, Height = 0;
            std::vector<std::byte> Pixels; // RGBA8, top row first; empty on failure.
        };
        struct Entry
        {
            fs::file_time_type             Modified;
            Ref<Texture2D>                 Texture;
            bool                           Pending = false;
            bool                           Failed  = false;
            std::list<std::string>::iterator Use;
        };
        void Work();
        static Decoded Decode(const Request& request);

        std::unordered_map<std::string, Entry> m_Entries;
        std::list<std::string>                 m_Recent; // Most recently used first.

        std::mutex              m_Mutex;
        std::condition_variable m_Wake;
        std::deque<Request>     m_Requests; // Newest first: visible items load first.
        std::vector<Decoded>    m_Finished;
        bool                    m_Stop = false;
        std::thread             m_Worker;
    };
} // namespace Kans::Browser
