# ============================================================
# KansEditor — ConsoleApp (C++17)
# ============================================================

file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

add_executable(KansEditor ${EDITOR_SOURCES})

# ---- Windows: application icon (Explorer, taskbar and window class) ----
if(WIN32)
    target_sources(KansEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/res/KansEditor.rc)
endif()

# ---- Include directories (vendor headers propagate via Kans3D PUBLIC links) ----
target_include_directories(KansEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---- Link editor core and engine (--start-group handles static archive references) ----
if(UNIX AND NOT APPLE)
    target_link_libraries(KansEditor PRIVATE -Wl,--start-group KansEditorCore Kans3D -Wl,--end-group)
else()
    target_link_libraries(KansEditor PRIVATE KansEditorCore)
endif()

# ---- Windows: common dialogs (OpenFileDialog) ----
if(WIN32)
    target_link_libraries(KansEditor PRIVATE comdlg32)
endif()

# ---- assimp DLL is now linked via Kans3D -> assimp (IMPORTED) target ----
# (KansEditor still needs to copy the DLL at post-build)

# ---- Output & properties ----
kans_set_output_dirs(KansEditor)
set_target_properties(KansEditor PROPERTIES
    # Preserve the original default output location after moving this target into App.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/KansEditor"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/KansEditor"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/KansEditor"
    FOLDER "Editor"
    CXX_STANDARD 17
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    VS_DEBUGGER_WORKING_DIRECTORY "${OUTDIR_BASE_Debug}/KansEditor"
)

# ---- Post-build: copy assimp DLL to output (Windows only) ----
if(WIN32)
    add_custom_command(TARGET KansEditor POST_BUILD 
        # assimp DLL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<$<CONFIG:Debug>:${VENDOR_DIR}/assimp/bin/Debug/assimp-vc142-mtd.dll>
            $<$<OR:$<CONFIG:Release>,$<CONFIG:Dist>>:${VENDOR_DIR}/assimp/bin/Release/assimp-vc142-mt.dll>
            "$<TARGET_FILE_DIR:KansEditor>"
    )
endif()   
# Templates and resources are editor data and must refresh on every build.
add_custom_target(KansEditorData ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${KANSEDITOR_DIR}/Resources" "$<TARGET_FILE_DIR:KansEditor>/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${KANSEDITOR_DIR}/Templates" "$<TARGET_FILE_DIR:KansEditor>/Templates"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KANSEDITOR_DIR}/KansEditor.ini" "$<TARGET_FILE_DIR:KansEditor>/KansEditor.ini"
    VERBATIM)

add_dependencies(KansEditor KansEditorData)
set_target_properties(KansEditorData PROPERTIES FOLDER "Editor")
# ScriptCore is staged into Resources/Scripts by the root CMakeLists (KansEditorScripts).
