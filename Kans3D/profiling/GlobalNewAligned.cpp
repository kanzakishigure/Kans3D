#include "GlobalAllocation.h"

#ifdef _WIN32
#include <malloc.h>
#endif

void* operator new(size_t size, std::align_val_t alignment)
{
    return Kans::Profiling::ReplaceNew(size, [alignment](size_t bytes) -> void* {
#ifdef _WIN32
        return _aligned_malloc(bytes, size_t(alignment));
#else
        void* ptr = nullptr;
        return posix_memalign(&ptr, size_t(alignment), bytes) == 0 ? ptr : nullptr;
#endif
    });
}
void* operator new[](size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }

void operator delete(void* ptr, std::align_val_t) noexcept
{
    Kans::Profiling::ReportDelete(ptr);
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
void operator delete[](void* ptr, std::align_val_t alignment) noexcept { ::operator delete(ptr, alignment); }
void operator delete(void* ptr, size_t, std::align_val_t alignment) noexcept { ::operator delete(ptr, alignment); }
void operator delete[](void* ptr, size_t, std::align_val_t alignment) noexcept { ::operator delete(ptr, alignment); }
