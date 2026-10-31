// Keyboard bindings shared by the runtime, the F1 overlay and the launcher:
// the actions that can be bound (wet.ini key_<action> = KEY[, KEY]) and the key
// names. Plain data, no <windows.h> (the values are Windows virtual-key codes).
#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wet::keybinds
{
    struct Action
    {
        const char* id;        // wet.ini: key_<id>
        const char* label;     // shown in the overlay and the launcher
        const char* fallback;  // default key(s)
    };

    // Order matters: input.cpp maps these indices to buttons and stick directions.
    inline constexpr Action kActions[] = {
        { "move_up", "Move forward", "W" },
        { "move_down", "Move back", "S" },
        { "move_left", "Move left", "A" },
        { "move_right", "Move right", "D" },
        { "a", "A (jump)", "SPACE" },
        { "b", "B", "ESC" },
        { "x", "X", "X" },
        { "y", "Y", "Y" },
        { "lb", "LB", "Q" },
        { "rb", "RB", "E" },
        { "lt", "LT", "Z" },
        { "rt", "RT", "C" },
        { "lthumb", "Left stick click", "LSHIFT" },
        { "rthumb", "Right stick click", "V" },
        { "start", "Start", "ENTER" },
        { "back", "Back", "BACKSPACE" },
        { "dpad_up", "D-pad up", "UP" },
        { "dpad_down", "D-pad down", "DOWN" },
        { "dpad_left", "D-pad left", "LEFT" },
        { "dpad_right", "D-pad right", "RIGHT" },
    };
    inline constexpr int kActionCount = int(sizeof(kActions) / sizeof(kActions[0]));

    struct Key { const char* name; int vk; };

    // F1 and F6-F11 are the runtime's own keys and can't be bound.
    inline constexpr Key kKeys[] = {
        { "A", 'A' }, { "B", 'B' }, { "C", 'C' }, { "D", 'D' }, { "E", 'E' }, { "F", 'F' }, { "G", 'G' },
        { "H", 'H' }, { "I", 'I' }, { "J", 'J' }, { "K", 'K' }, { "L", 'L' }, { "M", 'M' }, { "N", 'N' },
        { "O", 'O' }, { "P", 'P' }, { "Q", 'Q' }, { "R", 'R' }, { "S", 'S' }, { "T", 'T' }, { "U", 'U' },
        { "V", 'V' }, { "W", 'W' }, { "X", 'X' }, { "Y", 'Y' }, { "Z", 'Z' },
        { "0", '0' }, { "1", '1' }, { "2", '2' }, { "3", '3' }, { "4", '4' },
        { "5", '5' }, { "6", '6' }, { "7", '7' }, { "8", '8' }, { "9", '9' },
        { "SPACE", 0x20 }, { "ENTER", 0x0D }, { "ESC", 0x1B }, { "BACKSPACE", 0x08 }, { "TAB", 0x09 },
        { "LSHIFT", 0xA0 }, { "RSHIFT", 0xA1 }, { "LCTRL", 0xA2 }, { "RCTRL", 0xA3 },
        { "LALT", 0xA4 }, { "RALT", 0xA5 }, { "CAPSLOCK", 0x14 },
        { "UP", 0x26 }, { "DOWN", 0x28 }, { "LEFT", 0x25 }, { "RIGHT", 0x27 },
        { "INSERT", 0x2D }, { "DELETE", 0x2E }, { "HOME", 0x24 }, { "END", 0x23 },
        { "PAGEUP", 0x21 }, { "PAGEDOWN", 0x22 },
        { "NUM0", 0x60 }, { "NUM1", 0x61 }, { "NUM2", 0x62 }, { "NUM3", 0x63 }, { "NUM4", 0x64 },
        { "NUM5", 0x65 }, { "NUM6", 0x66 }, { "NUM7", 0x67 }, { "NUM8", 0x68 }, { "NUM9", 0x69 },
        { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 }, { "F5", 0x74 }, { "F12", 0x7B },
        { "MINUS", 0xBD }, { "EQUALS", 0xBB }, { "LBRACKET", 0xDB }, { "RBRACKET", 0xDD },
        { "SEMICOLON", 0xBA }, { "APOSTROPHE", 0xDE }, { "COMMA", 0xBC }, { "PERIOD", 0xBE },
        { "SLASH", 0xBF }, { "BACKSLASH", 0xDC }, { "GRAVE", 0xC0 },
    };
    inline constexpr int kKeyCount = int(sizeof(kKeys) / sizeof(kKeys[0]));

    inline const char* KeyName(int vk)
    {
        for (auto& k : kKeys)
            if (k.vk == vk)
                return k.name;
        return nullptr;
    }

    // "Q, F" -> the keys' virtual-key codes (unknown names and NONE are skipped).
    inline std::vector<int> Parse(const std::string& text)
    {
        std::vector<int> out;
        size_t i = 0;
        while (i < text.size())
        {
            size_t j = text.find(',', i);
            if (j == std::string::npos)
                j = text.size();
            std::string name;
            for (size_t k = i; k < j; k++)
                if (!std::isspace(static_cast<unsigned char>(text[k])))
                    name += char(std::toupper(static_cast<unsigned char>(text[k])));
            for (auto& key : kKeys)
                if (name == key.name)
                {
                    out.push_back(key.vk);
                    break;
                }
            i = j + 1;
        }
        return out;
    }
}
