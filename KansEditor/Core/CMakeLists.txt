set(KANSEDITORCORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

file(GLOB_RECURSE KANSEDITORCORE_HEADERS CONFIGURE_DEPENDS ${KANSEDITORCORE_SRC}/*.h)
file(GLOB_RECURSE KANSEDITORCORE_SOURCES CONFIGURE_DEPENDS ${KANSEDITORCORE_SRC}/*.cpp)

add_library(KansEditorCore STATIC
    ${KANSEDITORCORE_HEADERS}
    ${KANSEDITORCORE_SOURCES}
)

target_include_directories(KansEditorCore PUBLIC ${KANSEDITORCORE_SRC})

# Inherit engine include paths, platform defines and compile options (including MSVC).
target_link_libraries(KansEditorCore PUBLIC Kans3D)

# Build the same PCH with this target's compile settings.
target_precompile_headers(KansEditorCore PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${KANS3D_DIR}/src/kspch.h>
)

kans_set_output_dirs(KansEditorCore)
set_target_properties(KansEditorCore PROPERTIES
    # Preserve the original default output location after moving this target into Core.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/KansEditorCore"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/KansEditorCore"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/KansEditorCore"
    FOLDER "Editor"
    CXX_STANDARD 17
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)
