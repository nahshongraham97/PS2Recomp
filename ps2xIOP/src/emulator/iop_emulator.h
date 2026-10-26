#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    class IopEmulator
    {
    public:
        explicit IopEmulator(IopHost &host);
        ~IopEmulator();

        IopEmulator(const IopEmulator &) = delete;
        IopEmulator &operator=(const IopEmulator &) = delete;

        void reset();
        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadModuleScheduled(std::string_view path, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadModuleScheduledWithArgc(std::string_view path,
                                                                    const void *arguments,
                                                                    uint32_t argumentSize,
                                                                    uint32_t argumentCount);
        [[nodiscard]] ModuleLoadResult loadModuleBufferScheduled(uint32_t guestAddress, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadRomDirModuleScheduled(std::string_view imagePath,
                                                                 std::string_view moduleName,
                                                                 const void *arguments,
                                                                 uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadRomDirModuleScheduledWithArgc(std::string_view imagePath,
                                                                         std::string_view moduleName,
                                                                         const void *arguments,
                                                                         uint32_t argumentSize,
                                                                         uint32_t argumentCount);
        [[nodiscard]] IopModuleStartState moduleStartState(int32_t moduleId);
        [[nodiscard]] IopCallResult callExport(std::string_view library, uint16_t ordinal,
                                                uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                                uint32_t instructionBudget);
        [[nodiscard]] IopScheduledCall scheduleExportCall(std::string_view library, uint16_t ordinal,
                                                           uint32_t a0, uint32_t a1,
                                                           uint32_t a2, uint32_t a3);
        [[nodiscard]] IopScheduledCallState scheduledCallState(uint32_t callId) const;
        [[nodiscard]] bool releaseScheduledCall(uint32_t callId);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result);
        void runEeCycles(uint64_t eeCycles) noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        [[nodiscard]] bool hasRpcServer(uint32_t sid) const noexcept;
        void onSifTransfer(const SifTransfer &transfer);

        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool readSifRegister(uint32_t index, uint32_t &value) const;
        [[nodiscard]] bool writeSifRegisterFromEe(uint32_t index, uint32_t value);
        [[nodiscard]] bool writeSifRegisterFromIop(uint32_t index, uint32_t value);
        [[nodiscard]] bool setRom0Image(std::span<const uint8_t> image);
        [[nodiscard]] bool prepareCdvdDeviceState();
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] uint64_t cycles() const noexcept;
        [[nodiscard]] uint64_t instructions() const noexcept;
        [[nodiscard]] uint32_t loadedModuleCount() const noexcept;
        [[nodiscard]] uint32_t threadCount() const noexcept;
        [[nodiscard]] uint32_t rpcServerCount() const noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
