# Xenia's GPU emulation (Vulkan backend) built as static libraries for the runtime.
#
# Only the pieces the renderer needs are compiled: xenia-base, the Vulkan parts of
# xenia-ui, xenia-gpu (without the D3D12 backend, the trace tools and the shader
# compiler app) and their third-party dependencies. The handful of emulator
# interfaces xenia-gpu expects (xe::Memory, the kernel's host threads, ...) are
# provided by runtime/xenia_shim, which comes first on the include path.
#
# Xenia is BSD-licensed; see thirdparty/xenia/LICENSE.

set(XE_ROOT ${WET_ROOT}/thirdparty/xenia)
set(XE_SRC ${XE_ROOT}/src/xenia)
set(XE_TP ${XE_ROOT}/third_party)

foreach(dep fmt glslang Vulkan-Headers VulkanMemoryAllocator xxhash snappy utfcpp xbyak disruptorplus cpptoml date cxxopts)
    if(NOT EXISTS ${XE_TP}/${dep}/.git)
        message(FATAL_ERROR "Xenia dependency third_party/${dep} is missing. Run:\n  python tools/build.py\n"
                            "(or: python tools/fetch_deps.py)")
    endif()
endforeach()

# Xenia's platform-suffix convention (tools/build/scripts/platform_files.lua):
# foo.cc everywhere, foo_win.cc on Windows, foo_posix.cc / foo_linux.cc /
# foo_gnulinux.cc on Linux; *_main.cc and *_test.cc never.
function(xe_platform_files out dir)
    file(GLOB all CONFIGURE_DEPENDS ${dir}/*.cc ${dir}/*.cpp ${dir}/*.c)
    set(result)
    foreach(f ${all})
        get_filename_component(n ${f} NAME_WE)
        if(n MATCHES "_main$|_test$|_demo$")
            continue()
        endif()
        if(n MATCHES "_(win|posix|linux|gnulinux|x11|gtk|android|mac)$")
            set(sfx ${CMAKE_MATCH_1})
            if(WIN32 AND NOT sfx STREQUAL "win")
                continue()
            endif()
            if(NOT WIN32 AND NOT sfx MATCHES "^(posix|linux|gnulinux)$")
                continue()
            endif()
        endif()
        list(APPEND result ${f})
    endforeach()
    set(${out} ${result} PARENT_SCOPE)
endfunction()

function(xe_target name)
    set_target_properties(${name} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${name} BEFORE PUBLIC ${WET_ROOT}/runtime/xenia_shim)
    target_include_directories(${name} PUBLIC ${XE_ROOT} ${XE_ROOT}/src ${XE_TP}
                               ${XE_TP}/Vulkan-Headers/include ${XE_TP}/fmt/include)
    target_compile_definitions(${name} PUBLIC UNICODE _UNICODE NDEBUG _LIB XE_WET_RUNTIME=1)
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8 /W0 -Wno-everything)
    else()
        target_compile_options(${name} PRIVATE -w -mavx -fno-strict-aliasing)
    endif()
    if(MINGW)
        target_compile_definitions(${name} PUBLIC WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX)
    endif()
endfunction()

# ---- third party ----
add_library(xe_fmt STATIC ${XE_TP}/fmt/src/format.cc ${XE_TP}/fmt/src/os.cc)
xe_target(xe_fmt)

add_library(xe_xxhash STATIC ${XE_TP}/xxhash/xxhash.c)
xe_target(xe_xxhash)

add_library(xe_snappy STATIC ${XE_TP}/snappy/snappy.cc ${XE_TP}/snappy/snappy-sinksource.cc
                             ${XE_TP}/snappy/snappy-stubs-internal.cc)
xe_target(xe_snappy)
target_include_directories(xe_snappy PRIVATE ${WET_ROOT}/runtime/xenia_shim/snappy)

set(GLSLANG_SPIRV ${XE_TP}/glslang/SPIRV)
add_library(xe_glslang_spirv STATIC ${GLSLANG_SPIRV}/disassemble.cpp ${GLSLANG_SPIRV}/doc.cpp
            ${GLSLANG_SPIRV}/InReadableOrder.cpp ${GLSLANG_SPIRV}/Logger.cpp ${GLSLANG_SPIRV}/SpvBuilder.cpp)
xe_target(xe_glslang_spirv)

# ---- xenia-base ----
xe_platform_files(XE_BASE_SOURCES ${XE_SRC}/base)
list(FILTER XE_BASE_SOURCES EXCLUDE REGEX "/(console_app_main|main_init|main)_[a-z]+\\.cc$|/socket_win\\.cc$|/system_(gnulinux|win)\\.cc$")
add_library(xe_base STATIC ${XE_BASE_SOURCES})
xe_target(xe_base)
target_link_libraries(xe_base PUBLIC xe_fmt)
if(NOT WIN32)
    target_link_libraries(xe_base PUBLIC dl pthread rt)
endif()

# ---- xenia-ui (only what the Vulkan presenter needs) ----
set(XE_UI_SOURCES ${XE_SRC}/ui/presenter.cc ${XE_SRC}/ui/graphics_upload_buffer_pool.cc
                  ${XE_SRC}/ui/graphics_util.cc ${XE_SRC}/ui/immediate_drawer.cc
                  ${XE_SRC}/ui/renderdoc_api.cc ${XE_SRC}/ui/window.cc ${XE_SRC}/ui/windowed_app_context.cc
                  ${XE_SRC}/ui/menu_item.cc)
if(WIN32)
    list(APPEND XE_UI_SOURCES ${XE_SRC}/ui/surface_win.cc ${XE_SRC}/ui/window_win.cc
                              ${XE_SRC}/ui/windowed_app_context_win.cc)
endif()
xe_platform_files(XE_UI_VULKAN_SOURCES ${XE_SRC}/ui/vulkan)
xe_platform_files(XE_UI_VULKAN_FN_SOURCES ${XE_SRC}/ui/vulkan/functions)
add_library(xe_ui STATIC ${XE_UI_SOURCES} ${XE_UI_VULKAN_SOURCES} ${XE_UI_VULKAN_FN_SOURCES})
xe_target(xe_ui)
target_link_libraries(xe_ui PUBLIC xe_base)

# ---- xenia-gpu (Vulkan) ----
xe_platform_files(XE_GPU_SOURCES ${XE_SRC}/gpu)
list(FILTER XE_GPU_SOURCES EXCLUDE REGEX "/(dxbc_[a-z_]*|trace_dump|trace_viewer|trace_player|shader_compiler_main)\\.cc$")
xe_platform_files(XE_GPU_VULKAN_SOURCES ${XE_SRC}/gpu/vulkan)
list(FILTER XE_GPU_VULKAN_SOURCES EXCLUDE REGEX "/vulkan_(trace_[a-z_]*)\\.cc$")
add_library(xe_gpu STATIC ${XE_GPU_SOURCES} ${XE_GPU_VULKAN_SOURCES})
xe_target(xe_gpu)
target_link_libraries(xe_gpu PUBLIC xe_ui xe_base xe_glslang_spirv xe_xxhash xe_snappy)
