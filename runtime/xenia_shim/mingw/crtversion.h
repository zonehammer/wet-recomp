// llvm-mingw has no crtversion.h (MSVC CRT); FFmpeg's MSVC configuration only
// checks the major version. The universal CRT is version 14.
#pragma once
#define _VC_CRT_MAJOR_VERSION 14
#define _VC_CRT_MINOR_VERSION 0
