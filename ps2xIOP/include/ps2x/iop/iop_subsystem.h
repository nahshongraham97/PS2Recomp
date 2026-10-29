#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    class IopSubsystem
    {
    public:
        explicit IopSubsystem(IopHost &host);
        ~IopSubsystem();

        IopSubsystem(const IopSubsystem &) = delete;
        IopSubsystem &operator=(const IopSubsystem &) = delete;
        IopSubsystem(IopSubsystem &&) noexcept;
        IopSubsystem &operator=(IopSubsystem &&) noexcept;

        void reset();

        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadModuleScheduled(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadModuleBufferScheduled(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadRomDirModuleScheduled(std::string_view imagePath, std::string_view moduleName,
                                                                 const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] IopModuleStartState moduleStartState(int32_t moduleId);
        [[nodiscard]] IopCallResult callExport(std::string_view library, uint16_t ordinal,
                                                uint32_t a0 = 0, uint32_t a1 = 0,
                                                uint32_t a2 = 0, uint32_t a3 = 0,
                                                uint32_t instructionBudget = 100000);
        [[nodiscard]] IopScheduledCall scheduleExportCall(std::string_view library, uint16_t ordinal,
                                                           uint32_t a0 = 0, uint32_t a1 = 0,
                                                           uint32_t a2 = 0, uint32_t a3 = 0);
        [[nodiscard]] IopScheduledCallState scheduledCallState(uint32_t callId) const;
        [[nodiscard]] bool releaseScheduledCall(uint32_t callId);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result = nullptr);
        // Run LOADCORE callbacks after the complete boot-module start phase.
        // Unsupported callback-list shapes fail closed instead of being skipped.
        [[nodiscard]] bool runPostBootCallbacks();
        void runEeCycles(uint64_t eeCycles) noexcept;

        [[nodiscard]] RpcAbi selectRpcAbi(const RpcAbiRequest &request) const;
        [[nodiscard]] bool canBindRpc(uint32_t sid) const noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        void onSifTransfer(const SifTransfer &transfer);

        // Physical IOP RAM access shared by the emulator, SIF DMA, and HLE services. Addresses are IOP addresses.
        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool readSifRegister(uint32_t index, uint32_t &value) const;
        [[nodiscard]] bool writeSifRegisterFromEe(uint32_t index, uint32_t value);
        [[nodiscard]] bool writeSifRegisterFromIop(uint32_t index, uint32_t value);
        // Map selected user-provided 4 MB PS2 ROM0 into the IOP's read-only
        // BIOS window. The image persists across IOP reset.
        [[nodiscard]] bool setRom0Image(std::span<const uint8_t> image);
        // Prepare the runtime's host CDVD media model after reset when a
        // profile explicitly selects the builtin CDVDMAN implementation.
        [[nodiscard]] bool prepareCdvdDeviceState();
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] DebugSnapshot debugSnapshot() const;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
