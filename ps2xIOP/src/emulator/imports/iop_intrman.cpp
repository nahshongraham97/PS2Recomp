#include "iop_intrman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"

#include <cstdlib>
#include <iostream>

namespace ps2x::iop::detail
{
    namespace
    {
        bool cdvdTraceEnabled() noexcept
        {
            return std::getenv("PS2X_IOP_CDVD_TRACE") != nullptr;
        }
    }

    IopIntrman::IopIntrman(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopIntrman::reset()
    {
        m_handlers.clear();
        m_enabled.clear();
    }

    bool IopIntrman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const uint32_t a3 = cpu.gpr[7];
        if (std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
            std::cerr << "[INTRMAN_IMPORT] ordinal=" << ordinal
                      << " pc=0x" << std::hex << cpu.pc
                      << " a0=0x" << a0 << " a1=0x" << a1
                      << " a2=0x" << a2 << " a3=0x" << a3
                      << " ra=0x" << cpu.gpr[31] << std::dec << std::endl;
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        if (cdvdTraceEnabled() && (a0 & 0xFFu) == 0x02u &&
            (ordinal == 4u || ordinal == 5u || ordinal == 6u || ordinal == 7u ||
             ordinal == 8u || ordinal == 9u || ordinal == 17u || ordinal == 18u))
        {
            std::cerr << "[IOP_CDVD_INTRMAN_IMPORT] ordinal=" << ordinal
                      << " pc=0x" << std::hex << cpu.pc
                      << " irq_arg=0x" << a0 << " handler_or_arg=0x" << a2
                      << " gp=0x" << cpu.gpr[28]
                      << " i_stat=0x" << m_memory.interruptStatus()
                      << " i_mask=0x" << m_memory.interruptMask()
                      << " i_ctrl=0x" << m_memory.interruptControl()
                      << std::dec << std::endl;
        }

        switch (ordinal)
        {
        case 3:
            setV0(0u);
            return true;
        case 4: // RegisterIntrHandler
            m_handlers[static_cast<int>(a0)] = {a2, a3, cpu.gpr[28]};
            if (a0 == 0x2Bu && std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
                std::cerr << "[SIF1_IRQ_REGISTER] irq=0x2b handler=0x" << std::hex << a2
                          << " argument=0x" << a3 << " gp=0x" << cpu.gpr[28] << std::dec << std::endl;
            setV0(0u);
            return true;
        case 5: // ReleaseIntrHandler
            m_handlers.erase(static_cast<int>(a0));
            setV0(0u);
            return true;
        case 6: // EnableIntr
        {
            // IOP intrman accepts DMA interrupt identifiers with controller
            // selector bits above the low-byte IRQ number. In particular,
            // SIFCMD calls EnableIntr(0x200 | IOP_IRQ_DMA_SIF1), so dispatch
            // state must be keyed by 0x2B, not by the encoded 0x22B value.
            const uint32_t irqIndex = a0 & 0xFFu;
            m_enabled[static_cast<int>(irqIndex)] = true;
            if (irqIndex < 32u)
                m_memory.setInterruptMask(m_memory.interruptMask() | (1u << irqIndex));
            else if (irqIndex >= 0x20u && irqIndex <= 0x2Cu)
                m_memory.setInterruptMask(m_memory.interruptMask() | 8u);
            if (irqIndex == 0x2Bu && std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
                std::cerr << "[SIF1_IRQ_ENABLE] encoded=0x" << std::hex << a0
                          << " irq=0x2b enabled=true" << std::dec << std::endl;
            setV0(0u);
            return true;
        }
        case 7: // DisableIntr
            if (a1 != 0u)
                m_memory.write32(a1, a0);
            if (a0 < 32u)
                m_memory.setInterruptMask(m_memory.interruptMask() & ~(1u << a0));
            m_enabled[static_cast<int>(a0)] = false;
            setV0(0u);
            return true;
        case 8: // CpuDisableIntr
            m_memory.setInterruptControl(0u);
            setV0(0u);
            return true;
        case 9: // CpuEnableIntr
            m_memory.setInterruptControl(1u);
            setV0(0u);
            return true;
        case 14:
            setV0(a0 != 0u
                      ? executor.executeGuestFunctionWithBudget(a0, a1, a2, a3, 0u, cpu.gpr[28], 100000u)
                      : 0u);
            return true;
        case 15:
        case 16:
        case 23:
        case 24:
        case 25:
        case 28:
        case 30:
            setV0(0u);
            return true;
        case 17:
            if (a0 != 0u)
                m_memory.write32(a0, m_memory.interruptControl());
            m_memory.setInterruptControl(0u);
            setV0(0u);
            return true;
        case 18:
            m_memory.setInterruptControl(a0 != 0u ? 1u : 0u);
            setV0(0u);
            return true;
        default:
            return false;
        }
    }

    bool IopIntrman::dispatchInterrupt(int irq, IopGuestExecutor &executor) const
    {
        const auto enabled = m_enabled.find(irq);
        const auto handler = m_handlers.find(irq);
        if (irq == 0x02 && cdvdTraceEnabled())
        {
            std::cerr << "[IOP_CDVD_IRQ_DISPATCH] irq=0x2 enabled="
                      << (enabled != m_enabled.end() && enabled->second ? "true" : "false")
                      << " handler=";
            if (handler != m_handlers.end())
                std::cerr << "0x" << std::hex << handler->second.function << std::dec;
            else
                std::cerr << "missing";
            std::cerr << " i_stat=0x" << std::hex << m_memory.interruptStatus()
                      << " i_mask=0x" << m_memory.interruptMask()
                      << " i_ctrl=0x" << m_memory.interruptControl()
                      << " cdvd_i_stat=0x" << m_memory.read32(0x1F402008u)
                      << std::dec << std::endl;
        }
        if (irq == 0x2B && std::getenv("PS2X_SIF_TRACE_DATA") != nullptr)
        {
            std::cerr << "[SIF1_IRQ_STATE] irq=0x2b enabled="
                      << (enabled != m_enabled.end() && enabled->second ? "true" : "false")
                      << " handler="
                      << (handler != m_handlers.end() ? "0x" : "missing");
            if (handler != m_handlers.end())
                std::cerr << std::hex << handler->second.function << std::dec;
            std::cerr << std::endl;
        }
        if (enabled == m_enabled.end() || !enabled->second)
            return false;
        if (handler == m_handlers.end() || handler->second.function == 0u)
            return false;

        if (executor.scheduleGuestFunction(handler->second.function,
                                           handler->second.argument,
                                           0u,
                                           0u,
                                           0u,
                                           handler->second.gp))
            return true;

        const uint32_t result = executor.executeGuestFunctionWithBudget(handler->second.function,
                                                                         handler->second.argument,
                                                                         0u,
                                                                         0u,
                                                                         0u,
                                                                         handler->second.gp,
                                                                         100000u);
        if (irq == 0x02 && cdvdTraceEnabled())
            std::cerr << "[IOP_CDVD_IRQ_HANDLER_RETURN] entry=0x" << std::hex
                      << handler->second.function << " result=0x" << result << std::dec << std::endl;
        return true;
    }
}
