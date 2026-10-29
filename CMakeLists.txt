cmake_minimum_required(VERSION 3.20)
project(WetRecomp C CXX)

# The generated code relies on GNU extensions (weak/alias attributes, __int128,
# __builtin_*), so the whole project is built with Clang. On Windows that means
# clang-cl from Visual Studio's "C++ Clang tools for Windows" component.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "WET Recomp must be built with Clang.\n"
        "Windows: install Visual Studio 2022 with 'C++ Clang tools for Windows', then configure with\n"
        "  cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF)

set(WET_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(XR_DIR ${WET_ROOT}/thirdparty/XenonRecomp)
set(WET_GENERATED_DIR ${WET_ROOT}/generated/ppc)

# CI: build and link the whole runtime against a tiny stand-in for the generated
# code (ci/stub_ppc), so Windows builds can be checked without any game files.
option(WET_CI_STUB "Build against ci/stub_ppc instead of generated/ppc (no game files needed)" OFF)
if(WET_CI_STUB)
    set(WET_GENERATED_DIR ${WET_ROOT}/ci/stub_ppc)
endif()

# MinGW (llvm-mingw, MSYS2 clang64): link the C++ runtime statically so the
# executables run without extra DLLs next to them.
if(MINGW)
    add_link_options(-static)
    # The fmt bundled with XenonRecomp uses malloc/free without including
    # <cstdlib>; libc++ (llvm-mingw) doesn't provide them transitively.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdlib>")
endif()

# ---------------------------------------------------------------------------
# XenonRecomp (submodule) + the WET patch set
# ---------------------------------------------------------------------------
if(NOT EXISTS ${XR_DIR}/CMakeLists.txt OR NOT EXISTS ${XR_DIR}/thirdparty/simde/x86/sse.h)
    message(FATAL_ERROR "XenonRecomp submodule missing. Run:\n  git submodule update --init --recursive")
endif()

find_package(Git REQUIRED)
set(WET_PATCH ${WET_ROOT}/patches/xenonrecomp-wet.patch)
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${WET_PATCH}
    WORKING_DIRECTORY ${XR_DIR} RESULT_VARIABLE WET_PATCH_PRESENT OUTPUT_QUIET ERROR_QUIET)
if(NOT WET_PATCH_PRESENT EQUAL 0)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${WET_PATCH}
        WORKING_DIRECTORY ${XR_DIR} RESULT_VARIABLE WET_PATCH_RESULT)
    if(NOT WET_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not apply patches/xenonrecomp-wet.patch to thirdparty/XenonRecomp.\n"
            "Reset the submodule (git -C thirdparty/XenonRecomp checkout .) and configure again.")
    endif()
    message(STATUS "Applied WET patches to XenonRecomp")
endif()

add_subdirectory(${XR_DIR} thirdparty/XenonRecomp EXCLUDE_FROM_ALL)

# `cmake --build build --target recompile` : default.xex -> generated/ppc/*.cpp
add_custom_target(recompile
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WET_GENERATED_DIR}
    COMMAND $<TARGET_FILE:XenonRecomp> ${WET_ROOT}/config/WET_config.toml ${XR_DIR}/XenonUtils/ppc_context.h
    WORKING_DIRECTORY ${WET_ROOT}/config
    DEPENDS XenonRecomp
    COMMENT "Recompiling game/extracted/default.xex into generated/ppc (re-run CMake configure afterwards)"
    VERBATIM)

# ---------------------------------------------------------------------------
# Runtime (only once the generated code exists)
# ---------------------------------------------------------------------------
if(EXISTS ${WET_GENERATED_DIR}/ppc_func_mapping.cpp)
    add_subdirectory(runtime)
else()
    message(WARNING
        "generated/ppc/ does not exist yet, so the game itself is not part of this build.\n"
        "  1. python tools/prepare_game.py <your WET .iso>\n"
        "  2. cmake --build build --target recompile\n"
        "  3. cmake build   (re-configure; the WetRecomp target appears)\n"
        "  4. cmake --build build")
endif()
