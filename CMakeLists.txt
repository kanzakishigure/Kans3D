cmake_minimum_required(VERSION 3.21)
project(Kans3D LANGUAGES C CXX)

# Keep large Debug links serial while compilation can use the requested parallelism.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS kans_link_pool=1)
set(CMAKE_JOB_POOL_LINK kans_link_pool)

if(POLICY CMP0112)
    cmake_policy(SET CMP0112 NEW)
endif()

include(CTest)

find_program(DOTNET dotnet)
option(KANS_SCRIPTS_AOT "Publish a NativeAOT shared script library" OFF)
set(KANS_SCRIPT_PROJECT "" CACHE FILEPATH "User script csproj for NativeAOT packaging")
if(DOTNET)
    # .NET tools localise their output; English keeps it ASCII so no console or IDE code page garbles it.
    set(KANS_DOTNET_ENV DOTNET_CLI_UI_LANGUAGE=en VSLANG=1033)
    get_filename_component(KANS_DOTNET_REAL "${DOTNET}" REALPATH)
    get_filename_component(KANS_DOTNET_ROOT "${KANS_DOTNET_REAL}" DIRECTORY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${KANS_DOTNET_ENV} "${DOTNET}" --info OUTPUT_VARIABLE KANS_DOTNET_INFO)
    string(REGEX MATCH "RID:[ \t]+([^ \r\n]+)" _ "${KANS_DOTNET_INFO}")
    set(KANS_HOST_RID "${CMAKE_MATCH_1}")
    file(GLOB KANS_HOST_PACKS "${KANS_DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.${KANS_HOST_RID}/*/runtimes/${KANS_HOST_RID}/native")
    list(SORT KANS_HOST_PACKS COMPARE NATURAL ORDER DESCENDING)
    foreach(KANS_HOST_PACK IN LISTS KANS_HOST_PACKS)
        if(EXISTS "${KANS_HOST_PACK}/nethost.h")
            set(KANS_NETHOST_DIR "${KANS_HOST_PACK}")
            break()
        endif()
    endforeach()
    if(NOT KANS_NETHOST_DIR)
        message(FATAL_ERROR "dotnet was found but its ${KANS_HOST_RID} nethost SDK pack is missing")
    endif()
    message(STATUS "Kans scripting: ${DOTNET}, host pack ${KANS_NETHOST_DIR}")
else()
    message(WARNING "dotnet not found: scripting is disabled")
endif()


# ============================================================
# Enable CMP0091 so CMAKE_MSVC_RUNTIME_LIBRARY takes effect
# ============================================================
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# ============================================================
# MSVC runtime library: /MD for all configs (Vulkan SDK 1.4+ shaderc libs
# no longer ship Debug CRT variants; use Release CRT globally to match)
# ============================================================

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
# ============================================================
# Custom configuration types: Debug, Release, Dist
# ============================================================
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Dist" CACHE STRING "" FORCE)

# ============================================================
# Global C++ standard
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Allow member names identical to type names (MSVC-compatible pattern)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wno-changes-meaning)
endif()

# ============================================================
# Detect architecture & system (for output=dir naming)
# ============================================================
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH "x64")
else()
    set(ARCH "x86")
endif()

if(WIN32)
    set(SYSTEM_NAME "Windows")
elseif(UNIX AND NOT APPLE)
    set(SYSTEM_NAME "Linux")
else()
    set(SYSTEM_NAME "macOS")
endif()

# ============================================================
# Output & intermediate directories
# ============================================================
foreach(CONFIG ${CMAKE_CONFIGURATION_TYPES})
    set(OUTDIR_BASE_${CONFIG} "${CMAKE_BINARY_DIR}/bin/${CONFIG}-${SYSTEM_NAME}-${ARCH}" CACHE INTERNAL "")
    set(INTDIR_BASE_${CONFIG} "${CMAKE_BINARY_DIR}/bin-int/${CONFIG}-${SYSTEM_NAME}-${ARCH}" CACHE INTERNAL "")
endforeach()

# ============================================================
# Helper function: set per-target output dirs (calls from any subdir)
# ============================================================
function(kans_set_output_dirs TARGET_NAME)
    foreach(CONFIG ${CMAKE_CONFIGURATION_TYPES})
        set_target_properties(${TARGET_NAME} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${CONFIG}   "${OUTDIR_BASE_${CONFIG}}/${TARGET_NAME}"
            ARCHIVE_OUTPUT_DIRECTORY_${CONFIG}   "${OUTDIR_BASE_${CONFIG}}/${TARGET_NAME}"
            LIBRARY_OUTPUT_DIRECTORY_${CONFIG}   "${OUTDIR_BASE_${CONFIG}}/${TARGET_NAME}"
        )
    endforeach()
endfunction()

# ============================================================
# Vulkan SDK
#   Windows: single SDK directory from VULKAN_SDK environment variable
#   Linux:   system-installed packages (vulkan, shaderc, spirv-cross, SPIRV-Tools)
# ============================================================
if(WIN32)
    if(NOT DEFINED ENV{VULKAN_SDK})
        message(WARNING "VULKAN_SDK environment variable is not set. Vulkan features may not build.")
        set(VULKAN_SDK_DIR "" CACHE INTERNAL "")
    else()
        set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}" CACHE INTERNAL "")
    endif()
else()
    # Linux: use system packages, no monolithic SDK directory
    set(VULKAN_SDK_DIR "SYSTEM" CACHE INTERNAL "")
endif()

# ============================================================
# Path shortcuts (workspace-root-relative, visible to all subdirs)
# ============================================================
set(KANS3D_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/Kans3D"      CACHE INTERNAL "")
set(KANSEDITOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/KansEditor"  CACHE INTERNAL "")
set(VENDOR_DIR     "${KANS3D_DIR}/vendor"                   CACHE INTERNAL "")


# ============================================================
# Config-specific compile definitions (all targets inherit)
# ============================================================
add_compile_definitions(
    $<$<CONFIG:Debug>:KS_DEBUG>
    $<$<CONFIG:Release>:KS_RELEASE>
    $<$<CONFIG:Dist>:KS_DIST>
)

# ============================================================
# "Dist" configuration: same flags as Release (multi-config generators
# pick these up for the Dist config type)
# ============================================================
set(CMAKE_CXX_FLAGS_DIST        "${CMAKE_CXX_FLAGS_RELEASE}"        CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DIST          "${CMAKE_C_FLAGS_RELEASE}"          CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DIST "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DIST "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}" CACHE STRING "" FORCE)

# ============================================================
# Dependencies - vendor static libraries (centralized in vendor/CMakeLists.txt)
# ============================================================
add_subdirectory(${VENDOR_DIR})

# ============================================================
# Core - Kans3D engine static library
# ============================================================
add_subdirectory(${KANS3D_DIR})

# Editor-only services and renderer features.
add_subdirectory(KansEditorCore)

# ============================================================
# Core - KansScriptCore C# shared library
# ============================================================
if(DOTNET)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/KansScriptCore)
endif()
# ============================================================
# Editor - KansEditor console application
# ============================================================
add_subdirectory(${KANSEDITOR_DIR})

# Tests live in the Kans3D-tests submodule; the engine builds without it
# (submodule not pulled, KANS_BUILD_TESTS=OFF, or GoogleTest not installed).
option(KANS_BUILD_TESTS "Build the Kans3D-tests submodule when it is present" ON)
if(BUILD_TESTING AND KANS_BUILD_TESTS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
        message(STATUS "Kans3D: test/ submodule not checked out, skipping tests")
    else()
        find_package(GTest QUIET)
        if(GTest_FOUND)
            add_subdirectory(test)
        else()
            message(WARNING "Kans3D: GoogleTest not found, skipping tests")
        endif()
    endif()
endif()

# Stage the full managed output even when only a consumer target is requested.
# A separate copy target avoids relying on a native relink when C# alone changed.
if(DOTNET)
    # The test executables share one output directory, so the copies run one after another
    # instead of writing the same files in parallel.
    set(KANS_PREVIOUS_SCRIPTS "")
    foreach(consumer KansEditor SceneRender Kans3DTests RenderFeatureTests GpuTests
            RHIConformanceTests WindowPresentationTests AnimationPerformanceTests ScriptingTests)
        if(TARGET ${consumer})
            add_custom_target(${consumer}Scripts
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${consumer}>/Resources/Scripts"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${KANS_MANAGED_OUTPUT}" "$<TARGET_FILE_DIR:${consumer}>/Resources/Scripts"
                DEPENDS KansScriptCore VERBATIM)
            add_dependencies(${consumer} ${consumer}Scripts)
            if(KANS_PREVIOUS_SCRIPTS)
                add_dependencies(${consumer}Scripts ${KANS_PREVIOUS_SCRIPTS})
            endif()
            set(KANS_PREVIOUS_SCRIPTS ${consumer}Scripts)
            if(WIN32)
                add_custom_command(TARGET ${consumer}Scripts POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${KANS_NETHOST_DIR}/nethost.dll" "$<TARGET_FILE_DIR:${consumer}>"
                    VERBATIM)
            endif()
        endif()
    endforeach()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$")
        set(KANS_AOT_ARCH arm64)
    else()
        set(KANS_AOT_ARCH x64)
    endif()
    if(WIN32)
        set(KANS_AOT_RID "win-${KANS_AOT_ARCH}")
    else()
        set(KANS_AOT_RID "linux-${KANS_AOT_ARCH}")
    endif()
    if(KANS_SCRIPTS_AOT)
        if(NOT EXISTS "${KANS_SCRIPT_PROJECT}")
            message(FATAL_ERROR "KANS_SCRIPTS_AOT needs KANS_SCRIPT_PROJECT=<user.csproj>")
        endif()
        add_custom_target(KansScriptsAot
            COMMAND "${CMAKE_COMMAND}" -E env ${KANS_DOTNET_ENV} "${DOTNET}" publish "${PROJECT_SOURCE_DIR}/KansScriptCore/Aot/KansScriptAot.csproj"
                -c Release -r "${KANS_AOT_RID}"
                "-p:KansScriptProject=${KANS_SCRIPT_PROJECT}" -m:1 -nr:false
                -p:UseSharedCompilation=false --output "${CMAKE_BINARY_DIR}/scripts-aot"
            DEPENDS KansScriptCore VERBATIM)
    endif()
endif()
