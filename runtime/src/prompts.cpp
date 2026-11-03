// PlayStation button prompts.
//
// Every button icon the game shows - menus, HUD, tutorials - is one of a set of
// small DDS textures in global.lu (button_A_b.dds, button_3.dds, button_a.dds,
// ...), created through the HUD's image creator (A2MImageCreator, slot 1 =
// sub_82BA0318), which reads each one from the archive with a memory stream and
// the DDS loader sub_82B9AC80(stream). The Xbox disc has no PlayStation art
// (the movies' consoleType / the scripts' GetCurrentPlateform only change key
// handling, TRC messages and rendering paths), so the art is redrawn: as the
// loader is about to read one of the button textures, its pixels in the
// archive are replaced by the PlayStation version (prompt_art.h) - same shape,
// tint and size, a cross / circle / square / triangle or L1 / R1 / L2 / R2.
// Each texture is only redrawn the first time it is read: the game then
// converts it to the GPU's layout in place, so the choice is made at startup.
//
// The stream (sub_82B9C1A8): +16 the archive entry { +0 name, +8 data, +12 size }.
//
// Setting button_prompts: 0 = automatic (keyboard / mouse art with no
// controller connected as the images load, PlayStation art with a PlayStation
// controller, Xbox otherwise), 1 = Xbox, 2 = PlayStation, 3 = keyboard / mouse
// (each button shows the key or mouse button it is bound to).
#include "input.h"
#include "keybinds.h"
#include "log.h"
#include "memory.h"
#include "options.h"
#include "ppc_context.h"
#include "prompt_art.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    enum class Art { Xbox, PlayStation, Keyboard };

    // Decided as each texture loads (so in practice at startup).
    Art WantedArt()
    {
        switch (wet::options::ButtonPrompts())
        {
        case 1: return Art::Xbox;
        case 2: return Art::PlayStation;
        case 3: return Art::Keyboard;
        default:
        {
            bool any = false;
            for (auto& name : wet::ConnectedControllers())
                any |= !name.empty();
            if (!any)
                return Art::Keyboard;
            return wet::PlayStationControllerConnected() ? Art::PlayStation : Art::Xbox;
        }
        }
    }

    // The first key bound to a keybinds action ("" if none).
    std::string FirstKey(const char* action)
    {
        for (int i = 0; i < wet::keybinds::kActionCount; i++)
        {
            if (std::strcmp(action, wet::keybinds::kActions[i].id))
                continue;
            std::string keys = wet::options::KeyBinding(i);
            std::string first = keys.substr(0, keys.find(','));
            size_t a = first.find_first_not_of(" \t"), z = first.find_last_not_of(" \t");
            first = a == std::string::npos ? std::string() : first.substr(a, z - a + 1);
            return first == "NONE" ? std::string() : first;
        }
        return {};
    }

    // What a pad button is bound to on the keyboard / mouse: a mouse button
    // (with mouse aim on) first, otherwise the first key. False if neither.
    bool KeyboardLabel(wet::prompt_art::Symbol sym, wet::prompt_art::Label& label)
    {
        using wet::prompt_art::Symbol;
        const char* pad = nullptr;
        switch (sym)
        {
        case Symbol::Cross: pad = "A"; break;
        case Symbol::Circle: pad = "B"; break;
        case Symbol::Square: pad = "X"; break;
        case Symbol::Triangle: pad = "Y"; break;
        case Symbol::L1: pad = "LB"; break;
        case Symbol::R1: pad = "RB"; break;
        case Symbol::L2: pad = "LT"; break;
        case Symbol::R2: pad = "RT"; break;
        case Symbol::Start: pad = "START"; break;
        case Symbol::Back: pad = "BACK"; break;
        case Symbol::LPress: pad = "LTHUMB"; break;
        case Symbol::RPress: pad = "RTHUMB"; break;
        case Symbol::Up: pad = "DPAD_UP"; break;
        case Symbol::Down: pad = "DPAD_DOWN"; break;
        case Symbol::Left: pad = "DPAD_LEFT"; break;
        case Symbol::Right: pad = "DPAD_RIGHT"; break;
        case Symbol::LStick:   // moving: the movement keys
            label.kind = wet::prompt_art::Label::Kind::Text;
            label.text = wet::prompt_art::ShortKeyName(FirstKey("move_up")) + wet::prompt_art::ShortKeyName(FirstKey("move_left")) +
                         wet::prompt_art::ShortKeyName(FirstKey("move_down")) + wet::prompt_art::ShortKeyName(FirstKey("move_right"));
            if (label.text.size() > 4)
                label.text = "MOVE";
            return true;
        case Symbol::RStick:   // looking: the mouse
            if (!wet::options::MouseAim())
                return false;
            label.kind = wet::prompt_art::Label::Kind::Mouse;
            label.mouseButton = -1;
            return true;
        default: return false;
        }
        if (wet::options::MouseAim())
            for (int b = 0; b < 5; b++)
                if (wet::options::MouseBinding(b) == (std::strncmp(pad, "DPAD_", 5) == 0 ? pad + 5 : pad))
                {
                    if (b < 3)
                    {
                        label.kind = wet::prompt_art::Label::Kind::Mouse;
                        label.mouseButton = b;
                    }
                    else
                    {
                        label.kind = wet::prompt_art::Label::Kind::Text;
                        label.text = b == 3 ? "M4" : "M5";
                    }
                    return true;
                }
        std::string id = pad;
        for (auto& c : id)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        std::string first = FirstKey(id.c_str());
        if (first.empty())
            return false;
        label.kind = wet::prompt_art::Label::Kind::Text;
        label.text = wet::prompt_art::ShortKeyName(first);
        return true;
    }

    std::mutex s_lock;
    std::unordered_map<uint32_t, uint64_t> s_seen;   // data address -> hash of its DDS header
    int s_logged = 0, s_redrawn = 0;

    uint64_t Hash(const uint8_t* p, size_t n)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }

    bool Valid(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000u; }

    void Redraw(uint8_t* base, uint32_t stream)
    {
        uint32_t entry = PPC_LOAD_U32(stream + 16);
        if (!Valid(entry))
            return;
        uint32_t namePtr = PPC_LOAD_U32(entry + 0), data = PPC_LOAD_U32(entry + 8), size = PPC_LOAD_U32(entry + 12);
        if (!Valid(namePtr) || !Valid(data) || size < 128 || size > (1u << 20) || data + size < data)
            return;
        std::string name(reinterpret_cast<const char*>(base + namePtr), strnlen(reinterpret_cast<const char*>(base + namePtr), 128));
        bool orb, flatten;
        bool button = wet::prompt_art::ForName(name, orb, flatten) != wet::prompt_art::Symbol::None;

        std::lock_guard lock(s_lock);
        // WET_TEXTURE_LIST=1: every image this loader reads, once each, to
        // textures.txt (finding prompt images that aren't redrawn yet).
        {
            static std::unordered_map<std::string, int> s_names;
            static FILE* s_list = (std::getenv("WET_TEXTURE_LIST") || std::getenv("WET_DUMP_HUD_IMAGES"))
                                      ? std::fopen("textures.txt", "w")
                                      : nullptr;
            if (s_list && s_names.size() < 20000 && s_names.emplace(name, 1).second)
            {
                // WET_DUMP_HUD_IMAGES=<folder>: also the DDS files themselves.
                static const char* s_dump = std::getenv("WET_DUMP_HUD_IMAGES");
                if (s_dump && *s_dump)
                    if (FILE* f = std::fopen((std::string(s_dump) + "/" + name).c_str(), "wb"))
                    {
                        std::fwrite(base + data, 1, size, f);
                        std::fclose(f);
                    }
                std::fprintf(s_list, "%s\t%u bytes%s\n", name.c_str(), size, button ? "\tbutton" : "");
                std::fflush(s_list);
            }
        }
        if (s_logged < 6 || (button && s_logged < 12))
        {
            s_logged++;
            wet::Log("Prompts: image \"%s\" (%u bytes at %08X)%s", name.c_str(), size, data, button ? " - a button" : "");
        }
        if (!button)
            return;
        // Only the first time a texture is read: after that the game has
        // converted its pixels to the GPU's layout in place (redrawing those
        // gave garbage when the setting changed mid-game).
        uint8_t* dds = base + data;
        auto it = s_seen.find(data);
        if (it != s_seen.end() && it->second == Hash(dds, 128))
            return;
        Art art = WantedArt();
        wet::prompt_art::Label label;
        bool orbUnused, flattenUnused;
        wet::prompt_art::Symbol sym = wet::prompt_art::ForName(name, orbUnused, flattenUnused);
        if (art == Art::Keyboard && !KeyboardLabel(sym, label))
            art = Art::Xbox;   // not bound to anything: keep the pad button
        if (art == Art::PlayStation && sym > wet::prompt_art::Symbol::R2)
            art = Art::Xbox;   // start, stick and d-pad icons have no PlayStation version
        if (art == Art::Xbox)
        {
            s_seen[data] = Hash(dds, 128);
            return;
        }
        if (data >= 0xA0000000u)
            wet::PrepareHostWrite(data, size);
        if (!wet::prompt_art::RedrawDds(name, dds, size, art == Art::Keyboard ? &label : nullptr))
        {
            wet::Log("Prompts: \"%s\" isn't in a format the button art handles", name.c_str());
            return;
        }
        s_seen[data] = Hash(dds, 128);
        if (++s_redrawn <= 3 || s_redrawn % 20 == 0)
            wet::Log("Prompts: \"%s\" redrawn with %s art (%d so far)", name.c_str(),
                     art == Art::Keyboard ? "keyboard / mouse" : "PlayStation", s_redrawn);
    }
}

// ---- In-world prompts ----
// The button over an enemy to finish (and on usable objects) is a texture in the
// level's data, not the HUD's: an opaque 64x64 DXT1 disc in the button's colour
// (prompt_art::FindWorldOrb). The GPU emulation shows the runtime each small
// texture a draw reads, before it loads it (Xenia's WetCheckTextures); the disc
// is redrawn there, in guest memory (tiled, 8-in-16 byte order).
namespace
{
    // texture_address::Tiled2D: byte offset of block (x, y), pitch in blocks.
    uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitch, unsigned bpbLog2)
    {
        uint32_t outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
        uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
        uint32_t oib = (outer | inner) << bpbLog2;
        uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
        return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 15) | (((oib >> 4) & 1) << 5) |
               (((oib >> 5) & 7) << 8) | (oib >> 8 << 12);
    }

    std::mutex s_worldLock;
    int s_worldRedrawn = 0;
}

namespace wet::prompts
{
    void WorldTextureSeen(uint32_t phys, uint8_t* data, uint32_t format, uint32_t w, uint32_t h, bool tiled,
                          uint32_t endian, uint32_t pitch)
    {
        using namespace wet::prompt_art;
        constexpr uint32_t kDxt1 = 18;
        if (format != kDxt1 || w != 64 || h != 64 || !tiled || endian != 1 || wet::options::ButtonPrompts() == 1)
            return;
        // Untile and byte-swap the 16x16 blocks.
        constexpr uint32_t kBlocks = 16;
        std::vector<uint8_t> linear(kBlocks * kBlocks * 8);
        for (uint32_t y = 0; y < kBlocks; y++)
            for (uint32_t x = 0; x < kBlocks; x++)
            {
                const uint8_t* src = data + Tiled2D(x, y, pitch, 3);
                uint8_t* dst = &linear[(y * kBlocks + x) * 8];
                for (int i = 0; i < 8; i += 2)
                {
                    dst[i] = src[i + 1];
                    dst[i + 1] = src[i];
                }
            }
        std::vector<Rgba> px(64 * 64);
        DecodeDxt1(linear.data(), 64, 64, px.data());
        float cx, cy, radius;
        Symbol sym = FindWorldOrb(px.data(), 64, 64, cx, cy, radius);
        if (sym == Symbol::None)
            return;
        Art art = WantedArt();
        Label label;
        if (art == Art::Keyboard && !KeyboardLabel(sym, label))
            art = Art::Xbox;
        static const char* kNames[] = { "", "A", "B", "X", "Y" };
        const char* pad = kNames[sym == Symbol::Cross ? 1 : sym == Symbol::Circle ? 2 : sym == Symbol::Square ? 3 : 4];
        std::lock_guard lock(s_worldLock);
        if (art == Art::Xbox)
        {
            wet::Log("Prompts: in-world %s button at %08X (left as it is)", pad, phys);
            return;
        }
        RedrawWorldOrb(px.data(), 64, 64, cx, cy, radius, sym, art == Art::Keyboard ? &label : nullptr);
        EncodeDxt1(px.data(), 64, 64, linear.data());
        // The GPU emulation's copy of the old texture goes (the write-watch).
        wet::PrepareHostWrite(0xA0000000u + phys, pitch * 32 * 8);
        for (uint32_t y = 0; y < kBlocks; y++)
            for (uint32_t x = 0; x < kBlocks; x++)
            {
                uint8_t* dst = data + Tiled2D(x, y, pitch, 3);
                const uint8_t* src = &linear[(y * kBlocks + x) * 8];
                for (int i = 0; i < 8; i += 2)
                {
                    dst[i] = src[i + 1];
                    dst[i + 1] = src[i];
                }
            }
        ++s_worldRedrawn;
        wet::Log("Prompts: in-world %s button at %08X redrawn with %s art (%d so far)", pad, phys,
                 art == Art::Keyboard ? "keyboard / mouse" : "PlayStation", s_worldRedrawn);
    }
}

// The HUD's DDS loader: reads the texture from the stream (r3).
extern "C" void __imp__sub_82B9AC80(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82B9AC80)
{
    if (ctx.r3.u32 && wet::options::ButtonPrompts() != 1)
        Redraw(base, ctx.r3.u32);
    __imp__sub_82B9AC80(ctx, base);
}
