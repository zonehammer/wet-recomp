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
- **Any frame rate from 30 to 360 FPS** (or match the monitor) at the correct game speed; G-Sync / FreeSync work.
- **Resolution scaling** to 1440p or 4K, **AMD FSR / CAS** and **NVIDIA Image Scaling**,
  FXAA, and a **film grain** slider.
- **Controllers**: Xbox, PlayStation 4/5 (USB or Bluetooth), Switch Pro and others, up
  to four, with rumble.
- **Keyboard and mouse**: WASD movement and mouse aim, with on-screen button prompts
  that show your keys and mouse buttons (or Xbox / PlayStation symbols with a controller).
- **Five languages** (English, German, French, Spanish, Italian; text and speech), chosen
  automatically from Windows or in the settings.
- **Options the console didn't have**: field of view, widescreen / ultrawide, and
  motion blur, depth of field and camera shake on or off.
- **Achievements** with on-screen pop-ups and a list in the overlay.
- **Shader cache**, built in the background so the game starts straight away; shader
  compilation stutter only happens once.
- **Settings picked for your graphics card** on the first start.
- An in-game settings overlay (**F1**) and a **launcher** with settings and a Play button.
- **Quit to desktop** from the pause menu (hold Back / Backspace) or the F1 overlay.

## Building (Windows 10/11, 64-bit)

You need your WET disc image. Visual Studio is **not** required. Tested with the US/EU disc
(Redump: *WET (USA, Europe) (En,Fr,De,Es,It)*, SHA-1 `56f459e11ee9a7b3b7ce786d0d36cc1c331b5749`);
other dumps of the same disc (title ID `425307DB`) should work too.

**The easy way: WetSetup.exe.** Download it from the
[latest release](https://github.com/zonehammer/wet-recomp/releases/latest), pick your ISO
and a folder, press Install. It downloads whatever tools are missing (Git, CMake, Ninja,
Clang, Python - into the install folder, nothing system-wide), the source code, extracts
your disc and builds; then the desktop shortcut starts the launcher. Run it again to
update and rebuild. The first build takes 10-30 minutes and about 14 GB of disk.

**By hand:**

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
The launcher's banner is the game's dashboard art, read from your disc
(`game\extracted\nxeart`) each time it starts; to use your own picture instead, put
`launcher_banner.png` or `.jpg` next to the launcher (it's cropped to 690x150).

**Ready-to-run folder**: `python tools\package.py --with-game --with-saves --zip` makes
`dist\WET-Recomp-<version>` with the launcher, the game exe, a player README and the
licence notices. It contains code translated from your copy of the game, so it is for
**your own machines only**: don't upload or share it.

## Steam Deck, Bazzite and other Linux (experimental)

The game is built as the Windows program and runs with Proton. Everything is built on the
Deck / PC itself from your disc image; no root and nothing installed system-wide (the
tools go into the install folder). You need git and python3, which SteamOS and Bazzite
already have. On the Deck, use Desktop Mode and open Konsole:

```sh
git clone https://github.com/zonehammer/wet-recomp.git ~/wet-setup
python3 ~/wet-setup/tools/setup_linux.py --iso ~/path/to/WET.iso
```

It downloads CMake, Ninja, llvm-mingw and (when there's no clang) zig into `~/WET-Recomp/tools`,
builds the latest release into `~/WET-Recomp/wet-recomp`, and prints what to add to Steam:
`build/windows/runtime/WetLauncher.exe` as a non-Steam game, with Proton forced in its
Compatibility settings. The first build takes a long time on the Deck (an hour or more);
run the same command again (without `--iso`) to update.

By hand: `python3 tools/build.py --iso ... --llvm-mingw <llvm-mingw folder>` (needs clang,
CMake and Ninja).

## Controls

| Action | Controller | Keyboard / mouse |
|---|---|---|
| Move | Left stick | WASD |
| Camera / aim | Right stick | Mouse |
| Shoot / LT | RT / LT | Left / right click |
| A / B / X / Y | A / B / X / Y (PlayStation: Cross / Circle / Square / Triangle) | Space / Esc / X / Y |
| Bumpers / triggers | LB / RB, LT / RT | Q / E, Z / C |
| D-pad, Start, Back | D-pad, Start (Options), Back (Share / touchpad) | Arrows, Enter, Backspace |
| Stick clicks | L3 / R3 | Left Shift / V |

Every keyboard key can be rebound: in the F1 overlay (Keyboard bindings: click an
action, press a key), with the launcher's **Keyboard...** button, or in `wet.ini`
(`key_a = SPACE`, two keys as `key_lb = Q, F`).

| Key | |
|---|---|
| F1 | Settings overlay (General, Display, Graphics, Performance, Controls and Achievements tabs; Quit to desktop); frees the mouse |
| F6 | Cycle the frame rate |
| F11 | Fullscreen |
| F8 | Save the next 48 frames to `frames\` (bug reports) |
| F10 | Profiler on/off (`profile.txt`, bug reports) |

In the pause menu, **hold Back (Backspace) for 1.5 seconds to quit to desktop**.

Settings live in `wet.ini` next to the exe; the mouse buttons can be rebound there
(`mouse_left = RT` ...) as well as the keys. Widescreen / ultrawide (`widescreen = 1`, also in F1 and the launcher) fills 21:9 and
32:9 screens with a wider view and keeps the HUD 16:9 in the middle. The field of view is `fov` (horizontal degrees at 16:9, 90 = original; also in F1 and
the launcher). Direct mouse aim (`mouse_direct`, on by default) turns the third-person
camera by an angle per mouse count (native mouse look, as nikolaygorb/WetRecomp does it:
no stick dead zone, curve, turn speed or slow-motion scaling); elsewhere the mouse
turns as far as it moves, with no stick speed limit. `language` picks the game's
language (0 = the Windows language, 1 English, 3 German, 4 French, 5 Spanish, 6 Italian)
and `button_prompts` the prompt symbols (0 automatic, 1 Xbox, 2 PlayStation, 3 keyboard /
mouse). Saves, `achievements.ini` and the shader cache are in
`game\hdd`. `WetRecomp.exe --console` shows the log live; `wet.log` always has it.

## Testing a build

`python tools/soak_test.py` starts the game with an empty save folder and plays it with
scripted input: New Game, into the first level, a couple of minutes of moving and shooting,
the pause menu, then a clean exit. It fails on a crash, a hang (the log stops) or an early
exit, and lists any unimplemented system functions the game called. Logs go to `soak\`.
`--runs 3` repeats it; `--script` takes your own steps (see `runtime/src/input.cpp`).
`--record` lets you play it once yourself (from an empty save folder, until you quit);
after that every run replays your recording (`soak\recording.txt`) instead of the script.

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
- Achievements earned in saves from before achievement support aren't shown as unlocked.
- Tested on one game copy and a few PCs; please report problems with "Report a bug..." in the launcher (it zips `wet.log`, `wet.ini` and system info).

Planned work is in [ROADMAP.md](ROADMAP.md).

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
