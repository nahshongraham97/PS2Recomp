#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace armageddon_hle
{
#pragma pack(push, 1)
struct PadDataNew
{
    uint32_t frame;            // 0x00
    uint8_t  padding04[0x4C];  // 0x04..0x4F
    uint16_t modeTable[4];     // 0x50..0x57
    uint32_t frameCounter;     // 0x58..0x5B
    uint8_t  data[8];          // 0x5C..0x63
    uint8_t  modeConfig;       // 0x64
    uint8_t  modeCurId;        // 0x65
    uint8_t  padding66[7];     // 0x66..0x6C
    uint8_t  mode;             // 0x6D
    uint8_t  lock;             // 0x6E
    uint8_t  padding6F;        // 0x6F
    uint8_t  state;            // 0x70
    uint8_t  reqState;         // 0x71
    uint8_t  currentTask;      // 0x72
    uint8_t  length;           // 0x73
    uint8_t  padding74[0x0C];  // 0x74..0x7F
};
struct PadAreaNew
{
    PadDataNew buffers[2];
};
#pragma pack(pop)
static_assert(sizeof(PadDataNew) == 128, "PadDataNew must be exactly 128 bytes");
static_assert(offsetof(PadDataNew, modeTable) == 0x50, "modeTable offset must be 0x50");
static_assert(offsetof(PadDataNew, modeConfig) == 0x64, "modeConfig offset must be 0x64");
static_assert(offsetof(PadDataNew, modeCurId) == 0x65, "modeCurId offset must be 0x65");
static_assert(offsetof(PadDataNew, mode) == 0x6D, "mode offset must be 0x6D");
static_assert(offsetof(PadDataNew, lock) == 0x6E, "lock offset must be 0x6E");
static_assert(offsetof(PadDataNew, state) == 0x70, "state offset must be 0x70");
static_assert(offsetof(PadDataNew, reqState) == 0x71, "reqState offset must be 0x71");
static_assert(offsetof(PadDataNew, currentTask) == 0x72, "currentTask offset must be 0x72");
static_assert(offsetof(PadDataNew, length) == 0x73, "length offset must be 0x73");
static_assert(sizeof(PadAreaNew) == 256, "PadAreaNew must be exactly 256 bytes");

constexpr uint8_t PAD_STATE_DISCON   = 0;
constexpr uint8_t PAD_STATE_FINDPAD  = 1;
constexpr uint8_t PAD_STATE_FINDCTP1 = 2;
constexpr uint8_t PAD_STATE_EXECCMD  = 5;
constexpr uint8_t PAD_STATE_STABLE   = 6;
constexpr uint8_t PAD_STATE_ERROR    = 7;

constexpr uint8_t PAD_RSTAT_COMPLETE = 0;
constexpr uint8_t PAD_RSTAT_FAILED   = 1;
constexpr uint8_t PAD_RSTAT_BUSY     = 2;

void populatePadBuffer(PadDataNew &buf, uint32_t frameVal, uint32_t counterVal, uint8_t state, uint8_t reqState);
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
