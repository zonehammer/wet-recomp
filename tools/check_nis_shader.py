#!/usr/bin/env python3
"""Compiles the NVIDIA Image Scaling shader embedded in the patched Xenia
(src/xenia/ui/d3d12/wet_nis.h) with D3DCompiler_47.dll, the same way the game
does at startup. Windows only; used by CI."""
import ctypes
import re
import sys
from ctypes import wintypes
from pathlib import Path

root = Path(__file__).resolve().parent.parent
header = root / "thirdparty/xenia/src/xenia/ui/d3d12/wet_nis.h"
source = "".join(re.findall(r'R"WETNIS\((.*?)\)WETNIS"', header.read_text(), re.S)).encode()
if not source:
    sys.exit("no shader source found in " + str(header))

d3dc = ctypes.WinDLL("D3DCompiler_47.dll")


class Blob(ctypes.Structure):
    pass


BlobPtr = ctypes.POINTER(Blob)
d3dc.D3DCompile.restype = ctypes.c_long
d3dc.D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p,
                            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, wintypes.UINT,
                            wintypes.UINT, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]


def blob_bytes(p):
    # ID3DBlob vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
    vt = ctypes.cast(ctypes.cast(p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    return ctypes.string_at(get_ptr(p), get_size(p))


code, errors = ctypes.c_void_p(), ctypes.c_void_p()
hr = d3dc.D3DCompile(source, len(source), b"wet_nis.hlsl", None, None, b"main", b"ps_5_1",
                     1 << 15, 0, ctypes.byref(code), ctypes.byref(errors))
if errors.value:
    print(blob_bytes(errors).decode(errors="replace"))
if hr < 0 or not code.value:
    sys.exit("NIS shader failed to compile: 0x%08X" % (hr & 0xFFFFFFFF))
print("NIS shader compiled: %d bytes" % len(blob_bytes(code)))
