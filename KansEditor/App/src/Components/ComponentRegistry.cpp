#include "ComponentRegistry.h"

namespace Kans
{
    void EnsureBuiltinComponents()
    {
        static const bool initialized = [] {
            RegisterBuiltinComponents(ComponentRegistry::Runtime());
            return true;
        }();
        (void)initialized;
    }

} // namespace Kans
