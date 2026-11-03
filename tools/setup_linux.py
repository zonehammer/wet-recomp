#!/usr/bin/env python3
"""Set up and build WET Recompiled on Linux (Steam Deck, Bazzite, other distros).

    python3 setup_linux.py --iso ~/Games/WET.iso            # first time
    python3 setup_linux.py                                   # later: update and rebuild

The game is built as a Windows program and runs with Proton (or Wine). Nothing
is installed system-wide and no root is needed: the tools that aren't already
there (CMake, Ninja, llvm-mingw for the Windows build, a C++ compiler for the
recompiler) are downloaded into <folder>/tools, the source goes into
<folder>/wet-recomp, and tools/build.py builds it from your disc.
Needs git and python3 (both come with SteamOS and Bazzite).

Afterwards add <folder>/wet-recomp/build/windows/runtime/WetLauncher.exe to
Steam (Games > Add a Non-Steam Game) and force Proton in its Compatibility
settings. In Desktop Mode on the Deck: run this from Konsole.
"""
import argparse
import io
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

REPO_URL = "https://github.com/zonehammer/wet-recomp.git"
API = "https://api.github.com/repos/"


def say(msg):
    print(f"\n==> {msg}", flush=True)


def fetch(url, what=None):
    req = urllib.request.Request(url, headers={"User-Agent": "WET-Setup", "Accept": "application/octet-stream, */*"})
    with urllib.request.urlopen(req, timeout=60) as r:
        total = int(r.headers.get("Content-Length") or 0)
        data = io.BytesIO()
        got = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            data.write(chunk)
            got += len(chunk)
            if what:
                size = f" of {total >> 20} MB" if total else ""
                print(f"\r   {what}: {got >> 20} MB{size}   ", end="", flush=True)
        if what:
            print()
        return data.getvalue()


def latest_asset(repo, pattern):
    rel = json.loads(fetch(API + repo + "/releases/latest"))
    for a in rel.get("assets", []):
        if re.fullmatch(pattern, a["name"]):
            return a["browser_download_url"]
    sys.exit(f"No download of {repo} matches {pattern}.")


def unpack(data, name, dest):
    os.makedirs(dest, exist_ok=True)
    if name.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for info in z.infolist():
                path = z.extract(info, dest)
                mode = info.external_attr >> 16
                if mode:
                    os.chmod(path, mode)
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            t.extractall(dest)


def find(dest, rel):
    """First <dest>/<sub>/rel or <dest>/rel that exists."""
    if os.path.exists(os.path.join(dest, rel)):
        return dest
    for sub in sorted(os.listdir(dest)) if os.path.isdir(dest) else []:
        if os.path.exists(os.path.join(dest, sub, rel)):
            return os.path.join(dest, sub)
    return None


def tool(tools, label, rel, url_fn):
    """A downloaded tool's folder (the one containing rel), downloading it if needed."""
    dest = os.path.join(tools, label)
    root = find(dest, rel)
    if root:
        return root
    url = url_fn()
    print(f"   {label}: {url}")
    data = fetch(url, label)
    shutil.rmtree(dest, ignore_errors=True)
    unpack(data, url.rsplit("/", 1)[1], dest)
    root = find(dest, rel)
    if not root:
        sys.exit(f"{label} didn't contain {rel}.")
    return root


# A zig release known to build the recompiler (newer ones can change how its
# bundled clang behaves); the newest one only if this one is gone.
ZIG_VERSION = "0.14.1"


def zig_url():
    index = json.loads(fetch("https://ziglang.org/download/index.json"))
    if ZIG_VERSION in index:
        return index[ZIG_VERSION]["x86_64-linux"]["tarball"]
    versions = sorted((v for v in index if re.fullmatch(r"\d+\.\d+\.\d+", v)),
                      key=lambda v: tuple(map(int, v.split("."))))
    return index[versions[-1]]["x86_64-linux"]["tarball"]


def write_wrapper(path, line):
    with open(path, "w") as f:
        f.write("#!/bin/sh\n" + line + ' "$@"\n')
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def run(cmd, **kw):
    print("   $ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, **kw).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", help="your WET disc image (only needed the first time)")
    ap.add_argument("--dir", default=os.path.expanduser("~/WET-Recomp"), help="install folder (default ~/WET-Recomp)")
    ap.add_argument("--branch", help="build this branch / tag instead of the latest release")
    args = ap.parse_args()
    if platform.machine() not in ("x86_64", "AMD64"):
        sys.exit("WET Recompiled needs an x86-64 PC.")
    if not shutil.which("git"):
        sys.exit("git is needed (it comes with SteamOS and Bazzite; elsewhere install it with your package manager).")

    root = os.path.abspath(args.dir)
    tools = os.path.join(root, "tools")
    repo = os.path.join(root, "wet-recomp")
    os.makedirs(tools, exist_ok=True)
    if not args.iso and not os.path.exists(os.path.join(repo, "game", "extracted", "default.xex")):
        sys.exit("Pass your disc image the first time: python3 setup_linux.py --iso /path/to/WET.iso")
    if args.iso and not os.path.exists(args.iso):
        sys.exit(f"{args.iso} doesn't exist.")
    free = shutil.disk_usage(root).free >> 30
    if free < 14:
        print(f"   Warning: {free} GB free here; the first build needs about 14 GB.")

    say("Tools")
    env = dict(os.environ)
    path = []
    if not shutil.which("cmake"):
        path.append(os.path.join(tool(tools, "cmake", "bin/cmake",
                                      lambda: latest_asset("Kitware/CMake", r"cmake-[\d.]+-linux-x86_64\.tar\.gz")), "bin"))
    if not shutil.which("ninja"):
        path.append(tool(tools, "ninja", "ninja", lambda: latest_asset("ninja-build/ninja", r"ninja-linux\.zip")))
    mingw = tool(tools, "llvm-mingw", "bin/x86_64-w64-mingw32-clang++",
                 lambda: latest_asset("mstorsjo/llvm-mingw", r"llvm-mingw-\d+-ucrt-ubuntu-[\d.]+-x86_64\.tar\.xz"))
    env["LLVM_MINGW"] = mingw
    print(f"   llvm-mingw: {mingw}")
    # A C++ compiler for this machine (the recompiler runs here): clang if it's
    # installed, otherwise zig's bundled clang (SteamOS and Bazzite have none).
    if not (shutil.which("clang++") and shutil.which("clang")) and not env.get("CXX"):
        zig_dir = tool(tools, "zig-" + ZIG_VERSION, "zig", zig_url)
        zig = os.path.join(zig_dir, "zig")
        wrappers = os.path.join(tools, "zig-wrappers")
        os.makedirs(wrappers, exist_ok=True)
        for name, sub in (("cc", "cc"), ("c++", "c++"), ("ar", "ar"), ("ranlib", "ranlib")):
            write_wrapper(os.path.join(wrappers, name), f'exec "{zig}" {sub}')
        env["CC"] = os.path.join(wrappers, "cc")
        env["CXX"] = os.path.join(wrappers, "c++")
        env["AR"] = os.path.join(wrappers, "ar")
        env["RANLIB"] = os.path.join(wrappers, "ranlib")
        print(f"   C++ compiler: zig ({zig})")
    env["PATH"] = os.pathsep.join(path + [env.get("PATH", "")])

    say("Source code")
    if not os.path.isdir(os.path.join(repo, ".git")):
        if os.path.exists(repo) and os.listdir(repo):
            sys.exit(f"{repo} exists and isn't a copy of the project; choose another --dir.")
        if run(["git", "clone", REPO_URL, repo], env=env) != 0:
            sys.exit("Couldn't download the source code from GitHub.")
    run(["git", "-C", repo, "fetch", "--tags", "--force", "origin"], env=env)
    ref = args.branch
    if not ref:
        try:
            ref = json.loads(fetch(API + "zonehammer/wet-recomp/releases/latest"))["tag_name"]
        except Exception:
            ref = "main"
    if run(["git", "-C", repo, "checkout", "--force", ref], env=env) != 0:
        sys.exit(f"Couldn't check out {ref}.")
    if ref == "main" or (args.branch and not ref.startswith("v")):
        run(["git", "-C", repo, "pull", "--ff-only"], env=env)
    print(f"   building {ref}")

    say("Building")
    cmd = [sys.executable, os.path.join(repo, "tools", "build.py")]
    if args.iso:
        cmd += ["--iso", os.path.abspath(args.iso)]
    if run(cmd, cwd=repo, env=env) != 0:
        sys.exit("The build failed; the output above says why.")

    launcher = os.path.join(repo, "build", "windows", "runtime", "WetLauncher.exe")
    print(f"""
Done. To play:
  1. Steam > Games > Add a Non-Steam Game to My Library > Browse, and pick
     {launcher}
  2. In its Properties > Compatibility, tick "Force the use of a specific Steam
     Play compatibility tool" and choose Proton (the newest, or Proton Experimental).
  3. Start it from Steam (Game Mode works too). The launcher's Play starts the game.
Run this again later to update and rebuild.""")


if __name__ == "__main__":
    main()
