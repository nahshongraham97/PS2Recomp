#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace armageddon_hle
{
    [[nodiscard]] bool enabled(const PS2Runtime *runtime) noexcept;
    [[nodiscard]] bool readSifRegister(const PS2Runtime *runtime, uint32_t index, uint32_t &value);
    [[nodiscard]] bool readSifSreg(const PS2Runtime *runtime,
                                  uint8_t *rdram,
                                  uint32_t index,
                                  uint32_t &value);
    [[nodiscard]] bool skipIopReset(const PS2Runtime *runtime,
                                    const R5900Context *context,
                                    const char *operation);
    [[nodiscard]] bool canBindRpc(const PS2Runtime *runtime, uint32_t sid);
    [[nodiscard]] ps2x::iop::RpcResult handleRpc(const PS2Runtime *runtime,
                                                uint8_t *rdram,
                                                const ps2x::iop::RpcRequest &request);
}
