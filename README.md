# WET Recompiled

A native Windows port of **WET** (Xbox 360, 2009) made by **static recompilation**: the
game's PowerPC code is translated to C++ and compiled for x64, then run on a small
runtime that plays the part of the console. Graphics and audio use
[Xenia](https://github.com/xenia-project/xenia)'s GPU and audio emulation; the
recompiler is [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

**This repository contains no game data.** You need your own copy of WET (title ID
`425307DB`). The build extracts your disc and generates the C++ on your machine;
nothing derived from the game is ever committed.

## Features

- Plays through the game: Direct3D 12 rendering (Vulkan fallback), full audio, saves,
  intro and cutscene videos.
- **30 / 60 / 120 / 144 FPS** at the correct game speed.
- **Resolution scaling** to 1440p or 4K, **AMD FSR / CAS** and **NVIDIA Image Scaling**,
  FXAA, and a **film grain** slider.
- **Controllers**: Xbox, PlayStation 4/5 (USB or Bluetooth), Switch Pro and others, up
  to four, with rumble.
- **Keyboard and mouse**: WASD movement and mouse aim.
- **Achievements** with on-screen pop-ups and a list in the overlay.
- **Shader cache**, so shader compilation stutter only happens once.
- An in-game settings overlay (**F1**) and a **launcher** with settings and a Play button.

## Building (Windows 10/11, 64-bit)

You need your WET disc image and a Clang toolchain. Visual Studio is **not** required.

**1. Install the tools** (once):

- [Python 3](https://www.python.org/downloads/) (tick "Add python.exe to PATH") and
  [Git for Windows](https://git-scm.com).
- CMake and Ninja: `pip install cmake ninja`
- Clang: the latest **llvm-mingw** release (`llvm-mingw-<date>-ucrt-x86_64.zip` from
  [github.com/mstorsjo/llvm-mingw/releases](https://github.com/mstorsjo/llvm-mingw/releases)),
  unzipped to e.g. `C:\llvm-mingw`, with its `bin` folder added to your `PATH`.

**2. Build** (Command Prompt or PowerShell):

```bat
git clone --recursive https://github.com/zonehammer/wet-recomp.git
cd wet-recomp
python tools\build.py --iso "D:\path\to\WET.iso"
```

This extracts the disc into `game\extracted`, recompiles `default.xex` into
`generated\ppc` and builds everything. The first build compiles ~250 MB of generated C++
(5–15 minutes on a modern CPU); later, `python tools\build.py` only redoes what changed.

*With Visual Studio 2022 and "C++ Clang tools for Windows"*, run
`python tools\build.py --toolchain clang-cl` from an x64 Native Tools prompt instead.

**3. Play**: `build\runtime\WetLauncher.exe` (settings, then Play) or
`build\runtime\WetRecomp.exe` (straight in).

**Ready-to-run folder**: `python tools\package.py --with-game --with-saves --zip` makes
`dist\WET-Recomp-<version>` with the launcher, the game exe, a player README and the
licence notices. It contains code translated from your copy of the game, so it is for
**your own machines only**: don't upload or share it.

## Controls

| Action | Controller | Keyboard / mouse |
|---|---|---|
| Move | Left stick | WASD |
| Camera / aim | Right stick | Mouse |
| Shoot / LT | RT / LT | Left / right click |
| A / B / X / Y | A / B / X / Y (PlayStation: Cross / Circle / Square / Triangle) | Space / Esc / X / Y |
| Bumpers / triggers | LB / RB, LT / RT | Q / E, Z / C |
| D-pad, Start, Back | D-pad, Start (Options), Back (Share / touchpad) | Arrows, Enter, Backspace |

| Key | |
|---|---|
| F1 | Settings overlay (frame rate, resolution, scaling, film grain, mouse, controllers, achievements); frees the mouse |
| F6 | Cycle the frame rate |
| F11 | Fullscreen |
| F8 | Save the next 48 frames to `frames\` (bug reports) |
| F10 | Profiler on/off (`profile.txt`, bug reports) |

Settings live in `wet.ini` next to the exe; the mouse buttons can be rebound there
(`mouse_left = RT` ...). Saves, `achievements.ini` and the shader cache are in
`game\hdd`. `WetRecomp.exe --console` shows the log live; `wet.log` always has it.

## How it works

1. `tools/prepare_game.py` extracts the disc into `game/extracted/`.
2. A patched XenonRecomp translates `default.xex` into ~440 C++ files in `generated/ppc/`,
   guided by `config/WET_config.toml` and `config/WET_switch_tables.toml` (jump tables found
   by `tools/find_jump_tables.py`).
3. `runtime/` provides what the console did: guest memory with the 0xA/0xC/0xE physical
   views, threads and synchronisation, files, input, achievements and ~240 kernel/XAM
   functions.
4. The GPU commands the game submits run through Xenia's command processor, shader
   translator and caches (`runtime/src/gpu/xenia_bridge.cpp`); XMA audio is decoded by
   Xenia's decoder and played with XAudio2.

## Repository layout

```
config/       XenonRecomp configuration for WET
patches/      changes to XenonRecomp and Xenia (applied automatically by CMake)
runtime/      the host program (src/, src/kernel/, src/gpu/, xenia_shim/)
launcher/     the settings launcher
tools/        build.py, package.py, disc extraction and analysis scripts
cmake/        builds the parts of Xenia and SDL2 the runtime needs
ci/           stand-in for the generated code, for CI builds without game files
thirdparty/   XenonRecomp and Xenia (git submodules)
game/         YOUR game files (ignored by git)
generated/    generated C++ (ignored by git)
```

## Status and known issues

- Intro and cutscene videos run at 30 FPS ("Videos at 30 FPS", on by default) to avoid a
  flicker at the top of the picture caused by a timing race at higher frame rates.
- Mouse aim drives the right stick, so it has the stick's maximum turn speed; direct
  camera control is planned.
- Achievements earned in saves from before achievement support aren't shown as unlocked.
- Tested on one game copy and a few PCs; please report problems with `wet.log` attached.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev) — the recompiler;
  [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) showed how a finished
  recompilation project fits together.
- [Xenia](https://github.com/xenia-project/xenia) — GPU and audio emulation, and the
  reference for the console's behaviour.
- [SDL2](https://www.libsdl.org/), [Dear ImGui](https://github.com/ocornut/imgui),
  [FFmpeg](https://ffmpeg.org/), [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling).

## Legal

WET Recompiled's own code is released under the [MIT licence](LICENSE); the third-party
projects keep their own licences. This project contains no game code or assets and is
intended for people who own a legitimate copy of WET. It is not affiliated with or
endorsed by Bethesda Softworks or Artificial Mind and Movement. WET is a trademark of
its respective owner.
