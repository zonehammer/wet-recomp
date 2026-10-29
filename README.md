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

1. `tools/prepare_game.py` extracts the disc image into `game/extracted/` (`tools/build.py` runs every step for you).
2. CMake builds a patched XenonRecomp from `thirdparty/` and the `recompile` target
   translates `default.xex` (PowerPC) into ~440 C++ files in `generated/ppc/`, using
   `config/WET_config.toml` and `config/WET_switch_tables.toml`.
3. The `WetRecomp` program compiles that code together with `runtime/`, which plays the
   part of the console: guest memory, threads, synchronisation, files, input and the
   ~240 kernel/XAM functions the game imports.

## Building (Windows)

You need your WET disc image (title ID `425307DB`) and a Clang toolchain. Visual Studio is
**not** required.

**1. Install the tools** (once):

- [Python 3](https://www.python.org/downloads/) (tick "Add python.exe to PATH") and
  [Git for Windows](https://git-scm.com).
- CMake and Ninja: `pip install cmake ninja`
- Clang: download the latest **llvm-mingw** release
  (`llvm-mingw-<date>-ucrt-x86_64.zip` from
  [github.com/mstorsjo/llvm-mingw/releases](https://github.com/mstorsjo/llvm-mingw/releases)),
  unzip it somewhere like `C:\llvm-mingw`, and add its `bin` folder to your `PATH`.

**2. Build** (in a normal Command Prompt or PowerShell):

```bat
git clone --recursive https://github.com/zonehammer/Claudtest1.git wet-recomp
cd wet-recomp
python tools\build.py --iso "D:\path\to\WET (USA, Europe) (En,Fr,De,Es,It).iso"
```

That extracts the disc into `game\extracted`, recompiles `default.xex` into `generated\ppc`
and compiles everything. The first build compiles ~250 MB of generated C++; expect
5–15 minutes on a modern CPU. Later, just run `python tools\build.py` again; it only redoes
what changed.

**3. Run** from the repository folder:

```bat
build\runtime\WetRecomp.exe
```

It finds `game\extracted` automatically (or pass `--game <folder>`), writes save data to
`game\hdd`, and logs to `wet.log`.

*Already have Visual Studio 2022 with "C++ Clang tools for Windows"?* Run
`python tools\build.py --toolchain clang-cl` from an x64 Native Tools prompt instead of
installing llvm-mingw. Linux works with the system `clang`; it's used for testing the runtime.

### Continuous integration

Every push is built on GitHub's Windows machines with both llvm-mingw and clang-cl
(`.github/workflows/windows.yml`). CI has no game files, so it links the runtime against
`ci/stub_ppc`, a small stand-in that uses the same constructs as the generated code.
Each llvm-mingw run also publishes the patched `XenonRecomp.exe`/`XenonAnalyse.exe` as a
download on the run's page.

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
tools/        build.py (one-command build), disc extraction and analysis scripts
ci/           stand-in for the generated code, used by the Windows CI build
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
