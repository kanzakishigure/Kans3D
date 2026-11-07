# ============================================================
# Kans3D — StaticLib (C++17, PCH: kspch.h)
# ============================================================
set(KANS3D_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- Sources ----
file(GLOB_RECURSE KANS3D_HEADERS CONFIGURE_DEPENDS ${KANS3D_SRC}/*.h)
file(GLOB_RECURSE KANS3D_SOURCES CONFIGURE_DEPENDS ${KANS3D_SRC}/*.cpp)

add_library(Kans3D STATIC
    ${KANS3D_HEADERS}
    ${KANS3D_SOURCES}
)

# ---- Precompiled header ----
target_precompile_headers(Kans3D PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${KANS3D_SRC}/kspch.h>
)

# ---- Compile definitions ----
if(WIN32)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # /bigobj: large template-heavy units (serializers, VFX compiler, big test files) exceed
        # the default COFF section limit (C1128). PUBLIC so the editor and tests inherit it.
        target_compile_options(Kans3D PUBLIC /Zc:preprocessor /utf-8 /bigobj)
    endif()
    target_compile_definitions(Kans3D PUBLIC
        PLATFORM_WINDOWS
        NOMINMAX # Keep <Windows.h> min/max macros away from std::min/std::max.
        _CRT_SECURE_NO_WARNINGS)
elseif(LINUX)
    target_compile_definitions(Kans3D PUBLIC
        PLATFORM_LINUX)
endif()

# ---- Include directories (engine only; vendor headers come via targets) ----
target_include_directories(Kans3D PUBLIC
    ${KANS3D_SRC}
)

# ---- Link vendor dependencies (all defined in vendor/CMakeLists.txt) ----
target_link_libraries(Kans3D PUBLIC
    glfw
    ImGuiBackends
    spdlog
    yaml-cpp
    stb_image
    glm
    EnTT
    assimp
    VulkanMemoryAllocator
    harfbuzz
)

target_link_libraries(Kans3D PRIVATE miniaudio msdfgen-core)

# Vulkan SDK (shaderc, spirv-cross, vulkan loader)
if(VULKAN_SDK_DIR)
    target_link_libraries(Kans3D PUBLIC vulkan_sdk)
endif()

# ---- Output & properties ----
kans_set_output_dirs(Kans3D)
set_target_properties(Kans3D PROPERTIES
    FOLDER "Core"
    CXX_STANDARD 17
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

if(DOTNET)
    target_compile_definitions(Kans3D PRIVATE KANS_HAS_DOTNET=1 KANS_DOTNET_EXECUTABLE="${DOTNET}")
    target_link_libraries(Kans3D PRIVATE kans_nethost)
else()
    target_compile_definitions(Kans3D PRIVATE KANS_HAS_DOTNET=0)
    target_link_libraries(Kans3D PRIVATE ${CMAKE_DL_LIBS})
endif()

# Embed the same renderer-agnostic include used by resource shaders into the
# asset shader compiler; packaged games need no source-tree shader paths.
set(KANS_MSDF_INCLUDE "${PROJECT_SOURCE_DIR}/KansEditor/Resources/Shaders/Include/MSDF.glsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KANS_MSDF_INCLUDE}")
file(READ "${KANS_MSDF_INCLUDE}" KANS_MSDF_GLSL)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/KansMSDFInclude.h" "R\"KANSMSDF(${KANS_MSDF_GLSL})KANSMSDF\"")
target_include_directories(Kans3D PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# PhysX stays private: only Physics/PhysX implementation files include its SDK.
target_link_libraries(Kans3D PRIVATE PhysX PhysXCooking PhysXExtensions
    PhysXCharacterKinematic PhysXVehicle)
file(GLOB_RECURSE KANS_PHYSX_IMPLEMENTATION CONFIGURE_DEPENDS
    "${KANS3D_SRC}/Kans3D/Physics/PhysX/*.cpp")
kans_use_physx_sdk(${KANS_PHYSX_IMPLEMENTATION})
