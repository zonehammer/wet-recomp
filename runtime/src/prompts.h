// PlayStation button prompts (prompts.cpp).
#pragma once
#include <string>

namespace wet::prompts
{
    // The game opened a file (name only): menu archives trigger a scan for the
    // menus' console setting.
    void FileOpened(const std::string& name);
}
