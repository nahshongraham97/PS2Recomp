#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory
    {
    public:
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t Rom0Base = 0x1FC00000u;
        static constexpr uint32_t Rom0Size = 4u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        static constexpr uint32_t HeapBase = 0x00120000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;
        // The IOP kernel/exception area occupies the low portion of RAM; module
        // images and system allocations are managed above the loader's base.
        static constexpr uint32_t SysmemBase = 0x00010000u;
        static constexpr uint32_t SysmemFreeBit = 0x80000000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct SystemMemoryBlock
        {
            uint32_t address = 0;
            uint32_t size = 0;
            bool free = false;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
            uint32_t channel = 0u;
            uint32_t tadr = 0u;
            uint32_t madr = 0u;
            uint32_t chcr = 0u;
        };

        struct CdvdCommand
        {
            uint8_t command = 0u;
            std::array<uint8_t, 16u> parameters{};
            uint8_t parameterCount = 0u;
        };

        struct CdvdDmaStart
        {
            uint32_t madr = 0u;
            uint32_t bcr = 0u;
            uint32_t chcr = 0u;
        };

        IopMemory();

        void reset();
        [[nodiscard]] bool setRom0Image(std::span<const uint8_t> image);

        [[nodiscard]] uint8_t read8(uint32_t address) const;
        [[nodiscard]] uint16_t read16(uint32_t address) const;
        [[nodiscard]] uint32_t read32(uint32_t address) const;
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);
        [[nodiscard]] bool readSifRegister(uint32_t index, uint32_t &value) const;
        [[nodiscard]] bool writeSifRegisterFromEe(uint32_t index, uint32_t value);
        [[nodiscard]] bool writeSifRegisterFromIop(uint32_t index, uint32_t value);
        void setCdvdMediaState(bool present, uint8_t diskType) noexcept;
        [[nodiscard]] std::optional<CdvdCommand> takeCdvdNCommand() noexcept;
        [[nodiscard]] std::optional<CdvdCommand> takeCdvdSCommand() noexcept;
        [[nodiscard]] std::optional<CdvdDmaStart> takeCdvdDmaStart() noexcept;
        void completeCdvdNCommand(bool success, uint8_t interruptReason = 0x02u) noexcept;
        void completeCdvdSCommand(uint8_t result) noexcept;
        void completeCdvdDma(uint32_t finalMadr) noexcept;

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt);
        [[nodiscard]] uint32_t allocateSystem(uint32_t size, uint32_t mode, uint32_t address = 0u);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;
        [[nodiscard]] uint32_t totalFreeMemory() const;
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;
        [[nodiscard]] std::optional<SystemMemoryBlock> systemMemoryBlockContaining(uint32_t address) const;
        [[nodiscard]] bool registerModuleImage(uint32_t address, uint32_t size);
        [[nodiscard]] bool unregisterModuleImage(uint32_t address, uint32_t size);

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;
        void completeSif0Dma(uint32_t finalMadr, uint32_t finalTadr, bool interruptOutput);
        void acknowledgeSif0DmaInterrupt();
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }

        [[nodiscard]] static uint32_t physicalAddress(uint32_t address) noexcept;

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);

        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_rom0;
        std::vector<uint8_t> m_owned;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<Allocation> m_allocations;
        std::vector<Allocation> m_moduleImages;
        uint32_t m_heapCursor = HeapBase;
        uint32_t m_interruptStatus = 0;
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        uint8_t m_cdvdNcommandStatus = 0u;
        uint8_t m_cdvdNerror = 0u;
        uint8_t m_cdvdBreakPort = 0u;
        uint8_t m_cdvdDiskType = 0u;
        uint8_t m_cdvdScmd = 0u;
        uint8_t m_cdvdScmdStatus = 0x40u;
        uint8_t m_cdvdScmdResult = 0u;
        uint8_t m_cdvdInterruptStatus = 0u;
        std::array<uint8_t, 16u> m_cdvdNparameters{};
        std::array<uint8_t, 16u> m_cdvdSparameters{};
        uint8_t m_cdvdNparameterCount = 0u;
        uint8_t m_cdvdSparameterCount = 0u;
        std::optional<CdvdCommand> m_cdvdNcommand;
        std::optional<CdvdCommand> m_cdvdScommand;
        std::optional<CdvdDmaStart> m_cdvdDmaStart;
    };
}
