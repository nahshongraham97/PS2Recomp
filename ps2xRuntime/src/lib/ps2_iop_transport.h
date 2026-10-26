#pragma once

#include "ps2_runtime.h"
#include "ps2x/iop/iop_subsystem.h"

#include <utility>

void runPendingArmageddonHostReboot(PS2Runtime *runtime);

class PS2IopTransport
{
public:
    [[nodiscard]] static ps2x::iop::RpcAbi selectRpcAbi(
        const PS2Runtime *runtime,
        const ps2x::iop::RpcAbiRequest &request)
    {
        return runtime
                   ? runtime->selectIopRpcAbi(request)
                   : ps2x::iop::RpcAbi::RuntimeDefault;
    }

    [[nodiscard]] static ps2x::iop::RpcResult handleRpc(
        PS2Runtime *runtime,
        uint8_t *rdram,
        R5900Context *context,
        ps2x::iop::RpcRequest request)
    {
        return runtime
                   ? runtime->handleIopRpc(rdram, context, std::move(request))
                   : ps2x::iop::RpcResult{};
    }

    [[nodiscard]] static bool canBindRpc(const PS2Runtime *runtime, uint32_t sid)
    {
        return !runtime || runtime->canBindIopRpc(sid);
    }

    static void notifyTransfer(
        PS2Runtime *runtime,
        uint8_t *rdram,
        const ps2x::iop::SifTransfer &transfer)
    {
        if (runtime)
        {
            runtime->notifyIopSifTransfer(rdram, transfer);
        }
    }

    static void reset(PS2Runtime *runtime)
    {
        if (runtime)
        {
            runtime->resetIop();
        }
    }

    [[nodiscard]] static bool readSifRegister(
        const PS2Runtime *runtime,
        uint32_t index,
        uint32_t &value)
    {
        return runtime && runtime->m_iopSubsystem &&
               runtime->m_iopSubsystem->readSifRegister(index, value);
    }

    [[nodiscard]] static bool writeSifRegisterFromEe(
        PS2Runtime *runtime,
        uint32_t index,
        uint32_t value)
    {
        if (!runtime || !runtime->m_iopSubsystem ||
            !runtime->m_iopSubsystem->writeSifRegisterFromEe(index, value))
        {
            return false;
        }
        // Armageddon's original reset routine acknowledges BOOTEND, SIFINIT,
        // then CMDINIT. Run the explicitly selected host reboot only after the
        // final guest acknowledgement has cleared its pre-reset flags.
        if (index == 4u && value == 0x00020000u)
        {
            runPendingArmageddonHostReboot(runtime);
        }
        return true;
    }
};
