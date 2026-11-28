#pragma once

#include <algorithm>
#include <array>
#include <string_view>
#include "Kans3D/Core/Debug/Profiler.h"
#include "Kans3D/RHI/RHI.h"

namespace Kans
{
    struct ApplicationFrameProfile
    {
        uint64_t                            Frame           = 0;
        double                              CpuMilliseconds = 0;
        uint32_t                            FixedSteps      = 0;
        uint64_t                            ShaderCompiles  = 0;
        RenderHardware::DeviceFrameCounters DeviceWork;
    };

    // Keep the ordering here shared by the windowed loop and headless tests.
    template<class Acquire, class Update, class UI, class Present, class Audio, class Events, class Deferred>
    void RunApplicationFrame(bool     visible,
                             Acquire  acquire,
                             Update   update,
                             UI       ui,
                             Present  present,
                             Audio    audio,
                             Events   events,
                             Deferred deferred)
    {
        if (visible)
        {
            {
                KANS_PROFILE_SCOPE("Frame slot wait and setup");
                acquire();
            }
            {
                KANS_PROFILE_SCOPE("Layer updates");
                update();
            }
            {
                KANS_PROFILE_SCOPE("ImGui layer updates");
                ui();
            }
            {
                KANS_PROFILE_SCOPE("Application present");
                present();
            }
        }
        {
            KANS_PROFILE_SCOPE("Audio main-thread update");
            audio();
        }
        {
            KANS_PROFILE_SCOPE("Poll window events");
            events();
        }
        {
            KANS_PROFILE_SCOPE("Post-frame tasks");
            deferred();
        }
    }

#ifndef KS_DIST
    inline thread_local uint32_t ProfileFixedSteps     = 0;
    inline thread_local uint64_t ProfileShaderCompiles = 0;

    struct ProfileCompileNames
    {
        struct Entry
        {
            std::array<char, 256> Name;
            uint16_t              Length;
            bool                  Pipeline;
        };
        std::array<Entry, 128> Entries;
        size_t                 Count = 0, Omitted = 0;

        void Record(std::string_view name, bool pipeline)
        {
            if (Count == Entries.size() || name.size() > Entries.front().Name.size())
            {
                ++Omitted;
                return;
            }
            auto& entry  = Entries[Count++];
            entry.Length = uint16_t(name.size());
            if (entry.Length)
                std::copy_n(name.data(), entry.Length, entry.Name.data());
            entry.Pipeline = pipeline;
        }
    };
    inline thread_local ProfileCompileNames* ProfileCompileCapture = nullptr;

    inline void RecordProfileCompile(std::string_view name, bool pipeline)
    {
        if (ProfileCompileCapture)
            ProfileCompileCapture->Record(name, pipeline);
    }
#endif

    template<class Compile>
    auto ProfileShaderCompile([[maybe_unused]] std::string_view name, Compile&& compile)
    {
        KANS_PROFILE_SCOPE("Compile GLSL shader");
        KANS_PROFILE_TEXT(name.data(), name.size());
        KANS_FRAME_STAT(++ProfileShaderCompiles; RecordProfileCompile(name, false);)
        return compile();
    }
} // namespace Kans
