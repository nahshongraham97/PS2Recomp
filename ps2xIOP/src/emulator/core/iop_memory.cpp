#include "iop_memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaSif0Madr = 0x1F801520u;
        constexpr uint32_t kDmaSif0Chcr = 0x1F801528u;
        constexpr uint32_t kDmaSif0Tadr = 0x1F80152Cu;
        constexpr uint32_t kDmaCdvdMadr = 0x1F8010B0u;
        constexpr uint32_t kDmaCdvdBcr = 0x1F8010B4u;
        constexpr uint32_t kDmaCdvdChcr = 0x1F8010B8u;
        constexpr uint32_t kCdvdNcommand = 0x1F402004u;
        constexpr uint32_t kCdvdNparameter = 0x1F402005u;
        constexpr uint32_t kCdvdNinterruptStatus = 0x1F402008u;
        constexpr uint32_t kCdvdSparameter = 0x1F402017u;
        constexpr uint32_t kDmac2Dicr = 0x1F801574u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;
        constexpr int kDmaSif0Irq = 0x2A;
        constexpr uint32_t kDmaSif0InterruptFlag = 1u << 26u;

        std::optional<uint32_t> sifRegisterAddress(uint32_t index)
        {
            if (index < 1u || index > 4u)
                return std::nullopt;
            return IopMemory::SifBase + (index - 1u) * 0x10u;
        }

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    }

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        reset();
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        m_allocations.clear();
        m_moduleImages.clear();
        m_heapCursor = HeapBase;
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
        m_cdvdNcommandStatus = 0u;
        m_cdvdNerror = 0u;
        m_cdvdBreakPort = 0u;
        m_cdvdDiskType = 0u;
        m_cdvdScmd = 0u;
        m_cdvdScmdStatus = 0x40u;
        m_cdvdScmdResult = 0u;
        m_cdvdInterruptStatus = 0u;
        m_cdvdNparameters.fill(0u);
        m_cdvdSparameters.fill(0u);
        m_cdvdNparameterCount = 0u;
        m_cdvdSparameterCount = 0u;
        m_cdvdNcommand.reset();
        m_cdvdScommand.reset();
        m_cdvdDmaStart.reset();
    }

    bool IopMemory::setRom0Image(std::span<const uint8_t> image)
    {
        if (image.empty() || image.size() > Rom0Size)
            return false;
        m_rom0.assign(image.begin(), image.end());
        return true;
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= Rom0Base && phys - Rom0Base < m_rom0.size())
            return m_rom0[phys - Rom0Base];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= Rom0Base && phys - Rom0Base < m_rom0.size())
            return;
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        // These ports share aligned words with status/command registers. Treat
        // byte writes as FIFO operations; read/modify/write would replay the
        // neighboring command byte while appending each parameter.
        if (phys == kCdvdNparameter)
        {
            if (m_cdvdNparameterCount < m_cdvdNparameters.size())
                m_cdvdNparameters[m_cdvdNparameterCount++] = value;
            return;
        }
        if (phys == kCdvdSparameter)
        {
            if (m_cdvdSparameterCount < m_cdvdSparameters.size())
                m_cdvdSparameters[m_cdvdSparameterCount++] = value;
            return;
        }
        if (phys == kCdvdNparameter + 1u)
        {
            // N_ERROR is a distinct byte register. A generic aligned
            // read/modify/write would replay N_CMD at byte offset zero.
            m_cdvdNerror = value;
            return;
        }
        if (phys == kCdvdNparameter + 2u)
        {
            // BREAK is a command port at byte offset three; keep it separate
            // from N_CMD so this store cannot replay the prior command.
            m_cdvdBreakPort = value;
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys >= Rom0Base && phys - Rom0Base < m_rom0.size())
            return;
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= Rom0Base && phys - Rom0Base < m_rom0.size())
            return;
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readSifRegister(uint32_t index, uint32_t &value) const
    {
        const auto address = sifRegisterAddress(index);
        if (!address)
            return false;
        value = readHardware32(*address);
        return true;
    }

    bool IopMemory::writeSifRegisterFromEe(uint32_t index, uint32_t value)
    {
        const auto address = sifRegisterAddress(index);
        if (!address)
            return false;
        switch (index)
        {
        case 1u:
        case 2u:
            m_hardware[*address] = value;
            break;
        case 3u:
            m_hardware[*address] |= value;
            break;
        case 4u:
            m_hardware[*address] &= ~value;
            break;
        default:
            return false;
        }
        return true;
    }

    bool IopMemory::writeSifRegisterFromIop(uint32_t index, uint32_t value)
    {
        const auto address = sifRegisterAddress(index);
        if (!address)
            return false;
        switch (index)
        {
        case 3u:
            // IOP writes acknowledge/clear EE-to-IOP MSFLAG bits.
            m_hardware[*address] &= ~value;
            break;
        case 4u:
            // IOP publishes IOP-to-EE SMFLAG bits; the peer clears them.
            m_hardware[*address] |= value;
            break;
        default:
            return false;
        }
        return true;
    }

    void IopMemory::setCdvdMediaState(bool present, uint8_t diskType) noexcept
    {
        // DEV5 register 0x005 reports ready in bit 6 and N-command busy in bit 7.
        // An installed virtual disc is ready and idle; no media leaves both clear.
        m_cdvdNcommandStatus = present ? 0x40u : 0u;
        m_cdvdDiskType = present ? diskType : 0u;
        m_cdvdScmdStatus = 0x40u;
    }

    std::optional<IopMemory::CdvdCommand> IopMemory::takeCdvdNCommand() noexcept
    {
        auto command = m_cdvdNcommand;
        m_cdvdNcommand.reset();
        return command;
    }

    std::optional<IopMemory::CdvdCommand> IopMemory::takeCdvdSCommand() noexcept
    {
        auto command = m_cdvdScommand;
        m_cdvdScommand.reset();
        return command;
    }

    std::optional<IopMemory::CdvdDmaStart> IopMemory::takeCdvdDmaStart() noexcept
    {
        auto dma = m_cdvdDmaStart;
        m_cdvdDmaStart.reset();
        return dma;
    }

    void IopMemory::completeCdvdNCommand(bool success, uint8_t interruptReason) noexcept
    {
        m_cdvdNcommandStatus = success && m_cdvdDiskType != 0u ? 0x40u : 0u;
        m_cdvdInterruptStatus |= interruptReason;
        m_interruptStatus |= 1u << 2u;
    }

    void IopMemory::completeCdvdSCommand(uint8_t result) noexcept
    {
        // S_STAT bit 6 means the result FIFO is empty; bit 7 means the
        // command engine is busy. This narrow startup command has no result
        // payload, so completion returns to the observed empty state.
        m_cdvdScmdResult = result;
        m_cdvdScmdStatus = 0x40u;
        m_cdvdInterruptStatus |= 0x02u;
        m_interruptStatus |= 1u << 2u;
    }

    void IopMemory::completeCdvdDma(uint32_t finalMadr) noexcept
    {
        m_hardware[kDmaCdvdChcr] &= ~kDmaStart;
        m_hardware[kDmaCdvdMadr] = finalMadr;
        m_hardware[kDmaCdvdBcr] = 0u;
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        if (address == 0x1F402004u)
        {
            const auto value = m_hardware.find(address);
            const uint32_t command = value == m_hardware.end() ? 0u : value->second & 0xFFu;
            return command | (static_cast<uint32_t>(m_cdvdNcommandStatus) << 8u) |
                   (static_cast<uint32_t>(m_cdvdNerror) << 16u) |
                   (static_cast<uint32_t>(m_cdvdBreakPort) << 24u);
        }
        if (address == 0x1F40200Cu)
        {
            const auto value = m_hardware.find(address);
            const uint32_t low = value == m_hardware.end() ? 0u : value->second & 0x00FFFFFFu;
            return low | (static_cast<uint32_t>(m_cdvdDiskType) << 24u);
        }
        if (address == 0x1F402014u)
        {
            const auto value = m_hardware.find(address);
            const uint32_t low = value == m_hardware.end() ? 0u : value->second & 0x0000FFFFu;
            return low | (static_cast<uint32_t>(m_cdvdScmd) << 16u) |
                   (static_cast<uint32_t>(m_cdvdScmdStatus) << 24u);
        }
        if (address == 0x1F402018u)
            return m_cdvdScmdResult;
        if (address == kCdvdNinterruptStatus)
            return m_cdvdInterruptStatus;
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        if (address == kCdvdNcommand)
        {
            m_hardware[address] = value & 0xFFu;
            m_cdvdNcommandStatus = m_cdvdDiskType != 0u ? 0x80u : 0u;
            m_cdvdNcommand = CdvdCommand{
                static_cast<uint8_t>(value), m_cdvdNparameters, m_cdvdNparameterCount};
            m_cdvdNparameters.fill(0u);
            m_cdvdNparameterCount = 0u;
            return;
        }
        if (address == 0x1F40200Cu)
        {
            m_hardware[address] = value & 0x00FFFFFFu;
            return;
        }
        if (address == 0x1F402014u)
        {
            m_cdvdScmd = static_cast<uint8_t>(value >> 16u);
            m_hardware[address] = value & 0x0000FFFFu;
            m_cdvdScmdStatus = 0x80u;
            m_cdvdScommand = CdvdCommand{
                m_cdvdScmd, m_cdvdSparameters, m_cdvdSparameterCount};
            m_cdvdSparameters.fill(0u);
            m_cdvdSparameterCount = 0u;
            return;
        }
        if (address == kCdvdNinterruptStatus)
        {
            m_cdvdInterruptStatus &= static_cast<uint8_t>(~value);
            if (m_cdvdInterruptStatus == 0u)
                m_interruptStatus &= ~(1u << 2u);
            return;
        }
        if (address == 0x1F402018u)
            return;
        if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr &&
            ((address >= 0x1F801520u && address <= 0x1F80152Cu) ||
             (address >= 0x1F801574u && address <= 0x1F80157Cu)))
        {
            std::ostringstream out;
            out << "[IOP_DMA2_WRITE] address=0x" << std::hex << address
                << " value=0x" << value;
            std::cerr << out.str() << std::endl;
        }

        switch (address)
        {
        case 0x1D000020u:
            m_hardware[address] &= ~value;
            return;
        case 0x1D000030u:
            m_hardware[address] |= value;
            return;
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if (address == kDmaCdvdChcr && (value & kDmaStart) != 0u)
        {
            const auto madr = m_hardware.find(kDmaCdvdMadr);
            const auto bcr = m_hardware.find(kDmaCdvdBcr);
            m_cdvdDmaStart = CdvdDmaStart{
                madr == m_hardware.end() ? 0u : madr->second,
                bcr == m_hardware.end() ? 0u : bcr->second,
                value,
            };
            return;
        }
        if (address == kDmaSif0Chcr && (value & kDmaStart) != 0u)
        {
            const auto tadr = m_hardware.find(kDmaSif0Tadr);
            const auto madr = m_hardware.find(kDmaSif0Madr);
            m_dmaStart = DmaStart{
                kDmaSif0Irq,
                64u,
                9u,
                tadr == m_hardware.end() ? 0u : tadr->second,
                madr == m_hardware.end() ? 0u : madr->second,
                value,
            };
            return;
        }
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        const bool secondCore = address == kDmaSpu1Chcr;
        m_hardware[address] = value & ~kDmaStart;

        const uint32_t statusAddress = 0x1F900344u + (secondCore ? 0x400u : 0u);
        const uint32_t alignedStatus = statusAddress & ~3u;
        const uint32_t shift = (statusAddress & 2u) * 8u;
        uint32_t status = 0u;
        if (const auto current = m_hardware.find(alignedStatus); current != m_hardware.end())
            status = current->second;
        status |= 0x80u << shift;
        m_hardware[alignedStatus] = status;

        const uint32_t blockControlAddress = address - sizeof(uint32_t);
        uint32_t blockControl = 0u;
        if (const auto current = m_hardware.find(blockControlAddress); current != m_hardware.end())
            blockControl = current->second;
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t transferWords = static_cast<uint64_t>(wordsPerBlock) * blockCount;
        m_dmaStart = DmaStart{
            secondCore ? kDmaSpu1Irq : kDmaSpu0Irq,
            std::max<uint64_t>(transferWords * 2u, 64u),
        };
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    void IopMemory::completeSif0Dma(uint32_t finalMadr, uint32_t finalTadr,
                                    bool interruptOutput)
    {
        if (auto chcr = m_hardware.find(kDmaSif0Chcr); chcr != m_hardware.end())
            chcr->second &= ~kDmaStart;
        m_hardware[kDmaSif0Madr] = finalMadr;
        m_hardware[kDmaSif0Tadr] = finalTadr;
        if (interruptOutput)
            m_hardware[kDmac2Dicr] |= kDmaSif0InterruptFlag;
    }

    void IopMemory::acknowledgeSif0DmaInterrupt()
    {
        m_hardware[kDmac2Dicr] &= ~kDmaSif0InterruptFlag;
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        if (fixed)
        {
            const uint32_t address = *fixed;
            if (address < HeapBase || address + size > HeapLimit)
                return 0u;
            for (const auto &block : m_allocations)
                if (address < block.address + block.size && block.address < address + size)
                    return 0u;
            for (const auto &block : m_moduleImages)
                if (address < block.address + block.size && block.address < address + size)
                    return 0u;
            m_allocations.push_back({address, size});
            markOwned(address, size);
            return address;
        }

        uint32_t candidate = alignUp(m_heapCursor, alignment);
        for (;;)
        {
            bool overlap = false;
            for (const auto &block : m_allocations)
            {
                if (candidate < block.address + block.size && block.address < candidate + size)
                {
                    candidate = alignUp(block.address + block.size, alignment);
                    overlap = true;
                    break;
                }
            }
            if (!overlap)
            {
                for (const auto &block : m_moduleImages)
                {
                    if (candidate < block.address + block.size && block.address < candidate + size)
                    {
                        candidate = alignUp(block.address + block.size, alignment);
                        overlap = true;
                        break;
                    }
                }
            }
            if (!overlap)
                break;
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        m_allocations.push_back({candidate, size});
        markOwned(candidate, size);
        m_heapCursor = std::max(m_heapCursor, candidate + size);
        return candidate;
    }

    uint32_t IopMemory::allocateSystem(uint32_t size, uint32_t mode, uint32_t address)
    {
        constexpr uint32_t kAllocationQuantum = 0x100u;
        size = alignUp(std::max(size, 1u), kAllocationQuantum);
        if (size >= SysmemFreeBit || size > RamSize - SysmemBase)
            return 0u;

        const auto addAllocation = [&](uint32_t candidate) -> uint32_t
        {
            if ((candidate & (kAllocationQuantum - 1u)) != 0u ||
                candidate < SysmemBase || candidate > RamSize || size > RamSize - candidate)
                return 0u;
            const auto block = systemMemoryBlockContaining(candidate);
            if (!block || !block->free || size > block->size)
                return 0u;
            m_allocations.push_back({candidate, size});
            markOwned(candidate, size);
            return candidate;
        };

        // sysmem.h defines ALLOC_FIRST=0, ALLOC_LAST=1, and ALLOC_ADDRESS=2.
        if (mode == 2u)
            return addAllocation(physicalAddress(address));
        if (mode != 0u && mode != 1u)
            return 0u;

        uint32_t cursor = SysmemBase;
        uint32_t selected = 0u;
        while (cursor < RamSize)
        {
            const auto block = systemMemoryBlockContaining(cursor);
            if (!block || block->size == 0u)
                break;
            if (block->free && block->size >= size)
            {
                selected = block->address;
                if (mode == 0u)
                    break;
            }
            const uint64_t next = static_cast<uint64_t>(block->address) + block->size;
            if (next <= cursor || next > RamSize)
                break;
            cursor = static_cast<uint32_t>(next);
        }
        return selected == 0u ? 0u : addAllocation(selected);
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        uint32_t largest = 0u;
        uint32_t cursor = SysmemBase;
        while (cursor < RamSize)
        {
            const auto block = systemMemoryBlockContaining(cursor);
            if (!block || block->size == 0u)
                break;
            if (block->free)
                largest = std::max(largest, block->size);
            const uint64_t next = static_cast<uint64_t>(block->address) + block->size;
            if (next <= cursor || next > RamSize)
                break;
            cursor = static_cast<uint32_t>(next);
        }
        return largest;
    }

    uint32_t IopMemory::totalFreeMemory() const
    {
        uint64_t total = 0u;
        uint32_t cursor = SysmemBase;
        while (cursor < RamSize)
        {
            const auto block = systemMemoryBlockContaining(cursor);
            if (!block || block->size == 0u)
                break;
            if (block->free)
                total += block->size;
            const uint64_t next = static_cast<uint64_t>(block->address) + block->size;
            if (next <= cursor || next > RamSize)
                break;
            cursor = static_cast<uint32_t>(next);
        }
        return static_cast<uint32_t>(std::min<uint64_t>(total, RamSize));
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::optional<IopMemory::SystemMemoryBlock> IopMemory::systemMemoryBlockContaining(uint32_t address) const
    {
        address = physicalAddress(address);
        if (address >= RamSize)
            return std::nullopt;

        std::vector<Allocation> occupied;
        occupied.reserve(1u + m_moduleImages.size() + m_allocations.size());
        // The emulated kernel/exception area is reserved before module loading.
        occupied.push_back({0u, SysmemBase});
        occupied.insert(occupied.end(), m_moduleImages.begin(), m_moduleImages.end());
        occupied.insert(occupied.end(), m_allocations.begin(), m_allocations.end());
        std::sort(occupied.begin(), occupied.end(), [](const Allocation &lhs, const Allocation &rhs)
        {
            return lhs.address < rhs.address;
        });

        uint32_t cursor = 0u;
        for (const Allocation &block : occupied)
        {
            if (block.address > cursor && address < block.address)
                return SystemMemoryBlock{cursor, block.address - cursor, true};

            const uint64_t blockEnd64 = static_cast<uint64_t>(block.address) + block.size;
            const uint32_t blockEnd = static_cast<uint32_t>(std::min<uint64_t>(blockEnd64, RamSize));
            if (address >= block.address && address < blockEnd)
                return SystemMemoryBlock{block.address, blockEnd - block.address, false};
            cursor = std::max(cursor, blockEnd);
        }
        if (address >= cursor)
            return SystemMemoryBlock{cursor, RamSize - cursor, true};
        return std::nullopt;
    }

    bool IopMemory::registerModuleImage(uint32_t address, uint32_t size)
    {
        if (size == 0u || address < SysmemBase || address >= RamSize || size > RamSize - address)
            return false;
        const uint32_t end = address + size;
        for (const Allocation &block : m_moduleImages)
            if (address < block.address + block.size && block.address < end)
                return false;
        for (const Allocation &block : m_allocations)
            if (address < block.address + block.size && block.address < end)
                return false;
        m_moduleImages.push_back({address, size});
        return true;
    }

    bool IopMemory::unregisterModuleImage(uint32_t address, uint32_t size)
    {
        const auto module = std::find_if(m_moduleImages.begin(), m_moduleImages.end(),
                                         [&](const Allocation &block)
                                         { return block.address == address && block.size == size; });
        if (module == m_moduleImages.end())
            return false;
        m_moduleImages.erase(module);
        return true;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}
