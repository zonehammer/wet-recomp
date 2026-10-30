#!/usr/bin/env python3
"""Makes a ready-to-run WET Recomp folder (no Python, CMake or compiler needed to play).

    python tools/package.py                       # dist/WET-Recomp-<version>, without game files
    python tools/package.py --with-game           # also copies game/extracted (about 4.3 GB)
    python tools/package.py --with-saves --zip    # also copies saves/achievements/shader cache, and zips it

Layout of the folder:
    WET-Recomp-<version>/
      WET Launcher.exe         settings window with a Play button (start here)
      WetRecomp.exe            the game (starts straight away, skipping the launcher)
      README.txt               how to run it, controls, settings
      THIRD-PARTY-NOTICES.txt  licences of the libraries inside the exe
      game/extracted/          your extracted WET disc (default.xex, lu0, media, ...)
      game/hdd/                saves, achievements and the shader cache (created on first run)

wet.ini (settings) and wet.log are written next to WetRecomp.exe.

The exe contains code translated from your copy of WET, so the package is for your
own machines: don't share it publicly, and never commit it (dist/ is git-ignored).
Each player needs their own extracted copy of the game.
"""
import argparse
import os
import shutil
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NOTICES = [
    ("Xenia (GPU and audio emulation, BSD licence)", "thirdparty/xenia/LICENSE"),
    ("XenonRecomp (MIT licence)", "thirdparty/XenonRecomp/LICENSE.md"),
    ("SDL2 (controllers, zlib licence)", "thirdparty/xenia/third_party/SDL2/LICENSE.txt"),
    ("Dear ImGui (overlay, MIT licence)", "thirdparty/xenia/third_party/imgui/LICENSE.txt"),
    ("FFmpeg (XMA audio decoding, LGPL 2.1)", "thirdparty/xenia/third_party/FFmpeg/COPYING.LGPLv2.1"),
]

NIS_NOTICE = """NVIDIA Image Scaling SDK v1.0.3 (scaling shader, MIT licence)
Copyright (c) 2022 NVIDIA CORPORATION. All rights reserved.
Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following
conditions: The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
"""

README = """WET Recomp
==========

WET (Xbox 360) recompiled to run natively on Windows 10/11 (64-bit).

Before the first run
--------------------
Put your extracted WET disc in game\\extracted, so that this file exists:
    game\\extracted\\default.xex
(tools\\prepare_game.py in the source tree extracts it from your disc image.)

Then double-click "WET Launcher.exe": choose your settings and press Play.
(WetRecomp.exe starts the game directly with the last settings.)

Requirements: a DirectX 12 graphics card (Vulkan 1.1 is the fallback) and an
up-to-date graphics driver.

Controls
--------
Controllers: Xbox, PlayStation 4/5 (USB or Bluetooth), Switch Pro and others.
PlayStation buttons map by position: Cross = A, Circle = B, Square = X,
Triangle = Y, Options = Start, Share/Create or touchpad click = Back.

Keyboard and mouse: WASD move, the mouse turns the camera and aims, left click RT,
right click LT, Space A, Esc B, X / Y, Q / E bumpers, Z / C triggers, Enter Start,
Backspace Back, arrow keys D-pad. The cursor is hidden while you play; F1 or
Alt+Tab frees it.

Keys
----
F1   settings overlay (frame rate, resolution, scaling, film grain, mouse,
     controllers, achievements)
F6   cycle the frame rate (30 / 60 / 120 / 144)
F11  fullscreen
F8   save the next 48 frames to frames\\ (for bug reports)
F10  start / stop the profiler (profile.txt, for bug reports)

There's no console window; to watch the log while playing, start it with
WetRecomp.exe --console. Settings are saved in wet.ini next to WetRecomp.exe. Some (resolution, shader
cache) apply at the next start.

Saves, achievements, shader cache
---------------------------------
game\\hdd holds your saves, achievements.ini and the shader cache (shaders\\).
The shader cache makes stutter from new effects happen only once; if graphics
ever look wrong after a driver update, delete game\\hdd\\shaders.

Problems
--------
wet.log (next to WetRecomp.exe) records what happened, including crash details.

This package contains code translated from your copy of WET: it is for your own
use. Don't share it publicly. Each player needs their own copy of the game.
"""


def find_exe():
    for path in ("build/runtime/WetRecomp.exe", "build/runtime/WetRecomp"):
        full = os.path.join(REPO, path)
        if os.path.exists(full):
            return full
    return None


def copy_tree(src, dst):
    count = 0
    for root, _, files in os.walk(src):
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for name in files:
            shutil.copy2(os.path.join(root, name), os.path.join(dst, rel, name))
            count += 1
            if count % 500 == 0:
                print(f"   {count} files...", flush=True)
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    with open(os.path.join(REPO, "VERSION")) as v:
        version = v.read().strip()
    ap.add_argument("--out", default=os.path.join(REPO, "dist", "WET-Recomp-" + version), help="output folder")
    ap.add_argument("--with-game", action="store_true", help="copy game/extracted into the package (about 4.3 GB)")
    ap.add_argument("--with-saves", action="store_true",
                    help="copy game/hdd (saves, achievements, shader cache) into the package")
    ap.add_argument("--zip", action="store_true", help="also make <out>.zip")
    args = ap.parse_args()

    exe = find_exe()
    if not exe:
        sys.exit("No build/runtime/WetRecomp.exe - run python tools/build.py first.")
    out = os.path.abspath(args.out)
    print(f"== Packaging {exe}\n   into {out}")
    if os.path.exists(os.path.join(out, "WetRecomp.exe")):
        os.remove(os.path.join(out, "WetRecomp.exe"))
    os.makedirs(os.path.join(out, "game", "extracted"), exist_ok=True)
    os.makedirs(os.path.join(out, "game", "hdd"), exist_ok=True)
    shutil.copy2(exe, os.path.join(out, os.path.basename(exe)))
    launcher = os.path.join(os.path.dirname(exe), "WetLauncher.exe")
    if os.path.exists(launcher):
        shutil.copy2(launcher, os.path.join(out, "WET Launcher.exe"))
    else:
        print("   (no WetLauncher.exe in the build - the package has the game only)")

    with open(os.path.join(out, "README.txt"), "w", newline="\r\n") as f:
        f.write(README)
    with open(os.path.join(out, "THIRD-PARTY-NOTICES.txt"), "w", newline="\r\n", encoding="utf-8") as f:
        f.write("WetRecomp.exe includes the following libraries.\n\n")
        for title, path in NOTICES:
            full = os.path.join(REPO, path)
            f.write("=" * 78 + "\n" + title + "\n" + "=" * 78 + "\n")
            if os.path.exists(full):
                with open(full, encoding="utf-8", errors="replace") as lic:
                    f.write(lic.read().strip() + "\n\n")
            else:
                f.write(f"(see {path} in the source tree)\n\n")
        f.write("=" * 78 + "\n" + NIS_NOTICE)
        f.write("\nFFmpeg is used under the LGPL 2.1; its source is in the Xenia submodule\n"
                "(thirdparty/xenia/third_party/FFmpeg) of the WET Recomp source tree.\n")

    game_src = os.path.join(REPO, "game", "extracted")
    if args.with_game:
        if not os.path.exists(os.path.join(game_src, "default.xex")):
            sys.exit("--with-game: game/extracted/default.xex not found")
        print("== Copying game files (this takes a while)")
        n = copy_tree(game_src, os.path.join(out, "game", "extracted"))
        print(f"   {n} files")
    if args.with_saves:
        hdd_src = os.path.join(REPO, "game", "hdd")
        if os.path.isdir(hdd_src):
            print("== Copying saves, achievements and shader cache")
            n = copy_tree(hdd_src, os.path.join(out, "game", "hdd"))
            print(f"   {n} files")
    ini = os.path.join(REPO, "wet.ini")
    if os.path.exists(ini) and not os.path.exists(os.path.join(out, "wet.ini")):
        shutil.copy2(ini, os.path.join(out, "wet.ini"))   # your current settings as the starting point

    if args.zip:
        zip_path = out.rstrip("\\/") + ".zip"
        print(f"== Zipping to {zip_path}")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as z:
            for root, _, files in os.walk(out):
                for name in files:
                    full = os.path.join(root, name)
                    rel = os.path.relpath(full, os.path.dirname(out))
                    z.write(full, rel)
        print(f"   {os.path.getsize(zip_path) / (1 << 20):.0f} MB")

    has_game = os.path.exists(os.path.join(out, "game", "extracted", "default.xex"))
    print("\n== Done: " + out)
    if not has_game:
        print("   Put your extracted disc in game\\extracted (default.xex and the folders next to it),")
        print("   or run again with --with-game. Then double-click \"WET Launcher.exe\".")
    else:
        print("   Double-click \"WET Launcher.exe\" to play.")


if __name__ == "__main__":
    sys.exit(main())
