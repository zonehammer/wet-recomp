// Camera finder for direct mouse aim (diagnostics, F7). See camscan.cpp.
#pragma once

namespace wet::camscan
{
    void Start();
    // Shift+F7: which instructions read the pad object's sticks (padreaders.txt).
    void StartPadReaders();
}
