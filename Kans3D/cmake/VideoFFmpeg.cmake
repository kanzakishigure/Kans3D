# Optional shared FFmpeg. Verify the licenses of the libraries the target will use.
option(KANS_VIDEO_FFMPEG "Enable video decoding with shared LGPL FFmpeg" ON)
# Linux distributions often package a GPL-enabled FFmpeg. Linking it is fine for local builds;
# builds that are distributed must use an LGPL FFmpeg (set this OFF to enforce it).
option(KANS_VIDEO_ALLOW_GPL_FFMPEG "Accept a GPL-enabled system FFmpeg on Linux (local builds only)" ON)

function(kans_check_ffmpeg_license result libraries includes runtime)
    if(CMAKE_CROSSCOMPILING)
        set(${result} FALSE PARENT_SCOPE)
        return()
    endif()
    include(CheckCXXSourceRuns)
    set(CMAKE_REQUIRED_LIBRARIES ${libraries})
    set(CMAKE_REQUIRED_INCLUDES ${includes})
    if(runtime)
        set(saved_path "$ENV{PATH}")
        set(ENV{PATH} "${runtime};${saved_path}")
    endif()
    unset(KANS_FFMPEG_IS_LGPL CACHE)
    check_cxx_source_runs([=[
        #include <cstring>
        extern "C" {
        #include <libavformat/avformat.h>
        #include <libavcodec/avcodec.h>
        #include <libavutil/avutil.h>
        }
        int main() {
            return std::strncmp(avformat_license(), "LGPL", 4) ||
                   std::strncmp(avcodec_license(), "LGPL", 4) ||
                   std::strncmp(avutil_license(), "LGPL", 4);
        }
    ]=] KANS_FFMPEG_IS_LGPL)
    if(runtime)
        set(ENV{PATH} "${saved_path}")
    endif()
    set(${result} "${KANS_FFMPEG_IS_LGPL}" PARENT_SCOPE)
endfunction()

function(kans_configure_ffmpeg target source)
    set(enabled 0)
    if(KANS_VIDEO_FFMPEG AND WIN32)
        set(KANS_FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/Kans3D/vendor/ffmpeg-bin" CACHE PATH
            "Root of the x64 LGPL shared FFmpeg distribution")
        set(complete TRUE)
        foreach(lib avformat avcodec avutil)
            set(implib "${KANS_FFMPEG_ROOT}/lib/${lib}.lib")
            file(GLOB component_dlls CONFIGURE_DEPENDS "${KANS_FFMPEG_ROOT}/bin/${lib}-*.dll")
            if(EXISTS "${implib}" AND component_dlls)
                list(APPEND libraries "${implib}")
                list(APPEND dlls ${component_dlls})
            else()
                set(complete FALSE)
            endif()
        endforeach()
        if(complete AND EXISTS "${KANS_FFMPEG_ROOT}/include/libavformat/avformat.h")
            kans_check_ffmpeg_license(lgpl "${libraries}" "${KANS_FFMPEG_ROOT}/include" "${KANS_FFMPEG_ROOT}/bin")
            if(lgpl)
                # FFmpeg's avcodec DLL can import swresample even for video-only callers.
                file(GLOB audio_dlls CONFIGURE_DEPENDS "${KANS_FFMPEG_ROOT}/bin/swresample-*.dll")
                target_link_libraries(${target} PRIVATE ${libraries})
                set_property(SOURCE "${source}" APPEND PROPERTY INCLUDE_DIRECTORIES "${KANS_FFMPEG_ROOT}/include")
                set_property(TARGET ${target} PROPERTY KANS_FFMPEG_DLLS "${dlls};${audio_dlls}")
                set(enabled 1)
            endif()
        endif()
    elseif(KANS_VIDEO_FFMPEG AND UNIX AND NOT APPLE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(KANS_FFMPEG QUIET IMPORTED_TARGET libavformat>=59 libavcodec>=59 libavutil>=57)
        endif()
        if(TARGET PkgConfig::KANS_FFMPEG)
            set(shared TRUE)
            foreach(library IN LISTS KANS_FFMPEG_LINK_LIBRARIES)
                if(NOT library MATCHES "\\.so(\\.|$)")
                    set(shared FALSE)
                endif()
            endforeach()
            if(shared)
                kans_check_ffmpeg_license(lgpl PkgConfig::KANS_FFMPEG "" "")
                if(NOT lgpl AND KANS_VIDEO_ALLOW_GPL_FFMPEG)
                    message(WARNING "The system FFmpeg is GPL-enabled: video works in this build, "
                                    "but do not distribute it. Use an LGPL FFmpeg for releases.")
                    set(lgpl TRUE)
                    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_DEFINITIONS KANS_FFMPEG_ALLOW_GPL=1)
                endif()
                if(lgpl)
                    target_link_libraries(${target} PRIVATE PkgConfig::KANS_FFMPEG)
                    set(enabled 1)
                endif()
            endif()
        endif()
    endif()
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_DEFINITIONS KANS_HAS_FFMPEG=${enabled})
    if(NOT enabled)
        message(STATUS "Video decoding unavailable (requires shared LGPL FFmpeg)")
    endif()
endfunction()

function(kans_copy_ffmpeg_runtime target)
    if(WIN32)
        get_target_property(dlls Kans3D KANS_FFMPEG_DLLS)
        if(dlls)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${dlls} "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endif()
endfunction()
