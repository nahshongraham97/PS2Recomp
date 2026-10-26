#pragma once

#include "ps2_runtime.h"

#include <cstdint>
#include <iostream>

// Confirm reviewed entry points are present in the generated table. A
// successful result does not claim that the guest executed them or booted.
inline int armageddonRegistrationCheck()
{
    struct Target
    {
        uint32_t address;
        const char *name;
    };
    constexpr Target targets[] = {
        {0x00100008u, "ELF entry"},
        {0x001E0EB8u, "restored full owner FUN_001e0eb8"},
        {0x001D845Cu, "FUN_001d7ca8 continuation block"},
        {0x001D7CA8u, "restored owner FUN_001d7ca8"},
        {0x00125120u, "FUN_001250b8 internal resumable continuation"},
        {0x001250B8u, "FUN_001250b8 owner start"},
        {0x0010F5C8u, "set_sreg callback"},
        {0x0010F5E8u, "change_saddr callback"},
        {0x001162A0u, "relocation copy"},
        {0x001162B0u, "relocation continuation"},
        {0x001DB018u, "restored reset-path function"},
        {0x00648680u, "dynamic reset-path target"},
        {0x00648A40u, "SIF table target 7"},
        {0x00648A90u, "SIF table target 8"},
        {0x00648AB0u, "SIF table target 9"},
        {0x001E13B4u, "observed owner continuation"},
        {0x001E0FCCu, "FUN_001e0eb8 internal continuation"},
        {0x001E0FECu, "FUN_001e0eb8 internal tail continuation"},
    };

    unsigned missing = 0;
    for (const Target &target : targets)
    {
        bool registered = false;
        uint32_t slot = 0;
        if (target.address >= g_ps2RecompiledFunctionTableBase &&
            target.address < g_ps2RecompiledFunctionTableEnd &&
            ((target.address - g_ps2RecompiledFunctionTableBase) & 3u) == 0u)
        {
            slot = (target.address - g_ps2RecompiledFunctionTableBase) >> 2u;
            registered = slot < g_ps2RecompiledFunctionTableSlotCount &&
                g_ps2RecompiledFunctionTable[slot] != nullptr;
        }
        std::cout << target.name << " address=0x" << std::hex << target.address
                  << std::dec << " slot=" << slot << " registered="
                  << (registered ? "true" : "false") << '\n';
        if (!registered)
            ++missing;
    }
    std::cout << "missing=" << missing << " boot_verified=false\n";
    return missing == 0 ? 0 : 1;
}
