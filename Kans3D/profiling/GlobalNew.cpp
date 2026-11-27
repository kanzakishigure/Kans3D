#include "GlobalAllocation.h"

void* operator new(size_t size) { return Kans::Profiling::ReplaceNew(size, std::malloc); }
void* operator new[](size_t size) { return ::operator new(size); }

void operator delete(void* ptr) noexcept
{
    Kans::Profiling::ReportDelete(ptr);
    std::free(ptr);
}
void operator delete[](void* ptr) noexcept { ::operator delete(ptr); }
void operator delete(void* ptr, size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, size_t) noexcept { ::operator delete(ptr); }
