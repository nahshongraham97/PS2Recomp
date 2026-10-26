#include "ps2x/iop/iop_subsystem.h"
#include "emulator/core/iop_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace ps2x::iop;

    class TestHost final : public IopHost
    {
    public:
        explicit TestHost(size_t guestBytes = 4096u)
            : guest(guestBytes, 0u)
        {
        }

        bool readGuest(uint32_t address, void *destination, size_t size) const override
        {
            if ((!destination && size != 0u) || address > guest.size() || size > guest.size() - address)
                return false;
            if (size != 0u)
                std::memcpy(destination, guest.data() + address, size);
            return true;
        }

        bool writeGuest(uint32_t address, const void *source, size_t size) override
        {
            if ((!source && size != 0u) || address > guest.size() || size > guest.size() - address)
                return false;
            if (size != 0u)
                std::memcpy(guest.data() + address, source, size);
            return true;
        }

        bool zeroGuest(uint32_t address, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address)
                return false;
            if (size != 0u)
                std::memset(guest.data() + address, 0, size);
            return true;
        }

        bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override
        {
            normalized = address;
            return address <= guest.size();
        }

        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdRoot ? cdRoot : std::string{};
        }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return file.empty() ? 0u : 1u; }
        bool hostFileSize(uint64_t handle, uint64_t &size) const override
        {
            size = file.size();
            return handle == 1u && !file.empty();
        }
        bool readHostFile(uint64_t handle, uint64_t offset, void *destination, size_t size,
                          size_t &bytesRead) override
        {
            bytesRead = 0u;
            if (handle != 1u || offset > file.size() || (!destination && size != 0u))
                return false;
            bytesRead = std::min(size, file.size() - static_cast<size_t>(offset));
            if (bytesRead != 0u)
                std::memcpy(destination, file.data() + offset, bytesRead);
            return true;
        }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void notifyEeSif0DmaComplete() override
        {
            ++sif0Notifications;
            uint32_t observed = 0u;
            if (readGuest(0xC00u, &observed, sizeof(observed)) && observed == 0x53494621u)
                sif0PayloadVisibleAtNotification = true;
        }
        void log(LogLevel, std::string_view message) override { logs.emplace_back(message); }

        std::vector<uint8_t> guest;
        std::vector<uint8_t> file;
        std::vector<std::string> logs;
        std::string cdRoot;
        uint32_t sif0Notifications = 0u;
        bool sif0PayloadVisibleAtNotification = false;
    };

#pragma pack(push, 1)
    struct ElfHeader
    {
        uint8_t ident[16];
        uint16_t type;
        uint16_t machine;
        uint32_t version;
        uint32_t entry;
        uint32_t phoff;
        uint32_t shoff;
        uint32_t flags;
        uint16_t ehsize;
        uint16_t phentsize;
        uint16_t phnum;
        uint16_t shentsize;
        uint16_t shnum;
        uint16_t shstrndx;
    };

    struct ProgramHeader
    {
        uint32_t type;
        uint32_t offset;
        uint32_t vaddr;
        uint32_t paddr;
        uint32_t filesz;
        uint32_t memsz;
        uint32_t flags;
        uint32_t align;
    };

    struct SectionHeader
    {
        uint32_t name;
        uint32_t type;
        uint32_t flags;
        uint32_t address;
        uint32_t offset;
        uint32_t size;
        uint32_t link;
        uint32_t info;
        uint32_t alignment;
        uint32_t entrySize;
    };

    struct Relocation
    {
        uint32_t offset;
        uint32_t info;
    };
#pragma pack(pop)

    static_assert(sizeof(ElfHeader) == 52u);
    static_assert(sizeof(ProgramHeader) == 32u);
    static_assert(sizeof(SectionHeader) == 40u);
    static_assert(sizeof(Relocation) == 8u);

    void writeMinimalIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu;
        header.ident[1] = 'E';
        header.ident[2] = 'L';
        header.ident[3] = 'F';
        header.ident[4] = 1u;
        header.ident[5] = 1u;
        header.ident[6] = 1u;
        header.type = 2u;
        header.machine = 8u;
        header.version = 1u;
        header.entry = loadAddress;
        header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader);
        header.phentsize = sizeof(ProgramHeader);
        header.phnum = 1u;

        // Branch is deliberately included so the test checks that its delay slot runs.
        const uint32_t code[] = {
            0x24080001u, // addiu t0, zero, 1
            0x11080002u, // beq   t0, t0, +2
            0x24020007u, // addiu v0, zero, 7   (delay slot)
            0x24020063u, // addiu v0, zero, 99  (must be skipped)
            0x03E00008u, // jr    ra
            0x00000000u, // nop
        };

        ProgramHeader program{};
        program.type = 1u;
        program.offset = codeOffset;
        program.vaddr = loadAddress;
        program.paddr = loadAddress;
        program.filesz = sizeof(code);
        program.memsz = sizeof(code);
        program.flags = 5u;
        program.align = 4u;

        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, code, sizeof(code));
    }

    void writeRpcServerIrx(TestHost &host,
                           uint32_t address,
                           uint32_t sid = 0xF00DCAFEu)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t importTableOffset = 0x60u;
        constexpr uint32_t importStubAddress = loadAddress + importTableOffset + 20u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu;
        header.ident[1] = 'E';
        header.ident[2] = 'L';
        header.ident[3] = 'F';
        header.ident[4] = 1u;
        header.ident[5] = 1u;
        header.ident[6] = 1u;
        header.type = 2u;
        header.machine = 8u;
        header.version = 1u;
        header.entry = loadAddress;
        header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader);
        header.phentsize = sizeof(ProgramHeader);
        header.phnum = 1u;

        const uint32_t code[] = {
            0x27BDFFE0u, // addiu sp, sp, -0x20
            0xAFBF001Cu, // sw    ra, 0x1c(sp)
            0x3C040001u, // lui   a0, 1
            0x34840200u, // ori   a0, a0, 0x200 (server data)
            0x3C050000u | ((sid >> 16u) & 0xFFFFu), // lui a1, SID upper
            0x34A50000u | (sid & 0xFFFFu),          // ori a1, a1, SID lower
            0x3C060001u, // lui   a2, 1
            0x34C60300u, // ori   a2, a2, 0x300 (server function)
            0x3C070001u, // lui   a3, 1
            0x34E70400u, // ori   a3, a3, 0x400 (server buffer)
            0xAFA00010u, // sw    zero, 0x10(sp)
            0xAFA00014u, // sw    zero, 0x14(sp)
            0xAFA00018u, // sw    zero, 0x18(sp)
            0x0C000000u | ((importStubAddress >> 2u) & 0x03FFFFFFu), // jal sceSifRegisterRpc
            0x00000000u, // nop
            0x8FBF001Cu, // lw    ra, 0x1c(sp)
            0x00001021u, // addu  v0, zero, zero
            0x27BD0020u, // addiu sp, sp, 0x20
            0x03E00008u, // jr    ra
            0x00000000u, // nop
        };
        const uint32_t importTable[] = {
            0x41E00000u, // IRX import magic
            0x00000000u,
            0x00000101u,
            0x63666973u, // "sifc"
            0x0000646Du, // "md"
            0x03E00008u, // jr ra
            0x24000011u, // addiu zero, zero, 17 (sceSifRegisterRpc)
            0x00000000u,
            0x00000000u,
        };

        ProgramHeader program{};
        program.type = 1u;
        program.offset = codeOffset;
        program.vaddr = loadAddress;
        program.paddr = loadAddress;
        program.filesz = 0xA0u;
        program.memsz = 0x500u;
        program.flags = 7u;
        program.align = 4u;

        std::fill(host.guest.begin() + address, host.guest.end(), 0u);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, code, sizeof(code));
        std::memcpy(host.guest.data() + address + codeOffset + importTableOffset,
                    importTable, sizeof(importTable));
    }

    void writeRelocatableRpcServerIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t entryAddress = 0x20u;
        constexpr uint32_t importTableAddress = 0x80u;
        constexpr uint32_t importStubAddress = importTableAddress + 20u;
        constexpr uint32_t handlerAddress = 0xC0u;
        constexpr uint32_t gpAddress = 0xD0u;
        constexpr uint32_t iopModFileOffset = 0xE0u;
        constexpr uint32_t relocationFileOffset = 0x200u;
        constexpr uint32_t sectionTableOffset = 0x240u;
        constexpr uint32_t rpcSid = 0xA11CE001u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu;
        header.ident[1] = 'E';
        header.ident[2] = 'L';
        header.ident[3] = 'F';
        header.ident[4] = 1u;
        header.ident[5] = 1u;
        header.ident[6] = 1u;
        header.type = 0xFF80u; // ET_SCE_IOPRELEXEC
        header.machine = 8u;
        header.version = 1u;
        header.entry = entryAddress;
        header.phoff = sizeof(ElfHeader);
        header.shoff = sectionTableOffset;
        header.ehsize = sizeof(ElfHeader);
        header.phentsize = sizeof(ProgramHeader);
        header.phnum = 2u;
        header.shentsize = sizeof(SectionHeader);
        header.shnum = 3u;

        const uint32_t code[] = {
            0x27BDFFE0u, // addiu sp, sp, -0x20
            0xAFBF001Cu, // sw    ra, 0x1c(sp)
            0x00002021u, // addu  a0, zero, zero
            0x3C05A11Cu, // lui   a1, 0xa11c
            0x34A5E001u, // ori   a1, a1, 0xe001
            0x3C060000u, // lui   a2, 0 (HI16 handler address)
            0x24C600C0u, // addiu a2, a2, 0xc0 (LO16 handler address)
            0x00003821u, // addu  a3, zero, zero
            0xAFA00010u, // sw    zero, 0x10(sp)
            0xAFA00014u, // sw    zero, 0x14(sp)
            0xAFA00018u, // sw    zero, 0x18(sp)
            0x0C000000u | ((importStubAddress >> 2u) & 0x03FFFFFFu), // jal sceSifRegisterRpc
            0x00000000u, // nop
            0x8FBF001Cu, // lw    ra, 0x1c(sp)
            0x00001021u, // addu  v0, zero, zero
            0x27BD0020u, // addiu sp, sp, 0x20
            0x03E00008u, // jr    ra
            0x00000000u, // nop
        };
        const uint32_t importTable[] = {
            0x41E00000u,
            0x00000000u,
            0x00000101u,
            0x63666973u, // "sifc"
            0x0000646Du, // "md"
            0x03E00008u,
            0x24000011u, // sceSifRegisterRpc
            0x00000000u,
        };
        const uint32_t handler[] = {
            0x03E00008u, // jr ra
            0x03801021u, // addu v0, gp, zero
        };
        constexpr uint32_t gpData = 0x47505250u; // "GPRP"
        const uint32_t iopModuleHeader[] = {
            0u,
            entryAddress,
            gpAddress,
        };

        ProgramHeader iopModule{};
        iopModule.type = 0x70000080u; // PT_SCE_IOPMOD
        iopModule.offset = iopModFileOffset;
        iopModule.filesz = sizeof(iopModuleHeader);
        iopModule.memsz = sizeof(iopModuleHeader);
        iopModule.align = 4u;

        ProgramHeader program{};
        program.type = 1u;
        program.offset = codeOffset;
        program.vaddr = 0u;
        program.paddr = 0u;
        program.filesz = gpAddress + sizeof(gpData);
        program.memsz = 0x500u;
        program.flags = 7u;
        program.align = 4u;

        SectionHeader text{};
        text.type = 1u; // SHT_PROGBITS
        text.flags = 0x6u; // SHF_ALLOC | SHF_EXECINSTR
        text.address = entryAddress;
        text.offset = codeOffset + entryAddress;
        text.size = 0xB0u;
        text.alignment = 4u;

        SectionHeader relocations{};
        relocations.type = 9u; // SHT_REL
        relocations.offset = relocationFileOffset;
        relocations.size = 3u * sizeof(Relocation);
        relocations.info = 1u; // target .text
        relocations.alignment = 4u;
        relocations.entrySize = sizeof(Relocation);

        const Relocation relocationData[] = {
            {entryAddress + 5u * sizeof(uint32_t), 5u},  // symbol 0, R_MIPS_HI16
            {entryAddress + 6u * sizeof(uint32_t), 6u},  // symbol 0, R_MIPS_LO16
            {entryAddress + 11u * sizeof(uint32_t), 4u}, // symbol 0, R_MIPS_26
        };

        std::fill(host.guest.begin() + address, host.guest.end(), 0u);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &iopModule, sizeof(iopModule));
        std::memcpy(host.guest.data() + address + sizeof(header) + sizeof(iopModule), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + iopModFileOffset,
                    iopModuleHeader, sizeof(iopModuleHeader));
        std::memcpy(host.guest.data() + address + codeOffset + entryAddress, code, sizeof(code));
        std::memcpy(host.guest.data() + address + codeOffset + importTableAddress,
                    importTable, sizeof(importTable));
        std::memcpy(host.guest.data() + address + codeOffset + handlerAddress,
                    handler, sizeof(handler));
        std::memcpy(host.guest.data() + address + codeOffset + gpAddress,
                    &gpData, sizeof(gpData));
        std::memcpy(host.guest.data() + address + relocationFileOffset,
                    relocationData, sizeof(relocationData));
        std::memcpy(host.guest.data() + address + sectionTableOffset + sizeof(SectionHeader),
                    &text, sizeof(text));
        std::memcpy(host.guest.data() + address + sectionTableOffset + 2u * sizeof(SectionHeader),
                    &relocations, sizeof(relocations));
    }

    void writeExportProviderIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t targetAddress = loadAddress + 0x60u;
        constexpr uint32_t exportTableAddress = loadAddress + 0x80u;
        constexpr uint32_t importTableAddress = loadAddress + 0xC0u;
        constexpr uint32_t importStubAddress = importTableAddress + 20u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0xF0u; program.memsz = 0xF0u; program.flags = 7u; program.align = 4u;

        const uint32_t code[] = {
            0x27BDFFE0u, 0xAFBF001Cu,
            0x3C040001u, 0x34840080u,
            0x0C000000u | ((importStubAddress >> 2u) & 0x03FFFFFFu), 0x00000000u,
            0x8FBF001Cu, 0x00001021u,
            0x27BD0020u, 0x03E00008u, 0x00000000u,
        };
        const uint32_t target[] = {0x03E00008u, 0x24020042u};
        const uint32_t exportTable[] = {
            0x41C00000u, 0u, 0x00000101u,
            0x6C747374u, 0x00006269u, // "tstlib"
            loadAddress, loadAddress, loadAddress, targetAddress, 0u,
        };
        const uint32_t importTable[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x64616F6Cu, 0x65726F63u, // "loadcore"
            0x03E00008u, 0x24000006u, 0u, 0u,
        };

        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, code, sizeof(code));
        std::memcpy(host.guest.data() + address + codeOffset + 0x60u, target, sizeof(target));
        std::memcpy(host.guest.data() + address + codeOffset + 0x80u, exportTable, sizeof(exportTable));
        std::memcpy(host.guest.data() + address + codeOffset + 0xC0u, importTable, sizeof(importTable));
    }

    std::vector<uint8_t> makeRomDirImage(std::span<const uint8_t> module)
    {
        constexpr size_t directoryBytes = 4u * 16u;
        const size_t alignedModuleBytes = (module.size() + 15u) & ~size_t{15u};
        std::vector<uint8_t> image(directoryBytes + alignedModuleBytes, 0u);
        const auto setEntry = [&](size_t index, std::string_view name, uint32_t size)
        {
            uint8_t *const record = image.data() + index * 16u;
            std::memcpy(record, name.data(), std::min<size_t>(name.size(), 9u));
            record[10] = 0u;
            record[11] = 0u;
            record[12] = static_cast<uint8_t>(size);
            record[13] = static_cast<uint8_t>(size >> 8u);
            record[14] = static_cast<uint8_t>(size >> 16u);
            record[15] = static_cast<uint8_t>(size >> 24u);
        };
        setEntry(0u, "RESET", 0u);
        setEntry(1u, "ROMDIR", static_cast<uint32_t>(directoryBytes));
        setEntry(2u, "TEST", static_cast<uint32_t>(module.size()));
        std::memcpy(image.data() + directoryBytes, module.data(), module.size());
        return image;
    }

    void writeExportConsumerIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00011000u;
        constexpr uint32_t importTableAddress = loadAddress + 0x40u;
        constexpr uint32_t importStubAddress = importTableAddress + 20u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x80u; program.memsz = 0x80u; program.flags = 7u; program.align = 4u;

        const uint32_t code[] = {
            0x27BDFFF0u, 0xAFBF000Cu,
            0x0C000000u | ((importStubAddress >> 2u) & 0x03FFFFFFu), 0x00000000u,
            0x8FBF000Cu, 0x27BD0010u,
            0x03E00008u, 0x00000000u,
        };
        const uint32_t importTable[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x6C747374u, 0x00006269u, // "tstlib"
            0x03E00008u, 0x24000003u, 0u, 0u,
        };

        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, code, sizeof(code));
        std::memcpy(host.guest.data() + address + codeOffset + 0x40u, importTable, sizeof(importTable));
    }

    void writeGuestIomanProviderIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t targetAddress = loadAddress + 0x60u;
        constexpr uint32_t exportTableAddress = loadAddress + 0x80u;
        constexpr uint32_t loadcoreImportAddress = loadAddress + 0x100u;
        constexpr uint32_t loadcoreStubAddress = loadcoreImportAddress + 20u;
        constexpr uint32_t iomanImportAddress = loadAddress + 0x120u;
        constexpr uint32_t iomanStubAddress = iomanImportAddress + 20u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x160u; program.memsz = 0x160u; program.flags = 7u; program.align = 4u;

        const uint32_t entry[] = {
            0x27BDFFE0u, 0xAFBF001Cu,
            0x3C040001u, 0x34840080u,
            0x0C000000u | ((loadcoreStubAddress >> 2u) & 0x03FFFFFFu), 0x00000000u,
            0x8FBF001Cu, 0x00001021u,
            0x27BD0020u, 0x03E00008u, 0x00000000u,
        };
        const uint32_t target[] = {
            0x24020042u, // addiu $v0, $zero, 0x42
            0x03E00008u, 0x00000000u,
        };
        uint32_t exportTable[5u + 22u] = {
            0x41C00000u, 0u, 0x00000101u,
            0x616D6F69u, 0x0000006Eu, // "ioman"
        };
        for (uint32_t ordinal = 0u; ordinal <= 20u; ++ordinal)
            exportTable[5u + ordinal] = targetAddress;
        exportTable[5u + 21u] = 0u;
        const uint32_t loadcoreImports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x64616F6Cu, 0x65726F63u, // "loadcore"
            0x03E00008u, 0x24000006u, 0u, 0u,
        };
        const uint32_t iomanImports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x616D6F69u, 0x0000006Eu, // "ioman"
            0x03E00008u, 0x24000014u, 0u, 0u,
        };

        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, entry, sizeof(entry));
        std::memcpy(host.guest.data() + address + codeOffset + 0x60u, target, sizeof(target));
        std::memcpy(host.guest.data() + address + codeOffset + 0x80u, exportTable, sizeof(exportTable));
        std::memcpy(host.guest.data() + address + codeOffset + 0x100u, loadcoreImports, sizeof(loadcoreImports));
        std::memcpy(host.guest.data() + address + codeOffset + 0x120u, iomanImports, sizeof(iomanImports));
    }

    void writeGuestIomanConsumerIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00011000u;
        constexpr uint32_t importTableAddress = loadAddress + 0x40u;
        constexpr uint32_t importStubAddress = importTableAddress + 20u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x80u; program.memsz = 0x80u; program.flags = 7u; program.align = 4u;

        const uint32_t entry[] = {
            0x27BDFFF0u, 0xAFBF000Cu,
            0x24040000u, // a0 = NULL: HLE AddDrv would reject this
            0x0C000000u | ((importStubAddress >> 2u) & 0x03FFFFFFu), 0x00000000u,
            0x8FBF000Cu, 0x27BD0010u,
            0x03E00008u, 0x00000000u,
        };
        const uint32_t importTable[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x616D6F69u, 0x0000006Eu, // "ioman"
            0x03E00008u, 0x24000014u, 0u, 0u, // AddDrv, ordinal 20
        };

        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, entry, sizeof(entry));
        std::memcpy(host.guest.data() + address + codeOffset + 0x40u, importTable, sizeof(importTable));
    }

    void writeVblankSchedulingIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t highThreadAddress = loadAddress + 0x100u;
        constexpr uint32_t lowThreadAddress = loadAddress + 0x140u;
        constexpr uint32_t thbaseTableAddress = loadAddress + 0x180u;
        constexpr uint32_t createThreadStub = thbaseTableAddress + 20u;
        constexpr uint32_t startThreadStub = createThreadStub + 8u;
        constexpr uint32_t vblankTableAddress = loadAddress + 0x1C0u;
        constexpr uint32_t waitVblankEndStub = vblankTableAddress + 20u;
        constexpr uint32_t highThreadDescriptor = loadAddress + 0x300u;
        constexpr uint32_t lowThreadDescriptor = loadAddress + 0x320u;
        constexpr uint32_t lowThreadMarker = loadAddress + 0x400u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x500u; program.memsz = 0x500u; program.flags = 7u; program.align = 4u;

        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const auto jump = [](uint32_t target) { return 0x08000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const uint32_t entry[] = {
            0x27BDFFE0u, 0xAFBF001Cu,
            0x3C040001u, 0x34840300u, jal(createThreadStub), 0x00000000u,
            0x00408021u, // move s0, v0
            0x3C040001u, 0x34840320u, jal(createThreadStub), 0x00000000u,
            0x00408821u, // move s1, v0
            0x02002021u, 0x00002821u, jal(startThreadStub), 0x00000000u,
            0x02202021u, 0x00002821u, jal(startThreadStub), 0x00000000u,
            0x8FBF001Cu, 0x00001021u, 0x27BD0020u, 0x03E00008u, 0x00000000u,
        };
        const uint32_t highThread[] = {
            jal(waitVblankEndStub), 0x00000000u,
            jump(highThreadAddress), 0x00000000u,
        };
        const uint32_t lowThread[] = {
            0x3C080001u, 0x35080400u,
            0x24090001u, 0xAD090000u,
            0x03E00008u, 0x00000000u,
        };
        const uint32_t thbaseImports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x61626874u, 0x00006573u, // "thbase"
            0x03E00008u, 0x24000004u, // CreateThread
            0x03E00008u, 0x24000006u, // StartThread
            0u, 0u,
        };
        const uint32_t vblankImports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x616C6276u, 0x00006B6Eu, // "vblank"
            0x03E00008u, 0x24000005u, // WaitVblankEnd
            0u, 0u,
        };
        const uint32_t highDescriptor[] = {0u, 0u, highThreadAddress, 0x400u, 10u};
        const uint32_t lowDescriptor[] = {0u, 0u, lowThreadAddress, 0x400u, 20u};

        std::vector<uint8_t> segment(program.filesz, 0u);
        const auto put = [&](uint32_t offset, const void *data, size_t size)
        {
            std::memcpy(segment.data() + offset, data, size);
        };
        put(0u, entry, sizeof(entry));
        put(0x100u, highThread, sizeof(highThread));
        put(0x140u, lowThread, sizeof(lowThread));
        put(0x180u, thbaseImports, sizeof(thbaseImports));
        put(0x1C0u, vblankImports, sizeof(vblankImports));
        put(0x300u, highDescriptor, sizeof(highDescriptor));
        put(0x320u, lowDescriptor, sizeof(lowDescriptor));

        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, segment.data(), segment.size());
    }

    void writeSifDmaIrx(TestHost &host, uint32_t address, uint32_t attributes = 0u)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t importTableAddress = loadAddress + 0x100u;
        constexpr uint32_t setDmaStub = importTableAddress + 20u;
        constexpr uint32_t dmaStatStub = setDmaStub + 8u;
        constexpr uint32_t descriptorAddress = loadAddress + 0x180u;
        constexpr uint32_t payloadAddress = loadAddress + 0x1A0u;
        constexpr uint32_t eeDestination = 0xC00u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x200u; program.memsz = 0x200u; program.flags = 7u; program.align = 4u;

        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const uint32_t entry[] = {
            0x27BDFFF0u, 0xAFBF000Cu,
            0x3C040001u, 0x34840180u, 0x24050001u,
            jal(setDmaStub), 0x00000000u,
            0x00402021u, // move a0, v0
            jal(dmaStatStub), 0x00000000u,
            0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0x00000000u,
        };
        const uint32_t imports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x6D666973u, 0x00006E61u, // "sifman"
            0x03E00008u, 0x24000007u, // sceSifSetDma
            0x03E00008u, 0x24000008u, // sceSifDmaStat
            0u, 0u,
        };
        const uint32_t descriptor[] = {
            payloadAddress, eeDestination, sizeof(uint32_t), attributes,
        };
        constexpr uint32_t payload = 0x53494621u; // "SIF!"

        std::vector<uint8_t> segment(program.filesz, 0u);
        std::memcpy(segment.data(), entry, sizeof(entry));
        std::memcpy(segment.data() + 0x100u, imports, sizeof(imports));
        std::memcpy(segment.data() + 0x180u, descriptor, sizeof(descriptor));
        std::memcpy(segment.data() + 0x1A0u, &payload, sizeof(payload));
        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, segment.data(), segment.size());
    }

    void writeIopSif0DmaIrx(TestHost &host, uint32_t address, bool interruptOutput)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t importTableAddress = loadAddress + 0x100u;
        constexpr uint32_t registerIntrHandlerStub = importTableAddress + 20u;
        constexpr uint32_t enableIntrStub = registerIntrHandlerStub + 8u;
        constexpr uint32_t handlerAddress = loadAddress + 0x160u;
        constexpr uint32_t markerAddress = loadAddress + 0x1A0u;
        constexpr uint32_t tagAddress = loadAddress + 0x1B0u;
        constexpr uint32_t payloadAddress = loadAddress + 0x1D0u;
        constexpr uint32_t eeDestination = 0xC00u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x200u; program.memsz = 0x200u; program.flags = 7u; program.align = 4u;

        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const uint32_t entry[] = {
            0x27BDFFF0u, 0xAFBF000Cu,
            0x2404002Au,                         // a0 = IOP_IRQ_DMA_SIF0
            0x24050000u,                         // a1 = mode 0
            0x3C060001u, 0x34C60160u,            // a2 = handler address
            0x24070000u,                         // a3 = argument 0
            jal(registerIntrHandlerStub), 0x00000000u,
            0x2404022Au,                         // a0 = DMAC2 | IOP_IRQ_DMA_SIF0
            jal(enableIntrStub), 0x00000000u,
            0x3C081F80u,                         // t0 = IOP hardware base
            0x3C090001u, 0x352901B0u,            // t1 = SIF0 TADR (0x000101B0)
            0xAD09152Cu,                         // sw t1, D9_TADR(t0)
            0x34090020u,                         // BCR low block size
            0xAD091524u,                         // sw t1, D9_BCR(t0)
            0x3C090100u, 0x35290701u,            // D9_CHCR = 0x01000701
            0xAD091528u,
            0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0x24020000u,
        };
        const uint32_t handler[] = {
            0x3C081F80u, 0x8D091528u,            // read completed D9_CHCR
            0x3C080001u, 0x350801A0u,            // t0 = marker address
            0xAD090000u,                         // save CHCR for the assertion
            0x03E00008u, 0x00000000u,
        };
        const uint32_t imports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x72746E69u, 0x006E616Du,            // "intrman"
            0x03E00008u, 0x24000004u,            // RegisterIntrHandler
            0x03E00008u, 0x24000006u,            // EnableIntr
            0u, 0u,
        };
        const uint32_t tag[] = {
            payloadAddress | 0x80000000u,        // last SIF tag, source in IOP RAM
            1u,                                   // one data word
            0x10000001u | (interruptOutput ? 0x80000000u : 0u), // chain marker, QWC, INT_O
            eeDestination,
        };
        constexpr uint32_t payload = 0x53494621u; // "SIF!"

        std::vector<uint8_t> segment(program.filesz, 0u);
        std::memcpy(segment.data(), entry, sizeof(entry));
        std::memcpy(segment.data() + 0x100u, imports, sizeof(imports));
        std::memcpy(segment.data() + 0x160u, handler, sizeof(handler));
        std::memcpy(segment.data() + 0x1B0u, tag, sizeof(tag));
        std::memcpy(segment.data() + 0x1D0u, &payload, sizeof(payload));
        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, segment.data(), segment.size());
    }

    void writeSif1InterruptIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t importTableAddress = loadAddress + 0x100u;
        constexpr uint32_t registerIntrHandlerStub = importTableAddress + 20u;
        constexpr uint32_t enableIntrStub = registerIntrHandlerStub + 8u;
        constexpr uint32_t handlerAddress = loadAddress + 0x140u;
        constexpr uint32_t markerAddress = loadAddress + 0x180u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x200u; program.memsz = 0x200u; program.flags = 7u; program.align = 4u;

        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const uint32_t entry[] = {
            0x27BDFFF0u, 0xAFBF000Cu, // addiu sp,-16; sw ra,12(sp)
            0x2404002Bu,             // a0 = IOP_IRQ_DMA_SIF1
            0x24050000u,             // a1 = mode 0
            0x3C060001u, 0x34C60140u,// a2 = handler
            0x24070000u,             // a3 = argument 0
            jal(registerIntrHandlerStub), 0x00000000u,
            0x2404022Bu,             // a0 = 0x200 | IOP_IRQ_DMA_SIF1 (DMAC2)
            jal(enableIntrStub), 0x00000000u,
            0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0x24020000u,
        };
        const uint32_t handler[] = {
            0x3C080001u, 0x35080180u, // t0 = marker address
            0x24090001u,             // t1 = 1
            0xAD090000u,             // sw t1,0(t0)
            0x03E00008u, 0x00000000u,
        };
        const uint32_t imports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x72746E69u, 0x006E616Du, // "intrman"
            0x03E00008u, 0x24000004u, // RegisterIntrHandler
            0x03E00008u, 0x24000006u, // EnableIntr
            0u, 0u,
        };

        std::vector<uint8_t> segment(program.filesz, 0u);
        std::memcpy(segment.data(), entry, sizeof(entry));
        std::memcpy(segment.data() + 0x140u, handler, sizeof(handler));
        std::memcpy(segment.data() + 0x100u, imports, sizeof(imports));
        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, segment.data(), segment.size());
    }

    void writeMcmanRegistrationIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t initAddress = loadAddress + 0x100u;
        constexpr uint32_t deinitAddress = loadAddress + 0x120u;
        constexpr uint32_t callbackAddress = loadAddress + 0x140u;
        constexpr uint32_t deviceAddress = loadAddress + 0x200u;
        constexpr uint32_t operationsAddress = loadAddress + 0x220u;
        constexpr uint32_t nameAddress = loadAddress + 0x280u;
        constexpr uint32_t markerAddress = loadAddress + 0x290u;
        constexpr uint32_t secrmanTableAddress = loadAddress + 0x300u;
        constexpr uint32_t secrCommandStub = secrmanTableAddress + 20u;
        constexpr uint32_t secrDeviceIdStub = secrCommandStub + 8u;
        constexpr uint32_t modloadTableAddress = loadAddress + 0x340u;
        constexpr uint32_t setKelfCallbackStub = modloadTableAddress + 20u;
        constexpr uint32_t iomanTableAddress = loadAddress + 0x380u;
        constexpr uint32_t deleteMissingDriverStub = iomanTableAddress + 20u;
        constexpr uint32_t addDriverStub = deleteMissingDriverStub + 8u;
        constexpr uint32_t deleteDriverStub = addDriverStub + 8u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x400u; program.memsz = 0x400u; program.flags = 7u; program.align = 4u;

        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const auto jump = [](uint32_t target) { return 0x08000000u | ((target >> 2u) & 0x03FFFFFFu); };
        constexpr uint32_t epilogueAddress = loadAddress + 35u * sizeof(uint32_t);
        const uint32_t entry[] = {
            0x27BDFFE0u, 0xAFBF001Cu,
            0x3C040001u, 0x34840140u, jal(secrCommandStub), 0x00000000u,
            0x3C040001u, 0x34840140u, jal(secrDeviceIdStub), 0x00000000u,
            0x3C040001u, 0x34840140u, jal(setKelfCallbackStub), 0x00000000u,
            0x3C040001u, 0x34840280u, jal(deleteMissingDriverStub), 0x00000000u,
            0x3C040001u, 0x34840200u, jal(addDriverStub), 0x00000000u,
            0x1440000Bu, 0x00000000u, // bnez v0, failure
            0x3C040001u, 0x34840280u, jal(deleteDriverStub), 0x00000000u,
            0x14400005u, 0x00000000u, // bnez v0, failure
            0x3C080001u, 0x8D020290u, jump(epilogueAddress), 0x00000000u,
            0x2402FFFFu, // failure: return -1
            0x8FBF001Cu, 0x27BD0020u, 0x03E00008u, 0x00000000u,
        };
        const uint32_t init[] = {
            0x3C080001u, 0x35080290u, 0x24090001u, 0xAD090000u,
            0x03E00008u, 0x00001021u,
        };
        const uint32_t deinit[] = {
            0x3C080001u, 0x35080290u, 0x8D090000u, 0x00000000u,
            0x25290001u, 0xAD090000u, 0x03E00008u, 0x00001021u,
        };
        const uint32_t callback[] = {0x03E00008u, 0x00001021u};
        const uint32_t device[] = {nameAddress, 0u, 0u, 0u, operationsAddress};
        uint32_t operations[17]{};
        operations[0] = initAddress;
        operations[1] = deinitAddress;
        const char name[] = "mc";
        const uint32_t secrmanImports[] = {
            0x41E00000u, 0u, 0x00000104u,
            0x72636573u, 0x006E616Du, // "secrman"
            0x03E00008u, 0x24000004u,
            0x03E00008u, 0x24000005u,
            0u, 0u,
        };
        const uint32_t modloadImports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x6C646F6Du, 0x0064616Fu, // "modload"
            0x03E00008u, 0x2400000Du,
            0u, 0u,
        };
        const uint32_t iomanImports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x616D6F69u, 0x0000006Eu, // "ioman"
            0x03E00008u, 0x24000015u, // DelDrv
            0x03E00008u, 0x24000014u, // AddDrv
            0x03E00008u, 0x24000015u, // DelDrv
            0u, 0u,
        };

        std::vector<uint8_t> segment(program.filesz, 0u);
        const auto put = [&](uint32_t offset, const void *data, size_t size)
        {
            std::memcpy(segment.data() + offset, data, size);
        };
        put(0x000u, entry, sizeof(entry));
        put(0x100u, init, sizeof(init));
        put(0x120u, deinit, sizeof(deinit));
        put(0x140u, callback, sizeof(callback));
        put(0x200u, device, sizeof(device));
        put(0x220u, operations, sizeof(operations));
        put(0x280u, name, sizeof(name));
        put(0x300u, secrmanImports, sizeof(secrmanImports));
        put(0x340u, modloadImports, sizeof(modloadImports));
        put(0x380u, iomanImports, sizeof(iomanImports));

        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, segment.data(), segment.size());
    }

    void writeCdvdLifecycleIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t importTableAddress = loadAddress + 0x100u;
        constexpr uint32_t initStub = importTableAddress + 20u;
        constexpr uint32_t callbackStub = initStub + 8u;
        constexpr uint32_t epilogueAddress = loadAddress + 26u * sizeof(uint32_t);

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x140u; program.memsz = 0x140u; program.flags = 7u; program.align = 4u;

        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const auto jump = [](uint32_t target) { return 0x08000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const uint32_t entry[] = {
            0x27BDFFF0u, 0xAFBF000Cu,
            0x00002021u, jal(initStub), 0x00000000u,
            0x24080001u, 0x14480012u, 0x00000000u,
            0x3C041234u, 0x34845678u, jal(callbackStub), 0x00000000u,
            0x1440000Cu, 0x00000000u,
            0x3C0489ABu, 0x3484CDEFu, jal(callbackStub), 0x00000000u,
            0x3C081234u, 0x35085678u, 0x14480004u, 0x00000000u,
            0x00001021u, jump(epilogueAddress), 0x00000000u,
            0x2402FFFFu,
            0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0x00000000u,
        };
        const uint32_t imports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x64766463u, 0x006E616Du, // "cdvdman"
            0x03E00008u, 0x24000004u, // sceCdInit
            0x03E00008u, 0x24000025u, // sceCdCallback
            0u, 0u,
        };

        std::vector<uint8_t> segment(program.filesz, 0u);
        std::memcpy(segment.data(), entry, sizeof(entry));
        std::memcpy(segment.data() + 0x100u, imports, sizeof(imports));
        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, segment.data(), segment.size());
    }

    void writeCdvdPvdReadIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t readBuffer = loadAddress + 0x800u;
        constexpr uint32_t importTableAddress = loadAddress + 0x100u;
        constexpr uint32_t callbackStub = importTableAddress + 20u;
        constexpr uint32_t readStub = callbackStub + 8u;
        constexpr uint32_t callbackFunction = loadAddress + 0x180u;
        constexpr uint32_t epilogueAddress = loadAddress + 26u * sizeof(uint32_t);

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x200u; program.memsz = 0x1000u; program.flags = 7u; program.align = 4u;

        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const auto jump = [](uint32_t target) { return 0x08000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const uint32_t entry[] = {
            0x27BDFFF0u, 0xAFBF000Cu,
            0x3C040001u, 0x34840180u, jal(callbackStub), 0x00000000u,
            0x24040010u, 0x24050001u,
            0x3C060001u, 0x34C60800u, jal(readStub), 0x00000000u,
            0x1040000Cu, 0x00000000u,
            0x90C80000u, 0x24090001u, 0x15090008u, 0x00000000u,
            0x90C80001u, 0x24090043u, 0x15090004u, 0x00000000u,
            0x00001021u, jump(epilogueAddress), 0x00000000u,
            0x2402FFFFu,
            0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0x00000000u,
        };
        const uint32_t imports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x64766463u, 0x006E616Du, // "cdvdman"
            0x03E00008u, 0x24000025u, // sceCdCallback
            0x03E00008u, 0x24000006u, // sceCdRead
            0u, 0u,
        };
        const uint32_t callback[] = {
            0x3C080001u, 0x350801C0u, 0xAD040000u,
            0x03E00008u, 0x00000000u,
        };

        std::vector<uint8_t> segment(program.filesz, 0u);
        std::memcpy(segment.data(), entry, sizeof(entry));
        std::memcpy(segment.data() + 0x100u, imports, sizeof(imports));
        std::memcpy(segment.data() + (callbackFunction - loadAddress), callback, sizeof(callback));
        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, segment.data(), segment.size());
    }

    void writeCdvdSeekIrx(TestHost &host, uint32_t address)
    {
        constexpr uint32_t codeOffset = 0x100u;
        constexpr uint32_t loadAddress = 0x00010000u;
        constexpr uint32_t importTableAddress = loadAddress + 0x100u;
        constexpr uint32_t initStub = importTableAddress + 20u;
        constexpr uint32_t callbackStub = initStub + 8u;
        constexpr uint32_t seekStub = callbackStub + 8u;
        constexpr uint32_t callbackFunction = loadAddress + 0x180u;

        ElfHeader header{};
        header.ident[0] = 0x7Fu; header.ident[1] = 'E'; header.ident[2] = 'L'; header.ident[3] = 'F';
        header.ident[4] = 1u; header.ident[5] = 1u; header.ident[6] = 1u;
        header.type = 2u; header.machine = 8u; header.version = 1u;
        header.entry = loadAddress; header.phoff = sizeof(ElfHeader);
        header.ehsize = sizeof(ElfHeader); header.phentsize = sizeof(ProgramHeader); header.phnum = 1u;

        ProgramHeader program{};
        program.type = 1u; program.offset = codeOffset; program.vaddr = loadAddress; program.paddr = loadAddress;
        program.filesz = 0x200u; program.memsz = 0x200u; program.flags = 7u; program.align = 4u;

        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        const uint32_t entry[] = {
            0x27BDFFF0u, 0xAFBF000Cu,
            0x00002021u, jal(initStub), 0x00000000u,
            0x3C040001u, 0x34840180u, jal(callbackStub), 0x00000000u,
            0x24041234u, jal(seekStub), 0x00000000u,
            0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0x00000000u,
        };
        const uint32_t imports[] = {
            0x41E00000u, 0u, 0x00000101u,
            0x64766463u, 0x006E616Du, // "cdvdman"
            0x03E00008u, 0x24000004u, // sceCdInit
            0x03E00008u, 0x24000025u, // sceCdCallback
            0x03E00008u, 0x24000007u, // sceCdSeek
            0u, 0u,
        };
        const uint32_t callback[] = {
            0x3C080001u, 0x350801C0u, 0xAD040000u,
            0x03E00008u, 0x00001021u,
        };

        std::vector<uint8_t> segment(program.filesz, 0u);
        std::memcpy(segment.data(), entry, sizeof(entry));
        std::memcpy(segment.data() + 0x100u, imports, sizeof(imports));
        std::memcpy(segment.data() + (callbackFunction - loadAddress), callback, sizeof(callback));
        std::memset(host.guest.data() + address, 0, codeOffset + program.filesz);
        std::memcpy(host.guest.data() + address, &header, sizeof(header));
        std::memcpy(host.guest.data() + address + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + address + codeOffset, segment.data(), segment.size());
    }

    bool expect(bool value, const char *message)
    {
        if (!value)
            std::cerr << "FAIL: " << message << '\n';
        return value;
    }
}

int main()
{
    detail::IopMemory sifMemory;
    uint32_t sifFlag = UINT32_MAX;
    if (!expect(sifMemory.readSifRegister(4u, sifFlag) && sifFlag == 0u,
                "IOP reset must leave SMFLAG unset")) return 1;
    if (!expect(!sifMemory.readSifRegister(5u, sifFlag),
                "Invalid SIF register index must be rejected")) return 1;

    sifMemory.write32(0x1D000030u, 0x00010000u);
    sifMemory.write32(0x1D000030u, 0x00020000u);
    if (!expect(sifMemory.readSifRegister(4u, sifFlag) && sifFlag == 0x00030000u,
                "IOP SMFLAG writes must preserve previously published bits")) return 1;
    if (!expect(sifMemory.writeSifRegisterFromIop(4u, 0x00040000u),
                "IOP-side SMFLAG publication failed")) return 1;
    if (!expect(sifMemory.readSifRegister(4u, sifFlag) && sifFlag == 0x00070000u,
                "IOP-side SMFLAG publication must preserve earlier bits")) return 1;
    if (!expect(sifMemory.writeSifRegisterFromIop(3u, 0x00020000u),
                "IOP-side MSFLAG acknowledgement failed")) return 1;
    if (!expect(sifMemory.readSifRegister(3u, sifFlag) && sifFlag == 0u,
                "IOP-side MSFLAG acknowledgement must clear selected bits")) return 1;

    if (!expect(sifMemory.writeSifRegisterFromEe(3u, 0x00030000u),
                "EE MSFLAG write failed")) return 1;
    sifMemory.write32(0x1D000020u, 0x00020000u);
    if (!expect(sifMemory.readSifRegister(3u, sifFlag) && sifFlag == 0x00010000u,
                "IOP MSFLAG write must clear acknowledged bits")) return 1;

    if (!expect(sifMemory.writeSifRegisterFromEe(4u, 0x00020000u),
                "EE SMFLAG write failed")) return 1;
    if (!expect(sifMemory.readSifRegister(4u, sifFlag) && sifFlag == 0x00050000u,
                "EE SMFLAG write must clear acknowledged bits")) return 1;

    TestHost host(0x20000u);
    IopSubsystem iop(host);

    const auto hostCdvdSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path hostCdRoot = std::filesystem::temp_directory_path() /
        ("ps2x-iop-host-cdvd-" + std::to_string(hostCdvdSuffix));
    std::error_code hostCdError;
    std::filesystem::create_directory(hostCdRoot, hostCdError);
    if (!expect(!hostCdError, "Could not create host-CDVD provider test root")) return 1;
    host.cdRoot = hostCdRoot.string();
    const ModuleLoadResult hostCdvdman = iop.loadModuleScheduled("rom0:CDVDMAN");
    if (!expect(hostCdvdman.handled && hostCdvdman.moduleId > 0 &&
                    hostCdvdman.startResult == 0 && !hostCdvdman.startPending,
                "ROM0 CDVDMAN host implementation did not register as a completed HLE load")) return 1;
    if (!expect(iop.prepareCdvdDeviceState(),
                "host CDVDMAN did not prepare the configured extracted-CD media root")) return 1;
    iop.reset();
    host.cdRoot.clear();
    std::filesystem::remove_all(hostCdRoot, hostCdError);
    if (!expect(!hostCdError, "Could not remove host-CDVD provider test root")) return 1;

    // PCSX2's R3000A reset exposes the PS2 IOP PRid (0x1F) and BEV state.
    // Read PRid through real MFC0 instructions, including its load delay.
    writeMinimalIrx(host, 0x100u);
    const uint32_t readPrid[] = {
        0x40087800u, // mfc0 t0, $15 (PRid)
        0x00000000u, // nop (R3000 load delay)
        0x25020000u, // addiu v0, t0, 0
        0x03E00008u, // jr ra
        0x00000000u, // nop
    };
    std::memcpy(host.guest.data() + 0x200u, readPrid, sizeof(readPrid));
    const ModuleLoadResult pridResult = iop.loadModuleBuffer(0x100u);
    if (!expect(pridResult.handled && pridResult.startResult == 0x1F,
                "IOP reset did not expose the PS2 R3000A PRid")) return 1;
    iop.reset();

    // An entry that never returns must not be accepted as module initialization.
    writeMinimalIrx(host, 0x100u);
    const uint32_t spin[] = {0x08004000u, 0u}; // j 0x10000; nop
    std::memcpy(host.guest.data() + 0x200u, spin, sizeof(spin));
    const ModuleLoadResult exhausted = iop.loadModuleBuffer(0x100u);
    if (!expect(exhausted.handled && exhausted.moduleId < 0 && exhausted.startResult < 0,
                "Instruction-budget exhaustion was reported as module initialization")) return 1;
    if (!expect(iop.debugSnapshot().emulatorLoadedModules == 0u,
                "Failed initialization published a module")) return 1;
    iop.reset();

    writeMinimalIrx(host, 0x100u);
    const ModuleLoadResult result = iop.loadModuleBuffer(0x100u);
    const DebugSnapshot snapshot = iop.debugSnapshot();

    if (!expect(result.handled, "Emulator must claim IRX loads")) return 1;
    if (!expect(result.moduleId == 1, "First emulated IRX must get module id 1")) return 1;
    if (!expect(result.startResult == 7, "R3000A branch delay slot result mismatch")) return 1;
    if (!expect(snapshot.emulatorLoadedModules == 1u, "Loaded-module debug count mismatch")) return 1;
    if (!expect(snapshot.emulatorInstructions >= 5u, "Instruction counter did not advance")) return 1;

    int32_t stopResult = -1;
    if (!expect(iop.stopModule(result.moduleId, &stopResult), "Emulated module stop failed")) return 1;
    if (!expect(stopResult == 0, "Emulated module stop result mismatch")) return 1;
    if (!expect(iop.debugSnapshot().emulatorLoadedModules == 0u, "Module was not released")) return 1;

    constexpr uint32_t rpcSid = 0xF00DCAFEu;
    writeRpcServerIrx(host, 0x100u);
    const ModuleLoadResult rpcModule = iop.loadModuleBuffer(0x100u);
    if (!expect(rpcModule.handled && rpcModule.startResult == 0,
                "Synthetic RPC server IRX did not start")) return 1;
    if (!expect(iop.debugSnapshot().emulatorRpcServers == 1u,
                "Registered RPC server count mismatch")) return 1;
    if (!expect(iop.canBindRpc(rpcSid),
                "Emulator did not expose the SID registered by the IRX")) return 1;

    iop.reset();
    if (!expect(!iop.canBindRpc(rpcSid),
                "IOP reset did not remove the registered RPC server")) return 1;

    constexpr uint32_t lotrSoundSid = 0x00012345u;
    writeRpcServerIrx(host, 0x100u, lotrSoundSid);
    const ModuleLoadResult physicalSoundModule = iop.loadModuleBuffer(0x100u);
    if (!expect(physicalSoundModule.handled && physicalSoundModule.startResult == 0,
                "Synthetic physical sound RPC server did not start")) return 1;

    const uint32_t soundHandler[] = {
        0x3C020001u, // lui v0, 1
        0x34420400u, // ori v0, v0, 0x400 (registered server buffer)
        0x03E00008u, // jr ra
        0x00000000u, // nop
    };
    if (!expect(iop.writeMemory(0x00010300u, soundHandler, sizeof(soundHandler)),
                "Could not install the physical sound RPC handler")) return 1;
    constexpr uint32_t soundPayload = 0x1234ABCDu;
    std::memcpy(host.guest.data() + 0x800u, &soundPayload, sizeof(soundPayload));
    RpcRequest soundRequest{};
    soundRequest.sid = lotrSoundSid;
    soundRequest.send = {0x800u, sizeof(soundPayload)};
    soundRequest.receive = {0x900u, sizeof(soundPayload)};
    const uint64_t soundInstructionsBefore = iop.debugSnapshot().emulatorInstructions;
    const RpcResult soundResult = iop.handleRpc(soundRequest);
    uint32_t soundResponse = 0u;
    std::memcpy(&soundResponse, host.guest.data() + 0x900u, sizeof(soundResponse));
    if (!expect(soundResult.handled && soundResponse == soundPayload,
                "Physical sound RPC did not return its own payload")) return 1;
    if (!expect(iop.debugSnapshot().emulatorInstructions > soundInstructionsBefore,
                "Sound RPC did not execute the physical IOP handler")) return 1;

    constexpr uint32_t relocatableRpcSid = 0xA11CE001u;
    writeRelocatableRpcServerIrx(host, 0x100u);
    const ModuleLoadResult relocatableRpcModule = iop.loadModuleBuffer(0x100u);
    if (!expect(relocatableRpcModule.handled && relocatableRpcModule.startResult == 0,
                "Relocatable RPC server IRX did not start")) return 1;
    if (!expect(iop.canBindRpc(relocatableRpcSid),
                "R_MIPS_26 did not relocate a symbol-less IRX import call")) return 1;

    RpcRequest relocatableRequest{};
    relocatableRequest.sid = relocatableRpcSid;
    relocatableRequest.receive = {0x800u, sizeof(uint32_t)};
    const uint64_t rpcInstructionsBefore = iop.debugSnapshot().emulatorInstructions;
    const RpcResult relocatableRpcResult = iop.handleRpc(relocatableRequest);
    const uint64_t rpcInstructions = iop.debugSnapshot().emulatorInstructions - rpcInstructionsBefore;
    if (!expect(relocatableRpcResult.handled,
                "Relocatable IRX RPC handler was not dispatched")) return 1;
    if (!expect(rpcInstructions < 100u,
                "R_MIPS_HI16/LO16 did not relocate the IRX RPC handler")) return 1;
    uint32_t relocatedGpData = 0u;
    std::memcpy(&relocatedGpData, host.guest.data() + relocatableRequest.receive.address,
                sizeof(relocatedGpData));
    if (!expect(relocatedGpData == 0x47505250u,
                "Physical RPC callback did not inherit the registering module's GP")) return 1;

    iop.reset();
    writeExportProviderIrx(host, 0x100u);
    const ModuleLoadResult exportProvider = iop.loadModuleBufferScheduled(0x100u);
    if (!expect(exportProvider.handled && exportProvider.moduleId > 0 && exportProvider.startPending,
                "Synthetic export provider module_start was not scheduled")) return 1;
    const IopModuleStartState exportProviderBeforeRun = iop.moduleStartState(exportProvider.moduleId);
    if (!expect(exportProviderBeforeRun.found && exportProviderBeforeRun.pending,
                "Scheduled module_start did not expose a pending state")) return 1;
    iop.runEeCycles(8u * 128u);
    const IopModuleStartState exportProviderAfterRun = iop.moduleStartState(exportProvider.moduleId);
    if (!expect(exportProviderAfterRun.found && !exportProviderAfterRun.pending &&
                    exportProviderAfterRun.returned && exportProviderAfterRun.startResult == 0,
                "IOP module_start did not run to completion on EE-accounted IOP cycles")) return 1;
    writeExportConsumerIrx(host, 0x500u);
    const ModuleLoadResult exportConsumer = iop.loadModuleBuffer(0x500u);
    if (!expect(exportConsumer.handled && exportConsumer.startResult == 0x42,
                "IRX export ordinal was shifted by implicit function slots")) return 1;

    iop.reset();
    writeGuestIomanProviderIrx(host, 0x100u);
    const ModuleLoadResult guestIomanProvider = iop.loadModuleBuffer(0x100u);
    if (!expect(guestIomanProvider.handled && guestIomanProvider.startResult == 0,
                "Synthetic guest IOMAN provider did not register its exports")) return 1;
    writeGuestIomanConsumerIrx(host, 0x500u);
    const ModuleLoadResult guestIomanConsumer = iop.loadModuleBuffer(0x500u);
    if (!expect(guestIomanConsumer.handled && guestIomanConsumer.startResult == 0x42,
                "IOMAN AddDrv import did not prefer the loaded guest implementation")) return 1;

    iop.reset();
    writeExportProviderIrx(host, 0x100u);
    const std::vector<uint8_t> exportProviderImage(host.guest.begin() + 0x100u,
                                                    host.guest.begin() + 0x2F0u);
    host.file = makeRomDirImage(exportProviderImage);
    const ModuleLoadResult scheduledProvider =
        iop.loadRomDirModuleScheduled("cdrom0:/modules/dnas310.img", "TEST");
    if (!expect(scheduledProvider.handled && scheduledProvider.startPending,
                "Scheduled export provider did not queue module_start")) return 1;
    iop.runEeCycles(8u * 128u);
    const IopModuleStartState scheduledProviderState = iop.moduleStartState(scheduledProvider.moduleId);
    if (!expect(scheduledProviderState.found && !scheduledProviderState.pending &&
                    scheduledProviderState.returned,
                "Scheduled export provider did not finish before its export was called")) return 1;
    iop.reset();
    writeExportProviderIrx(host, 0x100u);
    host.file.assign(host.guest.begin() + 0x100u, host.guest.begin() + 0x2F0u);
    const ModuleLoadResult scheduledFileProvider =
        iop.loadModuleScheduled("cdrom0:/modules/test_provider.irx");
    if (!expect(scheduledFileProvider.handled && scheduledFileProvider.moduleId > 0 &&
                    scheduledFileProvider.startPending,
                "IopSubsystem did not schedule a physical module loaded from a host file")) return 1;
    iop.runEeCycles(8u * 128u);
    const IopModuleStartState scheduledFileProviderState =
        iop.moduleStartState(scheduledFileProvider.moduleId);
    if (!expect(scheduledFileProviderState.found && !scheduledFileProviderState.pending &&
                    scheduledFileProviderState.returned && scheduledFileProviderState.startResult == 0,
                "Scheduled host-file module_start did not complete under the IOP subsystem")) return 1;
    const uint32_t sifPollLoop[] = {
        0x3C081D00u, // lui $t0, 0x1d00: IOP SIF MMIO base
        0x8D020020u, // lw $v0, 0x20($t0): MSFLAG / SIF register 3
        0x3C090001u, // lui $t1, 1: CMDINIT mask
        0x00491024u, // and $v0, $v0, $t1
        0x1040FFFCu, // beq $v0, $zero, back to the MMIO read
        0x00000000u, // branch delay slot
        0x03E00008u, // jr $ra
        0x24020001u, // return value in the jump delay slot
    };
    if (!expect(iop.writeMemory(0x10060u, sifPollLoop, sizeof(sifPollLoop)),
                "Could not install the controlled SIF polling fixture")) return 1;
    const IopScheduledCall scheduledPoll = iop.scheduleExportCall("tstlib", 3u);
    if (!expect(scheduledPoll.found && scheduledPoll.entry == 0x10060u,
                "SIF polling export was not scheduled as an IOP thread")) return 1;
    iop.runEeCycles(8u * 128u);
    const IopScheduledCallState waitingPoll = iop.scheduledCallState(scheduledPoll.id);
    uint32_t smflag = 0u;
    if (!expect(waitingPoll.found && !waitingPoll.finished && !waitingPoll.returned &&
                    waitingPoll.instructions > 0u,
                "IOP polling call did not yield back to the EE cycle scheduler")) return 1;
    if (!expect(iop.readSifRegister(3u, smflag) && smflag == 0u,
                "Unseeded SIF fixture unexpectedly exposed a ready flag")) return 1;
    if (!expect(iop.writeSifRegisterFromEe(3u, 0x00010000u),
                "Fixture EE could not publish the observed MSFLAG event")) return 1;
    iop.runEeCycles(8u * 32u);
    const IopScheduledCallState returnedPoll = iop.scheduledCallState(scheduledPoll.id);
    if (!expect(returnedPoll.found && returnedPoll.finished && returnedPoll.returned &&
                    returnedPoll.value == 1u && returnedPoll.instructions > waitingPoll.instructions,
                "IOP polling function did not resume and return after the EE SIF write")) return 1;
    if (!expect(iop.releaseScheduledCall(scheduledPoll.id),
                "Completed scheduled IOP call could not release its thread and stack")) return 1;

    iop.reset();
    constexpr uint32_t eeSource = 0x100u;
    constexpr uint32_t iopBuffer = 0x500u;
    constexpr uint32_t eeDestination = 0x900u;
    const uint32_t transferPayload = 0x53494621u; // "SIF!"
    std::memcpy(host.guest.data() + eeSource, &transferPayload, sizeof(transferPayload));
    if (!expect(iop.writeMemory(iopBuffer, &transferPayload, sizeof(transferPayload)),
                "Could not initialize physical IOP memory")) return 1;
    iop.onSifTransfer({
        SifTransferKind::SetDma,
        SifTransferPhase::AfterCopy,
        eeSource,
        iopBuffer,
        sizeof(transferPayload),
    });
    std::memset(host.guest.data() + iopBuffer, 0, sizeof(transferPayload));
    iop.onSifTransfer({
        SifTransferKind::GetOtherData,
        SifTransferPhase::BeforeCopy,
        iopBuffer,
        eeDestination,
        sizeof(transferPayload),
    });
    uint32_t stagedIopPayload = 0u;
    std::memcpy(&stagedIopPayload, host.guest.data() + iopBuffer, sizeof(stagedIopPayload));
    if (!expect(stagedIopPayload == 0u,
                "SIF notification aliased physical IOP memory into EE RAM")) return 1;
    uint32_t physicalIopPayload = 0u;
    if (!expect(iop.readMemory(iopBuffer, &physicalIopPayload, sizeof(physicalIopPayload)) &&
                    physicalIopPayload == transferPayload,
                "SIF notification corrupted physical IOP memory")) return 1;

    iop.reset();
    writeSif1InterruptIrx(host, 0x100u);
    const ModuleLoadResult sif1InterruptModule = iop.loadModuleBuffer(0x100u);
    uint32_t sif1InterruptMarker = 0u;
    if (!expect(sif1InterruptModule.handled && sif1InterruptModule.startResult == 0,
                "SIF1 interrupt fixture did not register its guest handler")) return 1;
    if (!expect(iop.readMemory(0x10180u, &sif1InterruptMarker, sizeof(sif1InterruptMarker)) &&
                    sif1InterruptMarker == 0u,
                "SIF1 fixture marker was not initially clear")) return 1;

    iop.onSifTransfer({
        SifTransferKind::GetOtherData,
        SifTransferPhase::AfterCopy,
        0x100u,
        0x200u,
        sizeof(uint32_t),
    });
    iop.onSifTransfer({
        SifTransferKind::SetDma,
        SifTransferPhase::BeforeCopy,
        0x100u,
        0x200u,
        sizeof(uint32_t),
    });
    iop.runEeCycles(8u);
    if (!expect(iop.readMemory(0x10180u, &sif1InterruptMarker, sizeof(sif1InterruptMarker)) &&
                    sif1InterruptMarker == 0u,
                "SIF1 interrupt fired before an EE-to-IOP transfer completed")) return 1;

    iop.onSifTransfer({
        SifTransferKind::SetDma,
        SifTransferPhase::AfterCopy,
        0x100u,
        0x200u,
        sizeof(uint32_t),
    });
    if (!expect(iop.readMemory(0x10180u, &sif1InterruptMarker, sizeof(sif1InterruptMarker)) &&
                    sif1InterruptMarker == 0u,
                "SIF1 interrupt ran synchronously before IOP scheduler progress")) return 1;
    iop.runEeCycles(8u);
    if (!expect(iop.readMemory(0x10180u, &sif1InterruptMarker, sizeof(sif1InterruptMarker)) &&
                    sif1InterruptMarker == 1u,
                "Completed EE-to-IOP SIF DMA did not dispatch the registered SIF1 handler")) return 1;

    iop.reset();
    constexpr uint32_t sifDmaDestination = 0xC00u;
    writeSifDmaIrx(host, 0x100u);
    const ModuleLoadResult sifDmaModule = iop.loadModuleBuffer(0x100u);
    uint32_t sifDmaPayload = 0u;
    std::memcpy(&sifDmaPayload, host.guest.data() + sifDmaDestination,
                sizeof(sifDmaPayload));
    if (!expect(sifDmaModule.handled && sifDmaModule.startResult == -1,
                "sceSifDmaStat did not report the synchronous transfer as complete")) return 1;
    if (!expect(sifDmaPayload == 0x53494621u,
                "IOP sceSifSetDma did not copy the payload into EE memory")) return 1;
    if (!expect(host.sif0Notifications == 0u,
                "SIF0 interrupt fired for a transfer without SIF_DMA_INT_O")) return 1;

    iop.reset();
    host.sif0Notifications = 0u;
    host.sif0PayloadVisibleAtNotification = false;
    writeSifDmaIrx(host, 0x100u, 0x4u); // PS2SDK SIF_DMA_INT_O
    const ModuleLoadResult sifDmaInterruptModule = iop.loadModuleBuffer(0x100u);
    if (!expect(sifDmaInterruptModule.handled && sifDmaInterruptModule.startResult == -1,
                "SIF DMA with output interrupt did not complete synchronously")) return 1;
    if (!expect(host.sif0Notifications == 1u,
                "SIF_DMA_INT_O did not notify the EE SIF0 completion path exactly once")) return 1;
    if (!expect(host.sif0PayloadVisibleAtNotification,
                "EE SIF0 completion was notified before the DMA payload reached guest memory")) return 1;

    iop.reset();
    host.sif0Notifications = 0u;
    host.sif0PayloadVisibleAtNotification = false;
    std::memset(host.guest.data() + 0xC00u, 0, sizeof(uint32_t));
    writeIopSif0DmaIrx(host, 0x100u, true);
    const ModuleLoadResult guestSif0DmaModule = iop.loadModuleBuffer(0x100u);
    uint32_t guestSif0Payload = 0u;
    uint32_t guestSif0IrqMarker = 0u;
    std::memcpy(&guestSif0Payload, host.guest.data() + 0xC00u, sizeof(guestSif0Payload));
    if (!expect(guestSif0DmaModule.handled && guestSif0DmaModule.startResult == 0,
                "Guest-programmed IOP channel-9 SIF0 DMA fixture did not return")) return 1;
    if (guestSif0Payload != 0x53494621u)
    {
        for (const std::string &message : host.logs)
            std::cerr << "IOP test host log: " << message << std::endl;
        if (!expect(false,
                    "Guest-programmed IOP SIF0 chain did not copy its padded payload to EE RAM")) return 1;
    }
    if (!expect(host.sif0Notifications == 1u && host.sif0PayloadVisibleAtNotification,
                "Guest IOP SIF0 INT_O did not notify EE after the payload became visible")) return 1;
    if (!iop.readMemory(0x101A0u, &guestSif0IrqMarker, sizeof(guestSif0IrqMarker)))
        return expect(false, "Could not read the IOP SIF0 completion marker");
    if (!expect(guestSif0IrqMarker == 0u,
                "IOP SIF0 completion IRQ ran synchronously before its scheduled completion cycle")) return 1;
    iop.runEeCycles(512u); // The IOP scheduler advances at one cycle per eight EE cycles.
    if (!iop.readMemory(0x101A0u, &guestSif0IrqMarker, sizeof(guestSif0IrqMarker)))
        return expect(false, "Could not read the IOP SIF0 completion marker after IRQ dispatch");
    if (guestSif0IrqMarker != 0x00000701u)
    {
        std::cerr << "IOP SIF0 IRQ marker value=0x" << std::hex << guestSif0IrqMarker << std::dec << std::endl;
        for (const std::string &message : host.logs)
            std::cerr << "IOP test host log: " << message << std::endl;
        if (!expect(false,
                    "IOP SIF0 completion did not clear CHCR busy before dispatching IRQ 0x2A")) return 1;
    }

    iop.reset();
    host.sif0Notifications = 0u;
    host.sif0PayloadVisibleAtNotification = false;
    std::memset(host.guest.data() + 0xC00u, 0, sizeof(uint32_t));
    const uint32_t zeroMarker = 0u;
    if (!iop.writeMemory(0x101A0u, &zeroMarker, sizeof(zeroMarker)))
        return expect(false, "Could not clear the IOP SIF0 completion marker");
    writeIopSif0DmaIrx(host, 0x100u, false);
    const ModuleLoadResult guestSif0NoInterruptModule = iop.loadModuleBuffer(0x100u);
    std::memcpy(&guestSif0Payload, host.guest.data() + 0xC00u, sizeof(guestSif0Payload));
    if (!expect(guestSif0NoInterruptModule.handled && guestSif0NoInterruptModule.startResult == 0 &&
                    guestSif0Payload == 0x53494621u,
                "Guest IOP SIF0 transfer without INT_O did not copy data")) return 1;
    if (!expect(host.sif0Notifications == 0u,
                "Guest IOP SIF0 transfer without INT_O incorrectly notified the EE")) return 1;
    iop.runEeCycles(512u);
    if (!iop.readMemory(0x101A0u, &guestSif0IrqMarker, sizeof(guestSif0IrqMarker)))
        return expect(false, "Could not read the IOP SIF0 no-interrupt marker");
    if (!expect(guestSif0IrqMarker == 0u,
                "Guest IOP SIF0 transfer without INT_O incorrectly raised IRQ 0x2A")) return 1;

    iop.reset();
    writeVblankSchedulingIrx(host, 0x100u);
    const ModuleLoadResult vblankModule = iop.loadModuleBuffer(0x100u);
    if (!expect(vblankModule.handled && vblankModule.startResult == 0,
                "Synthetic VBlank scheduling IRX did not start")) return 1;
    iop.runEeCycles(8u * 4096u);
    iop.onSifTransfer({
        SifTransferKind::GetOtherData,
        SifTransferPhase::BeforeCopy,
        0x00010400u,
        0x1000u,
        sizeof(uint32_t),
    });
    uint32_t lowPriorityMarker = 0u;
    if (!expect(iop.readMemory(0x00010400u, &lowPriorityMarker, sizeof(lowPriorityMarker)),
                "Could not read the IOP scheduling marker")) return 1;
    if (!expect(lowPriorityMarker == 1u,
                "WaitVblankEnd returned immediately and starved a lower-priority IOP thread")) return 1;

    iop.reset();
    host.logs.clear();
    writeMcmanRegistrationIrx(host, 0x100u);
    const ModuleLoadResult mcmanRegistration = iop.loadModuleBuffer(0x100u);
    if (mcmanRegistration.startResult != 2)
    {
        std::cerr << "MCMAN synthetic start result: " << mcmanRegistration.startResult << '\n';
        for (const auto &message : host.logs)
            std::cerr << message << '\n';
    }
    if (!expect(mcmanRegistration.handled && mcmanRegistration.startResult == 2,
                "MCMAN callback registration or IOMAN AddDrv/DelDrv lifecycle failed")) return 1;
    const bool emittedUnhandledImport = std::any_of(
        host.logs.begin(), host.logs.end(),
        [](const std::string &message) { return message.find("unhandled import") != std::string::npos; });
    if (!expect(!emittedUnhandledImport,
                "MCMAN registration still emitted an unhandled IOP import")) return 1;

    iop.reset();
    host.logs.clear();
    writeCdvdLifecycleIrx(host, 0x100u);
    const ModuleLoadResult cdvdLifecycle = iop.loadModuleBuffer(0x100u);
    if (!expect(cdvdLifecycle.handled && cdvdLifecycle.startResult == 0,
                "CDVD init or callback replacement semantics failed")) return 1;
    const bool emittedUnhandledCdvdImport = std::any_of(
        host.logs.begin(), host.logs.end(),
        [](const std::string &message) { return message.find("unhandled import cdvdman") != std::string::npos; });
    if (!expect(!emittedUnhandledCdvdImport,
                "CDVD lifecycle still emitted an unhandled IOP import")) return 1;

    iop.reset();
    writeCdvdLifecycleIrx(host, 0x100u);
    const ModuleLoadResult cdvdAfterReset = iop.loadModuleBuffer(0x100u);
    if (!expect(cdvdAfterReset.handled && cdvdAfterReset.startResult == 0,
                "IOP reset did not clear the CDVD callback")) return 1;

    iop.reset();
    const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path virtualCdRoot =
        std::filesystem::temp_directory_path() /
        ("ps2x-iop-cdvd-" + std::to_string(uniqueSuffix));
    std::error_code cdRootError;
    std::filesystem::create_directory(virtualCdRoot, cdRootError);
    if (!expect(!cdRootError, "Could not create the virtual-CD test directory")) return 1;
    host.cdRoot = virtualCdRoot.string();
    writeCdvdPvdReadIrx(host, 0x100u);
    const ModuleLoadResult cdvdPvdRead = iop.loadModuleBuffer(0x100u);
    iop.runEeCycles(4096u);
    iop.onSifTransfer({
        SifTransferKind::GetOtherData,
        SifTransferPhase::BeforeCopy,
        0x000101C0u,
        0x1000u,
        sizeof(uint32_t),
    });
    uint32_t cdvdCallbackReason = 0u;
    if (!expect(iop.readMemory(0x000101C0u, &cdvdCallbackReason, sizeof(cdvdCallbackReason)),
                "Could not read the physical CDVD callback result")) return 1;
    host.cdRoot.clear();
    std::filesystem::remove(virtualCdRoot, cdRootError);
    if (!expect(cdvdPvdRead.handled && cdvdPvdRead.startResult == 0,
                "CDVD did not expose the extracted CD root as an ISO9660 PVD")) return 1;
    if (!expect(cdvdCallbackReason == 1u,
                "CDVD read completion did not invoke the registered guest callback")) return 1;

    iop.reset();
    host.logs.clear();
    writeCdvdSeekIrx(host, 0x100u);
    const ModuleLoadResult cdvdSeek = iop.loadModuleBuffer(0x100u);
    iop.runEeCycles(4096u);
    iop.onSifTransfer({
        SifTransferKind::GetOtherData,
        SifTransferPhase::BeforeCopy,
        0x000101C0u,
        0x1100u,
        sizeof(uint32_t),
    });
    uint32_t cdvdSeekCallbackReason = 0u;
    if (!expect(iop.readMemory(0x000101C0u, &cdvdSeekCallbackReason, sizeof(cdvdSeekCallbackReason)),
                "Could not read the physical CDVD callback result")) return 1;
    if (!expect(cdvdSeek.handled && cdvdSeek.startResult == 1,
                "sceCdSeek did not accept a valid LBN")) return 1;
    if (!expect(cdvdSeekCallbackReason == 4u,
                "sceCdSeek completion did not invoke the registered callback")) return 1;
    const bool emittedUnhandledSeek = std::any_of(
        host.logs.begin(), host.logs.end(),
        [](const std::string &message)
        { return message.find("unhandled import cdvdman:7") != std::string::npos; });
    if (!expect(!emittedUnhandledSeek,
                "sceCdSeek still emitted an unhandled IOP import")) return 1;

    std::cout << "ps2xIOP emulator smoke tests passed\n";
    return 0;
}
