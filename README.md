# WET Recomp

A static recompilation of **WET** (Xbox 360, 2009) to native Windows code, built on
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

> **Status: first boot, no graphics yet.** The game's executable is fully recompiled to C++
> and boots into its own initialisation code under a host runtime that implements the
> Xbox 360 kernel/XAM functions it needs. There is no GPU, audio or video playback yet,
> so nothing appears on screen. See [Roadmap](#roadmap).

**This repository contains no game data.** You need your own copy of WET; the build
extracts it and generates the C++ from it locally. Nothing under `game/` or
`generated/` is ever committed.

## How it works

1. `tools/prepare_game.py` extracts the disc image into `game/extracted/`.
2. CMake builds a patched XenonRecomp from `thirdparty/` and the `recompile` target
   translates `default.xex` (PowerPC) into ~440 C++ files in `generated/ppc/`, using
   `config/WET_config.toml` and `config/WET_switch_tables.toml`.
3. The `WetRecomp` program compiles that code together with `runtime/`, which plays the
   part of the console: guest memory, threads, synchronisation, files, input and the
   ~240 kernel/XAM functions the game imports.

## Building (Windows)

Requirements:

- **Visual Studio 2022** with the *Desktop development with C++* workload **and** the
  *C++ Clang tools for Windows* component (the generated code needs Clang).
- **CMake 3.20+**, **Ninja**, **Python 3.10+** and **Git** on your `PATH`
  (all included with Visual Studio's "C++ CMake tools", except Python).
- Your WET disc image (`.iso`), title ID `425307DB`.

From an **x64 Native Tools Command Prompt for VS 2022**:

```bat
git clone --recursive https://github.com/zonehammer/Claudtest1.git wet-recomp
cd wet-recomp

python tools\prepare_game.py "D:\path\to\WET (USA, Europe) (En,Fr,De,Es,It).iso"

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --target recompile
cmake build
cmake --build build
```

The last step compiles ~250 MB of generated C++; expect 5–15 minutes on a modern CPU.
Then run it from the repository folder:

```bat
build\runtime\WetRecomp.exe
```

It finds `game\extracted` automatically (or pass `--game <folder>`), writes save data to
`game\hdd`, and logs to `wet.log`.

Linux builds the same way with `clang`/`clang++` and is used for testing the runtime.

## What the log tells you

Every kernel call that isn't implemented yet is a logged stub:

```
STUB XexGetModuleSection(70004000, 8234A0D8, 700838E0, 70083900, 00000000) #1 lr=83037A80
```

Files the game opens are logged with where they resolved to. Crashes are reported with
the **guest** address and the last import the thread called, and an indirect call to an
address that wasn't recompiled says exactly which address to add to the config.

## Repository layout

```
config/       XenonRecomp config + jump tables for WET
patches/      changes to XenonRecomp (applied automatically by CMake)
runtime/      the host program
  include/    hooks force-included into the generated code (50 MHz timebase, checked indirect calls)
  src/        memory, threads, loader, logging
  src/kernel/ host implementations of xboxkrnl/XAM imports
tools/        disc extraction and analysis scripts
thirdparty/   XenonRecomp (git submodule)
game/         YOUR game files (ignored by git)
generated/    generated C++ (ignored by git)
```

## Changes to XenonRecomp

Upstream XenonRecomp was built around Sonic Unleashed; WET needed (`patches/xenonrecomp-wet.patch`):

- ~30 PowerPC instructions it didn't implement (update-form loads/stores, `bdzf`/`bdnzt`,
  `eqv`, `addc`, `addme`, `subfze`, `mulhd`, `vsel128`, `vpkswss`, `vcmpgtsw`, `vctuxs`, ...).
- `vcmpbfp`, `blrl`, and the FLOAT16 forms of `vpkd3d128`/`vupkd3d128`, which upstream
  compiled to a debug trap.
- `mftb` emits an overridable `PPC_QUERY_TIMEBASE()` hook instead of the host TSC.

## Jump tables

XenonAnalyse found 91 of WET's switch tables. `tools/find_jump_tables.py` finds the rest
(absolute, halfword-relative and byte-relative tables) by symbolically evaluating each
`bctr` back to its bounds check. Re-run against XenonAnalyse's results it reproduces every
table it detects identically; it added 163 more.

## Roadmap

1. **Kernel bring-up** — implement stubs as the log shows the game needs them, until it
   submits its first GPU commands. *(in progress)*
2. **GPU** — Xenos command processor, EDRAM, and shader translation to D3D12/Vulkan.
   This is the largest piece of work.
3. **Audio** — XMA decoding, the XAudio render driver callback.
4. **Video** — Bink playback for `movies0/1`.
5. **Saves, content, polish.**

[Xenia](https://github.com/xenia-project/xenia) is the reference for the console's
behaviour; [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) is the
reference for how a finished recompilation project is structured.

## Legal

This project contains no copyrighted game code or assets. It is intended for people who
own a legitimate copy of WET. Not affiliated with Bethesda Softworks or Artificial Mind
and Movement.
