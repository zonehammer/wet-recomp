cmake_minimum_required(VERSION 3.20)
project(WetRecomp C CXX)

# Release version (VERSION file) plus the git commit, e.g. "0.1.0 (f421997)".
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION WET_VERSION)
string(STRIP "${WET_VERSION}" WET_VERSION)
find_package(Git QUIET)
set(WET_COMMIT "")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE WET_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
set(WET_VERSION_STRING "${WET_VERSION}")
if(WET_COMMIT)
    set(WET_VERSION_STRING "${WET_VERSION} (${WET_COMMIT})")
endif()
# A generated header (only the files that show the version include it, so a new
# commit doesn't rebuild everything).
file(WRITE ${CMAKE_BINARY_DIR}/wet_version.h.tmp "#pragma once\n#define WET_VERSION \"${WET_VERSION_STRING}\"\n")
configure_file(${CMAKE_BINARY_DIR}/wet_version.h.tmp ${CMAKE_BINARY_DIR}/generated_include/wet_version.h COPYONLY)
set(WET_VERSION_INCLUDE ${CMAKE_BINARY_DIR}/generated_include)
# Re-run configure when the version or the checked-out commit changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD WET_GIT_HEAD)
    if(WET_GIT_HEAD MATCHES "^ref: (.*)\n?$")
        string(STRIP "${CMAKE_MATCH_1}" WET_GIT_REF)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git/${WET_GIT_REF})
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/.git/${WET_GIT_REF})
        endif()
    endif()
endif()
message(STATUS "WET Recomp ${WET_VERSION_STRING}")

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
# Applies patches/<patch> to a submodule once (re-applies from a clean checkout
# when the patch set changes).
function(wet_apply_patch dir patch)
    set(file ${WET_ROOT}/patches/${patch})
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${file}
        WORKING_DIRECTORY ${dir} RESULT_VARIABLE present OUTPUT_QUIET ERROR_QUIET)
    if(NOT present EQUAL 0)
        execute_process(COMMAND ${GIT_EXECUTABLE} checkout -- . WORKING_DIRECTORY ${dir} OUTPUT_QUIET ERROR_QUIET)
        # checkout doesn't remove files an earlier version of the patch added.
        file(READ ${file} text)
        string(REGEX MATCHALL "\n--- /dev/null\n\\+\\+\\+ b/[^\n]+" added "\n${text}")
        foreach(entry ${added})
            string(REGEX REPLACE ".*\\+\\+\\+ b/" "" path "${entry}")
            string(STRIP "${path}" path)
            file(REMOVE ${dir}/${path})
        endforeach()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${file}
            WORKING_DIRECTORY ${dir} RESULT_VARIABLE result)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Could not apply patches/${patch} to ${dir}.\n"
                "Reset the submodule (git -C ${dir} checkout .) and configure again.")
        endif()
        message(STATUS "Applied patches/${patch}")
    endif()
endfunction()
wet_apply_patch(${XR_DIR} xenonrecomp-wet.patch)

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
# Renderer: Xenia's GPU emulation (Vulkan)
# ---------------------------------------------------------------------------
option(WET_RENDERER "Build the Vulkan renderer (Xenia's GPU emulation)" ON)
if(WET_RENDERER)
    include(cmake/xenia.cmake)
endif()

include(cmake/sdl.cmake)

# ---------------------------------------------------------------------------
# WET Launcher: settings window that edits wet.ini and starts the game (Windows)
# ---------------------------------------------------------------------------
if(WIN32)
    enable_language(RC)
    add_executable(WetLauncher WIN32 launcher/launcher.cpp launcher/launcher.rc)
    set_target_properties(WetLauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime)
    target_compile_definitions(WetLauncher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_include_directories(WetLauncher PRIVATE ${WET_VERSION_INCLUDE})
    target_link_libraries(WetLauncher PRIVATE comctl32 shell32)
    # Optional icon (launcher/launcher.ico, not in git: use your own image).
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.ico)
        set_source_files_properties(launcher/launcher.rc PROPERTIES COMPILE_DEFINITIONS WET_LAUNCHER_ICON
                                    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.ico)
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/launcher)
    if(MINGW)
        target_link_options(WetLauncher PRIVATE -municode)
    endif()
endif()

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
