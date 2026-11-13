# Exact GLSL dependency closure for the seven SDK 1.1.4 FP32/no-wave passes.
set(KANS_FSR_FILES
    gpu/ffx_common_types.h
    gpu/ffx_core.h
    gpu/ffx_core_glsl.h
    gpu/ffx_core_gpu_common.h
    gpu/ffx_core_gpu_common_half.h
    gpu/ffx_core_portability.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_accumulate.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_common.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_luma_instability.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_reproject.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_sample.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid.h
    gpu/fsr3upscaler/ffx_fsr3upscaler_upsample.h
    gpu/spd/ffx_spd.h
    shaders/ffx_fsr3upscaler_accumulate_pass.glsl
    shaders/ffx_fsr3upscaler_luma_instability_pass.glsl
    shaders/ffx_fsr3upscaler_luma_pyramid_pass.glsl
    shaders/ffx_fsr3upscaler_prepare_inputs_pass.glsl
    shaders/ffx_fsr3upscaler_prepare_reactivity_pass.glsl
    shaders/ffx_fsr3upscaler_shading_change_pass.glsl
    shaders/ffx_fsr3upscaler_shading_change_pyramid_pass.glsl
)
if(NOT DEFINED OUTPUT)
    return()
endif()
set(code "// Generated from FidelityFX SDK sources. Do not edit.\n#include \"Kans3D/Renderer/Shading/FsrShaderSources.h\"\nnamespace Kans {\nnamespace {\n")
set(items "")
set(index 0)
foreach(relative IN LISTS KANS_FSR_FILES)
    file(READ "${ROOT}/${relative}" bytes HEX)
    string(APPEND code "static const unsigned char source${index}[] = {\n")
    string(LENGTH "${bytes}" length)
    set(offset 0)
    while(offset LESS length)
        math(EXPR remaining "${length} - ${offset}")
        if(remaining GREATER 128)
            set(remaining 128)
        endif()
        string(SUBSTRING "${bytes}" ${offset} ${remaining} chunk)
        string(REGEX REPLACE "(..)" "0x\\1," chunk "${chunk}")
        string(APPEND code "${chunk}\n")
        math(EXPR offset "${offset} + ${remaining}")
    endwhile()
    string(APPEND code "};\n")
    string(APPEND items "{\"${relative}\", std::string(reinterpret_cast<const char*>(source${index}), sizeof(source${index}))},\n")
    math(EXPR index "${index} + 1")
endforeach()
string(APPEND code "}\nconst std::map<std::string, std::string>& GetEmbeddedFsrSources() {\nstatic const std::map<std::string, std::string> sources {\n${items}};\nreturn sources;\n}\n}\n")
file(WRITE "${OUTPUT}" "${code}")
