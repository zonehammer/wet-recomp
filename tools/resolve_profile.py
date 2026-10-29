#!/usr/bin/env python3
"""Turns profile.txt (written by the runtime's F10 / WET_PROFILE profiler) into
a readable report using the linker map.

usage: resolve_profile.py [profile.txt] [build/runtime/WetRecomp.map] [--top N]

For every thread it lists where samples landed (self), which functions were on
the stack (inclusive), and the innermost recompiled guest function (sub_XXXXXXXX)
on the stack, which is usually the most useful view of what the game is doing.
"""
import bisect
import collections
import re
import shutil
import subprocess
import sys


def load_map(path):
    syms = []
    sym_line = re.compile(r"^\s*([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]+)\s+\d+\s+(\S.*)$")
    msvc_line = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")
    msvc_base = None
    with open(path, errors="replace") as f:
        for line in f:
            m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
            if m:
                msvc_base = int(m.group(1), 16)
                continue
            m = msvc_line.match(line)
            if m and msvc_base is not None:
                syms.append((int(m.group(2), 16) - msvc_base, m.group(1)))
                continue
            m = sym_line.match(line)
            if not m:
                continue
            name = m.group(3).strip()
            if re.search(r"\.(o|obj|a|lib)\)?:\(", name) or name.startswith((".", "$")):
                continue
            syms.append((int(m.group(1), 16), name))
    syms.sort()
    addrs = [a for a, _ in syms]
    return addrs, [n for _, n in syms]


def demangler(names):
    tool = shutil.which("llvm-cxxfilt") or "/opt/llvm-mingw/bin/llvm-cxxfilt"
    out = {}
    mangled = sorted({n for n in names if n.startswith("_Z")})
    if mangled and shutil.which(tool) or (mangled and tool.startswith("/")):
        try:
            res = subprocess.run([tool], input="\n".join(mangled), capture_output=True, text=True, check=True)
            for a, b in zip(mangled, res.stdout.splitlines()):
                out[a] = b
        except Exception:
            pass
    return lambda n: out.get(n, n)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 25
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args = [a for a in args if a != str(top)]
    prof = args[0] if args else "profile.txt"
    mp = args[1] if len(args) > 1 else "build/runtime/WetRecomp.map"
    addrs, names = load_map(mp)
    cache = {}

    def name_of(frame):
        if frame in cache:
            return cache[frame]
        if frame.startswith("exe:"):
            rva = int(frame[4:], 16)
            i = bisect.bisect_right(addrs, rva) - 1
            r = names[i] if i >= 0 else frame
        else:
            r = frame.split("+")[0]
        cache[frame] = r
        return r

    header, threads, chains = [], {}, []
    for line in open(prof):
        if line.startswith("#"):
            header.append(line.rstrip())
        elif line.startswith("T "):
            _, n, tid, name = line.rstrip("\n").split(" ", 3)
            threads[tid] = (int(n), name)
        elif line.startswith("C "):
            parts = line.split()
            chains.append((int(parts[1]), parts[2], parts[3:]))
    all_names = {name_of(f) for _, _, fr in chains for f in fr}
    dm = demangler(all_names)

    def guest(n):
        m = re.search(r"(?:__imp__)?sub_([0-9A-F]{8})", n)
        return "sub_" + m.group(1) if m else None

    print("\n".join(header[:2]))
    total = sum(n for n, _ in threads.values()) or 1
    by_thread = collections.defaultdict(list)
    for c in chains:
        by_thread[c[1]].append(c)
    for tid, (n, tname) in sorted(threads.items(), key=lambda kv: -kv[1][0]):
        cs = by_thread.get(tid, [])
        self_c, incl, gst = collections.Counter(), collections.Counter(), collections.Counter()
        for cnt, _, frames in cs:
            fn = [dm(name_of(f)) for f in frames if f != "-"]
            if not fn:
                continue
            self_c[fn[0]] += cnt
            for x in set(fn):
                incl[x] += cnt
            g = next((guest(x) for x in fn if guest(x)), None)
            gst[g or "(no guest frame)"] += cnt
        print(f"\n=== thread {tid} {tname}: {n} samples")
        print("  self:")
        for f, c in self_c.most_common(top):
            print(f"    {100.0 * c / n:5.1f}%  {f[:150]}")
        print("  innermost guest function:")
        for f, c in gst.most_common(top):
            print(f"    {100.0 * c / n:5.1f}%  {f}")
        print("  inclusive:")
        for f, c in incl.most_common(top):
            print(f"    {100.0 * c / n:5.1f}%  {f[:150]}")


if __name__ == "__main__":
    sys.exit(main())
