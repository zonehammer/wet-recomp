#pragma once

namespace wet
{
    // Allocates and publishes the kernel variables the title imports, and starts
    // the KeTimeStampBundle updater. Call after LoadTitleModule.
    void InitKernelVariables();
}
