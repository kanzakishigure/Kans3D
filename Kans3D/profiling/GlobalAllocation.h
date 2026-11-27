#pragma once

#include "Kans3D/Core/Debug/Profiler.h"

#include <cstdlib>
#include <new>

namespace Kans::Profiling
{
    inline constexpr char CppPool[] = "C++";

    template<class Allocate>
    void* ReplaceNew(size_t size, Allocate allocate)
    {
        const size_t bytes = size ? size : 1;
        for (;;)
        {
            if (void* ptr = allocate(bytes))
            {
                if (!ProfilerMemoryPaused)
                    KANS_PROFILE_ALLOC(ptr, size, CppPool);
                return ptr;
            }
            const auto handler = std::get_new_handler();
            if (!handler)
                throw std::bad_alloc();
            handler();
        }
    }

    inline void ReportDelete(void* ptr) noexcept
    {
        if (ptr && !ProfilerMemoryPaused)
            KANS_PROFILE_FREE(ptr, CppPool);
    }
} // namespace Kans::Profiling
