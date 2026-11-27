# ============================================================
# Kans3D — StaticLib (C++17, PCH: kspch.h)
# ============================================================
set(KANS3D_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- Sources ----
file(GLOB_RECURSE KANS3D_HEADERS CONFIGURE_DEPENDS ${KANS3D_SRC}/*.h)
file(GLOB_RECURSE KANS3D_SOURCES CONFIGURE_DEPENDS ${KANS3D_SRC}/*.cpp)

# Native caption handling is isolated from the cross-platform GLFW implementation.
if(NOT WIN32)
    list(FILTER KANS3D_SOURCES EXCLUDE REGEX "/Win32WindowFrame\\.cpp$")
endif()

add_library(Kans3D STATIC
    ${KANS3D_HEADERS}
    ${KANS3D_SOURCES}
)

option(KANS_TRACY "Enable on-demand Tracy profiling in Debug and Release" ON)
option(KANS_TRACY_MEMORY "Track global C++ allocations in Tracy" OFF)
if(KANS_TRACY AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tracy/public/TracyClient.cpp")
    message(WARNING "Tracy profiling disabled: Kans3D/vendor/tracy is not checked out "
                    "(run: git submodule update --init Kans3D/vendor/tracy)")
    set(KANS_TRACY OFF)
    set(KANS_TRACY OFF PARENT_SCOPE)
endif()
if(KANS_TRACY)
    set(kans_tracy_config "$<CONFIG:Debug,Release>")
    set(kans_tracy_public "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tracy/public")
    set(kans_tracy_client "${kans_tracy_public}/TracyClient.cpp")
    target_sources(Kans3D PRIVATE "$<${kans_tracy_config}:${kans_tracy_client}>")
    set_source_files_properties("${kans_tracy_client}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(Kans3D SYSTEM PUBLIC "$<${kans_tracy_config}:${kans_tracy_public}>")
    # Hosts that mask invariant TSC can use Tracy's monotonic clock fallback.
    target_compile_definitions(Kans3D PUBLIC "$<${kans_tracy_config}:TRACY_ENABLE;TRACY_ON_DEMAND;TRACY_TIMER_FALLBACK>"
        PRIVATE "$<${kans_tracy_config}:TRACY_NO_CRASH_HANDLER;TRACY_NO_SYSTEM_TRACING>")
    find_package(Threads REQUIRED)
    target_link_libraries(Kans3D PRIVATE "$<${kans_tracy_config}:Threads::Threads;${CMAKE_DL_LIBS}>")
    if(KANS_TRACY_MEMORY)
        # Separate the archive members so allocation-counting tests that supply
        # ordinary new/delete can still resolve the aligned replacements.
        target_sources(Kans3D PRIVATE
            "$<${kans_tracy_config}:${CMAKE_CURRENT_SOURCE_DIR}/profiling/GlobalNew.cpp>"
            "$<${kans_tracy_config}:${CMAKE_CURRENT_SOURCE_DIR}/profiling/GlobalNewAligned.cpp>")
        target_compile_definitions(Kans3D PRIVATE "$<${kans_tracy_config}:KANS_TRACY_MEMORY>")
    endif()
    if(WIN32)
        # FatalError owns the DbgHelp session. Avoid Tracy's startup symbol
        # preload and shared DbgHelp state; source locations still label zones.
        target_compile_definitions(Kans3D PUBLIC "$<${kans_tracy_config}:TRACY_NO_CALLSTACK>")
        target_link_libraries(Kans3D PRIVATE "$<${kans_tracy_config}:ws2_32;dbghelp;secur32>")
    endif()
endif()

# Embed only the source closure used by the seven FSR compute passes. Byte arrays
# avoid MSVC's string-literal size limits, including for the large FFX core headers.
set(KANS_FSR_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFsrSources.cmake")
include("${KANS_FSR_EMBED_SCRIPT}")
set(KANS_FSR_DEPENDS "${KANS_FSR_EMBED_SCRIPT}")
foreach(relative IN LISTS KANS_FSR_FILES)
    list(APPEND KANS_FSR_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/FidelityFX-FSR3/${relative}")
endforeach()
set(KANS_FSR_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/FsrShaderSources.cpp")
add_custom_command(OUTPUT "${KANS_FSR_GENERATED}"
    COMMAND "${CMAKE_COMMAND}"
        "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/vendor/FidelityFX-FSR3"
        "-DOUTPUT=${KANS_FSR_GENERATED}" -P "${KANS_FSR_EMBED_SCRIPT}"
    DEPENDS ${KANS_FSR_DEPENDS}
    VERBATIM)
target_sources(Kans3D PRIVATE "${KANS_FSR_GENERATED}")

# ---- Precompiled header ----
target_precompile_headers(Kans3D PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${KANS3D_SRC}/kspch.h>
)

# ---- Compile definitions ----
if(WIN32)
    target_link_libraries(Kans3D PRIVATE dwmapi dbghelp user32)
    target_compile_definitions(spdlog INTERFACE SPDLOG_WCHAR_FILENAMES)
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
    target_link_options(Kans3D INTERFACE -rdynamic)
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
kans_configure_ffmpeg(Kans3D "${KANS3D_SRC}/Kans3D/Video/VideoDecoder.cpp")
kans_configure_steam_audio(Kans3D "${KANS3D_SRC}/Kans3D/Audio/SteamAudio.cpp")

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

set_source_files_properties("${KANS3D_SRC}/Kans3D/Plugin/PluginDirectory.cpp" PROPERTIES
    COMPILE_DEFINITIONS KANS_ENGINE_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/Plugins")
