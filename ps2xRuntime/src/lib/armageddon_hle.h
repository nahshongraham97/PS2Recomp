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
    uint8_t  data[32];         // 0x00..0x1F
    uint32_t actDirData[2];    // 0x20..0x27
    uint32_t actAlignData[2];  // 0x28..0x2F
    uint8_t  actData[8][4];    // 0x30..0x4F
    uint16_t modeTable[4];     // 0x50..0x57
    uint32_t frame;            // 0x58..0x5B
    uint32_t findPadRetries;   // 0x5C..0x5F
    uint32_t length;           // 0x60..0x63
    uint8_t  modeConfig;       // 0x64
    uint8_t  modeCurId;        // 0x65
    uint8_t  model;            // 0x66
    uint8_t  buttonDataReady;  // 0x67
    uint8_t  nrOfModes;        // 0x68
    uint8_t  modeCurOffs;      // 0x69
    uint8_t  nrOfActuators;    // 0x6A
    uint8_t  numActComb;       // 0x6B
    uint8_t  val_c6;           // 0x6C
    uint8_t  mode;             // 0x6D
    uint8_t  lock;             // 0x6E
    uint8_t  actDirSize;       // 0x6F
    uint8_t  state;            // 0x70
    uint8_t  reqState;         // 0x71
    uint8_t  currentTask;      // 0x72
    uint8_t  runTask;          // 0x73
    uint8_t  stat70bit;        // 0x74
    uint8_t  padding[11];      // 0x75..0x7F
};
struct PadAreaNew
{
    PadDataNew buffers[2];
};
#pragma pack(pop)
static_assert(sizeof(PadDataNew) == 128, "PadDataNew must be exactly 128 bytes");
static_assert(offsetof(PadDataNew, data) == 0x00, "data offset must be 0x00");
static_assert(offsetof(PadDataNew, actDirData) == 0x20, "actDirData offset must be 0x20");
static_assert(offsetof(PadDataNew, actAlignData) == 0x28, "actAlignData offset must be 0x28");
static_assert(offsetof(PadDataNew, actData) == 0x30, "actData offset must be 0x30");
static_assert(offsetof(PadDataNew, modeTable) == 0x50, "modeTable offset must be 0x50");
static_assert(offsetof(PadDataNew, frame) == 0x58, "frame offset must be 0x58");
static_assert(offsetof(PadDataNew, findPadRetries) == 0x5C, "findPadRetries offset must be 0x5C");
static_assert(offsetof(PadDataNew, length) == 0x60, "length offset must be 0x60");
static_assert(offsetof(PadDataNew, modeConfig) == 0x64, "modeConfig offset must be 0x64");
static_assert(offsetof(PadDataNew, modeCurId) == 0x65, "modeCurId offset must be 0x65");
static_assert(offsetof(PadDataNew, model) == 0x66, "model offset must be 0x66");
static_assert(offsetof(PadDataNew, buttonDataReady) == 0x67, "buttonDataReady offset must be 0x67");
static_assert(offsetof(PadDataNew, nrOfModes) == 0x68, "nrOfModes offset must be 0x68");
static_assert(offsetof(PadDataNew, modeCurOffs) == 0x69, "modeCurOffs offset must be 0x69");
static_assert(offsetof(PadDataNew, nrOfActuators) == 0x6A, "nrOfActuators offset must be 0x6A");
static_assert(offsetof(PadDataNew, numActComb) == 0x6B, "numActComb offset must be 0x6B");
static_assert(offsetof(PadDataNew, val_c6) == 0x6C, "val_c6 offset must be 0x6C");
static_assert(offsetof(PadDataNew, mode) == 0x6D, "mode offset must be 0x6D");
static_assert(offsetof(PadDataNew, lock) == 0x6E, "lock offset must be 0x6E");
static_assert(offsetof(PadDataNew, actDirSize) == 0x6F, "actDirSize offset must be 0x6F");
static_assert(offsetof(PadDataNew, state) == 0x70, "state offset must be 0x70");
static_assert(offsetof(PadDataNew, reqState) == 0x71, "reqState offset must be 0x71");
static_assert(offsetof(PadDataNew, currentTask) == 0x72, "currentTask offset must be 0x72");
static_assert(offsetof(PadDataNew, runTask) == 0x73, "runTask offset must be 0x73");
static_assert(offsetof(PadDataNew, stat70bit) == 0x74, "stat70bit offset must be 0x74");
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

void populatePadBuffer(PadDataNew &buf, uint32_t frameVal, uint8_t state, uint8_t reqState);
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
