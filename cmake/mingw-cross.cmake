# Cross-compiling the Windows build from Linux (Steam Deck, Bazzite, ...) with
# llvm-mingw: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-cross.cmake -DLLVM_MINGW=<dir>
# (or with llvm-mingw's bin folder on PATH). The result runs under Proton / Wine.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT LLVM_MINGW AND DEFINED ENV{LLVM_MINGW})
    set(LLVM_MINGW $ENV{LLVM_MINGW})
endif()
if(NOT LLVM_MINGW)
    find_program(_wet_mingw_clang x86_64-w64-mingw32-clang++)
    if(_wet_mingw_clang)
        get_filename_component(LLVM_MINGW ${_wet_mingw_clang} DIRECTORY)
        get_filename_component(LLVM_MINGW ${LLVM_MINGW} DIRECTORY)
    endif()
endif()
if(NOT LLVM_MINGW OR NOT EXISTS ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
    message(FATAL_ERROR "llvm-mingw not found: pass -DLLVM_MINGW=<its folder> or put its bin folder on PATH.")
endif()
set(LLVM_MINGW ${LLVM_MINGW} CACHE PATH "llvm-mingw toolchain folder" FORCE)
# CMake's compiler checks run this file again in their own projects.
set(ENV{LLVM_MINGW} ${LLVM_MINGW})
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW)

set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Sources include <Windows.h>, <WinSock2.h>, ...; the mingw headers are lower case.
set(CMAKE_C_FLAGS_INIT "-idirafter \"${CMAKE_CURRENT_LIST_DIR}/wincase\"")
set(CMAKE_CXX_FLAGS_INIT "-idirafter \"${CMAKE_CURRENT_LIST_DIR}/wincase\"")
