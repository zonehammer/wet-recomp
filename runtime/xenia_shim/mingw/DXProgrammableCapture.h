// llvm-mingw has no DXProgrammableCapture.h (Windows SDK, PIX/graphics-tools
// programmatic capture). Xenia's D3D12 backend only needs the interface.
#pragma once
#include <unknwn.h>

#ifndef __IDXGraphicsAnalysis_INTERFACE_DEFINED__
#define __IDXGraphicsAnalysis_INTERFACE_DEFINED__
MIDL_INTERFACE("9f251514-9d4d-4902-9d60-18988ab7d4b5")
IDXGraphicsAnalysis : public IUnknown {
 public:
  virtual void STDMETHODCALLTYPE BeginCapture() = 0;
  virtual void STDMETHODCALLTYPE EndCapture() = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IDXGraphicsAnalysis, 0x9f251514, 0x9d4d, 0x4902, 0x9d, 0x60,
                0x18, 0x98, 0x8a, 0xb7, 0xd4, 0xb5)
#endif
#endif
