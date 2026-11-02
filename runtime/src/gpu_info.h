// The graphics card the game renders on (the high-performance one, as the
// Direct3D 12 renderer picks it) and the settings recommended for it on a
// first start. Header-only so the launcher can use it too.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <dxgi1_6.h>
#endif

namespace wet::gpu_info
{
    struct Info
    {
        bool found = false;
        std::string name;
        uint32_t vendor = 0;
        uint64_t dedicatedMB = 0;   // video memory on the card (small for integrated graphics)
        uint64_t budgetMB = 0;      // what the system lets this process use there
    };

#ifdef _WIN32
    // The adapter, AddRef'd (or null); the caller releases it.
    inline IDXGIAdapter1* HighPerformanceAdapter()
    {
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return nullptr;
        IDXGIAdapter1* adapter = nullptr;
        IDXGIFactory6* factory6 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6))))
        {
            for (UINT i = 0; !adapter; i++)
            {
                IDXGIAdapter1* a = nullptr;
                if (FAILED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                IID_PPV_ARGS(&a))))
                    break;
                DXGI_ADAPTER_DESC1 desc;
                if (SUCCEEDED(a->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                    adapter = a;
                else
                    a->Release();
            }
            factory6->Release();
        }
        if (!adapter)
            factory->EnumAdapters1(0, &adapter);
        factory->Release();
        return adapter;
    }

    inline Info Query()
    {
        Info info;
        IDXGIAdapter1* adapter = HighPerformanceAdapter();
        if (!adapter)
            return info;
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc)))
        {
            info.found = true;
            for (const wchar_t* c = desc.Description; *c; c++)
                info.name += *c < 128 ? char(*c) : '?';
            info.vendor = desc.VendorId;
            info.dedicatedMB = desc.DedicatedVideoMemory >> 20;
        }
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO memory;
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
                info.budgetMB = memory.Budget >> 20;
            adapter3->Release();
        }
        adapter->Release();
        return info;
    }
#else
    inline Info Query() { return {}; }
#endif

    // Settings for a first start, by how much video memory the card has (a
    // rough but reliable sign of how fast it is).
    struct Recommended
    {
        int scale;    // resolution_scale: 1 = 720p, 2 = 1440p, 3 = 4K
        int aa;       // antialiasing: 1 = FXAA, 3 = SMAA
        int filter;   // filter: 2 = AMD FSR (upscales to the screen)
        const char* tier;
    };

    inline Recommended Recommend(const Info& gpu)
    {
        if (!gpu.found)
            return { 1, 3, 2, "unknown graphics" };
        if (gpu.dedicatedMB < 2048)
            return { 1, 1, 2, "integrated or entry-level graphics" };
        if (gpu.dedicatedMB < 8000)
            return { 1, 3, 2, "a mid-range graphics card" };
        if (gpu.dedicatedMB < 16000)
            return { 2, 3, 2, "a fast graphics card" };
        return { 3, 3, 2, "a high-end graphics card" };
    }

    // Texture cache limits (Xenia's texture_cache_memory_limit_soft / _hard, in
    // MB) that keep it within about half of the card's video memory budget,
    // leaving the rest for render targets and the scaled resolve memory.
    // Xenia adds render_to_texture x (scale^2 - 1) on top of both.
    inline void TextureCacheLimits(const Info& gpu, int scaleX, int scaleY, uint32_t renderToTextureMB,
                                   uint32_t& softMB, uint32_t& hardMB)
    {
        softMB = 2048;
        hardMB = 4096;
        uint64_t budget = gpu.budgetMB ? gpu.budgetMB : gpu.dedicatedMB;
        if (!budget)
            return;
        uint64_t extra = uint64_t(renderToTextureMB) * uint64_t(scaleX * scaleY - 1);
        uint64_t half = budget / 2;
        uint64_t hard = half > extra ? half - extra : 0;
        hardMB = uint32_t(std::clamp<uint64_t>(hard, 768, 4096));
        softMB = std::max<uint32_t>(384, hardMB / 2);
    }
}
