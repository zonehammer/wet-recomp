# SDL2 (Xenia's submodule) for game controllers: PlayStation 4/5 (DualShock 4,
# DualSense) and others over HIDAPI, plus Xbox pads, with hot-plugging and rumble.
# Only the joystick side is used: no audio or rendering (the video subsystem is
# compiled, as SDL's Windows core needs it, but never started). Windows only; the
# runtime falls back to XInput if SDL is missing or fails to start.
set(WET_SDL_DIR ${WET_ROOT}/thirdparty/xenia/third_party/SDL2)
if(WIN32 AND EXISTS ${WET_SDL_DIR}/CMakeLists.txt)
    foreach(opt SDL_SHARED SDL_TEST SDL2_DISABLE_INSTALL SDL2_DISABLE_UNINSTALL SDL2_DISABLE_SDL2MAIN
                SDL_AUDIO SDL_RENDER SDL_POWER SDL_FILESYSTEM SDL_LOCALE SDL_MISC
                SDL_OPENGL SDL_OPENGLES SDL_VULKAN SDL_DIRECTX SDL_WASAPI SDL_RENDER_D3D
                SDL_DISKAUDIO SDL_DUMMYAUDIO SDL_OFFSCREEN)
        set(value OFF)
        if(opt MATCHES "^SDL2_DISABLE")
            set(value ON)
        endif()
        set(${opt} ${value} CACHE BOOL "" FORCE)
    endforeach()
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_STATIC_PIC OFF CACHE BOOL "" FORCE)
    add_subdirectory(${WET_SDL_DIR} ${CMAKE_BINARY_DIR}/thirdparty/SDL2 EXCLUDE_FROM_ALL)
    set(WET_HAVE_SDL ON)
    message(STATUS "Controllers: SDL2 (PlayStation, Xbox and other pads)")
else()
    set(WET_HAVE_SDL OFF)
    message(STATUS "Controllers: XInput only (SDL2 not fetched)")
endif()
