#include "iop_cpu.h"
#include "iop_memory.h"

#include <limits>
#include <cstdlib>
#include <iomanip>
#include <iostream>

namespace ps2x::iop::detail
{
    IopCpuCore::IopCpuCore(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopCpuCore::writeRegister(IopCpuState &cpu, uint32_t reg, uint32_t value, uint32_t &writtenReg)
    {
        if (reg == 0u)
            return;
        cpu.gpr[reg] = value;
        writtenReg = reg;
    }

    void IopCpuCore::scheduleLoad(uint32_t reg, uint32_t value, bool &scheduled, uint32_t &scheduledReg, uint32_t &scheduledValue)
    {
        if (reg == 0u)
            return;
        scheduled = true;
        scheduledReg = reg;
        scheduledValue = value;
    }

    void IopCpuCore::raiseException(IopCpuState &cpu, uint32_t code, uint32_t faultPc, bool delaySlot, std::optional<uint32_t> badAddress) const
    {
        uint32_t cause = cpu.cop0[13] & ~0x7Cu;
        cause |= (code & 0x1Fu) << 2u;
        if (delaySlot)
        {
            cause |= 0x80000000u;
            cpu.cop0[14] = faultPc - 4u;
        }
        else
        {
            cause &= ~0x80000000u;
            cpu.cop0[14] = faultPc;
        }
        cpu.cop0[13] = cause;
        if (badAddress)
            cpu.cop0[8] = *badAddress;
        const uint32_t status = cpu.cop0[12];
        cpu.cop0[12] = (status & ~0x3Fu) | ((status << 2u) & 0x3Fu);
        cpu.pc = (status & (1u << 22u)) ? 0xBFC00180u : 0x80000080u;
        cpu.branchPending = false;
        cpu.pendingLoad = false;
        cpu.exception = true;
    }

    bool IopCpuCore::executeInstruction(IopCpuState &cpu)
    {
        const uint32_t pc = cpu.pc;
        const uint32_t instruction = m_memory.read32(pc);
        if (const char *rangeText = std::getenv("PS2X_IOP_TRACE_PC_RANGE"))
        {
            static bool parsed = false;
            static uint32_t rangeStart = 0u;
            static uint32_t rangeEnd = 0u;
            static uint32_t emitted = 0u;
            if (!parsed)
            {
                parsed = true;
                char *end = nullptr;
                const unsigned long start = std::strtoul(rangeText, &end, 0);
                if (end != rangeText && *end == ':')
                {
                    char *rangeEndText = end + 1;
                    const unsigned long finish = std::strtoul(rangeEndText, &end, 0);
                    if (end != rangeEndText && *end == '\0' &&
                        start <= 0xFFFFFFFFul && finish <= 0xFFFFFFFFul && start < finish)
                    {
                        rangeStart = static_cast<uint32_t>(start);
                        rangeEnd = static_cast<uint32_t>(finish);
                    }
                }
            }
            if (rangeStart <= pc && pc < rangeEnd && emitted++ < 12000u)
            {
                std::cerr << "[IOP_PC_TRACE] pc=0x" << std::hex << pc
                          << " word=0x" << instruction
                          << " v0=0x" << cpu.gpr[2] << " v1=0x" << cpu.gpr[3]
                          << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5]
                          << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7]
                          << " t0=0x" << cpu.gpr[8] << " t1=0x" << cpu.gpr[9]
                          << " s0=0x" << cpu.gpr[16] << " s1=0x" << cpu.gpr[17]
                          << " s2=0x" << cpu.gpr[18] << " s3=0x" << cpu.gpr[19]
                          << " s4=0x" << cpu.gpr[20] << " s5=0x" << cpu.gpr[21]
                          << " sp=0x" << cpu.gpr[29] << " ra=0x" << cpu.gpr[31]
                          << std::dec << '\n';
            }
        }
        if (std::getenv("PS2X_IOP_TRACE_CDVD_CODE") != nullptr &&
            pc >= 0x00015C00u && pc < 0x00016800u)
        {
            static uint32_t emittedCdvdInstructions = 0u;
            if (emittedCdvdInstructions++ < 1200u)
            {
                std::cerr << "[IOP_CDVD_INSN] pc=0x" << std::hex << pc
                          << " word=0x" << instruction
                          << " v0=0x" << cpu.gpr[2] << " v1=0x" << cpu.gpr[3]
                          << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5]
                          << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7]
                          << " t0=0x" << cpu.gpr[8] << " t1=0x" << cpu.gpr[9]
                          << " s0=0x" << cpu.gpr[16] << " s1=0x" << cpu.gpr[17]
                          << " s2=0x" << cpu.gpr[18] << " sp=0x" << cpu.gpr[29]
                          << " ra=0x" << cpu.gpr[31] << std::dec << '\n';
            }
        }
        if (std::getenv("PS2X_IOP_TRACE_CDVDFS_CODE") != nullptr &&
            ((pc >= 0x00013100u && pc < 0x00013200u) ||
             (pc >= 0x00013200u && pc < 0x00013B00u) ||
             (pc >= 0x00014AF0u && pc < 0x00014C00u) ||
             (pc >= 0x00014E00u && pc < 0x00014F00u) ||
             (pc >= 0x000151A0u && pc < 0x00015280u)))
        {
            static uint32_t emittedCdvdFsInstructions = 0u;
            if (emittedCdvdFsInstructions++ < 3000u)
            {
                std::cerr << "[IOP_CDVDFS_INSN] pc=0x" << std::hex << pc
                          << " word=0x" << instruction
                          << " v0=0x" << cpu.gpr[2] << " v1=0x" << cpu.gpr[3]
                          << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5]
                          << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7]
                          << " t0=0x" << cpu.gpr[8] << " t1=0x" << cpu.gpr[9]
                          << " s0=0x" << cpu.gpr[16] << " s1=0x" << cpu.gpr[17]
                          << " s2=0x" << cpu.gpr[18] << " sp=0x" << cpu.gpr[29]
                          << " ra=0x" << cpu.gpr[31] << std::dec << '\n';
            }
        }
        if (std::getenv("PS2X_IOP_TRACE_SIF_CODE") != nullptr &&
            pc >= 0x00010000u && pc < 0x00013F00u)
        {
            static uint32_t emittedSifInstructions = 0u;
            if (emittedSifInstructions++ < 20000u)
            {
                std::cerr << "[IOP_SIF_INSN] pc=0x" << std::hex << pc
                          << " word=0x" << instruction
                          << " v0=0x" << cpu.gpr[2] << " v1=0x" << cpu.gpr[3]
                          << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5]
                          << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7]
                          << " t0=0x" << cpu.gpr[8] << " t1=0x" << cpu.gpr[9]
                          << " s0=0x" << cpu.gpr[16] << " s1=0x" << cpu.gpr[17]
                          << " s2=0x" << cpu.gpr[18] << " sp=0x" << cpu.gpr[29]
                          << " ra=0x" << cpu.gpr[31] << std::dec << '\n';
            }
        }
        if (std::getenv("PS2X_IOP_TRACE_CDVD_HELPER") != nullptr &&
            pc >= 0x00016190u && pc < 0x00016280u)
        {
            static uint32_t emittedCdvdHelperInstructions = 0u;
            if (emittedCdvdHelperInstructions++ < 500u)
            {
                std::cerr << "[IOP_CDVD_HELPER_INSN] pc=0x" << std::hex << pc
                          << " word=0x" << instruction
                          << " v0=0x" << cpu.gpr[2] << " v1=0x" << cpu.gpr[3]
                          << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5]
                          << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7]
                          << " ra=0x" << cpu.gpr[31] << std::dec << '\n';
            }
        }
        if (std::getenv("PS2X_IOP_TRACE_IOMAN_CODE") != nullptr &&
            ((pc >= 0x00010D30u && pc < 0x00010E10u) ||
             (pc >= 0x00011200u && pc < 0x00011230u)))
        {
            std::cerr << "[IOP_INSN] pc=0x" << std::hex << pc
                      << " word=0x" << instruction
                      << " at=0x" << cpu.gpr[1]
                      << " a0=0x" << cpu.gpr[4]
                      << " a1=0x" << cpu.gpr[5]
                      << " a2=0x" << cpu.gpr[6]
                      << " a3=0x" << cpu.gpr[7]
                      << " t0=0x" << cpu.gpr[8]
                      << " t1=0x" << cpu.gpr[9]
                      << " t2=0x" << cpu.gpr[10]
                      << " t3=0x" << cpu.gpr[11]
                      << " s0=0x" << cpu.gpr[16]
                      << " s1=0x" << cpu.gpr[17]
                      << " s2=0x" << cpu.gpr[18]
                      << " s3=0x" << cpu.gpr[19]
                      << " v0=0x" << cpu.gpr[2]
                      << " v1=0x" << cpu.gpr[3]
                      << " ra=0x" << cpu.gpr[31]
                      << std::dec << '\n';
        }
        const bool wasDelaySlot = cpu.branchPending;
        const uint32_t priorBranchTarget = cpu.branchTarget;

        cpu.branchPending = false;
        cpu.exception = false;
        cpu.yielded = false;

        const uint32_t opcode = instruction >> 26u;
        const uint32_t rs = (instruction >> 21u) & 31u;
        const uint32_t rt = (instruction >> 16u) & 31u;
        const uint32_t rd = (instruction >> 11u) & 31u;
        const uint32_t sa = (instruction >> 6u) & 31u;
        const uint32_t funct = instruction & 63u;
        const uint32_t imm = instruction & 0xFFFFu;
        const int32_t simm = static_cast<int16_t>(imm);
        const uint32_t nextPc = pc + 4u;

        uint32_t writtenReg = 0u;
        bool scheduledLoad = false;
        uint32_t scheduledReg = 0u;
        uint32_t scheduledValue = 0u;
        bool newBranch = false;
        uint32_t newBranchTarget = 0u;

        const auto traceCdvdAccess = [pc](const char *operation, uint32_t address,
                                        uint32_t value, uint32_t reg)
        {
            if (std::getenv("PS2X_IOP_TRACE_CDVD_MMIO") == nullptr)
                return;
            const uint32_t physical = IopMemory::physicalAddress(address);
            const bool cdvdRegister = physical >= 0x1F402000u && physical < 0x1F402020u;
            const bool cdvdDmaRegister = physical >= 0x1F8010B0u && physical <= 0x1F8010BCu;
            if (!cdvdRegister && !cdvdDmaRegister)
                return;
            static uint32_t emitted = 0u;
            if (emitted++ >= 256u)
                return;
            std::cerr << "[IOP_CDVD_MMIO] " << operation << " pc=0x" << std::hex << pc
                      << " address=0x" << address << " value=0x" << value
                      << " reg=" << std::dec << reg << '\n';
        };

        const auto traceAddressAccess = [pc](const char *operation, uint32_t address,
                                              uint32_t value, uint32_t reg)
        {
            const char *targetText = std::getenv("PS2X_IOP_TRACE_ADDRESS");
            if (!targetText || *targetText == '\0')
                return;
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(targetText, &end, 0);
            if (end == targetText || *end != '\0' || parsed > 0xFFFFFFFFul)
                return;
            const uint32_t target = IopMemory::physicalAddress(static_cast<uint32_t>(parsed));
            if (IopMemory::physicalAddress(address) != target)
                return;
            static uint32_t emitted = 0u;
            if (emitted++ >= 512u)
                return;
            std::cerr << "[IOP_ADDRESS_ACCESS] " << operation << " pc=0x" << std::hex << pc
                      << " address=0x" << address << " value=0x" << value
                      << " reg=" << std::dec << reg << '\n';
        };

        auto branch = [&](bool condition)
        {
            if (condition)
            {
                newBranch = true;
                newBranchTarget = nextPc + (static_cast<uint32_t>(simm) << 2u);
            }
        };
        auto write = [&](uint32_t reg, uint32_t value)
        {
            writeRegister(cpu, reg, value, writtenReg);
        };
        auto load = [&](uint32_t reg, uint32_t value)
        {
            scheduleLoad(reg, value, scheduledLoad, scheduledReg, scheduledValue);
        };
        auto overflowAdd = [&](int32_t lhs, int32_t rhs, uint32_t reg)
        {
            const int64_t result = static_cast<int64_t>(lhs) + rhs;
            if (result > std::numeric_limits<int32_t>::max() || result < std::numeric_limits<int32_t>::min())
                raiseException(cpu, 12u, pc, wasDelaySlot);
            else
                write(reg, static_cast<uint32_t>(static_cast<int32_t>(result)));
        };
        auto overflowSub = [&](int32_t lhs, int32_t rhs, uint32_t reg)
        {
            const int64_t result = static_cast<int64_t>(lhs) - rhs;
            if (result > std::numeric_limits<int32_t>::max() || result < std::numeric_limits<int32_t>::min())
                raiseException(cpu, 12u, pc, wasDelaySlot);
            else
                write(reg, static_cast<uint32_t>(static_cast<int32_t>(result)));
        };

        // TODO kill this magic number and make it a constant somewhere
        switch (opcode)
        {
        case 0x00:
            switch (funct)
            {
            case 0x00:
                write(rd, cpu.gpr[rt] << sa);
                break;
            case 0x02:
                write(rd, cpu.gpr[rt] >> sa);
                break;
            case 0x03:
                write(rd, static_cast<uint32_t>(static_cast<int32_t>(cpu.gpr[rt]) >> sa));
                break;
            case 0x04:
                write(rd, cpu.gpr[rt] << (cpu.gpr[rs] & 31u));
                break;
            case 0x06:
                write(rd, cpu.gpr[rt] >> (cpu.gpr[rs] & 31u));
                break;
            case 0x07:
                write(rd, static_cast<uint32_t>(static_cast<int32_t>(cpu.gpr[rt]) >> (cpu.gpr[rs] & 31u)));
                break;
            case 0x08:
                newBranch = true;
                newBranchTarget = cpu.gpr[rs];
                break;
            case 0x09:
                write(rd ? rd : 31u, pc + 8u);
                newBranch = true;
                newBranchTarget = cpu.gpr[rs];
                break;
            case 0x0C:
                raiseException(cpu, 8u, pc, wasDelaySlot);
                break;
            case 0x0D:
                raiseException(cpu, 9u, pc, wasDelaySlot);
                break;
            case 0x10:
                write(rd, cpu.hi);
                break;
            case 0x11:
                cpu.hi = cpu.gpr[rs];
                break;
            case 0x12:
                write(rd, cpu.lo);
                break;
            case 0x13:
                cpu.lo = cpu.gpr[rs];
                break;
            case 0x18:
            {
                const int64_t result = static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[rs])) * static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[rt]));
                cpu.lo = static_cast<uint32_t>(result);
                cpu.hi = static_cast<uint32_t>(static_cast<uint64_t>(result) >> 32u);
                break;
            }
            case 0x19:
            {
                const uint64_t result = static_cast<uint64_t>(cpu.gpr[rs]) * cpu.gpr[rt];
                cpu.lo = static_cast<uint32_t>(result);
                cpu.hi = static_cast<uint32_t>(result >> 32u);
                break;
            }
            case 0x1A:
            {
                const int32_t lhs = static_cast<int32_t>(cpu.gpr[rs]);
                const int32_t rhs = static_cast<int32_t>(cpu.gpr[rt]);
                if (rhs == 0)
                {
                    cpu.lo = lhs >= 0 ? 0xFFFFFFFFu : 1u;
                    cpu.hi = static_cast<uint32_t>(lhs);
                }
                else if (lhs == std::numeric_limits<int32_t>::min() && rhs == -1)
                {
                    cpu.lo = static_cast<uint32_t>(lhs);
                    cpu.hi = 0u;
                }
                else
                {
                    cpu.lo = static_cast<uint32_t>(lhs / rhs);
                    cpu.hi = static_cast<uint32_t>(lhs % rhs);
                }
                break;
            }
            case 0x1B:
                if (cpu.gpr[rt] == 0u)
                {
                    cpu.lo = 0xFFFFFFFFu;
                    cpu.hi = cpu.gpr[rs];
                }
                else
                {
                    cpu.lo = cpu.gpr[rs] / cpu.gpr[rt];
                    cpu.hi = cpu.gpr[rs] % cpu.gpr[rt];
                }
                break;
            case 0x20:
                overflowAdd(static_cast<int32_t>(cpu.gpr[rs]), static_cast<int32_t>(cpu.gpr[rt]), rd);
                break;
            case 0x21:
                write(rd, cpu.gpr[rs] + cpu.gpr[rt]);
                break;
            case 0x22:
                overflowSub(static_cast<int32_t>(cpu.gpr[rs]), static_cast<int32_t>(cpu.gpr[rt]), rd);
                break;
            case 0x23:
                write(rd, cpu.gpr[rs] - cpu.gpr[rt]);
                break;
            case 0x24:
                write(rd, cpu.gpr[rs] & cpu.gpr[rt]);
                break;
            case 0x25:
                write(rd, cpu.gpr[rs] | cpu.gpr[rt]);
                break;
            case 0x26:
                write(rd, cpu.gpr[rs] ^ cpu.gpr[rt]);
                break;
            case 0x27:
                write(rd, ~(cpu.gpr[rs] | cpu.gpr[rt]));
                break;
            case 0x2A:
                write(rd, static_cast<int32_t>(cpu.gpr[rs]) < static_cast<int32_t>(cpu.gpr[rt]) ? 1u : 0u);
                break;
            case 0x2B:
                write(rd, cpu.gpr[rs] < cpu.gpr[rt] ? 1u : 0u);
                break;
            default:
                raiseException(cpu, 10u, pc, wasDelaySlot);
                break;
            }
            break;
        case 0x01:
            switch (rt)
            {
            case 0x00:
                branch(static_cast<int32_t>(cpu.gpr[rs]) < 0);
                break;
            case 0x01:
                branch(static_cast<int32_t>(cpu.gpr[rs]) >= 0);
                break;
            case 0x10:
                write(31u, pc + 8u);
                branch(static_cast<int32_t>(cpu.gpr[rs]) < 0);
                break;
            case 0x11:
                write(31u, pc + 8u);
                branch(static_cast<int32_t>(cpu.gpr[rs]) >= 0);
                break;
            default:
                raiseException(cpu, 10u, pc, wasDelaySlot);
                break;
            }
            break;
        case 0x02:
            newBranch = true;
            newBranchTarget = (nextPc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2u);
            break;
        case 0x03:
            write(31u, pc + 8u);
            newBranch = true;
            newBranchTarget = (nextPc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2u);
            break;
        case 0x04:
            branch(cpu.gpr[rs] == cpu.gpr[rt]);
            break;
        case 0x05:
            branch(cpu.gpr[rs] != cpu.gpr[rt]);
            break;
        case 0x06:
            branch(static_cast<int32_t>(cpu.gpr[rs]) <= 0);
            break;
        case 0x07:
            branch(static_cast<int32_t>(cpu.gpr[rs]) > 0);
            break;
        case 0x08:
            overflowAdd(static_cast<int32_t>(cpu.gpr[rs]), simm, rt);
            break;
        case 0x09:
            write(rt, cpu.gpr[rs] + static_cast<uint32_t>(simm));
            break;
        case 0x0A:
            write(rt, static_cast<int32_t>(cpu.gpr[rs]) < simm ? 1u : 0u);
            break;
        case 0x0B:
            write(rt, cpu.gpr[rs] < static_cast<uint32_t>(simm) ? 1u : 0u);
            break;
        case 0x0C:
            write(rt, cpu.gpr[rs] & imm);
            break;
        case 0x0D:
            write(rt, cpu.gpr[rs] | imm);
            break;
        case 0x0E:
            write(rt, cpu.gpr[rs] ^ imm);
            break;
        case 0x0F:
            write(rt, imm << 16u);
            break;
        case 0x10:
        {
            const uint32_t copRs = rs;
            if (copRs == 0x00)
                load(rt, cpu.cop0[rd]);
            else if (copRs == 0x04)
                cpu.cop0[rd] = cpu.gpr[rt];
            else if (copRs == 0x10 && funct == 0x10)
            {
                const uint32_t status = cpu.cop0[12];
                cpu.cop0[12] = (status & ~0x0Fu) | ((status >> 2u) & 0x0Fu);
            }
            else
                raiseException(cpu, 10u, pc, wasDelaySlot);
            break;
        }
        case 0x20:
        case 0x24:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint8_t value = m_memory.read8(address);
            traceCdvdAccess(opcode == 0x20 ? "lb" : "lbu", address, value, rt);
            traceAddressAccess(opcode == 0x20 ? "lb" : "lbu", address, value, rt);
            load(rt, opcode == 0x20
                         ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(value)))
                         : value);
            break;
        }
        case 0x21:
        case 0x25:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            if (address & 1u)
            {
                raiseException(cpu, 4u, pc, wasDelaySlot, address);
                break;
            }
            const uint16_t value = m_memory.read16(address);
            traceCdvdAccess(opcode == 0x21 ? "lh" : "lhu", address, value, rt);
            traceAddressAccess(opcode == 0x21 ? "lh" : "lhu", address, value, rt);
            load(rt, opcode == 0x21 ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value))) : value);
            break;
        }
        case 0x22:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint32_t memory = m_memory.read32(address & ~3u);
            const uint32_t old = cpu.gpr[rt];
            static constexpr uint32_t masks[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
            static constexpr uint32_t shifts[4] = {24u, 16u, 8u, 0u};
            load(rt, (old & masks[address & 3u]) | (memory << shifts[address & 3u]));
            break;
        }
        case 0x23:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            if (address & 3u)
            {
                raiseException(cpu, 4u, pc, wasDelaySlot, address);
                break;
            }
            const uint32_t value = m_memory.read32(address);
            traceCdvdAccess("lw", address, value, rt);
            traceAddressAccess("lw", address, value, rt);
            load(rt, value);
            break;
        }
        case 0x26:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint32_t memory = m_memory.read32(address & ~3u);
            const uint32_t old = cpu.gpr[rt];
            static constexpr uint32_t masks[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
            static constexpr uint32_t shifts[4] = {0u, 8u, 16u, 24u};
            load(rt, (old & masks[address & 3u]) | (memory >> shifts[address & 3u]));
            break;
        }
        case 0x28:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            traceCdvdAccess("sb", address, static_cast<uint8_t>(cpu.gpr[rt]), rt);
            traceAddressAccess("sb", address, static_cast<uint8_t>(cpu.gpr[rt]), rt);
            m_memory.write8(address, static_cast<uint8_t>(cpu.gpr[rt]));
            break;
        }
        case 0x29:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            if (address & 1u)
            {
                raiseException(cpu, 5u, pc, wasDelaySlot, address);
                break;
            }
            traceCdvdAccess("sh", address, static_cast<uint16_t>(cpu.gpr[rt]), rt);
            traceAddressAccess("sh", address, static_cast<uint16_t>(cpu.gpr[rt]), rt);
            m_memory.write16(address, static_cast<uint16_t>(cpu.gpr[rt]));
            break;
        }
        case 0x2A:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint32_t aligned = address & ~3u;
            const uint32_t old = m_memory.read32(aligned);
            const uint32_t value = cpu.gpr[rt];
            uint32_t result = old;
            switch (address & 3u)
            {
            case 0u:
                result = (old & 0xFFFFFF00u) | (value >> 24u);
                break;
            case 1u:
                result = (old & 0xFFFF0000u) | (value >> 16u);
                break;
            case 2u:
                result = (old & 0xFF000000u) | (value >> 8u);
                break;
            case 3u:
                result = value;
                break;
            }
            m_memory.write32(aligned, result);
            break;
        }
        case 0x2B:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            if (address & 3u)
            {
                raiseException(cpu, 5u, pc, wasDelaySlot, address);
                break;
            }
            traceCdvdAccess("sw", address, cpu.gpr[rt], rt);
            traceAddressAccess("sw", address, cpu.gpr[rt], rt);
            m_memory.write32(address, cpu.gpr[rt]);
            break;
        }
        case 0x2E:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint32_t aligned = address & ~3u;
            const uint32_t old = m_memory.read32(aligned);
            const uint32_t value = cpu.gpr[rt];
            uint32_t result = old;
            switch (address & 3u)
            {
            case 0u:
                result = value;
                break;
            case 1u:
                result = (old & 0x000000FFu) | (value << 8u);
                break;
            case 2u:
                result = (old & 0x0000FFFFu) | (value << 16u);
                break;
            case 3u:
                result = (old & 0x00FFFFFFu) | (value << 24u);
                break;
            }
            m_memory.write32(aligned, result);
            break;
        }
        default:
            raiseException(cpu, 10u, pc, wasDelaySlot);
            break;
        }

        cpu.gpr[0] = 0u;
        if (cpu.exception)
            return !cpu.stopped;

        if (cpu.pendingLoad)
        {
            if (cpu.pendingLoadReg != 0u && cpu.pendingLoadReg != writtenReg)
                cpu.gpr[cpu.pendingLoadReg] = cpu.pendingLoadValue;
            cpu.pendingLoad = false;
        }
        if (scheduledLoad)
        {
            cpu.pendingLoad = true;
            cpu.pendingLoadReg = scheduledReg;
            cpu.pendingLoadValue = scheduledValue;
        }
        cpu.gpr[0] = 0u;

        if (wasDelaySlot)
        {
            cpu.pc = priorBranchTarget;
            cpu.branchPending = false;
        }
        else
        {
            cpu.pc = nextPc;
            cpu.branchPending = newBranch;
            cpu.branchTarget = newBranchTarget;
        }
        return !cpu.stopped;
    }
}
