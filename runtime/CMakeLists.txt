# ---------------------------------------------------------------------------
# WetPPC: the recompiled game code (generated/ppc), compiled as one static lib.
# ---------------------------------------------------------------------------
file(GLOB WET_PPC_SOURCES CONFIGURE_DEPENDS ${WET_GENERATED_DIR}/ppc_recomp.*.cpp)
add_library(WetPPC STATIC ${WET_PPC_SOURCES} ${WET_GENERATED_DIR}/ppc_func_mapping.cpp)

target_include_directories(WetPPC PUBLIC
    ${WET_GENERATED_DIR}
    ${XR_DIR}/thirdparty/simde
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every generated file sees our hooks first (timebase, checked indirect calls).
set(WET_HOOKS_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/wet_ppc_hooks.h)

if(MSVC) # clang-cl
    target_compile_options(WetPPC PUBLIC /FI${WET_HOOKS_HEADER} -msse4.1)
    # Always optimise the generated code: unoptimised it uses enormous stack frames.
    target_compile_options(WetPPC PRIVATE /W0 /O2 /clang:-fno-strict-aliasing)
else()
    target_compile_options(WetPPC PUBLIC -include ${WET_HOOKS_HEADER} -msse4.1)
    target_compile_options(WetPPC PRIVATE -w -O2 -fno-strict-aliasing)
endif()

# The 113k-line declarations header is included by all ~440 files.
target_precompile_headers(WetPPC PRIVATE ${WET_GENERATED_DIR}/ppc_recomp_shared.h)

# ---------------------------------------------------------------------------
# WetRecomp: the host program
# ---------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

file(GLOB_RECURSE WET_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)
file(GLOB_RECURSE WET_KERNEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/kernel/*.cpp)

# Logging stubs for every import not implemented under src/kernel.
set(WET_STUBS_CPP ${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp)
add_custom_command(
    OUTPUT ${WET_STUBS_CPP}
    COMMAND Python3::Interpreter ${WET_ROOT}/tools/gen_import_stubs.py
            ${WET_GENERATED_DIR}/ppc_recomp_shared.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/kernel
            ${WET_STUBS_CPP}
    DEPENDS ${WET_ROOT}/tools/gen_import_stubs.py ${WET_GENERATED_DIR}/ppc_recomp_shared.h ${WET_KERNEL_SOURCES}
    COMMENT "Generating import stubs")

add_executable(WetRecomp ${WET_RUNTIME_SOURCES} ${WET_STUBS_CPP})
# XenonUtils headers come via the XenonUtils link (after WetPPC, so the generated
# ppc_context.h wins over the template of the same name).
target_include_directories(WetRecomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${XR_DIR}/thirdparty/TinySHA1)
target_link_libraries(WetRecomp PRIVATE WetPPC XenonUtils Threads::Threads)

if(WIN32)
    target_compile_definitions(WetRecomp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(MINGW)
        target_link_libraries(WetRecomp PRIVATE xinput1_4)
        # Guest code recurses deeply; give the main thread a big stack too.
        target_link_options(WetRecomp PRIVATE -Wl,--stack,16777216)
    else()
        target_link_libraries(WetRecomp PRIVATE xinput)
    endif()
endif()
if(MSVC)
    target_compile_options(WetRecomp PRIVATE /clang:-fno-strict-aliasing -Wno-unused-parameter)
    # Guest code recurses deeply; give the main thread a big stack too.
    target_link_options(WetRecomp PRIVATE /STACK:16777216)
else()
    target_compile_options(WetRecomp PRIVATE -fno-strict-aliasing -Wall -Wno-unused-parameter -Wno-unused-function)
endif()
