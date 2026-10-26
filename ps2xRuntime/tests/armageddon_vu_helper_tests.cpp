// GPL-3.0 reference fixtures/checks: OGmidway/mksm-recomp @ 9c014184b54f8f4235db9327bf6a5d77566475c5.
#include "ps2_runtime.h"

#include <array>
#include <cstring>
#include <iostream>

struct Fixture
{
    uint32_t op;
    std::array<uint32_t, 4> left;
    std::array<uint32_t, 4> right;
    std::array<uint32_t, 4> result;
    std::array<uint32_t, 4> flags;
};

static const Fixture fixtures[] = {
#include "fixtures/armageddon_vu_sub_cases.inc"
};

void armageddonVuSub(R5900Context *ctx, uint32_t word);

int main()
{
    unsigned cases = 0;
    for (const auto &fixture : fixtures)
    {
        for (unsigned mask = 0; mask < 16; ++mask)
        {
            for (const unsigned destination : {0u, 1u, 2u, 7u})
            {
                R5900Context context{};
                context.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
                context.vu0_vf[7] = _mm_set_ps(-14.0f, -13.0f, -12.0f, -11.0f);
                std::memcpy(&context.vu0_vf[1], fixture.left.data(), 16);
                std::memcpy(&context.vu0_vf[2], fixture.right.data(), 16);
                context.vu0_q = 3.0f;
                context.vu0_i = 3.0f;
                context.vu0_mac_flags = 0xDEADu;
                context.vu0_status = 0xA3Fu;

                std::array<uint32_t, 4> expected{};
                std::memcpy(expected.data(), &context.vu0_vf[destination], 16);
                uint32_t expectedMac = 0;
                uint32_t status = 0;
                for (unsigned lane = 0; lane < 4; ++lane)
                {
                    const uint32_t laneMask = 8u >> lane;
                    if ((mask & laneMask) == 0)
                        continue;
                    if (destination != 0)
                        expected[lane] = fixture.result[lane];
                    for (unsigned bit = 0; bit < 4; ++bit)
                    {
                        if ((fixture.flags[lane] & (1u << bit)) != 0)
                            expectedMac |= laneMask << (4u * bit);
                    }
                    status |= fixture.flags[lane];
                }

                const uint32_t word = 0x4A000000u | (mask << 21) | (2u << 16) |
                                      (1u << 11) | (destination << 6) | fixture.op;
                armageddonVuSub(&context, word);

                std::array<uint32_t, 4> actual{};
                std::memcpy(actual.data(), &context.vu0_vf[destination], 16);
                const uint16_t expectedStatus = static_cast<uint16_t>(
                    (0xA3Fu & 0xFF0u) | status | (status << 6u));
                if (actual != expected || context.vu0_mac_flags != expectedMac ||
                    context.vu0_status != expectedStatus || context.vu0_vpu_stat != 0)
                {
                    std::cerr << "FAIL VSUB op=0x" << std::hex << fixture.op
                              << " mask=0x" << mask << " destination=" << std::dec
                              << destination << '\n';
                    return 1;
                }
                ++cases;
            }
        }
    }

    std::cout << "PASS: " << cases
              << " VSUB mask/destination cases; VF0, aliasing, flags, and edge values\n";
    return 0;
}
