# ============================================================
# Kans3D — StaticLib (C++17, PCH: kspch.h)
# ============================================================
set(KANS3D_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(KANS3D_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

# ---- Sources ----
file(GLOB_RECURSE KANS3D_HEADERS CONFIGURE_DEPENDS ${KANS3D_SRC}/*.h)
file(GLOB_RECURSE KANS3D_SOURCES CONFIGURE_DEPENDS ${KANS3D_SRC}/*.cpp)

add_library(Kans3D STATIC
    ${KANS3D_HEADERS}
    ${KANS3D_SOURCES}
)

# Use the stock backend: ImGui 1.92 separates sampled images and samplers.
# Kans supplies per-image samplers through the backend's draw callback API.
target_include_directories(Kans3D PRIVATE "${KANS3D_VENDOR}/imgui/backends")

# Our GLFW checkout reports 3.4 but predates glfwGetPlatform(). Keep every other
# backend feature test intact and gate just this API on its accompanying constant.
set(KANS_IMGUI_GLFW_SOURCE "${KANS3D_VENDOR}/imgui/backends/imgui_impl_glfw.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KANS_IMGUI_GLFW_SOURCE}")
file(READ "${KANS_IMGUI_GLFW_SOURCE}" KANS_IMGUI_GLFW_TEXT)
set(KANS_IMGUI_GLFW_PLATFORM_TEST "#define GLFW_HAS_GETPLATFORM            (GLFW_VERSION_COMBINED >= 3400)")
string(REGEX MATCHALL "#define GLFW_HAS_GETPLATFORM[^\n]*" KANS_IMGUI_GLFW_PLATFORM_SITES "${KANS_IMGUI_GLFW_TEXT}")
list(LENGTH KANS_IMGUI_GLFW_PLATFORM_SITES KANS_IMGUI_GLFW_PLATFORM_SITE_COUNT)
if(NOT KANS_IMGUI_GLFW_PLATFORM_SITE_COUNT EQUAL 1)
    message(FATAL_ERROR "Review Kans3D's ImGui GLFW platform feature detection for this ImGui version")
endif()
string(FIND "${KANS_IMGUI_GLFW_TEXT}" "${KANS_IMGUI_GLFW_PLATFORM_TEST}" KANS_IMGUI_GLFW_PLATFORM_OFFSET)
if(KANS_IMGUI_GLFW_PLATFORM_OFFSET EQUAL -1)
    message(FATAL_ERROR "ImGui GLFW platform feature detection changed")
endif()
string(REPLACE "${KANS_IMGUI_GLFW_PLATFORM_TEST}"
    "#ifdef GLFW_PLATFORM_X11\n${KANS_IMGUI_GLFW_PLATFORM_TEST}\n#else\n#define GLFW_HAS_GETPLATFORM 0\n#endif"
    KANS_IMGUI_GLFW_TEXT "${KANS_IMGUI_GLFW_TEXT}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/imgui-glfw")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/imgui-glfw/imgui_impl_glfw_kans.cpp" "${KANS_IMGUI_GLFW_TEXT}")
set_source_files_properties("${KANS3D_SRC}/Kans3D/RHI/ImGuiPlatformBuild.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/imgui-glfw")

# ---- Precompiled header ----
target_precompile_headers(Kans3D PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${KANS3D_SRC}/kspch.h>
)

# ---- Compile definitions ----
target_compile_definitions(Kans3D PUBLIC
    GLFW_INCLUDE_NONE
)
if(WIN32)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(Kans3D PUBLIC /Zc:preprocessor /utf-8)
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
    ${KANS3D_VENDOR}
)

# ---- Link vendor dependencies (all defined in vendor/CMakeLists.txt) ----
target_link_libraries(Kans3D PUBLIC
    glfw
    ImGui
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
    target_include_directories(Kans3D PRIVATE "${KANS_NETHOST_DIR}")
    if(WIN32)
        # The SDK's static libnethost.lib is built /MT; the engine uses /MD (LNK2038). Use nethost.dll
        # through its import library; the root CMakeLists copies the DLL next to each consumer.
        add_library(kans_nethost SHARED IMPORTED)
        set_target_properties(kans_nethost PROPERTIES
            IMPORTED_IMPLIB "${KANS_NETHOST_DIR}/nethost.lib"
            IMPORTED_LOCATION "${KANS_NETHOST_DIR}/nethost.dll")
        target_link_libraries(Kans3D PRIVATE kans_nethost)
    else()
        target_link_libraries(Kans3D PRIVATE "${KANS_NETHOST_DIR}/libnethost.a" ${CMAKE_DL_LIBS})
    endif()
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
# Match SDK header ABI and Windows static import decoration only in private SDK users.
# MSVC Debug uses the release CRT (/MD), so neither _DEBUG nor NDEBUG is set; PhysX requires one.
set_source_files_properties(${KANS_PHYSX_IMPLEMENTATION} PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "PX_PHYSX_STATIC_LIB;DISABLE_CUDA_PHYSX;PX_SUPPORT_PVD=0;PX_SUPPORT_OMNI_PVD=0;$<$<CONFIG:Debug>:PX_DEBUG=1>;$<$<CONFIG:Debug>:PX_CHECKED=1>;$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:MSVC>>:NDEBUG>")
