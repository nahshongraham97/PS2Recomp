#include "iop_sysmem.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <cstdlib>
#include <sstream>
#include <string>

namespace ps2x::iop::detail
{
    IopSysmem::IopSysmem(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopSysmem::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // AllocSysMemory
        {
            const uint32_t address = m_memory.allocateSystem(a1, a0, a2);
            setV0(address);
            if (std::getenv("PS2X_IOP_TRACE_SYSMEM") != nullptr)
            {
                std::ostringstream out;
                out << "[IOP_SYSMEM_IMPORT] ordinal=4 pc=0x" << std::hex << cpu.pc
                    << " ra=0x" << cpu.gpr[31]
                    << " mode=0x" << a0 << " size=0x" << a1
                    << " fixed=0x" << a2 << " result=0x" << address;
                m_host.log(LogLevel::Info, out.str());
            }
            return true;
        }
        case 5: // FreeSysMemory
            setV0(m_memory.freeAllocation(a0) ? 0u : 0xFFFFFFFFu);
            return true;
        case 6: // QueryMemSize
            setV0(IopMemory::RamSize);
            return true;
        case 7: // QueryMaxFreeMemSize
            setV0(m_memory.maxFreeMemory());
            return true;
        case 8: // QueryTotalFreeMemSize
            setV0(m_memory.totalFreeMemory());
            return true;
        case 9: // QueryBlockTopAddress
        {
            const auto block = m_memory.systemMemoryBlockContaining(a0 & 0x7FFFFFFFu);
            const uint32_t value = block
                                       ? block->address | (block->free ? IopMemory::SysmemFreeBit : 0u)
                                       : 0u;
            setV0(value);
            if (std::getenv("PS2X_IOP_TRACE_SYSMEM") != nullptr)
            {
                std::ostringstream out;
                out << "[IOP_SYSMEM_IMPORT] ordinal=9 pc=0x" << std::hex << cpu.pc
                    << " ra=0x" << cpu.gpr[31] << " query=0x" << a0 << " result=0x" << value;
                m_host.log(LogLevel::Info, out.str());
            }
            return true;
        }
        case 10: // QueryBlockSize
        {
            const auto block = m_memory.systemMemoryBlockContaining(a0 & 0x7FFFFFFFu);
            const uint32_t value = block
                                       ? block->size | (block->free ? IopMemory::SysmemFreeBit : 0u)
                                       : 0u;
            setV0(value);
            if (std::getenv("PS2X_IOP_TRACE_SYSMEM") != nullptr)
            {
                std::ostringstream out;
                out << "[IOP_SYSMEM_IMPORT] ordinal=10 pc=0x" << std::hex << cpu.pc
                    << " ra=0x" << cpu.gpr[31] << " query=0x" << a0 << " result=0x" << value;
                m_host.log(LogLevel::Info, out.str());
            }
            return true;
        }
        case 14: // Kprintf
        {
            const std::string format = m_memory.readString(a0, 512u);
            m_host.log(LogLevel::Info, std::string("[IOP Kprintf] ") + format);
            setV0(static_cast<uint32_t>(format.size()));
            return true;
        }
        case 15:
            setV0(0u);
            return true;
        default:
            return false;
        }
    }
}
