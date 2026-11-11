cmake_minimum_required(VERSION 3.8)
project(btbh_renderer LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC)
endif()

# ========================================
# Standards and Policies
# ========================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    # This section configures MSVC (Visual Studio) to enable "Edit and Continue" debugging
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# ========================================
# Project Configuration
# ========================================

set(BTBH_RENDERER_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# The option names predate the SDL_GPU renderer; the game build scripts pass
# them, so they keep their names.
option(R3D_ASSIMP_VENDORED "Use vendored assimp from submodule" OFF)
option(
    BTBH_BUILD_GPU_SPIKE
    "Build the SDL_GPU production shaders, native library and native tests"
    OFF
)
option(
    BTBH_BUILD_GPU_NATIVE_TESTS
    "Build the SDL_GPU native test executables (with BTBH_BUILD_GPU_SPIKE)"
    OFF
)
option(
    BTBH_BUILD_GPU_PACKAGE_RUNTIME
    "Build native dependencies for the production Swift GPU package"
    OFF
)

if(APPLE AND BTBH_BUILD_GPU_PACKAGE_RUNTIME)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()

# ========================================
# Output Directories
# ========================================

set(BTBH_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(BTBH_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(BTBH_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BTBH_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${BTBH_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${BTBH_ARCHIVE_OUTPUT_DIRECTORY})

# ========================================
# Dependencies: Assimp
# ========================================

if(R3D_ASSIMP_VENDORED)
    set(BTBH_ASSIMP_SUBMODULE_PATH "${BTBH_RENDERER_ROOT}/external/assimp")
    set(BTBH_ASSIMP_CMAKELISTS "${BTBH_ASSIMP_SUBMODULE_PATH}/CMakeLists.txt")

    if(NOT EXISTS "${BTBH_ASSIMP_CMAKELISTS}")
        message(FATAL_ERROR
            "Vendored assimp not found!\n"
            "Missing: ${BTBH_ASSIMP_CMAKELISTS}\n"
            "Please initialize the submodule with:\n"
            "    git submodule update --init --recursive")
    endif()

    message(STATUS "Using vendored assimp from: ${BTBH_ASSIMP_SUBMODULE_PATH}")

    # Common settings
    set(ASSIMP_INJECT_DEBUG_POSTFIX     OFF CACHE BOOL "")
    set(ASSIMP_WARNINGS_AS_ERRORS       OFF CACHE BOOL "")
    set(ASSIMP_IGNORE_GIT_HASH          ON CACHE BOOL "")
    set(ASSIMP_BUILD_TESTS              OFF CACHE BOOL "")
    set(ASSIMP_NO_EXPORT                ON CACHE BOOL "")
    set(ASSIMP_INSTALL                  OFF CACHE BOOL "")

    # Enabled importers
    set(_ASSIMP_ENABLED_IMPORTERS
        COLLADA GLTF OBJ FBX IQM
    )

    # Disabled importers
    set(_ASSIMP_DISABLED_IMPORTERS
        AMF BVH OFF COB STL 3DS AC ASE ASSBIN B3D DXF CSM HMP
        IRRMESH IRR LWO LWS M3D MD2 MD3 MD5 MDC MDL NFF NDO OGRE
        OPENGEX PLY MS3D BLEND IFC XGL Q3D Q3BSP RAW SIB SMD
        TERRAGEN 3D X X3D 3MF MMD
    )

    foreach(importer ${_ASSIMP_ENABLED_IMPORTERS})
        set(ASSIMP_BUILD_${importer}_IMPORTER ON CACHE BOOL "")
    endforeach()

    foreach(importer ${_ASSIMP_DISABLED_IMPORTERS})
        set(ASSIMP_BUILD_${importer}_IMPORTER OFF CACHE BOOL "")
    endforeach()

    add_subdirectory("${BTBH_ASSIMP_SUBMODULE_PATH}")

    set(BTBH_ASSIMP_INC "${BTBH_ASSIMP_SUBMODULE_PATH}/include")
    set(BTBH_ASSIMP_LIB "assimp")

    set_target_properties(assimp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${BTBH_RUNTIME_OUTPUT_DIRECTORY}
        LIBRARY_OUTPUT_DIRECTORY ${BTBH_LIBRARY_OUTPUT_DIRECTORY}
        ARCHIVE_OUTPUT_DIRECTORY ${BTBH_ARCHIVE_OUTPUT_DIRECTORY}
    )
else()
    find_package(assimp QUIET)
    if(assimp_FOUND)
        message(STATUS "Using system-installed Assimp")
        set(BTBH_ASSIMP_INC "${ASSIMP_INCLUDE_DIRS}")
        set(BTBH_ASSIMP_LIB "${ASSIMP_LIBRARIES}")
    else()
        message(FATAL_ERROR
            "System Assimp not found.\n"
            "Please install Assimp development files for your platform, "
            "or use the vendored version included with this project.")
    endif()
endif()

# ========================================
# Dependencies: SDL3
# ========================================

if(BTBH_BUILD_GPU_SPIKE OR BTBH_BUILD_GPU_PACKAGE_RUNTIME)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${BTBH_RENDERER_ROOT}/external/SDL"
        "${CMAKE_CURRENT_BINARY_DIR}/external/SDL"
        EXCLUDE_FROM_ALL
    )
endif()

# ========================================
# SDL_GPU shaders, native library and tests
# ========================================

if(BTBH_BUILD_GPU_SPIKE)
    enable_testing()

    set(SDLSHADERCROSS_DXC ON CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_SHARED OFF CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_STATIC ON CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_SPIRVCROSS_SHARED OFF CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_CLI ON CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_CLI_STATIC ON CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    add_subdirectory(
        "${BTBH_RENDERER_ROOT}/external/SDL_shadercross"
        "${CMAKE_CURRENT_BINARY_DIR}/external/SDL_shadercross"
        EXCLUDE_FROM_ALL
    )
    # SDL_shadercross sets its own deployment target; pass the parent's only when one is set.
    if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
        target_link_options(
            shadercross
            PRIVATE
                "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        )
    endif()
    set(
        BTBH_GPU_SHADER_OUTPUT_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/btbh_gpu/shaders"
    )
    if(WIN32 AND TARGET dxcompiler AND TARGET dxildll)
        set_target_properties(
            dxcompiler
            dxildll
            PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${BTBH_RUNTIME_OUTPUT_DIRECTORY}"
        )
    endif()

    set(BTBH_GPU_SHADER_ARTIFACTS)
    set(BTBH_GPU_SHADER_SPECS
        "production_model.vert|vertex"
        "production_animated_model.vert|vertex"
        "production_shadow_fast.vert|vertex"
        "production_shadow_fast.frag|fragment"
        "production_shadow.vert|vertex"
        "production_animated_shadow.vert|vertex"
        "production_shadow.frag|fragment"
        "production_model.frag|fragment"
        "production_model_2d_shadows.frag|fragment"
        "production_model_unshadowed.frag|fragment"
        "production_model_unlit.frag|fragment"
        "production_model_rc.frag|fragment"
        "production_model_rc_2d_shadows.frag|fragment"
        "production_model_rc_unshadowed.frag|fragment"
        "production_post.vert|vertex"
        "production_ssao.frag|fragment"
        "production_ssao_denoise.frag|fragment"
        "production_ssao_resolve.frag|fragment"
        "production_hbao.frag|fragment"
        "production_hbao_denoise.frag|fragment"
        "production_hbao_resolve.frag|fragment"
        "production_scene_distance.frag|fragment"
        "production_rc_source.vert|vertex"
        "production_rc_source.frag|fragment"
        "production_rc_directional_source.vert|vertex"
        "production_rc_directional_source.frag|fragment"
        "production_rc_direct_light.vert|vertex"
        "production_rc_direct_light.frag|fragment"
        "production_rc_material_source.vert|vertex"
        "production_rc_animated_material_source.vert|vertex"
        "production_rc_material_source.frag|fragment"
        "production_hrc_trace.comp|compute"
        "production_hrc_merge_up.comp|compute"
        "production_hrc_merge_down.comp|compute"
        "production_hrc_reproject.comp|compute"
        "production_hrc_finish.comp|compute"
        "production_hrc_blur.comp|compute"
        "production_hrc_resolve.comp|compute"
        "production_hrc_reconstruct.comp|compute"
        "production_fsr3_prepare_inputs.comp|compute"
        "production_fsr3_luma_pyramid.comp|compute"
        "production_fsr3_shading_change_pyramid.comp|compute"
        "production_fsr3_shading_change.comp|compute"
        "production_fsr3_prepare_reactivity.comp|compute"
        "production_fsr3_luma_instability.comp|compute"
        "production_fsr3_accumulate.comp|compute"
        "production_fsr3_debug_view.comp|compute"
        "production_fsr3_clear.comp|compute"
        "production_fsr3_motion_view.comp|compute"
        "production_rc_compose.frag|fragment"
        "production_bloom_downsample.frag|fragment"
        "production_bloom_upsample.frag|fragment"
        "production_hdr_composite.frag|fragment"
        "production_lut.frag|fragment"
        "production_cas.frag|fragment"
        "production_tilt_shift.frag|fragment"
        "production_upscale_lanczos.frag|fragment"
        "production_upscale_fsr_easu.frag|fragment"
        "production_upscale_fsr_rcas.frag|fragment"
        "production_post.frag|fragment"
        "production_fxaa.frag|fragment"
        "production_smaa_edge_medium.frag|fragment"
        "production_smaa_edge_high.frag|fragment"
        "production_smaa_weights_medium.frag|fragment"
        "production_smaa_weights_high.frag|fragment"
        "production_smaa_blend.frag|fragment"
        "production_canvas.vert|vertex"
        "production_canvas.frag|fragment"
        "production_canvas_textured.frag|fragment"
    )
    foreach(_shader_spec IN LISTS BTBH_GPU_SHADER_SPECS)
        string(REPLACE "|" ";" _shader_parts "${_shader_spec}")
        list(GET _shader_parts 0 _shader_name)
        list(GET _shader_parts 1 _shadercross_stage)

        set(
            _shader_source
            "${BTBH_RENDERER_ROOT}/shaders/gpu/${_shader_name}.hlsl"
        )
        set(
            _shader_spirv
            "${BTBH_GPU_SHADER_OUTPUT_DIR}/${_shader_name}.spv"
        )
        set(
            _shader_msl
            "${BTBH_GPU_SHADER_OUTPUT_DIR}/${_shader_name}.msl"
        )
        set(
            _shader_reflection
            "${BTBH_GPU_SHADER_OUTPUT_DIR}/${_shader_name}.json"
        )
        set(
            _shader_dependencies
            shadercross
            "${_shader_source}"
            "${BTBH_RENDERER_ROOT}/shaders/gpu/production_surface.hlsli"
            "${BTBH_RENDERER_ROOT}/shaders/gpu/production_dig_mask.hlsli"
        )
        if(
            _shader_name STREQUAL "production_model_unshadowed.frag" OR
            _shader_name STREQUAL "production_model_2d_shadows.frag" OR
            _shader_name STREQUAL "production_model_rc.frag" OR
            _shader_name STREQUAL "production_model_rc_2d_shadows.frag" OR
            _shader_name STREQUAL "production_model_rc_unshadowed.frag"
        )
            list(
                APPEND
                _shader_dependencies
                "${BTBH_RENDERER_ROOT}/shaders/gpu/production_model.frag.hlsl"
            )
        endif()
        if(
            _shader_name MATCHES "^production_model" OR
            _shader_name MATCHES "^production_animated_model" OR
            _shader_name MATCHES "^production_rc_.*material_source"
        )
            list(
                APPEND
                _shader_dependencies
                "${BTBH_RENDERER_ROOT}/shaders/gpu/production_temporal.hlsli"
            )
        endif()
        if(_shader_name MATCHES "^production_fsr3_")
            file(
                GLOB_RECURSE
                _fsr3_headers
                "${BTBH_RENDERER_ROOT}/shaders/gpu/fsr3/*.h"
                "${BTBH_RENDERER_ROOT}/shaders/gpu/fsr3/*.hlsli"
            )
            list(APPEND _shader_dependencies ${_fsr3_headers})
        endif()
        if(_shader_name MATCHES "^production_smaa_")
            list(
                APPEND
                _shader_dependencies
                "${BTBH_RENDERER_ROOT}/shaders/gpu/production_smaa_common.hlsli"
                "${BTBH_RENDERER_ROOT}/shaders/external/SMAA.hlsl"
            )
        endif()
        if(_shader_name MATCHES "^production_hbao")
            list(
                APPEND
                _shader_dependencies
                "${BTBH_RENDERER_ROOT}/shaders/gpu/production_hbao.hlsli"
            )
        endif()
        if(_shader_name STREQUAL "production_hrc_resolve.comp")
            list(
                APPEND
                _shader_dependencies
                "${BTBH_RENDERER_ROOT}/shaders/gpu/production_rc_omni_shadow.hlsli"
            )
        endif()
        if(
            _shader_name STREQUAL "production_hrc_trace.comp" OR
            _shader_name STREQUAL "production_hrc_finish.comp"
        )
            list(
                APPEND
                _shader_dependencies
                "${BTBH_RENDERER_ROOT}/shaders/gpu/production_hrc_source_transport.hlsli"
            )
        endif()
        if(
            _shader_name STREQUAL "production_lut.frag" OR
            _shader_name STREQUAL "production_post.frag" OR
            _shader_name STREQUAL "production_tilt_shift.frag"
        )
            list(
                APPEND
                _shader_dependencies
                "${BTBH_RENDERER_ROOT}/shaders/gpu/production_tonemap.hlsli"
            )
        endif()
        add_custom_command(
            OUTPUT "${_shader_spirv}"
            COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "${BTBH_GPU_SHADER_OUTPUT_DIR}"
            COMMAND
                "$<TARGET_FILE:shadercross>"
                "${_shader_source}"
                -s HLSL
                -d SPIRV
                -t "${_shadercross_stage}"
                -e main
                -I "${BTBH_RENDERER_ROOT}/shaders/gpu"
                -o "${_shader_spirv}"
            DEPENDS ${_shader_dependencies}
            COMMENT "Compiling ${_shader_name} HLSL to SPIR-V"
            VERBATIM
        )
        add_custom_command(
            OUTPUT
                "${_shader_msl}"
                "${_shader_reflection}"
            COMMAND
                "$<TARGET_FILE:shadercross>"
                "${_shader_spirv}"
                -s SPIRV
                -d MSL
                -t "${_shadercross_stage}"
                -e main
                --msl-version 2.4.0
                -o "${_shader_msl}"
            COMMAND
                "$<TARGET_FILE:shadercross>"
                "${_shader_spirv}"
                -s SPIRV
                -d JSON
                -t "${_shadercross_stage}"
                -e main
                -o "${_shader_reflection}"
            DEPENDS
                shadercross
                "${_shader_spirv}"
            COMMENT "Translating and reflecting ${_shader_name} SPIR-V"
            VERBATIM
        )

        list(
            APPEND
            BTBH_GPU_SHADER_ARTIFACTS
            "${_shader_spirv}"
            "${_shader_msl}"
            "${_shader_reflection}"
        )

        # These FSR 3 passes' MSL gives two resources one Metal index (SRVs read with Load are renumbered as
        # storage textures), so they do not compile to metallib; btbh_gpu_fsr3 keeps FSR 1 on Metal.
        set(_shader_metallib_supported TRUE)
        if(_shader_name MATCHES "^production_fsr3_(accumulate|luma_instability|luma_pyramid|prepare_inputs|prepare_reactivity|shading_change_pyramid)\\.comp$")
            set(_shader_metallib_supported FALSE)
        endif()
        if(APPLE AND _shader_metallib_supported)
            set(
                _shader_air
                "${BTBH_GPU_SHADER_OUTPUT_DIR}/${_shader_name}.air"
            )
            set(
                _shader_metallib
                "${BTBH_GPU_SHADER_OUTPUT_DIR}/${_shader_name}.metallib"
            )
            add_custom_command(
                OUTPUT "${_shader_metallib}"
                COMMAND
                    xcrun -sdk macosx metal
                    -std=macos-metal2.4
                    -x metal
                    -c "${_shader_msl}"
                    -o "${_shader_air}"
                COMMAND
                    xcrun -sdk macosx metallib
                    "${_shader_air}"
                    -o "${_shader_metallib}"
                DEPENDS "${_shader_msl}"
                COMMENT "Compiling ${_shader_name} MSL to metallib"
                VERBATIM
            )
            list(APPEND BTBH_GPU_SHADER_ARTIFACTS "${_shader_metallib}")
        endif()
    endforeach()

    set(BTBH_GPU_RUNTIME_ASSET_ARTIFACTS)
    foreach(_asset_name IN ITEMS
        "brdf_lut_512_rg16_float.raw"
        "smaa_area_160x560_rg8.raw"
        "smaa_search_64x16_r8.raw"
    )
        set(_asset_source "${BTBH_RENDERER_ROOT}/assets/${_asset_name}")
        set(
            _asset_destination
            "${BTBH_GPU_SHADER_OUTPUT_DIR}/${_asset_name}"
        )
        add_custom_command(
            OUTPUT "${_asset_destination}"
            COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "${BTBH_GPU_SHADER_OUTPUT_DIR}"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                "${_asset_source}"
                "${_asset_destination}"
            DEPENDS "${_asset_source}"
            COMMENT "Staging GPU runtime asset ${_asset_name}"
            VERBATIM
        )
        list(
            APPEND
            BTBH_GPU_RUNTIME_ASSET_ARTIFACTS
            "${_asset_destination}"
        )
    endforeach()

    add_custom_target(
        btbh_gpu_shaders
        DEPENDS
            ${BTBH_GPU_SHADER_ARTIFACTS}
            ${BTBH_GPU_RUNTIME_ASSET_ARTIFACTS}
    )

    add_library(btbh_gpu STATIC
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_canvas.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_contract.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_custom_mesh.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_device.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_font.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_frame.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_fsr3.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_instance_buffer.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_model.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_holographic_radiance_cascades.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_radiance_cascades.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_resources.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_scene.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_sdf_terrain.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_texture.c"
        "${BTBH_RENDERER_ROOT}/src/gpu/btbh_gpu_window.c"
    )
    # cgltf, stb_image and stb_truetype come from the pinned raylib submodule.
    target_include_directories(btbh_gpu
        PUBLIC
            "${BTBH_RENDERER_ROOT}/include"
        PRIVATE
            "${BTBH_RENDERER_ROOT}/external/raylib/src/external"
            "${BTBH_ASSIMP_INC}"
    )
    target_compile_definitions(
        btbh_gpu
        PRIVATE
            SDL_MAIN_HANDLED
            BTBH_GPU_HAS_ASSIMP=1
    )
    target_link_libraries(
        btbh_gpu
        PRIVATE
            SDL3::SDL3-static
            ${BTBH_ASSIMP_LIB}
    )
    set_property(TARGET btbh_gpu PROPERTY C_STANDARD 11)
    if(MSVC)
        target_compile_options(btbh_gpu PRIVATE /W4 /WX)
    else()
        target_compile_options(
            btbh_gpu
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
        )
    endif()

    # Not every native test source is committed, so a checkout configures without
    # the tests unless they are asked for; the game builds need none of them.
    if(BTBH_BUILD_GPU_NATIVE_TESTS)
        add_executable(btbh_gpu_contract_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_contract_test.c"
        )
        target_link_libraries(btbh_gpu_contract_test PRIVATE btbh_gpu)
        add_test(NAME btbh_gpu_contract COMMAND btbh_gpu_contract_test)

        add_executable(btbh_gpu_radiance_cascades_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_radiance_cascades_test.c"
        )
        target_include_directories(
            btbh_gpu_radiance_cascades_test
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_link_libraries(
            btbh_gpu_radiance_cascades_test
            PRIVATE btbh_gpu
        )
        add_test(
            NAME btbh_gpu_radiance_cascades
            COMMAND btbh_gpu_radiance_cascades_test
        )

        add_executable(btbh_gpu_scene_blend_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_scene_blend_test.c"
        )
        target_include_directories(
            btbh_gpu_scene_blend_test
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_link_libraries(
            btbh_gpu_scene_blend_test
            PRIVATE btbh_gpu SDL3::SDL3-static
        )
        add_test(
            NAME btbh_gpu_scene_blend
            COMMAND btbh_gpu_scene_blend_test
        )

        add_executable(btbh_gpu_gamepad_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_gamepad_test.c"
        )
        target_link_libraries(btbh_gpu_gamepad_test PRIVATE btbh_gpu SDL3::SDL3-static)
        add_test(NAME btbh_gpu_gamepad COMMAND btbh_gpu_gamepad_test)

        add_executable(btbh_gpu_window_config_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_window_config_test.c"
        )
        target_include_directories(
            btbh_gpu_window_config_test
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_link_libraries(
            btbh_gpu_window_config_test
            PRIVATE btbh_gpu SDL3::SDL3-static
        )
        add_test(
            NAME btbh_gpu_window_config
            COMMAND btbh_gpu_window_config_test
        )

        add_executable(btbh_gpu_animation_math_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_animation_math_test.c"
        )
        target_include_directories(
            btbh_gpu_animation_math_test
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_compile_definitions(
            btbh_gpu_animation_math_test
            PRIVATE
                BTBH_GPU_ANIMATION_FIXTURE_PATH="${BTBH_RENDERER_ROOT}/Tests/Fixtures/blended-animation.gltf"
                BTBH_GPU_ROOT_AFTER_CHILDREN_FIXTURE_PATH="${BTBH_RENDERER_ROOT}/Tests/Fixtures/root-after-children-skin.gltf"
        )
        target_link_libraries(
            btbh_gpu_animation_math_test
            PRIVATE btbh_gpu SDL3::SDL3-static
        )
        add_test(
            NAME btbh_gpu_animation_math
            COMMAND btbh_gpu_animation_math_test
        )

        add_executable(btbh_gpu_model_orm_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_model_orm_test.c"
        )
        target_include_directories(
            btbh_gpu_model_orm_test
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_link_libraries(
            btbh_gpu_model_orm_test
            PRIVATE btbh_gpu SDL3::SDL3-static
        )
        add_test(
            NAME btbh_gpu_model_orm
            COMMAND btbh_gpu_model_orm_test
        )

        add_executable(btbh_gpu_model_skinning_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_model_skinning_test.c"
        )
        target_include_directories(
            btbh_gpu_model_skinning_test
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_link_libraries(
            btbh_gpu_model_skinning_test
            PRIVATE btbh_gpu SDL3::SDL3-static
        )
        add_test(
            NAME btbh_gpu_model_skinning
            COMMAND btbh_gpu_model_skinning_test
        )

        add_executable(btbh_gpu_public_header_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_public_header_test.c"
        )
        target_compile_definitions(
            btbh_gpu_public_header_test
            PRIVATE
                BTBH_GPU_PUBLIC_HEADER_PATH="${BTBH_RENDERER_ROOT}/include/btbh_gpu/btbh_gpu.h"
        )
        add_test(
            NAME btbh_gpu_public_header
            COMMAND btbh_gpu_public_header_test
        )

        add_executable(btbh_gpu_resources_public_header_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_public_header_test.c"
        )
        target_compile_definitions(
            btbh_gpu_resources_public_header_test
            PRIVATE
                BTBH_GPU_PUBLIC_HEADER_PATH="${BTBH_RENDERER_ROOT}/include/btbh_gpu/btbh_gpu_resources.h"
        )
        add_test(
            NAME btbh_gpu_resources_public_header
            COMMAND btbh_gpu_resources_public_header_test
        )

        add_executable(btbh_gpu_resource_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_resource_test.c"
        )
        target_link_libraries(btbh_gpu_resource_test PRIVATE btbh_gpu)
        if(MSVC)
            target_compile_options(btbh_gpu_resource_test PRIVATE /W4 /WX)
        else()
            target_compile_options(btbh_gpu_resource_test PRIVATE
                -Wall -Wextra -Wpedantic -Werror
            )
        endif()
        add_test(NAME btbh_gpu_resources COMMAND btbh_gpu_resource_test)

        # RFC 0007: the SDF terrain's CPU core (no window): determinism with and without jobs and budgets,
        # removal, heights against the mesh, seams, the edit cap and faceted meshes.
        add_executable(btbh_gpu_sdf_terrain_test
            "${BTBH_RENDERER_ROOT}/Tests/btbh_gpu_sdf_terrain_test.c"
        )
        target_include_directories(
            btbh_gpu_sdf_terrain_test
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_link_libraries(
            btbh_gpu_sdf_terrain_test
            PRIVATE btbh_gpu SDL3::SDL3-static
        )
        if(MSVC)
            target_compile_options(btbh_gpu_sdf_terrain_test PRIVATE /W4 /WX)
        else()
            target_compile_options(btbh_gpu_sdf_terrain_test PRIVATE
                -Wall -Wextra -Wpedantic -Werror
            )
        endif()
        add_test(NAME btbh_gpu_sdf_terrain COMMAND btbh_gpu_sdf_terrain_test)

        add_executable(btbh_gpu_phase2_shader_test
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_phase2_shader_test.c"
        )
        target_compile_definitions(btbh_gpu_phase2_shader_test
            PRIVATE
                BTBH_PRODUCTION_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model.vert.json"
                BTBH_PRODUCTION_ANIMATED_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_animated_model.vert.json"
                BTBH_PRODUCTION_SHADOW_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_shadow.vert.json"
                BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_shadow_fast.vert.json"
                BTBH_PRODUCTION_ANIMATED_SHADOW_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_animated_shadow.vert.json"
                BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model.frag.json"
                BTBH_PRODUCTION_2D_SHADOW_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model_2d_shadows.frag.json"
                BTBH_PRODUCTION_UNSHADOWED_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model_unshadowed.frag.json"
                BTBH_PRODUCTION_UNLIT_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model_unlit.frag.json"
                BTBH_PRODUCTION_RC_MATERIAL_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model_rc.frag.json"
                BTBH_PRODUCTION_RC_2D_SHADOW_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model_rc_2d_shadows.frag.json"
                BTBH_PRODUCTION_RC_UNSHADOWED_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model_rc_unshadowed.frag.json"
                BTBH_PRODUCTION_SSAO_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_ssao.frag.json"
                BTBH_PRODUCTION_SSAO_DENOISE_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_ssao_denoise.frag.json"
                BTBH_PRODUCTION_SSAO_RESOLVE_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_ssao_resolve.frag.json"
                BTBH_PRODUCTION_HBAO_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hbao.frag.json"
                BTBH_PRODUCTION_HBAO_DENOISE_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hbao_denoise.frag.json"
                BTBH_PRODUCTION_HBAO_RESOLVE_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hbao_resolve.frag.json"
                BTBH_PRODUCTION_RC_SOURCE_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_source.vert.json"
                BTBH_PRODUCTION_RC_SOURCE_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_source.frag.json"
                BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_directional_source.vert.json"
                BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_directional_source.frag.json"
                BTBH_PRODUCTION_RC_DIRECT_LIGHT_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_direct_light.vert.json"
                BTBH_PRODUCTION_RC_DIRECT_LIGHT_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_direct_light.frag.json"
                BTBH_PRODUCTION_RC_MATERIAL_SOURCE_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_material_source.vert.json"
                BTBH_PRODUCTION_RC_ANIMATED_MATERIAL_SOURCE_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_animated_material_source.vert.json"
                BTBH_PRODUCTION_RC_MATERIAL_SOURCE_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_material_source.frag.json"
                BTBH_PRODUCTION_HRC_TRACE_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hrc_trace.comp.json"
                BTBH_PRODUCTION_HRC_MERGE_UP_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hrc_merge_up.comp.json"
                BTBH_PRODUCTION_HRC_MERGE_DOWN_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hrc_merge_down.comp.json"
                BTBH_PRODUCTION_HRC_REPROJECT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hrc_reproject.comp.json"
                BTBH_PRODUCTION_HRC_FINISH_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hrc_finish.comp.json"
                BTBH_PRODUCTION_HRC_BLUR_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hrc_blur.comp.json"
                BTBH_PRODUCTION_HRC_RESOLVE_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hrc_resolve.comp.json"
                BTBH_PRODUCTION_HRC_RECONSTRUCT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_hrc_reconstruct.comp.json"
                BTBH_PRODUCTION_RC_COMPOSE_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_rc_compose.frag.json"
                BTBH_PRODUCTION_LUT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_lut.frag.json"
                BTBH_PRODUCTION_CAS_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_cas.frag.json"
                BTBH_PRODUCTION_TILT_SHIFT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_tilt_shift.frag.json"
                BTBH_PRODUCTION_POST_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_post.frag.json"
        )
        add_dependencies(btbh_gpu_phase2_shader_test btbh_gpu_shaders)
        add_test(NAME btbh_gpu_phase2_shaders COMMAND btbh_gpu_phase2_shader_test)

        add_executable(btbh_gpu_device_smoke
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_device_smoke.c"
        )
        target_compile_definitions(btbh_gpu_device_smoke PRIVATE SDL_MAIN_HANDLED)
        target_link_libraries(
            btbh_gpu_device_smoke
            PRIVATE
                btbh_gpu
                SDL3::SDL3-static
        )
        add_test(NAME btbh_gpu_device_smoke COMMAND btbh_gpu_device_smoke)

        add_executable(btbh_gpu_window_smoke
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_window_smoke.c"
        )
        target_compile_definitions(btbh_gpu_window_smoke PRIVATE SDL_MAIN_HANDLED)
        target_link_libraries(
            btbh_gpu_window_smoke
            PRIVATE
                btbh_gpu
                SDL3::SDL3-static
        )
        add_test(NAME btbh_gpu_window_smoke COMMAND btbh_gpu_window_smoke)

        # A scene rendered into a render target beside a window's FSR 3 frames (two hidden windows).
        add_executable(btbh_gpu_scene_target_smoke
            "${BTBH_RENDERER_ROOT}/tests/btbh_gpu_scene_target_smoke.c"
        )
        target_include_directories(
            btbh_gpu_scene_target_smoke
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_compile_definitions(btbh_gpu_scene_target_smoke
            PRIVATE
                SDL_MAIN_HANDLED
                BTBH_GPU_SMOKE_SHADER_DIR="${BTBH_GPU_SHADER_OUTPUT_DIR}"
        )
        target_link_libraries(
            btbh_gpu_scene_target_smoke
            PRIVATE
                btbh_gpu
                SDL3::SDL3-static
        )
        add_dependencies(btbh_gpu_scene_target_smoke btbh_gpu_shaders)
        add_test(NAME btbh_gpu_scene_target_smoke COMMAND btbh_gpu_scene_target_smoke)
        set_tests_properties(btbh_gpu_scene_target_smoke PROPERTIES SKIP_RETURN_CODE 77)

        # RFC 0007: an SDF terrain's crater patch over a dug ground, rendered into a target (a hidden window).
        add_executable(btbh_gpu_sdf_terrain_smoke
            "${BTBH_RENDERER_ROOT}/Tests/btbh_gpu_sdf_terrain_smoke.c"
        )
        target_include_directories(
            btbh_gpu_sdf_terrain_smoke
            PRIVATE "${BTBH_RENDERER_ROOT}/src/gpu"
        )
        target_compile_definitions(btbh_gpu_sdf_terrain_smoke
            PRIVATE
                SDL_MAIN_HANDLED
                BTBH_GPU_SMOKE_SHADER_DIR="${BTBH_GPU_SHADER_OUTPUT_DIR}"
                BTBH_GPU_SMOKE_FIXTURE_DIR="${BTBH_RENDERER_ROOT}/Tests/Fixtures"
        )
        target_link_libraries(
            btbh_gpu_sdf_terrain_smoke
            PRIVATE
                btbh_gpu
                SDL3::SDL3-static
        )
        add_dependencies(btbh_gpu_sdf_terrain_smoke btbh_gpu_shaders)
        add_test(NAME btbh_gpu_sdf_terrain_smoke COMMAND btbh_gpu_sdf_terrain_smoke)
        set_tests_properties(btbh_gpu_sdf_terrain_smoke PROPERTIES SKIP_RETURN_CODE 77)

        # The tests check with assert(): keep it in optimised builds, where NDEBUG would
        # compile every check away.
        get_property(_btbh_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_btbh_target IN LISTS _btbh_targets)
            if(_btbh_target MATCHES "^btbh_gpu_.*(_test|_smoke)$")
                target_compile_options(${_btbh_target} PRIVATE -UNDEBUG)
            endif()
        endforeach()
    endif()
endif()
