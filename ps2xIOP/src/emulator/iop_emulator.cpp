#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        // PS2SDK intrman.h names IOP_IRQ_DMA_SIF1 as DMA IRQ 0x2B. EE-to-IOP
        // SIF transfers complete on this channel. Delivery is deferred to the
        // existing IOP cycle scheduler, after the host has copied the payload.
        constexpr int kIopIrqDmaSif1 = 0x2B;
        constexpr int kIopIrqDmaSif0 = 0x2A;
        constexpr int kIopIrqCdvd = 0x02;
        constexpr int kNoDmaInterrupt = -1;
        constexpr uint32_t kIopSif0ChcrChainMode = 0x00000700u;
        constexpr uint32_t kIopSif0TagChain = 0x10000000u;
        constexpr uint32_t kIopSif0TagInterruptOutput = 0x80000000u;
        constexpr uint32_t kIopSif0TagEnd = 0x80000000u;
        constexpr uint32_t kIopSif0MaxTags = 32u;
        constexpr uint32_t kIopSif0MaxBytes = 32u * 1024u * 1024u;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
            bool startPending = false;
            bool startReturned = false;
            int32_t startResult = -1;
            int startThreadId = 0;
            uint32_t startArguments = 0;
            uint32_t startStopPc = 0;
            uint64_t startInstructions = 0;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        struct PendingDma
        {
            int irq = kNoDmaInterrupt;
            uint64_t completionCycle = 0u;
            uint32_t channel = 0u;
            uint32_t finalMadr = 0u;
            uint32_t finalTadr = 0u;
            bool sif0 = false;
            bool interruptOutput = false;
        };

        struct Sif0Transfer
        {
            uint32_t source = 0u;
            uint32_t destination = 0u;
            uint32_t size = 0u;
            bool interruptOutput = false;
            std::vector<uint8_t> bytes;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            scheduledExportCalls.clear();
            nextScheduledCallId = 1u;
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            const auto dma = memory.takeDmaStart();
            if (!dma)
                return;

            if (dma->channel != 9u)
            {
                pendingDmaInterrupts[dma->irq] = PendingDma{
                    dma->irq, totalCycles + dma->delayCycles, dma->channel, 0u, 0u, false, false};
                return;
            }

            if ((dma->chcr & kIopSif0ChcrChainMode) != kIopSif0ChcrChainMode)
            {
                log(LogLevel::Error, "[IOP_SIF0_CHAIN_REJECTED] unsupported IOP SIF0 DMA mode");
                return;
            }

            std::vector<Sif0Transfer> transfers;
            transfers.reserve(kIopSif0MaxTags);
            uint32_t tagAddress = dma->tadr;
            uint32_t finalMadr = dma->madr;
            uint32_t finalTadr = tagAddress;
            uint32_t totalBytes = 0u;
            bool chainEnded = false;
            bool interruptOutput = false;

            for (uint32_t i = 0u; i < kIopSif0MaxTags; ++i)
            {
                uint32_t tag[4]{};
                if (tagAddress > IopMemory::RamSize - sizeof(tag) ||
                    !memory.readRam(tagAddress, tag, sizeof(tag)))
                {
                    log(LogLevel::Error, "[IOP_SIF0_CHAIN_REJECTED] tag outside IOP RAM");
                    return;
                }
                if ((tag[2] & kIopSif0TagChain) == 0u)
                {
                    log(LogLevel::Error, "[IOP_SIF0_CHAIN_REJECTED] unrecognized SIF tag format");
                    return;
                }

                const uint32_t words = tag[1] & 0x00FFFFFFu;
                const uint32_t qwords = tag[2] & 0x0000FFFFu;
                const uint64_t usedBytes = static_cast<uint64_t>(words) * sizeof(uint32_t);
                const uint64_t transferBytes = static_cast<uint64_t>(qwords) * 16u;
                if (words == 0u || qwords == 0u || usedBytes > transferBytes ||
                    transferBytes > kIopSif0MaxBytes || totalBytes > kIopSif0MaxBytes - transferBytes)
                {
                    log(LogLevel::Error, "[IOP_SIF0_CHAIN_REJECTED] invalid SIF tag size");
                    return;
                }

                Sif0Transfer transfer{};
                transfer.source = tag[0] & 0x00FFFFFFu;
                transfer.destination = tag[3] & 0x1FFFFFFFu;
                transfer.size = static_cast<uint32_t>(transferBytes);
                transfer.interruptOutput = (tag[2] & kIopSif0TagInterruptOutput) != 0u;
                transfer.bytes.resize(transfer.size);
                if (!memory.readRam(transfer.source, transfer.bytes.data(), transfer.bytes.size()))
                {
                    log(LogLevel::Error, "[IOP_SIF0_CHAIN_REJECTED] source outside IOP RAM");
                    return;
                }
                std::vector<uint8_t> destinationProbe(transfer.size);
                if (!host.readGuest(transfer.destination, destinationProbe.data(), destinationProbe.size()))
                {
                    log(LogLevel::Error, "[IOP_SIF0_CHAIN_REJECTED] destination outside EE RAM");
                    return;
                }

                totalBytes += transfer.size;
                interruptOutput = interruptOutput || transfer.interruptOutput;
                finalMadr = (transfer.source + transfer.size) & 0x00FFFFFFu;
                finalTadr = tagAddress + sizeof(tag);
                chainEnded = (tag[0] & kIopSif0TagEnd) != 0u;
                transfers.push_back(std::move(transfer));
                if (chainEnded)
                    break;
                tagAddress = finalTadr;
            }

            if (!chainEnded || transfers.empty())
            {
                log(LogLevel::Error, "[IOP_SIF0_CHAIN_REJECTED] unterminated or empty SIF chain");
                return;
            }

            for (const Sif0Transfer &transfer : transfers)
            {
                if (!host.writeGuest(transfer.destination, transfer.bytes.data(), transfer.bytes.size()))
                {
                    log(LogLevel::Error, "[IOP_SIF0_CHAIN_REJECTED] EE destination write failed after validation");
                    return;
                }
            }

            if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
            {
                std::ostringstream out;
                out << "[IOP_SIF0_CHAIN] tadr=0x" << std::hex << dma->tadr
                    << " tags=" << std::dec << transfers.size()
                    << " bytes=" << totalBytes
                    << " final_madr=0x" << std::hex << finalMadr
                    << " final_tadr=0x" << finalTadr
                    << " interrupt_output=" << (interruptOutput ? "true" : "false");
                for (const Sif0Transfer &transfer : transfers)
                {
                    out << " transfer{src=0x" << std::hex << transfer.source
                        << ",dst=0x" << transfer.destination
                        << ",bytes=0x" << transfer.size << "}";
                    if (transfer.size <= 64u)
                    {
                        constexpr char hex[] = "0123456789abcdef";
                        out << " data=";
                        for (uint8_t byte : transfer.bytes)
                        {
                            out << hex[(byte >> 4u) & 0x0Fu] << hex[byte & 0x0Fu];
                        }
                    }
                }
                log(LogLevel::Info, out.str());
            }

            if (interruptOutput)
                host.notifyEeSif0DmaComplete();

            const int irq = interruptOutput ? kIopIrqDmaSif0 : kNoDmaInterrupt;
            pendingDmaInterrupts[irq] = PendingDma{
                irq,
                totalCycles + dma->delayCycles,
                9u,
                finalMadr,
                finalTadr,
                true,
                interruptOutput,
            };
        }

        void serviceCdvdDevice()
        {
            if (!cdvd.serviceDevice())
                return;

            if (std::getenv("PS2X_IOP_CDVD_TRACE") != nullptr)
            {
                std::cerr << "[IOP_CDVD_COMPLETION_QUEUED] cycle=" << totalCycles
                          << " i_stat=0x" << std::hex << memory.interruptStatus()
                          << " i_mask=0x" << memory.interruptMask()
                          << " i_ctrl=0x" << memory.interruptControl()
                          << " cdvd_i_stat=0x" << memory.read32(0x1F402008u)
                          << std::dec << std::endl;
            }

            // CDVD command completion is delivered through the guest's
            // registered IOP IRQ 2 handler. The one-cycle delay lets the
            // current MMIO store retire before the handler reads I_STAT.
            pendingDmaInterrupts[kIopIrqCdvd] = PendingDma{
                kIopIrqCdvd, totalCycles + 1u, 0u, 0u, 0u, false, false};
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                if (std::getenv("PS2X_IOP_TRACE_THREAD_IMPORTS") != nullptr)
                {
                    static uint32_t emittedThreadImports = 0u;
                    if (emittedThreadImports++ < 512u)
                    {
                        std::ostringstream out;
                        out << "[IOP_THREAD_IMPORT] library=" << call.library
                            << " ordinal=" << call.ordinal
                            << " pc=0x" << std::hex << cpu.pc
                            << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5]
                            << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7]
                            << " ra=0x" << cpu.gpr[31];
                        log(LogLevel::Info, out.str());
                    }
                }
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                const uint32_t guestTarget = imports.resolve(call.library, call.ordinal, call.version);
                if (guestTarget != 0u)
                {
                    if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
                    {
                        std::ostringstream out;
                        out << "[IOP_GUEST_IMPORT] library=sifcmd ordinal=" << call.ordinal
                            << " target=0x" << std::hex << guestTarget;
                        log(LogLevel::Info, out.str());
                    }
                    cpu.pc = guestTarget;
                    cpu.branchPending = false;
                    return ImportDisposition::JumpToGuest;
                }
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman"))
            {
                const uint32_t guestTarget = imports.resolve(call.library, call.ordinal, call.version);
                if (guestTarget != 0u)
                {
                    if (std::getenv("PS2X_IOP_IOMAN_TRACE") != nullptr)
                    {
                        std::ostringstream out;
                        out << "[IOP_GUEST_IMPORT] library=ioman ordinal=" << call.ordinal
                            << " target=0x" << std::hex << guestTarget;
                        if (call.ordinal == 20u)
                        {
                            const uint32_t descriptor = cpu.gpr[4];
                            const uint32_t namePointer = memory.read32(descriptor);
                            const uint32_t opsPointer = memory.read32(descriptor + 16u);
                            out << " adddrv_a0=0x" << descriptor
                                << " name_ptr=0x" << namePointer
                                << " name='" << memory.readString(namePointer, 64u) << "'"
                                << " ops_ptr=0x" << opsPointer;
                            if (opsPointer != 0u)
                            {
                                out << " init=0x" << memory.read32(opsPointer)
                                    << " deinit=0x" << memory.read32(opsPointer + 4u);
                            }
                        }
                        else if (call.ordinal == 4u)
                        {
                            out << " open_path_ptr=0x" << cpu.gpr[4]
                                << " open_path='" << memory.readString(cpu.gpr[4], 256u) << "'"
                                << " mode=0x" << cpu.gpr[5]
                                << " caller_ra=0x" << cpu.gpr[31]
                                << " caller_gp=0x" << cpu.gpr[28]
                                << " caller_s0=0x" << cpu.gpr[16]
                                << " caller_s2=0x" << cpu.gpr[18]
                                << " caller_s2_word=0x" << memory.read32(cpu.gpr[18])
                                << " caller_s2_string='" << memory.readString(cpu.gpr[18], 128u) << "'"
                                << " caller_s4=0x" << cpu.gpr[20];
                        }
                        log(LogLevel::Info, out.str());
                    }
                    cpu.pc = guestTarget;
                    cpu.branchPending = false;
                    return ImportDisposition::JumpToGuest;
                }
                if (ioman.dispatchImport(call.ordinal, cpu, *this))
                    return ImportDisposition::Handled;
            }
            if (iequals(call.library, "sifman"))
            {
                const uint32_t guestTarget = imports.resolve(call.library, call.ordinal, call.version);
                if (guestTarget != 0u)
                {
                    if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
                    {
                        std::ostringstream out;
                        out << "[IOP_GUEST_IMPORT] library=sifman ordinal=" << call.ordinal
                            << " target=0x" << std::hex << guestTarget;
                        log(LogLevel::Info, out.str());
                    }
                    cpu.pc = guestTarget;
                    cpu.branchPending = false;
                    return ImportDisposition::JumpToGuest;
                }
                if (call.ordinal == 7u && std::getenv("PS2X_ARMAGEDDON_GUEST_SIF_ONLY") != nullptr)
                {
                    std::ostringstream out;
                    out << "[IOP_SIF_HLE_REJECTED] library=sifman ordinal=7"
                        << " pc=0x" << std::hex << cpu.pc
                        << " reason=Armageddon requires original guest sceSifSetDma";
                    log(LogLevel::Error, out.str());
                    cpu.stopped = true;
                    cpu.exception = true;
                    return ImportDisposition::Missing;
                }
                if (call.ordinal == 7u && std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
                {
                    std::ostringstream out;
                    out << "[IOP_SIF_HLE_FALLBACK] library=sifman ordinal=7"
                        << " pc=0x" << std::hex << cpu.pc;
                    log(LogLevel::Warning, out.str());
                }
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                const uint32_t guestTarget = imports.resolve(call.library, call.ordinal, call.version);
                if (guestTarget != 0u)
                {
                    if (std::getenv("PS2X_IOP_SYSCLIB_TRACE") != nullptr)
                    {
                        std::ostringstream out;
                        out << "[IOP_GUEST_IMPORT] library=sysclib ordinal=" << call.ordinal
                            << " target=0x" << std::hex << guestTarget;
                        log(LogLevel::Info, out.str());
                    }
                    cpu.pc = guestTarget;
                    cpu.branchPending = false;
                    return ImportDisposition::JumpToGuest;
                }
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            cpu.stopped = true;
            cpu.exception = true;
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (const auto import = imports.decode(cpu.pc))
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                serviceCdvdDevice();
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            serviceCdvdDevice();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            lastCallInstructions = runCpu(cpu, budget);
            lastCallCompleted = cpu.pc == kCallReturnSentinel && !cpu.exception;
            lastCallStopPc = cpu.pc;
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<PendingDma> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second.completionCycle > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->second);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const PendingDma &dma : completed)
                {
                    if (dma.sif0)
                    {
                        memory.completeSif0Dma(dma.finalMadr, dma.finalTadr, dma.interruptOutput);
                        // The DMAC completion controller acknowledges DICR2's
                        // channel-9 flag before calling the registered SIFMAN
                        // handler. The guest handler therefore sees CHCR idle
                        // and may start the next queued SIF0 chain.
                        if (dma.interruptOutput)
                            memory.acknowledgeSif0DmaInterrupt();
                    }

                    if (dma.irq < 0)
                        continue;
                    const bool dispatched = intrman.dispatchInterrupt(dma.irq, *this);
                    if (dma.irq == kIopIrqCdvd && std::getenv("PS2X_IOP_CDVD_TRACE") != nullptr)
                    {
                        std::cerr << "[IOP_CDVD_COMPLETION_DISPATCHED] cycle=" << totalCycles
                                  << " irq=0x" << std::hex << dma.irq
                                  << " dispatched=" << (dispatched ? "true" : "false")
                                  << " i_stat=0x" << memory.interruptStatus()
                                  << " i_mask=0x" << memory.interruptMask()
                                  << " i_ctrl=0x" << memory.interruptControl()
                                  << " cdvd_i_stat=0x" << memory.read32(0x1F402008u)
                                  << std::dec << std::endl;
                    }
                    if (dma.irq == kIopIrqDmaSif1 && std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
                    {
                        std::ostringstream out;
                        out << "[SIF1_IRQ_DISPATCH] cycle=" << totalCycles
                            << " irq=0x" << std::hex << dma.irq << std::dec
                            << " handler_dispatched=" << (dispatched ? "true" : "false");
                        log(LogLevel::Info, out.str());
                    }
                    if (dma.irq == kIopIrqDmaSif0 && std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
                    {
                        std::ostringstream out;
                        out << "[SIF0_IRQ_DISPATCH] cycle=" << totalCycles
                            << " irq=0x" << std::hex << dma.irq << std::dec
                            << " handler_dispatched=" << (dispatched ? "true" : "false");
                        log(LogLevel::Info, out.str());
                    }
                }
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, dma] : pendingDmaInterrupts)
                        {
                            (void)irq;
                            nextWake = std::min(nextWake, dma.completionCycle);
                        }
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    const int threadId = next->id;
                    const uint32_t executed = runCpu(next->cpu,
                                                     static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    if (IopThread *thread = kernel.findThread(threadId))
                    {
                        thread->instructionsExecuted += executed;
                        kernel.endTimeslice(*thread, kThreadReturnSentinel);
                    }
                    refreshPendingModuleStarts();
                    if (totalCycles == before)
                        ++totalCycles;
                }
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        void refreshPendingModuleStarts()
        {
            for (auto &[moduleId, module] : modules)
            {
                (void)moduleId;
                if (!module.startPending)
                    continue;
                const IopThread *thread = kernel.findThread(module.startThreadId);
                if (!thread)
                {
                    module.startPending = false;
                    module.startReturned = false;
                    module.startResult = -1;
                    module.startStopPc = 0u;
                    if (module.startArguments != 0u)
                        freeAllocation(module.startArguments);
                    module.startArguments = 0u;
                    module.startThreadId = 0;
                    continue;
                }
                if (thread->state != IopThreadState::Dormant && thread->state != IopThreadState::Dead)
                    continue;

                module.startReturned = thread->cpu.pc == kThreadReturnSentinel && !thread->cpu.exception;
                module.startResult = module.startReturned ? static_cast<int32_t>(thread->cpu.gpr[2]) : -1;
                module.startStopPc = thread->cpu.pc;
                module.startInstructions = thread->instructionsExecuted;
                module.startPending = false;
                module.resident = module.startReturned &&
                                  (module.startResult == 0 || module.startResult == 2);
                if (module.startArguments != 0u)
                    freeAllocation(module.startArguments);
                module.startArguments = 0u;
                module.startThreadId = 0;
                (void)kernel.releaseThread(thread->id);
            }
        }

        IopModuleStartState moduleStartState(int32_t moduleId)
        {
            refreshPendingModuleStarts();
            IopModuleStartState result{};
            const auto found = modules.find(moduleId);
            if (found == modules.end())
                return result;
            const Module &module = found->second;
            result.found = true;
            result.pending = module.startPending;
            result.returned = module.startReturned;
            result.moduleId = module.id;
            result.startResult = module.startResult;
            result.entry = module.entry;
            result.stopPc = module.startStopPc;
            result.instructions = module.startInstructions;
            if (module.startPending)
            {
                if (const IopThread *thread = kernel.findThread(module.startThreadId))
                {
                    result.threadId = static_cast<uint32_t>(thread->id);
                    result.threadState = static_cast<uint32_t>(thread->state);
                    result.currentPc = thread->cpu.pc;
                    result.waitId = static_cast<uint32_t>(thread->waitId);
                    result.waitBits = thread->waitBits;
                    result.instructions = thread->instructionsExecuted;
                    result.a0 = thread->cpu.gpr[4];
                    result.a1 = thread->cpu.gpr[5];
                    result.ra = thread->cpu.gpr[31];
                }
            }
            return result;
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image,
                                   const void *arguments, uint32_t argumentSize,
                                   bool scheduleStart = false,
                                   std::optional<uint32_t> moduleStartArgc = std::nullopt)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!memory.registerModuleImage(loaded.base, loaded.size))
            {
                log(LogLevel::Error, "[IOP] module image overlaps a reserved or allocated system-memory block");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            if (module.path.size() >= 8u &&
                iequals(std::string_view(module.path).substr(module.path.size() - 8u), "#CDVDMAN"))
            {
                const bool mediaPresent = cdvd.prepareDeviceState();
                if (std::getenv("PS2X_IOP_CDVD_TRACE") != nullptr)
                {
                    std::ostringstream out;
                    out << "[IOP_CDVD_MEDIA] source=host-image-or-root present="
                        << (mediaPresent ? "true" : "false");
                    log(LogLevel::Info, out.str());
                }
            }

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            if (scheduleStart)
            {
                const uint32_t startArgc = moduleStartArgc.value_or(argumentSize);
                const int threadId = kernel.scheduleFunction(module.entry, startArgc, args, 0u, 0u, module.gp);
                if (threadId <= 0)
                {
                    if (args != 0u)
                        freeAllocation(args);
                    (void)memory.unregisterModuleImage(module.base, module.size);
                    return result;
                }
                module.startPending = true;
                module.startThreadId = threadId;
                module.startArguments = args;
                const int32_t moduleId = module.id;
                modules[module.id] = std::move(module);
                result.moduleId = moduleId;
                result.startResult = -1;
                result.startPending = true;
                std::ostringstream out;
                out << "[IOP] scheduled IRX module_start id=" << moduleId
                    << " entry=0x" << std::hex << modules[moduleId].entry;
                log(LogLevel::Info, out.str());
                return result;
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            if (!lastCallCompleted)
            {
                // Exhaustion/yield/fault is not a returned MODULE_RESIDENT_END.
                // Do not publish partially initialized exports or RPC servers.
                kernel.terminateThreadsInRange(module.base, module.size);
                rpc.removeServersInRange(module.base, module.size);
                imports.eraseRange(module.base, module.size);
                kernel.cleanupDeadThreads();
                (void)memory.unregisterModuleImage(module.base, module.size);
                std::ostringstream error;
                error << "[IOP] module start did not return entry=0x" << std::hex << module.entry
                      << " stop_pc=0x" << lastCallStopPc << std::dec
                      << " instructions=" << lastCallInstructions;
                lastError = error.str();
                log(LogLevel::Error, lastError);
                return result;
            }
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleScheduled(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1, false};
            }
            return loadImage(std::string(path), image, arguments, argumentSize, true);
        }

        ModuleLoadResult loadModuleScheduledWithArgc(std::string_view path,
                                                      const void *arguments,
                                                      uint32_t argumentSize,
                                                      uint32_t argumentCount)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1, false};
            }
            return loadImage(std::string(path), image, arguments, argumentSize, true, argumentCount);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBufferScheduled(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1, false};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize, true);
        }

        ModuleLoadResult loadRomDirModuleScheduled(std::string_view imagePath,
                                                   std::string_view moduleName,
                                                   const void *arguments,
                                                   uint32_t argumentSize,
                                                   std::optional<uint32_t> moduleStartArgc = std::nullopt)
        {
            std::vector<uint8_t> image;
            std::vector<uint8_t> moduleImage;
            if (!IopModuleLoader::readWholeHostFile(host, imagePath, image) ||
                !IopModuleLoader::extractRomDirModule(image, moduleName, moduleImage))
            {
                std::ostringstream error;
                error << "[IOP] ROMDIR module unavailable image='" << imagePath
                      << "' module='" << moduleName << "'";
                lastError = error.str();
                log(LogLevel::Warning, lastError);
                return {true, -1, -1, false};
            }
            std::string modulePath(imagePath);
            modulePath.push_back('#');
            modulePath.append(moduleName);
            return loadImage(std::move(modulePath), moduleImage, arguments, argumentSize, true, moduleStartArgc);
        }

        IopCallResult callExport(std::string_view library, uint16_t ordinal,
                                 uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t instructionBudget)
        {
            IopCallResult result{};
            if (instructionBudget == 0u)
                return result;
            const uint32_t target = imports.resolve(library, ordinal);
            if (target == 0u)
                return result;
            const auto owner = std::find_if(modules.begin(), modules.end(), [&](const auto &item)
            {
                const Module &module = item.second;
                return target >= module.base && target - module.base < module.size;
            });
            if (owner == modules.end())
                return result;
            result.found = true;
            result.entry = target;
            result.value = callFunction(target, a0, a1, a2, a3, owner->second.gp, instructionBudget);
            result.returned = lastCallCompleted;
            result.stopPc = lastCallStopPc;
            result.instructions = lastCallInstructions;
            return result;
        }

        IopScheduledCall scheduleExportCall(std::string_view library, uint16_t ordinal,
                                            uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
        {
            IopScheduledCall result{};
            const uint32_t target = imports.resolve(library, ordinal);
            if (target == 0u)
                return result;
            const auto owner = std::find_if(modules.begin(), modules.end(), [&](const auto &item)
            {
                const Module &module = item.second;
                return target >= module.base && target - module.base < module.size;
            });
            if (owner == modules.end())
                return result;

            const int threadId = kernel.scheduleFunction(target, a0, a1, a2, a3, owner->second.gp);
            if (threadId <= 0)
                return result;
            uint32_t callId = nextScheduledCallId++;
            if (callId == 0u)
                callId = nextScheduledCallId++;
            scheduledExportCalls.emplace(callId, ScheduledExportCall{threadId, target});
            result.found = true;
            result.id = callId;
            result.entry = target;
            return result;
        }

        IopScheduledCallState scheduledCallState(uint32_t callId) const
        {
            IopScheduledCallState result{};
            const auto call = scheduledExportCalls.find(callId);
            if (call == scheduledExportCalls.end())
                return result;
            result.found = true;
            result.entry = call->second.entry;
            const IopThread *thread = kernel.findThread(call->second.threadId);
            if (!thread)
            {
                result.finished = true;
                return result;
            }
            result.finished = thread->state == IopThreadState::Dormant ||
                              thread->state == IopThreadState::Dead;
            result.returned = thread->cpu.pc == kThreadReturnSentinel && !thread->cpu.exception;
            result.stopPc = thread->cpu.pc;
            result.instructions = thread->instructionsExecuted;
            result.value = thread->cpu.gpr[2];
            return result;
        }

        bool releaseScheduledCall(uint32_t callId)
        {
            const auto call = scheduledExportCalls.find(callId);
            if (call == scheduledExportCalls.end())
                return false;
            const IopThread *thread = kernel.findThread(call->second.threadId);
            if (thread && thread->state == IopThreadState::Running)
                return false;
            if (thread && !kernel.releaseThread(call->second.threadId))
                return false;
            scheduledExportCalls.erase(call);
            return true;
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            (void)memory.unregisterModuleImage(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, PendingDma> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        struct ScheduledExportCall
        {
            int threadId = 0;
            uint32_t entry = 0;
        };
        std::map<uint32_t, ScheduledExportCall> scheduledExportCalls;
        uint32_t nextScheduledCallId = 1u;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        CpuState *activeCpu = nullptr;
        bool lastCallCompleted = false;
        uint32_t lastCallStopPc = 0;
        uint32_t lastCallInstructions = 0;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleScheduled(std::string_view path,
                                                      const void *arguments,
                                                      uint32_t argumentSize)
    {
        return m_impl->loadModuleScheduled(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleScheduledWithArgc(std::string_view path,
                                                               const void *arguments,
                                                               uint32_t argumentSize,
                                                               uint32_t argumentCount)
    {
        return m_impl->loadModuleScheduledWithArgc(path, arguments, argumentSize, argumentCount);
    }

    ModuleLoadResult IopEmulator::loadModuleBufferScheduled(uint32_t guestAddress,
                                                            const void *arguments,
                                                            uint32_t argumentSize)
    {
        return m_impl->loadModuleBufferScheduled(guestAddress, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadRomDirModuleScheduled(std::string_view imagePath,
                                                            std::string_view moduleName,
                                                            const void *arguments,
                                                            uint32_t argumentSize)
    {
        return m_impl->loadRomDirModuleScheduled(imagePath, moduleName, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadRomDirModuleScheduledWithArgc(std::string_view imagePath,
                                                                    std::string_view moduleName,
                                                                    const void *arguments,
                                                                    uint32_t argumentSize,
                                                                    uint32_t argumentCount)
    {
        return m_impl->loadRomDirModuleScheduled(imagePath, moduleName, arguments, argumentSize,
                                                 argumentCount);
    }

    IopModuleStartState IopEmulator::moduleStartState(int32_t moduleId)
    {
        return m_impl->moduleStartState(moduleId);
    }

    IopCallResult IopEmulator::callExport(std::string_view library, uint16_t ordinal,
                                          uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                          uint32_t instructionBudget)
    {
        return m_impl->callExport(library, ordinal, a0, a1, a2, a3, instructionBudget);
    }

    IopScheduledCall IopEmulator::scheduleExportCall(std::string_view library, uint16_t ordinal,
                                                     uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
    {
        return m_impl->scheduleExportCall(library, ordinal, a0, a1, a2, a3);
    }

    IopScheduledCallState IopEmulator::scheduledCallState(uint32_t callId) const
    {
        return m_impl->scheduledCallState(callId);
    }

    bool IopEmulator::releaseScheduledCall(uint32_t callId)
    {
        return m_impl->releaseScheduledCall(callId);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
        if (transfer.kind == SifTransferKind::SetDma &&
            transfer.phase == SifTransferPhase::AfterCopy)
        {
            // The transfer payload is already in IOP RAM at this point. Queue
            // SIF1 completion so a registered guest handler (for example the
            // retail SIFCMD command interrupt handler) runs when the IOP gets
            // its next scheduler slice. Hardware latency remains uncalibrated.
            if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
            {
                std::ostringstream out;
                out << "[SIF1_DMA_RECEIVE] source=0x" << std::hex << transfer.sourceAddress
                    << " destination=0x" << transfer.destinationAddress
                    << " size=0x" << transfer.size << std::dec
                    << " cycle=" << m_impl->totalCycles;
                m_impl->log(LogLevel::Info, out.str());
            }
            m_impl->pendingDmaInterrupts[kIopIrqDmaSif1] = IopEmulator::Impl::PendingDma{
                kIopIrqDmaSif1, m_impl->totalCycles, 10u, 0u, 0u, false, false};
        }
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::readSifRegister(uint32_t index, uint32_t &value) const
    {
        return m_impl->memory.readSifRegister(index, value);
    }

    bool IopEmulator::writeSifRegisterFromEe(uint32_t index, uint32_t value)
    {
        return m_impl->memory.writeSifRegisterFromEe(index, value);
    }

    bool IopEmulator::writeSifRegisterFromIop(uint32_t index, uint32_t value)
    {
        return m_impl->memory.writeSifRegisterFromIop(index, value);
    }

    bool IopEmulator::prepareCdvdDeviceState()
    {
        return m_impl->cdvd.prepareDeviceState();
    }

    bool IopEmulator::setRom0Image(std::span<const uint8_t> image)
    {
        return m_impl->memory.setRom0Image(image);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
