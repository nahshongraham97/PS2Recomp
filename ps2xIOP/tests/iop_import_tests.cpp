#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/imports/iop_imports.h"
#include "emulator/imports/iop_intrman.h"
#include "emulator/imports/iop_loadcore.h"
#include "emulator/imports/iop_sysclib.h"
#include "emulator/imports/iop_sysmem.h"
#include "emulator/imports/iop_timrman.h"
#include "emulator/services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

#include <cstdint>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;

    constexpr uint32_t kExportMagic = 0x41C00000u;
    constexpr int32_t kLibraryNotFound = -213;
    constexpr int32_t kIllegalLibrary = -214;

    class NullHost : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    class CdRootHost final : public NullHost
    {
    public:
        explicit CdRootHost(std::filesystem::path rootPath)
            : root(std::move(rootPath))
        {
        }

        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdRoot ? root.string() : std::string{};
        }

    private:
        std::filesystem::path root;
    };

    class RecordingExecutor final : public IopGuestExecutor
    {
    public:
        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t gp) override
        {
            ++calls;
            lastAddress = address;
            lastArgument = a0;
            lastGp = gp;
            return callbackResult;
        }

        uint32_t callbackResult = 0u;
        uint32_t calls = 0u;
        uint32_t lastAddress = 0u;
        uint32_t lastArgument = 0u;
        uint32_t lastGp = 0u;
    };

    bool expect(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    bool testSystemMemoryBlocksAndAllocation()
    {
        NullHost host;
        IopMemory memory;
        IopSysmem sysmem(host, memory);

        const auto kernelBlock = memory.systemMemoryBlockContaining(0x1860u);
        if (!expect(kernelBlock.has_value(), "sysmem did not report the reserved kernel block") ||
            !expect(kernelBlock->address == 0u && kernelBlock->size == IopMemory::SysmemBase && !kernelBlock->free,
                    "sysmem reported an incorrect reserved kernel block"))
            return false;

        constexpr uint32_t moduleBase = IopMemory::SysmemBase;
        constexpr uint32_t moduleSize = 0x1000u;
        if (!expect(memory.registerModuleImage(moduleBase, moduleSize), "sysmem rejected a valid loaded module image"))
            return false;
        const auto moduleBlock = memory.systemMemoryBlockContaining(moduleBase + 0x80u);
        const auto freeBlock = memory.systemMemoryBlockContaining(moduleBase + moduleSize);
        if (!expect(moduleBlock && moduleBlock->address == moduleBase && moduleBlock->size == moduleSize && !moduleBlock->free,
                    "sysmem did not expose the loaded module as a used block") ||
            !expect(freeBlock && freeBlock->address == moduleBase + moduleSize && freeBlock->free,
                    "sysmem did not expose the gap after the loaded module as free"))
            return false;

        IopCpuState cpu{};
        cpu.gpr[4] = moduleBase + moduleSize + 0x20u;
        if (!expect(sysmem.dispatchImport(9u, cpu), "QueryBlockTopAddress import was not handled") ||
            !expect(cpu.gpr[2] == (IopMemory::SysmemFreeBit | (moduleBase + moduleSize)),
                    "QueryBlockTopAddress omitted the free-block marker or returned the wrong block start"))
            return false;

        cpu = {};
        cpu.gpr[4] = moduleBase + moduleSize;
        if (!expect(sysmem.dispatchImport(10u, cpu), "QueryBlockSize import was not handled") ||
            !expect(cpu.gpr[2] == (IopMemory::SysmemFreeBit | (IopMemory::RamSize - moduleBase - moduleSize)),
                    "QueryBlockSize omitted the free-block marker or returned the wrong free size"))
            return false;

        cpu = {};
        cpu.gpr[4] = 2u; // ALLOC_ADDRESS
        cpu.gpr[5] = 0x420u;
        cpu.gpr[6] = moduleBase + moduleSize;
        if (!expect(sysmem.dispatchImport(4u, cpu), "AllocSysMemory import was not handled") ||
            !expect(cpu.gpr[2] == moduleBase + moduleSize, "ALLOC_ADDRESS failed in a discovered free block"))
            return false;
        const auto allocatedBlock = memory.systemMemoryBlockContaining(moduleBase + moduleSize + 0x10u);
        if (!expect(allocatedBlock && !allocatedBlock->free && allocatedBlock->address == moduleBase + moduleSize &&
                    allocatedBlock->size == 0x500u,
                    "AllocSysMemory did not register the 256-byte-rounded used block"))
            return false;

        cpu = {};
        cpu.gpr[4] = 2u;
        cpu.gpr[5] = 0x100u;
        cpu.gpr[6] = moduleBase;
        if (!expect(sysmem.dispatchImport(4u, cpu), "overlap allocation import was not handled") ||
            !expect(cpu.gpr[2] == 0u, "ALLOC_ADDRESS overlapped a loaded module image"))
            return false;

        cpu = {};
        cpu.gpr[4] = 0u; // ALLOC_FIRST
        cpu.gpr[5] = 0x100u;
        if (!expect(sysmem.dispatchImport(4u, cpu), "first-fit allocation import was not handled") ||
            !expect(cpu.gpr[2] == moduleBase + moduleSize + 0x500u,
                    "ALLOC_FIRST did not choose the first sufficiently large free block"))
            return false;

        cpu = {};
        cpu.gpr[4] = 1u; // ALLOC_LAST
        cpu.gpr[5] = 0x100u;
        if (!expect(sysmem.dispatchImport(4u, cpu), "last-fit allocation import was not handled") ||
            !expect(cpu.gpr[2] > moduleBase + moduleSize + 0x500u,
                    "ALLOC_LAST did not choose the highest-address suitable free block"))
            return false;

        cpu = {};
        cpu.gpr[4] = moduleBase + moduleSize;
        if (!expect(sysmem.dispatchImport(5u, cpu), "FreeSysMemory import was not handled") ||
            !expect(cpu.gpr[2] == 0u, "FreeSysMemory rejected an allocated sysmem block"))
            return false;
        if (!expect(memory.unregisterModuleImage(moduleBase, moduleSize), "module image block could not be unregistered"))
            return false;
        const auto reopenedBlock = memory.systemMemoryBlockContaining(moduleBase);
        return expect(reopenedBlock && reopenedBlock->free && reopenedBlock->address == moduleBase,
                      "unloading a module did not return its range to free system memory");
    }

    bool testLoadcoreRebootLibraryMode()
    {
        IopMemory memory;
        IopImportRegistry imports(memory);
        IopLoadcore loadcore(memory, imports);

        IopCpuState cpu{};
        cpu.gpr[4] = 0u;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kIllegalLibrary,
                    "loadcore:27 did not reject a null export table"))
            return false;

        constexpr uint32_t table = 0x1000u;
        memory.write32(table, kExportMagic);
        memory.write16(table + 8u, 0x0101u);
        memory.write16(table + 10u, 0x1234u);
        const char name[8] = {'t', 'e', 's', 't', 'l', 'i', 'b', '\0'};
        (void)memory.writeRam(table + 12u, name, sizeof(name));
        memory.write32(table + 20u, 0u);

        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a valid export table") ||
            !expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a valid export table") ||
            !expect(memory.read16(table + 10u) == 0x1232u,
                    "loadcore:27 did not replace only export mode bits 1 and 2"))
            return false;

        constexpr uint32_t invalidTable = 0x1100u;
        memory.write32(invalidTable, 0xDEADBEEFu);
        cpu = {};
        cpu.gpr[4] = invalidTable;
        cpu.gpr[5] = 6u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 did not consume an invalid-table call") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kLibraryNotFound,
                    "loadcore:27 returned the wrong invalid-table error"))
            return false;

        if (!expect(imports.registerExportTable(table), "test export table did not register"))
            return false;
        memory.write32(table, 0u);
        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 6u;
        return expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a registered table") &&
               expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a registered table") &&
               expect(memory.read16(table + 10u) == 0x1236u,
                      "loadcore:27 did not update a registered table's mode");
    }

    bool testSysclibCtypeLookup()
    {
        IopMemory memory;
        IopSysclib sysclib(memory);
        IopCpuState cpu{};

        const auto lookup = [&](uint32_t character, uint32_t expected, std::string_view label)
        {
            cpu = {};
            cpu.gpr[4] = character;
            return expect(sysclib.dispatchImport(8u, cpu), "sysclib:8 look_ctype_table was not handled") &&
                   expect(cpu.gpr[2] == expected, label);
        };

        return lookup('0', 0x04u, "look_ctype_table did not classify digit '0'") &&
               lookup('9', 0x04u, "look_ctype_table did not classify digit '9'") &&
               lookup('A', 0x41u, "look_ctype_table returned wrong flags for uppercase hex 'A'") &&
               lookup('g', 0x02u, "look_ctype_table returned wrong flags for lowercase 'g'") &&
               lookup(' ', 0x88u, "look_ctype_table returned wrong flags for space") &&
               lookup('\n', 0x28u, "look_ctype_table returned wrong flags for newline") &&
               lookup(':', 0x10u, "look_ctype_table returned wrong flags for colon") &&
               lookup(0x80u, 0u, "look_ctype_table classified a non-ASCII byte");
    }

    bool testCdvdMmioIdleAndMountedMedia()
    {
        IopMemory memory;
        if (!expect(memory.read8(0xBF402005u) == 0u,
                    "CDVD N-command status reported ready without mounted media") ||
            !expect(memory.read8(0xBF40200Fu) == 0u,
                    "CDVD disk type reported a disc without mounted media") ||
            !expect(memory.read8(0xBF402017u) == 0x40u,
                    "idle CDVD S-command status did not report an empty result FIFO"))
            return false;

        memory.setCdvdMediaState(true, 0x14u);
        if (!expect(memory.read8(0xBF402005u) == 0x40u,
                    "mounted virtual media did not report ready and idle") ||
            !expect(memory.read8(0xBF40200Fu) == 0x14u,
                    "mounted virtual DVD reported the wrong disk type") ||
            !expect(memory.read8(0xBF402017u) == 0x40u,
                    "mounting media incorrectly created an S-command result"))
            return false;

        memory.write8(0xBF402017u, 0u);
        return expect(memory.read8(0xBF402017u) == 0x40u,
                       "write to read-only S-command status changed its idle state");
    }

    bool testIopRom0ReadOnlyAliasesAndResetPersistence()
    {
        IopMemory memory;
        const std::array<uint8_t, 8u> rom0Probe{{0x50u, 0x53u, 0x32u, 0x21u,
                                                 0x11u, 0x22u, 0x33u, 0x44u}};
        if (!expect(memory.setRom0Image(rom0Probe), "IOP rejected a nonempty ROM0 mapping") ||
            !expect(memory.read32(0xBFC00000u) == 0x21325350u,
                    "IOP KSEG1 ROM0 did not expose the mapped BIOS bytes") ||
            !expect(memory.read32(0x9FC00000u) == 0x21325350u,
                    "IOP KSEG0 ROM0 alias did not expose the same BIOS bytes"))
            return false;

        memory.write32(0xBFC00000u, 0u);
        if (!expect(memory.read32(0xBFC00000u) == 0x21325350u,
                    "IOP ROM0 mapping accepted a write"))
            return false;

        memory.reset();
        return expect(memory.read32(0xBFC00000u) == 0x21325350u,
                      "IOP reset discarded the physical ROM0 mapping");
    }

    bool testGuestCdvdReadDmaAndInterrupt()
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("ps2x-iop-cdvd-dma-" + suffix);
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (!expect(!error, "could not create the temporary CD root for CDVD DMA"))
            return false;

        CdRootHost host(root);
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();
        if (!expect(cdvd.prepareDeviceState(), "empty temporary CD root did not mount as a virtual ISO"))
        {
            std::filesystem::remove_all(root, error);
            return false;
        }

        IopIntrman intrman(memory);
        intrman.reset();
        RecordingExecutor executor;
        IopCpuState cpu{};
        cpu.gpr[4] = 2u;
        cpu.gpr[6] = 0x12340u;
        cpu.gpr[7] = 0x56780u;
        cpu.gpr[28] = 0x89AB0u;
        if (!expect(intrman.dispatchImport(4u, cpu, executor), "RegisterIntrHandler for CDVD IRQ 2 failed") ||
            !expect(cpu.gpr[2] == 0u, "RegisterIntrHandler returned an error for CDVD IRQ 2"))
        {
            std::filesystem::remove_all(root, error);
            return false;
        }
        cpu = {};
        cpu.gpr[4] = 2u;
        if (!expect(intrman.dispatchImport(6u, cpu, executor), "EnableIntr for CDVD IRQ 2 failed") ||
            !expect(cpu.gpr[2] == 0u, "EnableIntr returned an error for CDVD IRQ 2"))
        {
            std::filesystem::remove_all(root, error);
            return false;
        }

        constexpr uint32_t destination = 0x4000u;
        // CDVDMAN's observed channel-3 request is a device-to-RAM 2048-byte
        // transfer: BCR 0x00100020 and CHCR 0x41000200.
        memory.write32(0x1F8010B0u, destination);
        memory.write32(0x1F8010B4u, 0x00100020u);
        memory.write32(0x1F8010B8u, 0x41000200u);
        if (!expect(!cdvd.serviceDevice(), "channel-3 DMA completed before a guest CDVD command") ||
            !expect((memory.read32(0x1F8010B8u) & (1u << 24u)) != 0u,
                    "channel-3 DMA start bit cleared before the CDVD command arrived"))
        {
            std::filesystem::remove_all(root, error);
            return false;
        }

        // CDVDMAN issues NOP during startup. It completes with the N-command
        // IRQ but must leave the separately armed channel-3 read intact.
        memory.write8(0x1F402004u, 0x00u);
        if (!expect(cdvd.serviceDevice(), "guest CDVD NOP did not complete") ||
            !expect(memory.read8(0xBF402005u) == 0x40u,
                    "guest CDVD NOP did not return the drive to ready") ||
            !expect((memory.read32(0x1F8010B8u) & (1u << 24u)) != 0u,
                    "guest CDVD NOP incorrectly consumed the armed channel-3 read") ||
            !expect((memory.read8(0xBF402008u) & 0x02u) != 0u,
                    "guest CDVD NOP did not set the command-complete I_STAT reason") ||
            !expect(intrman.dispatchInterrupt(2, executor),
                    "guest CDVD NOP did not dispatch the registered IRQ 2 handler"))
        {
            std::filesystem::remove_all(root, error);
            return false;
        }
        memory.write8(0xBF402008u, 0x02u);

        const uint8_t readCdParameters[11] = {
            16u, 0u, 0u, 0u, // LSN of the ISO primary volume descriptor.
            1u, 0u, 0u, 0u,  // one sector.
            0u, 0u, 0u,      // 2048-byte user data mode.
        };
        for (uint8_t parameter : readCdParameters)
            memory.write8(0x1F402005u, parameter);
        memory.write8(0x1F402004u, 0x06u);

        if (!expect(cdvd.serviceDevice(), "guest READ CD command did not produce a CDVD completion") ||
            !expect(memory.read8(destination) == 1u &&
                        memory.read8(destination + 1u) == 'C' &&
                        memory.read8(destination + 2u) == 'D' &&
                        memory.read8(destination + 3u) == '0' &&
                        memory.read8(destination + 4u) == '0' &&
                        memory.read8(destination + 5u) == '1',
                    "channel-3 DMA did not place the generated ISO PVD at the guest destination") ||
            !expect(memory.read32(0x1F8010B0u) == destination + 2048u,
                    "channel-3 MADR did not advance by the completed transfer size") ||
            !expect(memory.read32(0x1F8010B4u) == 0u,
                    "channel-3 BCR did not clear on completion") ||
            !expect((memory.read32(0x1F8010B8u) & (1u << 24u)) == 0u,
                    "channel-3 CHCR remained active after completion") ||
            !expect(memory.read8(0xBF402005u) == 0x40u,
                    "CDVD N-command did not return to ready after the sector transfer") ||
            !expect((memory.read8(0xBF402008u) & 1u) != 0u,
                    "CDVD read completion did not set its data-ready interrupt status"))
        {
            std::filesystem::remove_all(root, error);
            return false;
        }

        memory.write8(0xBF402006u, 0x80u);
        if (!expect(memory.read8(0xBF402006u) == 0x80u,
                    "N_ERROR byte write did not stay in its own register") ||
            !expect(!cdvd.serviceDevice(),
                    "N_ERROR byte write replayed the previous N_CMD"))
        {
            std::filesystem::remove_all(root, error);
            return false;
        }

        const bool delivered = intrman.dispatchInterrupt(2, executor);
        memory.write8(0xBF402008u, 1u);
        const bool passed =
            expect(delivered, "CDVD completion did not dispatch the registered guest IRQ 2 handler") &&
            expect(executor.calls == 2u && executor.lastAddress == 0x12340u &&
                       executor.lastArgument == 0x56780u && executor.lastGp == 0x89AB0u,
                   "CDVD IRQ 2 lost its registered guest handler arguments") &&
            expect((memory.read8(0xBF402008u) & 1u) == 0u,
                   "CDVD I_STAT write-one-to-clear did not acknowledge the N-command interrupt") &&
            expect((memory.interruptStatus() & (1u << 2u)) == 0u,
                   "CDVD acknowledgement left IOP I_STAT IRQ 2 pending");
        std::filesystem::remove_all(root, error);
        return passed;
    }

    bool pollEvent(IopKernel &kernel, int eventId, uint32_t bits, uint32_t resultAddress, int32_t expected)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(eventId);
        cpu.gpr[5] = bits;
        cpu.gpr[6] = 0u; // WEF_AND
        cpu.gpr[7] = resultAddress;
        return expect(kernel.dispatchEventImport(11u, cpu), "PollEventFlag was not handled") &&
               expect(static_cast<int32_t>(cpu.gpr[2]) == expected, "PollEventFlag returned an unexpected result");
    }

    bool testSystemStatusEventFlag()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();

        IopCpuState getStatus{};
        if (!expect(kernel.dispatchThreadImport(41u, getStatus, 0u),
                    "thbase:41 GetSystemStatusFlag was not handled"))
            return false;
        const int eventId = static_cast<int32_t>(getStatus.gpr[2]);
        if (!expect(eventId == 0x00264303,
                    "GetSystemStatusFlag did not return the captured Armageddon system-event ID"))
            return false;

        IopCpuState referStatus{};
        referStatus.gpr[4] = static_cast<uint32_t>(eventId);
        referStatus.gpr[5] = 0x2200u;
        if (!expect(kernel.dispatchEventImport(13u, referStatus),
                    "thevent:13 ReferEventFlagStatus was not handled") ||
            !expect(referStatus.gpr[2] == 0u, "system event status query failed") ||
            !expect(memory.read32(0x2208u) == 0u,
                    "system event unexpectedly began with the CMDINIT-completion bit set"))
            return false;

        const int waiterId = kernel.scheduleFunction(0x1000u, 0u, 0u, 0u, 0u, 0u);
        IopThread *waiter = kernel.beginNextReady(0u);
        if (!expect(waiterId > 0 && waiter != nullptr, "could not schedule a system-event waiter") ||
            !expect(waiter->id == waiterId, "scheduler selected the wrong system-event waiter"))
            return false;

        waiter->cpu.gpr[4] = static_cast<uint32_t>(eventId);
        waiter->cpu.gpr[5] = 0x100u;
        waiter->cpu.gpr[6] = 0u; // WEF_AND; wait without clear-on-wake.
        waiter->cpu.gpr[7] = 0x2200u;
        if (!expect(kernel.dispatchEventImport(10u, waiter->cpu),
                    "thevent:10 WaitEventFlag was not handled") ||
            !expect(waiter->state == IopThreadState::EventFlag && waiter->cpu.yielded,
                    "WaitEventFlag did not block while bit 0x100 was clear"))
            return false;
        kernel.endTimeslice(*waiter, 0xFFFFFFFFu);

        IopCpuState setStatus{};
        setStatus.gpr[4] = static_cast<uint32_t>(eventId);
        setStatus.gpr[5] = 0x100u;
        if (!expect(kernel.dispatchEventImport(6u, setStatus),
                    "thevent:6 SetEventFlag was not handled") ||
            !expect(setStatus.gpr[2] == 0u, "SetEventFlag failed for the system event") ||
            !expect(waiter->state == IopThreadState::Ready,
                    "SetEventFlag did not wake the blocked system-event waiter") ||
            !expect(waiter->cpu.gpr[2] == 0u,
                    "woken WaitEventFlag did not receive a successful return"))
            return false;

        IopThread *resumed = kernel.beginNextReady(1u);
        if (!expect(resumed == waiter, "system-event waiter was not scheduled after SetEventFlag") ||
            !expect(resumed->cpu.gpr[2] == 0u, "resumed waiter lost its successful result"))
            return false;
        kernel.endTimeslice(*resumed, 0xFFFFFFFFu);

        IopCpuState invalidZeroId{};
        invalidZeroId.gpr[4] = 0u;
        invalidZeroId.gpr[5] = 0x100u;
        const bool passed = expect(kernel.dispatchEventImport(11u, invalidZeroId),
                                   "thevent:11 PollEventFlag did not handle an invalid ID") &&
                            expect(static_cast<int32_t>(invalidZeroId.gpr[2]) == -1,
                                   "ID zero unexpectedly resolved to the system event flag");
        if (passed)
            std::cout << "[PASS] system status event flag: valid ID, blocking wait, guest SetEventFlag wake, ID 0 rejected\n";
        return passed;
    }

    bool testCdvdSpecialControl()
    {
        NullHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t param = 0x2000u;
        constexpr uint32_t eventResult = 0x2010u;
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(-11); // sceCdSC: return cdvdman interrupt event flag
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "cdvdman:50 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "sceCdSC(-11) did not return a valid event flag"))
            return false;
        const int eventId = static_cast<int>(cpu.gpr[2]);

        if (!pollEvent(kernel, eventId, 0x29u, eventResult, 0) ||
            !expect(memory.read32(eventResult) == 0x29u, "cdvdman event flag did not start with bits 0x29"))
            return false;

        IopCpuState clear{};
        clear.gpr[4] = static_cast<uint32_t>(eventId);
        clear.gpr[5] = ~0x29u;
        if (!expect(kernel.dispatchEventImport(8u, clear), "ClearEventFlag was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, -418))
            return false;

        cpu = {};
        cpu.gpr[4] = 0x12345u;
        if (!expect(cdvd.dispatchImport(7u, cpu), "sceCdSeek was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, 0))
            return false;

        memory.write8(param, 0x30u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-2);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-2) was not handled") ||
            !expect(cpu.gpr[2] == 0x30u, "sceCdSC(-2) did not store the low-byte error"))
            return false;

        memory.write32(param, 0u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-1) was not handled") ||
            !expect(cpu.gpr[2] == 0u, "sceCdSC(-1) returned the wrong initial stream state") ||
            !expect(memory.read32(param) == 0x30u, "sceCdSC(-1) did not publish the last error"))
            return false;

        cpu = {};
        cpu.gpr[4] = 2u;
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(2) was not handled") ||
            !expect(cpu.gpr[2] == 2u, "sceCdSC(2) did not update the stream state"))
            return false;

        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        return expect(cdvd.dispatchImport(50u, cpu), "second sceCdSC(-1) was not handled") &&
               expect(cpu.gpr[2] == 2u, "sceCdSC(-1) did not preserve the stream state");
    }

    bool testCdvdSearchFile()
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("ps2x-iop-cdvd-search-" + suffix);
        const std::filesystem::path modulesDirectory = root / "MODULES";
        const std::filesystem::path dnasPath = modulesDirectory / "DNAS310.IMG";
        const std::filesystem::path movieDirectory = root / "MOVIE";
        const std::filesystem::path moviePath = movieDirectory / "OPENING.PSS";
        std::error_code error;
        std::filesystem::create_directories(modulesDirectory, error);
        if (!expect(!error, "could not create the temporary CD module directory"))
            return false;
        std::filesystem::create_directories(movieDirectory, error);
        if (!expect(!error, "could not create the temporary CD root"))
            return false;
        {
            std::ofstream dnas(dnasPath, std::ios::binary);
            dnas.write("IRX!", 4);
        }
        {
            std::ofstream movie(moviePath, std::ios::binary);
            movie.write("PSS!", 4);
        }

        CdRootHost host(root);
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t resultAddress = 0x2400u;
        constexpr uint32_t pathAddress = 0x2480u;
        const char path[] = "cdrom0:\\movie\\opening.pss;1";
        (void)memory.writeRam(pathAddress, path, sizeof(path));

        IopCpuState cpu{};
        cpu.gpr[4] = resultAddress;
        cpu.gpr[5] = pathAddress;
        const bool handled = cdvd.dispatchImport(10u, cpu);
        bool passed =
            expect(handled, "cdvdman:10 was not handled") &&
            expect(cpu.gpr[2] == 1u, "sceCdSearchFile did not find a case-insensitive ISO path") &&
            expect(memory.read32(resultAddress) >= 20u, "sceCdSearchFile returned an invalid LSN") &&
            expect(memory.read32(resultAddress + 4u) == 4u, "sceCdSearchFile returned the wrong size") &&
            expect(memory.readString(resultAddress + 8u, 16u) == "OPENING.PSS",
                   "sceCdSearchFile returned the wrong file name");

        const uint32_t pvdAddress = memory.allocate(2048u, 16u);
        cpu = {};
        cpu.gpr[4] = 16u;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = pvdAddress;
        const bool readPvd = cdvd.dispatchImport(6u, cpu);
        std::array<uint8_t, 2048u> pvd{};
        (void)memory.readRam(pvdAddress, pvd.data(), pvd.size());
        const auto little32 = [](const uint8_t *bytes)
        {
            return static_cast<uint32_t>(bytes[0]) |
                   (static_cast<uint32_t>(bytes[1]) << 8u) |
                   (static_cast<uint32_t>(bytes[2]) << 16u) |
                   (static_cast<uint32_t>(bytes[3]) << 24u);
        };
        const auto big32 = [](const uint8_t *bytes)
        {
            return (static_cast<uint32_t>(bytes[0]) << 24u) |
                   (static_cast<uint32_t>(bytes[1]) << 16u) |
                   (static_cast<uint32_t>(bytes[2]) << 8u) |
                   static_cast<uint32_t>(bytes[3]);
        };
        const uint32_t pathTableSize = little32(pvd.data() + 132u);
        const uint32_t typeLPathLsn = little32(pvd.data() + 140u);
        const uint32_t typeMPathLsn = big32(pvd.data() + 148u);
        passed = passed &&
            expect(readPvd && cpu.gpr[2] == 1u, "virtual ISO did not return its PVD") &&
            expect(pvd[0] == 1u && std::memcmp(pvd.data() + 1u, "CD001", 5u) == 0,
                   "virtual ISO PVD identifier is invalid") &&
            expect(pathTableSize == 40u, "virtual ISO PVD path table size is inconsistent") &&
            expect(typeLPathLsn == 18u && typeMPathLsn == 19u,
                   "virtual ISO PVD path table locations are inconsistent");

        const uint32_t typeLAddress = memory.allocate(2048u, 16u);
        cpu = {};
        cpu.gpr[4] = typeLPathLsn;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = typeLAddress;
        const bool readTypeL = cdvd.dispatchImport(6u, cpu);
        std::array<uint8_t, 2048u> typeL{};
        (void)memory.readRam(typeLAddress, typeL.data(), typeL.size());
        passed = passed &&
            expect(readTypeL && cpu.gpr[2] == 1u, "virtual ISO Type-L path table read failed") &&
            expect(typeL[0] == 1u && typeL[8] == 0u && typeL[9] == 0u,
                   "virtual ISO path table root entry is invalid") &&
            expect(typeL[10] == 7u && std::memcmp(typeL.data() + 18u, "MODULES", 7u) == 0,
                   "virtual ISO path table omitted MODULES") &&
            expect(little32(typeL.data() + 12u) >= 20u &&
                   static_cast<uint16_t>(typeL[16] | (static_cast<uint16_t>(typeL[17]) << 8u)) == 1u,
                   "virtual ISO MODULES path-table entry has invalid LSN or parent");

        const uint32_t typeMAddress = memory.allocate(2048u, 16u);
        cpu = {};
        cpu.gpr[4] = typeMPathLsn;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = typeMAddress;
        const bool readTypeM = cdvd.dispatchImport(6u, cpu);
        std::array<uint8_t, 2048u> typeM{};
        (void)memory.readRam(typeMAddress, typeM.data(), typeM.size());
        passed = passed &&
            expect(readTypeM && cpu.gpr[2] == 1u, "virtual ISO Type-M path table read failed") &&
            expect(typeM[10] == 7u && std::memcmp(typeM.data() + 18u, "MODULES", 7u) == 0 &&
                   big32(typeM.data() + 12u) >= 20u,
                   "virtual ISO Type-M path table entry is invalid");

        const char dnasGuestPath[] = "cdrom0:\\modules\\dnas310.img;1";
        (void)memory.writeRam(pathAddress, dnasGuestPath, sizeof(dnasGuestPath));
        cpu = {};
        cpu.gpr[4] = resultAddress;
        cpu.gpr[5] = pathAddress;
        const bool foundDnas = cdvd.dispatchImport(10u, cpu);
        passed = passed &&
            expect(foundDnas && cpu.gpr[2] == 1u, "virtual ISO did not index MODULES/DNAS310.IMG") &&
            expect(memory.read32(resultAddress + 4u) == 4u,
                   "virtual ISO DNAS310.IMG entry has an incorrect size");

        std::filesystem::remove_all(root, error);
        return passed;
    }

    bool testTimrmanPeriodicCallback()
    {
        IopTimrman timrman;
        timrman.reset();
        IopCpuState cpu{};

        cpu.gpr[4] = 1u; // SYSCLK
        cpu.gpr[5] = 32u;
        cpu.gpr[6] = 1u;
        if (!expect(timrman.dispatchImport(4u, cpu, 100u), "AllocHardTimer was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "AllocHardTimer did not allocate a 32-bit timer"))
            return false;
        const uint32_t timerId = cpu.gpr[2];

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 100u;
        cpu.gpr[6] = 0x12340u;
        cpu.gpr[7] = 0x45670u;
        cpu.gpr[28] = 0x89AB0u;
        if (!expect(timrman.dispatchImport(20u, cpu, 100u), "SetTimerHandler was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetTimerHandler failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = 0u;
        cpu.gpr[7] = 1u;
        if (!expect(timrman.dispatchImport(22u, cpu, 100u), "SetupHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetupHardTimer failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        if (!expect(timrman.dispatchImport(23u, cpu, 100u), "StartHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "StartHardTimer failed") ||
            !expect(timrman.nextEventCycle(1000u) == 200u, "timer compare was scheduled at the wrong cycle"))
            return false;

        RecordingExecutor executor;
        executor.callbackResult = 100u;
        timrman.serviceDue(199u, executor);
        if (!expect(executor.calls == 0u, "timer callback ran too early"))
            return false;
        timrman.serviceDue(200u, executor);
        return expect(executor.calls == 1u, "timer callback did not run") &&
               expect(executor.lastAddress == 0x12340u, "timer called the wrong handler") &&
               expect(executor.lastArgument == 0x45670u, "timer passed the wrong common argument") &&
               expect(executor.lastGp == 0x89AB0u, "timer callback lost the registering module GP") &&
               expect(timrman.nextEventCycle(1000u) == 300u, "timer callback return did not rearm compare");
    }
}

int main()
{
    if (!testSystemMemoryBlocksAndAllocation() || !testLoadcoreRebootLibraryMode() || !testSysclibCtypeLookup() ||
        !testIopRom0ReadOnlyAliasesAndResetPersistence() ||
        !testCdvdMmioIdleAndMountedMedia() || !testGuestCdvdReadDmaAndInterrupt() ||
        !testSystemStatusEventFlag() ||
        !testCdvdSpecialControl() || !testCdvdSearchFile() ||
        !testTimrmanPeriodicCallback())
        return 1;
    std::cout << "ps2xIOP import tests passed\n";
    return 0;
}
