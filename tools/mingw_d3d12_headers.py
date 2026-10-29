#!/usr/bin/env python3
"""Copies DirectX-Headers' include/directx for llvm-mingw builds, adding
by-value overloads of the D3D12 methods that return structures.

For MinGW the headers declare those methods (GetDesc,
GetCPUDescriptorHandleForHeapStart, ...) in their ABI-correct form, taking a
pointer to the result (`T* GetDesc(T* RetVal)`), while Xenia calls the MSVC form
(`T GetDesc()`). Next to each such declaration this adds a non-virtual inline
overload with the MSVC signature that forwards to it.

usage: mingw_d3d12_headers.py <DirectX-Headers/include/directx> <output dir>
"""
import os
import re
import shutil
import sys

BLOCK = re.compile(
    r"#if defined\(_MSC_VER\) \|\| !defined\(_WIN32\)\n"
    r"(?P<msvc>.*?)\n#else\n(?P<mingw>.*?)\n#endif", re.S)
DECL = re.compile(
    r"virtual\s+(?P<ret>[\w\s]+?)\s*\*?\s*STDMETHODCALLTYPE\s+(?P<name>\w+)\s*\((?P<params>.*?)\)\s*(?P<const>const)?\s*=\s*0\s*;", re.S)
ANNOT = re.compile(r"_(In|Out|Inout)\w*(\([^)]*\))?\s*")


def param_names(params):
    params = params.strip()
    if not params or params == "void":
        return [], []
    decls = [p.strip() for p in params.split(",")]
    names = [re.findall(r"\w+", ANNOT.sub("", d))[-1] for d in decls]
    return decls, names


def transform(text):
    def repl(m):
        msvc = DECL.search(m.group("msvc"))
        if not msvc:
            return m.group(0)
        ret, name = msvc.group("ret").strip(), msvc.group("name")
        decls, names = param_names(msvc.group("params"))
        const = " const" if msvc.group("const") else ""
        wrapper = (f"        {ret} {name}({', '.join(decls) if decls else 'void'}){const} {{\n"
                   f"            {ret} xe_mingw_result; {name}({', '.join(['&xe_mingw_result'] + names)}); "
                   f"return xe_mingw_result; }}")
        return m.group("mingw") + "\n" + wrapper
    return BLOCK.sub(repl, text)


def main(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    for name in ("d3d12.h", "d3d12video.h"):
        path = os.path.join(dst, name)
        if os.path.exists(path):
            with open(path, encoding="utf-8", errors="surrogateescape") as f:
                text = f.read()
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
                f.write(transform(text))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
