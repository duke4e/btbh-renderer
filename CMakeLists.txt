cmake_minimum_required(VERSION 3.8)
project(r3d VERSION 0.10.0 LANGUAGES C CXX)

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

set(R3D_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(R3D_IS_MAIN ON)
else()
    set(R3D_IS_MAIN OFF)
endif()

set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${R3D_ROOT_PATH}/cmake")

option(R3D_RAYLIB_VENDORED "Use vendored raylib from submodule" OFF)
option(R3D_ASSIMP_VENDORED "Use vendored assimp from submodule" OFF)
option(R3D_SUPPORT_ASSIMP "Build with Assimp importer support" ON)
option(R3D_BUILD_EXAMPLES "Build the examples" ${R3D_IS_MAIN})
option(R3D_BUILD_DOCS "Build the doxygen documentation" OFF)
option(R3D_BUILD_TESTS "Build focused r3d regression tests" ${R3D_IS_MAIN})
option(BTBH_BUILD_GPU_SPIKE "Build the isolated SDL_GPU Phase 2 spike" OFF)
option(
    BTBH_BUILD_GPU_PACKAGE_RUNTIME
    "Build SDL3-static for the production Swift GPU package"
    OFF
)

# ========================================
# Output Directories
# ========================================

set(R3D_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(R3D_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(R3D_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${R3D_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${R3D_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${R3D_ARCHIVE_OUTPUT_DIRECTORY})

# ========================================
# R3D Compile-Time Configuration
# ========================================

set(R3D_CONFIG_DEFAULTS
    R3D_MAX_SHADER_CODE_LENGTH          16384
    R3D_MAX_SHADER_SAMPLERS             4
    R3D_MAX_SHADER_UNIFORMS             16
    R3D_MAX_SCREEN_SHADERS              4       #< Max possible in each screen shader chain hook
    R3D_SHADER_LIGHT_FORWARD_UBO_CAP    32
    R3D_SHADER_PROBE_UBO_CAP            16
    R3D_ENABLE_TRACELOG                 1       #< Log level can be set via raylib
)

list(LENGTH R3D_CONFIG_DEFAULTS _len)
math(EXPR _count "${_len} / 2")
math(EXPR _last "${_count} - 1")

foreach(i RANGE 0 ${_last})
    math(EXPR key_index "2 * ${i}")
    math(EXPR val_index "2 * ${i} + 1")
    list(GET R3D_CONFIG_DEFAULTS ${key_index} key)
    list(GET R3D_CONFIG_DEFAULTS ${val_index} val)
    set(${key} ${val} CACHE STRING "R3D compile-time configuration")
endforeach()

set(R3D_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/r3d_config.h.in
    ${R3D_GENERATED_INCLUDE_DIR}/r3d_config.h
    @ONLY
)

# ========================================
# Dependencies: Assimp
# ========================================

if(R3D_SUPPORT_ASSIMP)
    if(R3D_ASSIMP_VENDORED)
        set(R3D_ASSIMP_SUBMODULE_PATH "${R3D_ROOT_PATH}/external/assimp")
        set(R3D_ASSIMP_CMAKELISTS "${R3D_ASSIMP_SUBMODULE_PATH}/CMakeLists.txt")

        if(NOT EXISTS "${R3D_ASSIMP_CMAKELISTS}")
            message(FATAL_ERROR 
                "Vendored assimp not found!\n"
                "Missing: ${R3D_ASSIMP_CMAKELISTS}\n"
                "Please initialize the submodule with:\n"
                "    git submodule update --init --recursive")
        endif()

        message(STATUS "Using vendored assimp from: ${R3D_ASSIMP_SUBMODULE_PATH}")

        # Common settings
        set(ASSIMP_INJECT_DEBUG_POSTFIX     OFF CACHE BOOL "")
        set(ASSIMP_WARNINGS_AS_ERRORS       OFF CACHE BOOL "")
        set(ASSIMP_IGNORE_GIT_HASH          ON CACHE BOOL "")
        set(ASSIMP_BUILD_TESTS              OFF CACHE BOOL "")
        set(ASSIMP_NO_EXPORT                ON CACHE BOOL "")
        set(ASSIMP_INSTALL                  OFF CACHE BOOL "")

        # Enabled importers
        # Note: There may be conflicts between Assimp and raylib if both enable the M3D format
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

        add_subdirectory("${R3D_ASSIMP_SUBMODULE_PATH}")

        set(R3D_ASSIMP_INC "${R3D_ASSIMP_SUBMODULE_PATH}/include" CACHE STRING "" FORCE)
        set(R3D_ASSIMP_LIB "assimp" CACHE STRING "" FORCE)

        set_target_properties(assimp PROPERTIES 
            RUNTIME_OUTPUT_DIRECTORY ${R3D_RUNTIME_OUTPUT_DIRECTORY}
            LIBRARY_OUTPUT_DIRECTORY ${R3D_LIBRARY_OUTPUT_DIRECTORY}
            ARCHIVE_OUTPUT_DIRECTORY ${R3D_ARCHIVE_OUTPUT_DIRECTORY}
        )
    else()
        find_package(assimp QUIET)
        if(assimp_FOUND)
            message(STATUS "Using system-installed Assimp")
            set(R3D_ASSIMP_INC "${ASSIMP_INCLUDE_DIRS}" CACHE STRING "" FORCE)
            set(R3D_ASSIMP_LIB "${ASSIMP_LIBRARIES}" CACHE STRING "" FORCE)
        else()
            message(FATAL_ERROR
                "System Assimp not found.\n"
                "Please install Assimp development files for your platform, "
                "or use the vendored version included with this project.")
        endif()
    endif()
endif()

# ========================================
# Dependencies: Raylib
# ========================================

if(R3D_RAYLIB_VENDORED)
    set(R3D_RAYLIB_SUBMODULE_PATH "${R3D_ROOT_PATH}/external/raylib")
    set(R3D_RAYLIB_CMAKELISTS "${R3D_RAYLIB_SUBMODULE_PATH}/CMakeLists.txt")

    if(NOT EXISTS "${R3D_RAYLIB_CMAKELISTS}")
        message(FATAL_ERROR 
            "Vendored raylib not found!\n"
            "Missing: ${R3D_RAYLIB_CMAKELISTS}\n"
            "Please initialize the submodule with:\n"
            "    git submodule update --init --recursive")
    endif()

    message(STATUS "Using vendored raylib from: ${R3D_RAYLIB_SUBMODULE_PATH}")

    set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)

    # Enabled file formats
    set(_RAYLIB_ENABLED_FORMATS
        HDR JPG
    )

    # Disabled file formats
    # r3d provides its own model importers via Assimp
    # Note: There may be conflicts between raylib and Assimp if both enable the M3D format
    set(_RAYLIB_DISABLED_FORMATS
        OBJ MTL IQM GLTF VOX M3D
    )

    foreach(format ${_RAYLIB_ENABLED_FORMATS})
        set(SUPPORT_FILEFORMAT_${format} ON CACHE BOOL "" FORCE)
    endforeach()

    foreach(format ${_RAYLIB_DISABLED_FORMATS})
        set(SUPPORT_FILEFORMAT_${format} OFF CACHE BOOL "" FORCE)
    endforeach()

    add_subdirectory("${R3D_RAYLIB_SUBMODULE_PATH}")

    set(R3D_RAYLIB_INC "${R3D_RAYLIB_SUBMODULE_PATH}/src" CACHE STRING "" FORCE)
    set(R3D_RAYLIB_LIB "raylib" CACHE STRING "" FORCE)

    set_target_properties(raylib PROPERTIES 
        RUNTIME_OUTPUT_DIRECTORY ${R3D_RUNTIME_OUTPUT_DIRECTORY}
        LIBRARY_OUTPUT_DIRECTORY ${R3D_LIBRARY_OUTPUT_DIRECTORY}
        ARCHIVE_OUTPUT_DIRECTORY ${R3D_ARCHIVE_OUTPUT_DIRECTORY}
    )
else()
    find_package(raylib QUIET)
    if(raylib_FOUND)
        message(STATUS "Using system-installed raylib")
        set(R3D_RAYLIB_INC "${RAYLIB_INCLUDE_DIRS}" CACHE STRING "" FORCE)
        set(R3D_RAYLIB_LIB "${RAYLIB_LIBRARIES}" CACHE STRING "" FORCE)
    else()
        message(FATAL_ERROR
            "System raylib not found.\n"
            "Please install raylib development files for your platform, "
            "or use the vendored version included with this project.")
    endif()
endif()

# ========================================
# Library Definition
# ========================================

include(CheckCSourceCompiles)
include(CMakePushCheckState)
include(CheckLibraryExists)
include(EmbedShaders)
include(EmbedAssets)

if(BUILD_SHARED_LIBS)
    add_definitions(-DR3D_BUILD_SHARED)
    add_definitions(-DGLAD_API_CALL_EXPORT)
endif()

if(R3D_SUPPORT_ASSIMP)
    set(R3D_IMPORTER_SOURCES
        "${R3D_ROOT_PATH}/src/importer/r3d_importer_animation.c"
        "${R3D_ROOT_PATH}/src/importer/r3d_importer_material.c"
        "${R3D_ROOT_PATH}/src/importer/r3d_importer_skeleton.c"
        "${R3D_ROOT_PATH}/src/importer/r3d_importer_texture.c"
        "${R3D_ROOT_PATH}/src/importer/r3d_importer_mesh.c"
    )
endif()

add_library(${PROJECT_NAME}
    # Importer
    ${R3D_IMPORTER_SOURCES}
    # Common
    "${R3D_ROOT_PATH}/src/common/r3d_anim.c"
    "${R3D_ROOT_PATH}/src/common/r3d_helper.c"
    "${R3D_ROOT_PATH}/src/common/r3d_image.c"
    "${R3D_ROOT_PATH}/src/common/r3d_stack.c"
    "${R3D_ROOT_PATH}/src/common/r3d_pool.c"
    "${R3D_ROOT_PATH}/src/common/r3d_pass.c"
    # Modules
    "${R3D_ROOT_PATH}/src/modules/r3d_texture.c"
    "${R3D_ROOT_PATH}/src/modules/r3d_target.c"
    "${R3D_ROOT_PATH}/src/modules/r3d_shader.c"
    "${R3D_ROOT_PATH}/src/modules/r3d_driver.c"
    "${R3D_ROOT_PATH}/src/modules/r3d_light.c"
    "${R3D_ROOT_PATH}/src/modules/r3d_render.c"
    "${R3D_ROOT_PATH}/src/modules/r3d_env.c"
    # Core
    "${R3D_ROOT_PATH}/src/r3d_animation_player.c"
    "${R3D_ROOT_PATH}/src/r3d_animation_tree.c"
    "${R3D_ROOT_PATH}/src/r3d_animation.c"
    "${R3D_ROOT_PATH}/src/r3d_color.c"
    "${R3D_ROOT_PATH}/src/r3d_core.c"
    "${R3D_ROOT_PATH}/src/r3d_camera.c"
    "${R3D_ROOT_PATH}/src/r3d_cubemap.c"
    "${R3D_ROOT_PATH}/src/r3d_draw.c"
    "${R3D_ROOT_PATH}/src/r3d_decal.c"
    "${R3D_ROOT_PATH}/src/r3d_ambient_map.c"
    "${R3D_ROOT_PATH}/src/r3d_environment.c"
    "${R3D_ROOT_PATH}/src/r3d_frustum.c"
    "${R3D_ROOT_PATH}/src/r3d_importer.c"
    "${R3D_ROOT_PATH}/src/r3d_instance.c"
    "${R3D_ROOT_PATH}/src/r3d_kinematics.c"
    "${R3D_ROOT_PATH}/src/r3d_lighting.c"
    "${R3D_ROOT_PATH}/src/r3d_surface_shader.c"
    "${R3D_ROOT_PATH}/src/r3d_screen_shader.c"
    "${R3D_ROOT_PATH}/src/r3d_sky_shader.c"
    "${R3D_ROOT_PATH}/src/r3d_material.c"
    "${R3D_ROOT_PATH}/src/r3d_texture.c"
    "${R3D_ROOT_PATH}/src/r3d_mesh.c"
    "${R3D_ROOT_PATH}/src/r3d_mesh_data.c"
    "${R3D_ROOT_PATH}/src/r3d_model.c"
    "${R3D_ROOT_PATH}/src/r3d_pack.c"
    "${R3D_ROOT_PATH}/src/r3d_probe.c"
    "${R3D_ROOT_PATH}/src/r3d_skeleton.c"
    "${R3D_ROOT_PATH}/src/r3d_shape.c"
    "${R3D_ROOT_PATH}/src/r3d_sky.c"
    "${R3D_ROOT_PATH}/src/r3d_utils.c"
    "${R3D_ROOT_PATH}/src/r3d_vertex.c"
)

# ========================================
# Embedded Resources
# ========================================

embed_shaders(${PROJECT_NAME}
    # Generic
    "${R3D_ROOT_PATH}/shaders/generic/color.frag"
    "${R3D_ROOT_PATH}/shaders/generic/screen.vert"
    "${R3D_ROOT_PATH}/shaders/generic/cubemap.vert"
    # Prepare
    "${R3D_ROOT_PATH}/shaders/prepare/denoiser_atrous.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/denoiser_sparse.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/smaa_blending_weigths.vert"
    "${R3D_ROOT_PATH}/shaders/prepare/smaa_blending_weigths.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/smaa_edge_detection.vert"
    "${R3D_ROOT_PATH}/shaders/prepare/smaa_edge_detection.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/blur_down.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/blur_up.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/luminance.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/exposure_adapt.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/depth_pyramid.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/ssao_in_down.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/ssao.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/ssil_in_down.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/ssil.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/ssgi_in_down.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/ssgi.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/ssr_in_down.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/ssr.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/dof_coc.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/dof_down.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/dof_blur.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/bloom_down.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/bloom_up.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/cubemap_from_equirectangular.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/cubemap_irradiance.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/cubemap_prefilter.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/cubemap_procedural_sky.frag"
    "${R3D_ROOT_PATH}/shaders/prepare/cubemap_custom_sky.frag"
    # Scene
    "${R3D_ROOT_PATH}/shaders/scene/scene.vert"
    "${R3D_ROOT_PATH}/shaders/scene/geometry.frag"
    "${R3D_ROOT_PATH}/shaders/scene/forward.frag"
    "${R3D_ROOT_PATH}/shaders/scene/unlit.frag"
    "${R3D_ROOT_PATH}/shaders/scene/depth.frag"
    "${R3D_ROOT_PATH}/shaders/scene/depth_cube.frag"
    "${R3D_ROOT_PATH}/shaders/scene/decal.frag"
    "${R3D_ROOT_PATH}/shaders/scene/skybox.vert"
    "${R3D_ROOT_PATH}/shaders/scene/skybox.frag"
    # Deferred
    "${R3D_ROOT_PATH}/shaders/deferred/ambient.frag"
    "${R3D_ROOT_PATH}/shaders/deferred/lighting.frag"
    "${R3D_ROOT_PATH}/shaders/deferred/compose.frag"
    "${R3D_ROOT_PATH}/shaders/deferred/fog.frag"
    "${R3D_ROOT_PATH}/shaders/deferred/vfog_transmittance.frag"
    "${R3D_ROOT_PATH}/shaders/deferred/vfog_radiance.frag"
    "${R3D_ROOT_PATH}/shaders/deferred/vfog_compose.frag"
    # Post
    "${R3D_ROOT_PATH}/shaders/post/dof.frag"
    "${R3D_ROOT_PATH}/shaders/post/bloom.frag"
    "${R3D_ROOT_PATH}/shaders/post/auto_exposure.frag"
    "${R3D_ROOT_PATH}/shaders/post/screen.frag"
    "${R3D_ROOT_PATH}/shaders/post/output.frag"
    "${R3D_ROOT_PATH}/shaders/post/fxaa.frag"
    "${R3D_ROOT_PATH}/shaders/post/smaa.vert"
    "${R3D_ROOT_PATH}/shaders/post/smaa.frag"
    "${R3D_ROOT_PATH}/shaders/post/visualizer.frag"
    # Blit
    "${R3D_ROOT_PATH}/shaders/blit/common_copy.frag"
    "${R3D_ROOT_PATH}/shaders/blit/common_nearest.frag"
    "${R3D_ROOT_PATH}/shaders/blit/common_linear.frag"
    "${R3D_ROOT_PATH}/shaders/blit/up_bicubic.frag"
    "${R3D_ROOT_PATH}/shaders/blit/up_lanczos.frag"
    "${R3D_ROOT_PATH}/shaders/blit/down_rgss.frag"
    "${R3D_ROOT_PATH}/shaders/blit/down_pdss.frag"
)

embed_assets(${PROJECT_NAME}
    "${R3D_ROOT_PATH}/assets/brdf_lut_512_rg16_float.raw"
    "${R3D_ROOT_PATH}/assets/smaa_area_160x560_rg8.raw"
    "${R3D_ROOT_PATH}/assets/smaa_search_64x16_r8.raw"
)

# ========================================
# Include Directories
# ========================================

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${R3D_ROOT_PATH}/external/tinycthread"
        "${R3D_ROOT_PATH}/external/uthash"
        "${R3D_ROOT_PATH}/external/glad"
        "${R3D_GENERATED_INCLUDE_DIR}"
        "${R3D_ROOT_PATH}/include"
        "${R3D_RAYLIB_INC}"
)

if(R3D_SUPPORT_ASSIMP)
    target_include_directories(${PROJECT_NAME}
        PRIVATE
            "${R3D_ASSIMP_INC}"
    )
endif()

# ========================================
# Link Libraries
# ========================================

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        "${R3D_RAYLIB_LIB}"
)

if(R3D_SUPPORT_ASSIMP)
    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            "${R3D_ASSIMP_LIB}"
    )
endif()

check_library_exists(m cos "" HAVE_LIB_M)
if(HAVE_LIB_M)
    target_link_libraries(${PROJECT_NAME} PUBLIC m)
endif()

# ========================================
# Compiler Settings
# ========================================

if(CMAKE_VERSION VERSION_GREATER 3.12)
    set_property(TARGET ${PROJECT_NAME} PROPERTY C_STANDARD 11)
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /experimental:c11atomics)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ========================================
# Threading Support
# ========================================

cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-std=c11")
    check_c_source_compiles("
        #include <threads.h>
        int dummy_thread(void *arg) { return 0; }
        int main(void) {
            thrd_t t;
            thrd_create(&t, dummy_thread, NULL);
            thrd_join(t, NULL);
            struct timespec ts = {0, 0};
            thrd_sleep(&ts, NULL);
            mtx_t m;
            mtx_init(&m, mtx_plain);
            mtx_lock(&m);
            mtx_unlock(&m);
            mtx_destroy(&m);
            return 0;
        }
    " R3D_HAS_C11_THREADS)
cmake_pop_check_state()

if(NOT R3D_HAS_C11_THREADS)
    message(STATUS "Threading: threads.h unavailable or incomplete, using tinycthread")
    target_sources(${PROJECT_NAME} PRIVATE "${R3D_ROOT_PATH}/external/tinycthread/tinycthread.c")
    target_compile_definitions(${PROJECT_NAME} PRIVATE R3D_NO_C11_THREADS)
else()
    message(STATUS "Threading: using native threads.h")
endif()

# ========================================
# Documentation
# ========================================

if(R3D_BUILD_DOCS)
    find_package(Doxygen QUIET)

    if(NOT Doxygen_FOUND)
        message(FATAL_ERROR "Doxygen not found. Install Doxygen to build documentation.")
    endif()

    set(DOXYGEN_IN ${R3D_ROOT_PATH}/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()

# ========================================
# Examples
# ========================================

if(R3D_BUILD_EXAMPLES)
    include("${R3D_ROOT_PATH}/examples/CMakeLists.txt")
endif()

if(R3D_BUILD_TESTS)
    enable_testing()
    add_executable(r3d_animation_identity_test
        "${R3D_ROOT_PATH}/tests/r3d_animation_identity_test.c"
    )
    target_include_directories(r3d_animation_identity_test PRIVATE
        "${R3D_ROOT_PATH}/include"
        "${R3D_RAYLIB_INC}"
    )
    target_link_libraries(r3d_animation_identity_test PRIVATE r3d)
    add_test(NAME r3d_animation_identity COMMAND r3d_animation_identity_test)
endif()

if(BTBH_BUILD_GPU_SPIKE OR BTBH_BUILD_GPU_PACKAGE_RUNTIME)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${R3D_ROOT_PATH}/external/SDL"
        "${CMAKE_CURRENT_BINARY_DIR}/external/SDL"
        EXCLUDE_FROM_ALL
    )
endif()

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
        "${R3D_ROOT_PATH}/external/SDL_shadercross"
        "${CMAKE_CURRENT_BINARY_DIR}/external/SDL_shadercross"
        EXCLUDE_FROM_ALL
    )
    set(
        BTBH_GPU_SHADER_OUTPUT_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/btbh_gpu/shaders"
    )
    if(APPLE)
        set(BTBH_GPU_RUNTIME_SHADER_EXTENSION "metallib")
    else()
        set(BTBH_GPU_RUNTIME_SHADER_EXTENSION "spv")
    endif()
    if(WIN32 AND TARGET dxcompiler AND TARGET dxildll)
        set_target_properties(
            dxcompiler
            dxildll
            PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${R3D_RUNTIME_OUTPUT_DIRECTORY}"
        )
    endif()

    set(BTBH_GPU_SHADER_ARTIFACTS)
    set(BTBH_GPU_SHADER_SPECS
        "vertical_spike.vert|vertex"
        "vertical_spike.frag|fragment"
        "vertical_spike.comp|compute"
        "phase2_shadow.vert|vertex"
        "phase2_shadow.frag|fragment"
        "phase2_gbuffer.vert|vertex"
        "phase2_gbuffer.frag|fragment"
        "phase2_interval.comp|compute"
        "phase2_lighting.vert|vertex"
        "phase2_lighting.frag|fragment"
        "phase2_transparent.frag|fragment"
        "production_model.vert|vertex"
        "production_animated_model.vert|vertex"
        "production_shadow.vert|vertex"
        "production_animated_shadow.vert|vertex"
        "production_shadow.frag|fragment"
        "production_model.frag|fragment"
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
            "${R3D_ROOT_PATH}/shaders/gpu_spike/${_shader_name}.hlsl"
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
                -o "${_shader_spirv}"
            DEPENDS
                shadercross
                "${_shader_source}"
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

        if(APPLE)
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

    add_custom_target(
        btbh_gpu_shaders
        DEPENDS ${BTBH_GPU_SHADER_ARTIFACTS}
    )

    add_library(btbh_gpu STATIC
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_canvas.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_contract.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_custom_mesh.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_device.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_font.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_frame.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_instance_buffer.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_model.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_phase2_runtime.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_resources.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_scene.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_spike_scene.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_texture.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_vertical_smoke.c"
        "${R3D_ROOT_PATH}/src/gpu/btbh_gpu_window.c"
    )
    target_include_directories(btbh_gpu
        PUBLIC
            "${R3D_ROOT_PATH}/include"
        PRIVATE
            "${R3D_ROOT_PATH}/external/raylib/src/external"
    )
    target_compile_definitions(btbh_gpu PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(btbh_gpu PRIVATE SDL3::SDL3-static)
    if(WIN32)
        target_include_directories(
            btbh_gpu
            PRIVATE
                "${R3D_ASSIMP_INC}"
        )
        target_link_libraries(btbh_gpu PRIVATE ${R3D_ASSIMP_LIB})
    endif()
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

    add_executable(btbh_gpu_contract_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_contract_test.c"
    )
    target_link_libraries(btbh_gpu_contract_test PRIVATE btbh_gpu)
    add_test(NAME btbh_gpu_contract COMMAND btbh_gpu_contract_test)

    add_executable(btbh_gpu_public_header_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_public_header_test.c"
    )
    target_compile_definitions(
        btbh_gpu_public_header_test
        PRIVATE
            BTBH_GPU_PUBLIC_HEADER_PATH="${R3D_ROOT_PATH}/include/btbh_gpu/btbh_gpu.h"
    )
    add_test(
        NAME btbh_gpu_public_header
        COMMAND btbh_gpu_public_header_test
    )

    add_executable(btbh_gpu_resources_public_header_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_public_header_test.c"
    )
    target_compile_definitions(
        btbh_gpu_resources_public_header_test
        PRIVATE
            BTBH_GPU_PUBLIC_HEADER_PATH="${R3D_ROOT_PATH}/include/btbh_gpu/btbh_gpu_resources.h"
    )
    add_test(
        NAME btbh_gpu_resources_public_header
        COMMAND btbh_gpu_resources_public_header_test
    )

    add_executable(btbh_gpu_resource_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_resource_test.c"
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

    add_executable(btbh_gpu_spike_scene_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_spike_scene_test.c"
    )
    target_include_directories(btbh_gpu_spike_scene_test
        PRIVATE "${R3D_ROOT_PATH}/src/gpu"
    )
    target_link_libraries(btbh_gpu_spike_scene_test PRIVATE btbh_gpu)
    if(MSVC)
        target_compile_options(btbh_gpu_spike_scene_test PRIVATE /W4 /WX)
    else()
        target_compile_options(btbh_gpu_spike_scene_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()
    add_test(NAME btbh_gpu_spike_scene COMMAND btbh_gpu_spike_scene_test)

    add_executable(btbh_gpu_phase2_shader_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_phase2_shader_test.c"
    )
    target_compile_definitions(btbh_gpu_phase2_shader_test
        PRIVATE
            BTBH_SHADOW_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_shadow.vert.json"
            BTBH_GBUFFER_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_gbuffer.vert.json"
            BTBH_GBUFFER_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_gbuffer.frag.json"
            BTBH_INTERVAL_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_interval.comp.json"
            BTBH_LIGHTING_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_lighting.frag.json"
            BTBH_TRANSPARENT_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_transparent.frag.json"
            BTBH_PRODUCTION_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model.vert.json"
            BTBH_PRODUCTION_ANIMATED_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_animated_model.vert.json"
            BTBH_PRODUCTION_SHADOW_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_shadow.vert.json"
            BTBH_PRODUCTION_ANIMATED_SHADOW_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_animated_shadow.vert.json"
            BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/production_model.frag.json"
    )
    add_dependencies(btbh_gpu_phase2_shader_test btbh_gpu_shaders)
    add_test(NAME btbh_gpu_phase2_shaders COMMAND btbh_gpu_phase2_shader_test)

    function(btbh_configure_phase2_runtime_target target)
        target_include_directories(${target}
            PRIVATE "${R3D_ROOT_PATH}/src/gpu"
        )
        target_compile_definitions(${target}
            PRIVATE
            BTBH_PHASE2_SHADOW_VERTEX_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_shadow.vert.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
            BTBH_PHASE2_SHADOW_FRAGMENT_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_shadow.frag.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
            BTBH_PHASE2_GBUFFER_VERTEX_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_gbuffer.vert.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
            BTBH_PHASE2_GBUFFER_FRAGMENT_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_gbuffer.frag.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
            BTBH_PHASE2_PATTERN_COMPUTE_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.comp.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
            BTBH_PHASE2_INTERVAL_COMPUTE_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_interval.comp.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
            BTBH_PHASE2_LIGHTING_VERTEX_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_lighting.vert.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
            BTBH_PHASE2_LIGHTING_FRAGMENT_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_lighting.frag.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
            BTBH_PHASE2_TRANSPARENT_FRAGMENT_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/phase2_transparent.frag.${BTBH_GPU_RUNTIME_SHADER_EXTENSION}"
        )
        add_dependencies(${target} btbh_gpu_shaders)
    endfunction()

    add_executable(btbh_gpu_phase2_runtime_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_phase2_runtime_test.c"
    )
    target_compile_definitions(btbh_gpu_phase2_runtime_test
        PRIVATE
            BTBH_PHASE2_CAPTURE_PATH="${CMAKE_CURRENT_BINARY_DIR}/btbh_gpu_phase2_runtime_test.bmp"
    )
    target_link_libraries(
        btbh_gpu_phase2_runtime_test
        PRIVATE btbh_gpu SDL3::SDL3-static
    )
    btbh_configure_phase2_runtime_target(btbh_gpu_phase2_runtime_test)
    add_test(NAME btbh_gpu_phase2_runtime COMMAND btbh_gpu_phase2_runtime_test)

    add_executable(btbh_gpu_phase2_runtime
        "${R3D_ROOT_PATH}/spikes/sdl_gpu/phase2_runtime_main.c"
    )
    target_link_libraries(
        btbh_gpu_phase2_runtime
        PRIVATE btbh_gpu SDL3::SDL3-static
    )
    btbh_configure_phase2_runtime_target(btbh_gpu_phase2_runtime)

    add_executable(btbh_gpu_phase2_metrics
        "${R3D_ROOT_PATH}/spikes/sdl_gpu/phase2_metrics_main.c"
    )
    target_link_libraries(
        btbh_gpu_phase2_metrics
        PRIVATE btbh_gpu SDL3::SDL3-static
    )
    btbh_configure_phase2_runtime_target(btbh_gpu_phase2_metrics)
    add_test(
        NAME btbh_gpu_phase2_metrics
        COMMAND
            btbh_gpu_phase2_metrics
            "${CMAKE_CURRENT_BINARY_DIR}/btbh_gpu_phase2_metrics_test.bmp"
            320
            180
            1
            3
    )

    add_executable(btbh_gpu_device_smoke
        "${R3D_ROOT_PATH}/tests/btbh_gpu_device_smoke.c"
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
        "${R3D_ROOT_PATH}/tests/btbh_gpu_window_smoke.c"
    )
    target_compile_definitions(btbh_gpu_window_smoke PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(
        btbh_gpu_window_smoke
        PRIVATE
            btbh_gpu
            SDL3::SDL3-static
    )
    add_test(NAME btbh_gpu_window_smoke COMMAND btbh_gpu_window_smoke)

    add_executable(btbh_gpu_shader_artifact_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_shader_artifact_test.c"
    )
    target_compile_definitions(
        btbh_gpu_shader_artifact_test
        PRIVATE
            BTBH_VERTEX_SPIRV_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.vert.spv"
            BTBH_VERTEX_MSL_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.vert.msl"
            BTBH_VERTEX_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.vert.json"
            BTBH_FRAGMENT_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.frag.json"
            BTBH_COMPUTE_REFLECTION_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.comp.json"
    )
    add_dependencies(btbh_gpu_shader_artifact_test btbh_gpu_shaders)
    add_test(
        NAME btbh_gpu_shader_artifacts
        COMMAND btbh_gpu_shader_artifact_test
    )

    function(btbh_configure_vertical_smoke_target target)
        target_include_directories(
            ${target}
            PRIVATE
                "${R3D_ROOT_PATH}/src/gpu"
        )
        if(APPLE)
            target_compile_definitions(
                ${target}
                PRIVATE
                    BTBH_VERTEX_SHADER_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.vert.metallib"
                    BTBH_FRAGMENT_SHADER_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.frag.metallib"
                    BTBH_COMPUTE_SHADER_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.comp.metallib"
                    BTBH_SHADER_ENTRYPOINT="main0"
            )
        else()
            target_compile_definitions(
                ${target}
                PRIVATE
                    BTBH_VERTEX_SHADER_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.vert.spv"
                    BTBH_FRAGMENT_SHADER_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.frag.spv"
                    BTBH_COMPUTE_SHADER_PATH="${BTBH_GPU_SHADER_OUTPUT_DIR}/vertical_spike.comp.spv"
                    BTBH_SHADER_ENTRYPOINT="main"
            )
        endif()
        add_dependencies(${target} btbh_gpu_shaders)
    endfunction()

    add_executable(btbh_gpu_vertical_smoke_test
        "${R3D_ROOT_PATH}/tests/btbh_gpu_vertical_smoke_test.c"
    )
    target_compile_definitions(
        btbh_gpu_vertical_smoke_test
        PRIVATE
            BTBH_CAPTURE_PATH="${CMAKE_CURRENT_BINARY_DIR}/btbh_gpu_vertical_smoke_test.bmp"
    )
    target_link_libraries(
        btbh_gpu_vertical_smoke_test
        PRIVATE
            btbh_gpu
            SDL3::SDL3-static
    )
    btbh_configure_vertical_smoke_target(btbh_gpu_vertical_smoke_test)
    add_test(
        NAME btbh_gpu_vertical_smoke
        COMMAND btbh_gpu_vertical_smoke_test
    )

    add_executable(btbh_gpu_vertical_smoke
        "${R3D_ROOT_PATH}/spikes/sdl_gpu/vertical_smoke_main.c"
    )
    target_link_libraries(
        btbh_gpu_vertical_smoke
        PRIVATE
            btbh_gpu
            SDL3::SDL3-static
    )
    btbh_configure_vertical_smoke_target(btbh_gpu_vertical_smoke)

    add_executable(btbh_gpu_spike
        "${R3D_ROOT_PATH}/spikes/sdl_gpu/main.c"
    )
    target_compile_definitions(btbh_gpu_spike PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(
        btbh_gpu_spike
        PRIVATE
            btbh_gpu
            SDL3::SDL3-static
    )
endif()
