#include "iop_stdio.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace ps2x::iop::detail
{
    IopStdio::IopStdio(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopStdio::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto logString = [&](std::string_view prefix, uint32_t address, uint32_t resultBias = 0u)
        {
            const std::string text = m_memory.readString(address, 2048u);
            m_host.log(LogLevel::Info, std::string(prefix) + text);
            setV0(static_cast<uint32_t>(text.size()) + resultBias);
        };

        switch (ordinal)
        {
        case 4: // printf
        {
            if (std::getenv("PS2X_IOP_STDIO_TRACE_ARGS") != nullptr)
            {
                const std::string format = m_memory.readString(a0, 2048u);
                std::ostringstream trace;
                trace << "[IOP_PRINTF_ARGS] format=\"" << format << '\"';
                if (format.find("open fail name %s") != std::string::npos)
                {
                    trace << " name_ptr=0x" << std::hex << a1 << std::dec
                          << " name=\"" << m_memory.readString(a1, 260u) << '\"';
                    m_host.log(LogLevel::Info, trace.str());
                }
                else if (format.find("loadmodule: fname %s") != std::string::npos)
                {
                    trace << " filename_ptr=0x" << std::hex << a1
                          << " arg_count=" << std::dec << cpu.gpr[6]
                          << " arg_ptr=0x" << std::hex << cpu.gpr[7] << std::dec
                          << " filename=\"" << m_memory.readString(a1, 260u)
                          << "\" arg=\"" << m_memory.readString(cpu.gpr[7], 260u) << '\"';
                    m_host.log(LogLevel::Info, trace.str());
                }
            }
            logString("[IOP printf] ", a0);
            return true;
        }
        case 5: // getchar
        case 10:
            setV0(0xFFFFFFFFu);
            return true;
        case 6: // putchar
            m_host.log(LogLevel::Info, std::string("[IOP putchar] ") + static_cast<char>(a0 & 0xFFu));
            setV0(a0 & 0xFFu);
            return true;
        case 7: // puts
            logString("[IOP puts] ", a0, 1u);
            return true;
        case 8: // gets
        case 13:
            setV0(0u);
            return true;
        case 9: // fdprintf
            logString("[IOP fdprintf] ", a1);
            return true;
        case 11:
            setV0(a0 & 0xFFu);
            return true;
        case 12: // fdputs
            logString("[IOP fdputs] ", a0);
            return true;
        case 14: // vfdprintf
            logString("[IOP vfdprintf] ", a1);
            return true;
        default:
            return false;
        }
    }
}
