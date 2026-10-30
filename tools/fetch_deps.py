#!/usr/bin/env python3
"""Fetches the git submodules the build needs (and only those).

XenonRecomp is fetched recursively. Xenia (whose GPU emulation is the renderer)
has dozens of submodules, FFmpeg among them; only the few its GPU, UI and base
libraries use are fetched.
"""
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XENIA_DEPS = [
    "third_party/fmt", "third_party/glslang", "third_party/Vulkan-Headers",
    "third_party/VulkanMemoryAllocator", "third_party/xxhash", "third_party/snappy",
    "third_party/utfcpp", "third_party/xbyak", "third_party/disruptorplus",
    "third_party/cpptoml", "third_party/date", "third_party/cxxopts", "third_party/imgui",
    "third_party/DirectX-Headers", "third_party/SPIRV-Tools", "third_party/FFmpeg",
    "third_party/SDL2",
]


def run(cmd, cwd=REPO):
    print("   $", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def main():
    run(["git", "submodule", "update", "--init", "--recursive", "thirdparty/XenonRecomp"])
    run(["git", "submodule", "update", "--init", "thirdparty/xenia"])
    run(["git", "submodule", "update", "--init", "--depth", "1"] + XENIA_DEPS,
        cwd=os.path.join(REPO, "thirdparty", "xenia"))


if __name__ == "__main__":
    sys.exit(main())
