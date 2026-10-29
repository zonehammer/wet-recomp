#!/usr/bin/env python3
"""One-command build of WET Recomp.

    python tools/build.py --iso "D:/path/to/WET.iso"      # first time
    python tools/build.py                                  # later rebuilds

Steps (each skipped when already done):
  1. check the toolchain (Clang, CMake, Ninja, Git)
  2. fetch the submodules (XenonRecomp; Xenia's GPU code for the renderer)
  3. extract the disc into game/extracted
  4. configure CMake
  5. recompile default.xex into generated/ppc (again whenever config/ changes)
  6. build build/runtime/WetRecomp(.exe)

Windows without Visual Studio: install Python and Git, then
    pip install cmake ninja
and unzip llvm-mingw (https://github.com/mstorsjo/llvm-mingw/releases, the
"ucrt-x86_64" zip), adding its bin folder to PATH. --toolchain clang-cl uses
Visual Studio's Clang instead (run from a VS developer prompt).
"""
import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, "build")
GAME_XEX = os.path.join(REPO, "game", "extracted", "default.xex")
GENERATED = os.path.join(REPO, "generated", "ppc")
STAMP = os.path.join(GENERATED, ".config-stamp")
CONFIG_FILES = [os.path.join(REPO, "config", f) for f in ("WET_config.toml", "WET_switch_tables.toml")]
PATCH = os.path.join(REPO, "patches", "xenonrecomp-wet.patch")


def step(msg):
    print(f"\n==> {msg}", flush=True)


def run(cmd, **kw):
    print("   $ " + " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    r = subprocess.run(cmd, cwd=kw.pop("cwd", REPO), **kw)
    if r.returncode != 0:
        sys.exit(f"\nFAILED (exit code {r.returncode}): {' '.join(cmd)}")


def find_llvm_mingw():
    """Locate an unzipped llvm-mingw when clang++ isn't on PATH (Windows)."""
    import glob
    roots = [os.environ.get("SystemDrive", "C:") + "\\", os.path.expanduser("~"),
             os.path.join(os.path.expanduser("~"), "Downloads"), os.path.join(os.path.expanduser("~"), "Desktop"),
             os.path.dirname(REPO), REPO]
    roots += [f"{d}:\\" for d in "CDEFGH"]
    for root in roots:
        for pattern in ("llvm-mingw*/bin/clang++.exe", "llvm-mingw*/llvm-mingw*/bin/clang++.exe"):
            hits = sorted(glob.glob(os.path.join(root, pattern)))
            if hits:
                return os.path.dirname(hits[-1])
    return None


def need(tool, hint):
    if not shutil.which(tool):
        sys.exit(f"\n'{tool}' was not found on PATH.\n{hint}")


def config_fingerprint():
    h = 0
    for f in CONFIG_FILES + [PATCH]:
        with open(f, "rb") as fp:
            for b in fp.read():
                h = (h * 131 + b) & 0xFFFFFFFFFFFF
    return f"{h:012x}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", help="your WET disc image (only needed the first time)")
    ap.add_argument("--toolchain", choices=["auto", "mingw", "clang-cl"], default="auto",
                    help="auto: clang/clang++ (llvm-mingw on Windows); clang-cl: Visual Studio's Clang")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compile jobs (default: all cores)")
    ap.add_argument("--debug", action="store_true", help="RelWithDebInfo build (debug symbols)")
    args = ap.parse_args()

    step("Checking tools")
    need("git", "Install Git: https://git-scm.com")
    need("cmake", "Install CMake: pip install cmake")
    need("ninja", "Install Ninja: pip install ninja")
    if args.toolchain == "clang-cl":
        cc = cxx = "clang-cl"
        need("clang-cl", "Run this from an 'x64 Native Tools Command Prompt for VS 2022' with the Clang component installed.")
    else:
        cc, cxx = "clang", "clang++"
        if not shutil.which("clang++") and os.name == "nt":
            found = find_llvm_mingw()
            if found:
                print(f"   clang++ not on PATH; using llvm-mingw found at {found}")
                os.environ["PATH"] = found + os.pathsep + os.environ["PATH"]
        need("clang++", "Install a Clang toolchain. On Windows without Visual Studio: unzip llvm-mingw\n"
                        "(https://github.com/mstorsjo/llvm-mingw/releases, *-ucrt-x86_64.zip) to C:\\ (e.g. C:\\llvm-mingw)\n"
                        "and build.py will find it, or add its bin folder (the one containing clang++.exe) to PATH.")
    subprocess.run([cxx, "--version"])

    step("Submodules (XenonRecomp, Xenia's GPU code and its libraries)")
    have = os.path.exists(os.path.join(REPO, "thirdparty", "XenonRecomp", "thirdparty", "simde", "x86", "sse.h")) and \
        os.path.exists(os.path.join(REPO, "thirdparty", "xenia", "third_party", "SPIRV-Tools", "include")) and \
        os.path.exists(os.path.join(REPO, "thirdparty", "xenia", "third_party", "FFmpeg", "libavcodec", "avcodec.h"))
    if not have:
        run([sys.executable, os.path.join(REPO, "tools", "fetch_deps.py")])
    else:
        print("   present")

    step("Game files")
    if not os.path.exists(GAME_XEX):
        if not args.iso:
            sys.exit("\ngame/extracted/default.xex not found. Pass your disc image once:\n"
                     '    python tools/build.py --iso "path/to/WET.iso"')
        run([sys.executable, os.path.join(REPO, "tools", "prepare_game.py"), args.iso])
    else:
        print("   game/extracted present")

    build_type = "RelWithDebInfo" if args.debug else "Release"
    configure = ["cmake", "-S", REPO, "-B", BUILD, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={build_type}",
                 f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"]
    jobs = ["-j", str(args.jobs)] if args.jobs else []

    step("Configuring")
    run(configure)

    step("Recompiling default.xex")
    fp = config_fingerprint()
    current = open(STAMP).read().strip() if os.path.exists(STAMP) else None
    if current != fp or not os.path.exists(os.path.join(GENERATED, "ppc_func_mapping.cpp")):
        run(["cmake", "--build", BUILD, "--target", "recompile"] + jobs)
        with open(STAMP, "w") as f:
            f.write(fp)
        run(configure)  # pick up the generated sources
    else:
        print("   generated/ppc is up to date with config/")

    step("Building (the first build compiles ~250 MB of generated C++ and takes a while)")
    run(["cmake", "--build", BUILD] + jobs)

    exe = os.path.join(BUILD, "runtime", "WetRecomp.exe" if os.name == "nt" else "WetRecomp")
    print(f"\nDone: {exe}\nRun it from the repository folder; it writes wet.log next to where you run it.")


if __name__ == "__main__":
    main()
