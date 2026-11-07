file(GLOB_RECURSE KANS_MANAGED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Source/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Generators/*.cs")
set(KANS_MANAGED_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set(KANS_MANAGED_FILES
    "${KANS_MANAGED_OUTPUT}/ScriptCore.dll"
    "${KANS_MANAGED_OUTPUT}/ScriptCore.runtimeconfig.json"
    "${KANS_MANAGED_OUTPUT}/ScriptCore.deps.json")
add_custom_command(OUTPUT ${KANS_MANAGED_FILES}
    COMMAND "${CMAKE_COMMAND}" -E env ${KANS_DOTNET_ENV} "${DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/Core/KansScriptCore.csproj"
        -c "$<IF:$<CONFIG:Debug>,Debug,Release>" -m:1 -nr:false --nologo
        -p:UseSharedCompilation=false --output "${KANS_MANAGED_OUTPUT}"
    DEPENDS ${KANS_MANAGED_SOURCES}
        Core/KansScriptCore.csproj Generators/KansScriptCore.Generators.csproj Directory.Build.props
        "${KANS3D_DIR}/src/Kans3D/Script/ComponentTypes.def"
    VERBATIM)
add_custom_target(KansScriptCore ALL DEPENDS ${KANS_MANAGED_FILES})
set_target_properties(KansScriptCore PROPERTIES FOLDER "Core")
set(KANS_MANAGED_OUTPUT "${KANS_MANAGED_OUTPUT}" PARENT_SCOPE)
set(KANS_MANAGED_FILES "${KANS_MANAGED_FILES}" PARENT_SCOPE)
# Compatibility with the editor's existing MSBuild-only copy command. No editor source changes.
add_library(ScriptCore SHARED IMPORTED GLOBAL)
foreach(config Debug Release Dist)
    string(TOUPPER "${config}" config_upper)
    set_property(TARGET ScriptCore PROPERTY IMPORTED_LOCATION_${config_upper} "${CMAKE_CURRENT_BINARY_DIR}/${config}/ScriptCore.dll")
endforeach()
add_dependencies(ScriptCore KansScriptCore)
