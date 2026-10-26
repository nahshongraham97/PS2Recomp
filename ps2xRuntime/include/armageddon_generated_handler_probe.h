#pragma once
// Explicit authored-fixture test entry point for the ACTUAL linked translations.
// This contains setup/observation only: no replacement implementation of either
// guest handler, no numeric-syscall substitution, and no equivalence claim for
// the original PS2 kernel. Install beside the diagnostic runner's other headers.
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <string>
#include <utility>

namespace armageddon_handler_probe {
inline constexpr uint32_t base = 0x00010000u;
inline constexpr size_t wordCount = 64;

inline uint64_t low64(const R5900Context &context, unsigned reg) {
    uint64_t value = 0;
    std::memcpy(&value, &context.r[reg], sizeof(value));
    return value;
}
inline void setLow64(R5900Context &context, unsigned reg, uint64_t value) {
    // Fixtures deliberately zero the upper 64 bits. Comparison is explicitly
    // restricted to the low 64 bits used by these scalar instructions.
    std::memset(&context.r[reg], 0, sizeof(context.r[reg]));
    std::memcpy(&context.r[reg], &value, sizeof(value));
}
}

inline int armageddonGeneratedHandlerProbe(const std::string &elf,
                                          const std::string &fixturePath,
                                          const std::string &outputPath) {
    using namespace armageddon_handler_probe;
    try {
        std::ifstream input(fixturePath);
        std::ofstream output(outputPath, std::ios::trunc);
        if (!input || !output) throw std::runtime_error("Cannot open handler probe files");
        auto runtime = std::make_unique<PS2Runtime>();
        runtime->setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
        if (!runtime->initializeHeadless() || !runtime->loadELF(elf))
            throw std::runtime_error("Actual runtime/ELF initialization failed");
        auto *rdram = runtime->memory().getRDRAM();
        if (!rdram) throw std::runtime_error("Actual runtime RDRAM unavailable");
        std::string line;
        size_t count = 0;
        while (std::getline(input, line)) {
            if (line.empty()) throw std::invalid_argument("Empty handler fixture");
            if (++count > 2048) throw std::invalid_argument("Too many handler fixtures");
            std::istringstream row(line);
            uint32_t handler = 0;
            std::array<uint64_t, 3> arguments{};
            std::array<uint32_t, wordCount> words{};
            if (!(row >> std::hex >> handler >> arguments[0] >> arguments[1] >> arguments[2]))
                throw std::invalid_argument("Malformed handler fixture arguments");
            if (handler != 0x00116b08u && handler != 0x00116b40u)
                throw std::invalid_argument("Unsupported handler fixture target");
            for (auto &word : words)
                if (!(row >> std::hex >> word)) throw std::invalid_argument("Missing fixture word");
            std::string extra;
            if (row >> extra) throw std::invalid_argument("Unexpected fixture field");
            if (!runtime->hasFunction(handler))
                throw std::runtime_error("Actual generated handler is unregistered");
            std::memcpy(rdram + base, words.data(), sizeof(words));

            R5900Context main{};
            // Every run is an authored active invocation, not guest boot state.
            // bindMainContextForSyscall makes the scheduler's real main thread
            // current. invokeCurrent installs the actual GuestInvocation record.
            auto &scheduler = runtime->eeScheduler();
            scheduler.reset(rdram, main);
            scheduler.bindMainContextForSyscall(main, rdram);
            GuestInvocation invocation;
            invocation.kind = GuestInvocationKind::SyscallOverride;
            invocation.tag = handler;
            invocation.context.pc = handler;
            for (unsigned reg = 1; reg < 32; ++reg)
                setLow64(invocation.context, reg, UINT64_C(0x0123000000000000) | reg);
            for (unsigned i = 0; i < arguments.size(); ++i)
                setLow64(invocation.context, 4u + i, arguments[i]);
            setLow64(invocation.context, 29, 0x01fff000u);
            setLow64(invocation.context, 31, 0);
            bool transferred = false;
            try { scheduler.invokeCurrent(std::move(invocation)); }
            catch (const EeDispatcherTransfer &) { transferred = true; }
            if (!transferred || !scheduler.currentThread() ||
                scheduler.currentThread()->invocations.size() != 1)
                throw std::runtime_error("Actual GuestInvocation was not installed");
            auto *context = scheduler.currentContext();
            if (!context) throw std::runtime_error("Actual invocation context unavailable");
            unsigned dispatches = 0;
            while (context->pc != 0 && !runtime->isStopRequested()) {
                if (++dispatches > 4096u)
                    throw std::runtime_error("Generated handler exceeded dispatch bound");
                if (!runtime->hasFunction(context->pc))
                    throw std::runtime_error("Generated handler continuation is unregistered");
                // The callable is supplied by the real generated registration
                // table and invoked using the actual PS2Runtime/R5900Context ABI.
                const auto function = runtime->lookupFunction(context->pc);
                function(rdram, context, runtime.get());
            }
            if (runtime->isStopRequested() || context->pc != 0)
                throw std::runtime_error("Actual generated handler stopped before legitimate invocation return");
            if (scheduler.currentThread()->invocations.size() != 1)
                throw std::runtime_error("Generated handler unexpectedly altered invocation ownership");

            output << "{\"pc\":" << context->pc << ",\"gpr_low64\":[";
            for (unsigned reg = 0; reg < 32; ++reg) {
                if (reg) output << ',';
                output << low64(*context, reg);
            }
            output << "],\"words\":[";
            std::memcpy(words.data(), rdram + base, sizeof(words));
            for (size_t i = 0; i < words.size(); ++i) {
                if (i) output << ',';
                output << words[i];
            }
            output << "]}\n";
            output.flush();
            if (!output) throw std::runtime_error("Cannot flush handler probe result");
            scheduler.currentThread()->invocations.clear();
        }
        if (!input.eof() || count == 0) throw std::invalid_argument("No complete handler fixtures");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Actual generated-handler probe failed: " << error.what() << '\n';
        return 2;
    }
}
