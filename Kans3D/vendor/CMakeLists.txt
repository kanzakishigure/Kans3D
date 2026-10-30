# ============================================================
# Vendor CMakeLists.txt — Centralized vendor build definitions
#
# All vendor targets (static libs, header-only, imported, pre-built)
# are defined here instead of inside submodule directories, so that
# git submodule state is never dirtied by build-system files.
# ============================================================

# ============================================================
# GLFW — uses its own CMakeLists.txt (part of the GLFW submodule)
# ============================================================
# Disable GLFW examples & tests (they need dwmapi.lib, we only need the lib)


set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
# The pinned GLFW declares CMake 3.1 policies, unsupported by CMake 4.
# Scope the compatibility override to this dependency, not the whole project.
function(kans_add_glfw)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()
    add_subdirectory(GLFW)
endfunction()
kans_add_glfw()
if(WIN32)
    target_link_libraries(glfw PRIVATE dwmapi.lib)
endif()
set_target_properties(glfw PROPERTIES
    FOLDER "Dependencies"
)
# ============================================================
# ImGui 1.92 — core, demo and C++ string helpers (C++17)
# ============================================================
add_library(ImGui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imconfig.h
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.h
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_internal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imstb_rectpack.h
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imstb_textedit.h
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imstb_truetype.h
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(ImGui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
kans_set_output_dirs(ImGui)
set_target_properties(ImGui PROPERTIES
    FOLDER "Dependencies"
    CXX_STANDARD 17
)
if(UNIX)
    set_target_properties(ImGui PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ============================================================
# yaml-cpp — StaticLib (C++17)
# ============================================================
file(GLOB_RECURSE YAML_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/yaml-cpp/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/yaml-cpp/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/yaml-cpp/include/*.h
)
add_library(yaml-cpp STATIC ${YAML_SOURCES})
target_include_directories(yaml-cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/yaml-cpp/include)
# The pinned emitterutils.cpp uses fixed-width integers without including
# <cstdint>. Keep the workaround private to yaml-cpp and leave its checkout clean.
target_compile_options(yaml-cpp PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/FIcstdint>"
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-include>"
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:cstdint>"
)
kans_set_output_dirs(yaml-cpp)
set_target_properties(yaml-cpp PROPERTIES
    FOLDER "Dependencies"
    CXX_STANDARD 17
)
if(UNIX)
    set_target_properties(yaml-cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ============================================================
# glm — Header-only INTERFACE library
# ============================================================
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/glm)


# ============================================================
# Vulkan Memory Allocator — Header-only INTERFACE library
# ============================================================
add_library(VulkanMemoryAllocator INTERFACE)
target_include_directories(VulkanMemoryAllocator SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/VulkanMemoryAllocator/include)

# ============================================================
# EnTT — Header-only INTERFACE library
# ============================================================
add_library(EnTT INTERFACE)
target_include_directories(EnTT INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/entt/include)

# ============================================================
# stb_image — StaticLib (compiled separately, no PCH dependency)
# ============================================================
add_library(stb_image STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_image/stb_image.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb_image/stb_image.cpp
)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb_image)
kans_set_output_dirs(stb_image)
set_target_properties(stb_image PROPERTIES
    FOLDER "Dependencies"
    CXX_STANDARD 17
)

# ============================================================
# miniaudio — one C implementation including the Vorbis backend
# ============================================================
add_library(miniaudio STATIC ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio)
kans_set_output_dirs(miniaudio)
set_target_properties(miniaudio PROPERTIES FOLDER "Dependencies")
if(UNIX)
    set_target_properties(miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} pthread m)
endif()

# ============================================================
# ImGuizmo — StaticLib (depends on ImGui)
# ============================================================
add_library(ImGuizmo STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo/src/ImGuizmo.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo/src/ImGuizmo.cpp
)
target_include_directories(ImGuizmo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo/src)
target_link_libraries(ImGuizmo PUBLIC ImGui)
kans_set_output_dirs(ImGuizmo)
set_target_properties(ImGuizmo PROPERTIES
    FOLDER "Dependencies"
    CXX_STANDARD 17
)

# ============================================================
# assimp — Pre-built IMPORTED library (Windows) / system (Linux)
# ============================================================
if(WIN32)
    add_library(assimp INTERFACE)                                                                                                                                                                                              
    target_include_directories(assimp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/assimp/include)                                                                                                                                    
    target_link_libraries(assimp INTERFACE                                                                                                                                                                                     
        "$<$<CONFIG:Debug>:${CMAKE_CURRENT_SOURCE_DIR}/assimp/bin/Debug/assimp-vc142-mtd.lib>"                                                                                                                                 
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:Dist>>:${CMAKE_CURRENT_SOURCE_DIR}/assimp/bin/Release/assimp-vc142-mt.lib>"                                                                                                         
    )
else()
    find_library(ASSIMP_LIBRARY assimp)
    find_path(ASSIMP_INCLUDE_DIR assimp/Importer.hpp)
    if(ASSIMP_LIBRARY AND ASSIMP_INCLUDE_DIR)
        add_library(assimp INTERFACE)
        target_include_directories(assimp INTERFACE ${ASSIMP_INCLUDE_DIR})
        target_link_libraries(assimp INTERFACE ${ASSIMP_LIBRARY})
    else()
        message(WARNING "assimp not found. Install: sudo pacman -S assimp")
        add_library(assimp INTERFACE)
    endif()
endif()

set_target_properties(assimp PROPERTIES
    FOLDER "Dependencies"
    CXX_STANDARD 17
)

# ============================================================
# Vulkan SDK — INTERFACE library bundling shaderc, spirv-cross, vulkan
# ============================================================
if(VULKAN_SDK_DIR)
    if(WIN32)
        # Windows: use monolithic Vulkan SDK directory
        add_library(vulkan_sdk INTERFACE)
        target_link_libraries(vulkan_sdk INTERFACE
            ${VULKAN_SDK_DIR}/Lib/shaderc_combined.lib
            ${VULKAN_SDK_DIR}/Lib/spirv-cross-core.lib
            ${VULKAN_SDK_DIR}/Lib/spirv-cross-glsl.lib
            ${VULKAN_SDK_DIR}/Lib/SPIRV-Tools$<$<CONFIG:Debug>:d>.lib
            ${VULKAN_SDK_DIR}/Lib/vulkan-1.lib
        )
        target_include_directories(vulkan_sdk INTERFACE ${VULKAN_SDK_DIR}/Include
                                                        ${CMAKE_CURRENT_SOURCE_DIR}/shaderc/libshaderc_util/include)
    else()
        # Linux: use system-installed packages (headers/libs are in separate locations)
        find_package(Vulkan REQUIRED)

        # shaderc (use combined static lib which includes shaderc_util symbols)
        find_library(SHADERC_LIBRARY shaderc_combined REQUIRED)
        find_path(SHADERC_INCLUDE_DIR shaderc/shaderc.h REQUIRED)

        # glslang (required by shaderc_combined)
        find_library(GLSLANG_LIBRARY glslang REQUIRED)

        # spirv-cross (install: sudo apt install libspirv-cross-dev or equivalent)
        find_library(SPIRV_CROSS_CORE_LIB spirv-cross-core)
        find_library(SPIRV_CROSS_GLSL_LIB spirv-cross-glsl REQUIRED)
        find_path(SPIRV_CROSS_INCLUDE_DIR spirv_cross/spirv_cross.hpp)
        if(NOT SPIRV_CROSS_CORE_LIB OR NOT SPIRV_CROSS_INCLUDE_DIR)
            message(WARNING "spirv-cross not found. Shader reflection will not build. "
                            "Install it: sudo apt install libspirv-cross-dev")
        endif()

        # SPIRV-Tools (multiple libs: shaderc_combined needs opt + link)
        find_library(SPIRV_TOOLS_LIB SPIRV-Tools)
        find_library(SPIRV_TOOLS_OPT_LIB SPIRV-Tools-opt)
        find_library(SPIRV_TOOLS_LINK_LIB SPIRV-Tools-link)
        find_library(SPIRV_LIB SPIRV)
        find_path(SPIRV_TOOLS_INCLUDE_DIR spirv-tools/libspirv.h)

        add_library(vulkan_sdk INTERFACE)
        target_link_libraries(vulkan_sdk INTERFACE
            Vulkan::Vulkan
            ${SHADERC_LIBRARY}
            ${GLSLANG_LIBRARY}
            ${SPIRV_CROSS_GLSL_LIB}
            ${SPIRV_CROSS_CORE_LIB}
            ${SPIRV_TOOLS_OPT_LIB}
            ${SPIRV_TOOLS_LINK_LIB}
            ${SPIRV_TOOLS_LIB}
            ${SPIRV_LIB}
        )
        target_include_directories(vulkan_sdk INTERFACE
            ${SHADERC_INCLUDE_DIR}
            ${SPIRV_CROSS_INCLUDE_DIR}
            ${SPIRV_TOOLS_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaderc/libshaderc_util/include
        )
    endif()
endif()

# ============================================================
# spdlog — header-only INTERFACE library 
# ============================================================
 add_library(spdlog INTERFACE)                                                                                                                                                                                              
target_include_directories(spdlog INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/spdlog)

# CPU text dependencies. No platform font APIs or optional external backends.
add_library(harfbuzz STATIC harfbuzz/src/harfbuzz.cc)
target_include_directories(harfbuzz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/harfbuzz/src)
target_compile_definitions(harfbuzz PRIVATE HB_NO_CORETEXT HB_NO_DIRECTWRITE HB_NO_GDI)
file(GLOB MSDFGEN_CORE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/msdfgen/core/*.cpp)
add_library(msdfgen-core STATIC ${MSDFGEN_CORE})
target_include_directories(msdfgen-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/msdfgen)
target_compile_definitions(msdfgen-core PUBLIC MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
foreach(text_dependency harfbuzz msdfgen-core)
    kans_set_output_dirs(${text_dependency})
    set_target_properties(${text_dependency} PROPERTIES FOLDER "Dependencies" CXX_STANDARD 17
        POSITION_INDEPENDENT_CODE ON MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    if(MSVC)
        target_compile_options(${text_dependency} PRIVATE /utf-8 /bigobj)
    endif()
endforeach()

# PhysX 5.9 CPU SDK. Reuse the pinned SDK's module source lists, without its
# top-level configure (which writes PxConfig.h into the checkout and replaces
# the parent's configurations/CRT flags). No packman or GPU/PVD runtime.
function(kans_add_physx)
    set(PHYSX_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/PhysX/physx")
    set(PROJECT_CMAKE_FILES_DIR source/compiler/cmake)
    set(PX_GENERATE_STATIC_LIBRARIES ON)
    set(PX_GENERATE_GPU_PROJECTS OFF)
    set(PX_GENERATE_GPU_PROJECTS_ONLY OFF)
    set(PX_ENABLE_INSTALL OFF)
    set(PUBLIC_RELEASE 1)
    if(WIN32)
        set(TARGET_BUILD_PLATFORM windows)
    else()
        set(TARGET_BUILD_PLATFORM linux)
    endif()
    set(base_defs PX_PHYSX_STATIC_LIB PX_PUBLIC_RELEASE=1 DISABLE_CUDA_PHYSX
        PX_SUPPORT_PVD=0 PX_SUPPORT_OMNI_PVD=0 PX_NVTX=0)
    set(PHYSX_LINUX_COMPILE_DEFS ${base_defs})
    set(PHYSX_WINDOWS_COMPILE_DEFS ${base_defs} NOMINMAX _CRT_SECURE_NO_WARNINGS)
    foreach(platform LINUX WINDOWS)
        set(PHYSX_${platform}_DEBUG_COMPILE_DEFS PX_DEBUG=1 PX_CHECKED=1)
        set(PHYSX_${platform}_RELEASE_COMPILE_DEFS NDEBUG)
    endforeach()
    # The disabled PVD dependency appears in upstream link lists only.
    add_library(PhysXPvdSDK INTERFACE)
    set(modules PhysXFoundation LowLevel LowLevelAABB LowLevelDynamics PhysXTask
        SceneQuery SimulationController PhysXCommon PhysXCooking PhysX
        PhysXExtensions PhysXCharacterKinematic PhysXVehicle)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/physx/include")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/physx/include/PxConfig.h"
        "#pragma once\n#define PX_PHYSX_STATIC_LIB\n")
    foreach(module IN LISTS modules)
        include("${PHYSX_ROOT_DIR}/${PROJECT_CMAKE_FILES_DIR}/${module}.cmake")
        get_target_property(module_sources ${module} SOURCES)
        # GPU module loaders are guarded stubs in a CPU SDK; omit them entirely.
        list(FILTER module_sources EXCLUDE REGEX "/(gpu|gpucommon|cudamanager)/.*\\.(cpp|cu)$")
        set_property(TARGET ${module} PROPERTY SOURCES "${module_sources}")
        set_target_properties(${module} PROPERTIES CXX_STANDARD 17
            POSITION_INDEPENDENT_CODE ON FOLDER "Dependencies/PhysX"
            MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
        target_compile_definitions(${module} PRIVATE ${base_defs}
            "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>")
        target_include_directories(${module} SYSTEM PUBLIC
            "${CMAKE_CURRENT_BINARY_DIR}/physx/include" "${PHYSX_ROOT_DIR}/include")
        # Prevent upstream PUBLIC include paths from exposing vendor warnings.
        set_property(TARGET ${module} PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_BINARY_DIR}/physx/include;${PHYSX_ROOT_DIR}/include")
        if(MSVC)
            target_compile_options(${module} PRIVATE /w /bigobj /utf-8)
        else()
            # Line-level SDK debugging keeps static consumers small enough to link
            # on memory-limited builders. Engine debug information is unchanged.
            target_compile_options(${module} PRIVATE -w -fno-strict-aliasing
                "$<$<CONFIG:Debug>:-g1>")
        endif()
        kans_set_output_dirs(${module})
    endforeach()
    if(UNIX)
        target_link_libraries(PhysXFoundation PRIVATE pthread ${CMAKE_DL_LIBS})
    endif()
endfunction()
kans_add_physx()
